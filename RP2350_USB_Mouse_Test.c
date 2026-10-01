// ================================================================
// RP2350 — USB 鼠标 → FC 裕兴 V9.2 协议桥接（精简版）
//
// 硬件：
//   GP2  CLK     ← 输入   裕兴 → Pico
//   GP3  STROBE  ← 输入   裕兴 → Pico
//   GP4  DATA    → 输出   Pico → 裕兴（开漏，内部上拉兜底，未接外部电阻）
//   GP12 USB-A D+（D- = GP13），PIO-USB 主机，接 USB HID 鼠标
//
// 裕兴协议（Pico 为从机，被动应答）：
//   每一位：① 等 STROBE 低脉冲  ② 摆 1 位到 DATA  ③ 等 CLK 低脉冲
//   27 位 = 3 包 × 9 位，MSB 先出。
//   包1: 0 1 L R X7 X6 Y7 Y6 1
//   包2: 0 0 Y5 Y4 Y3 Y2 Y1 Y0 1
//   包3: 0 0 X5 X4 X3 X2 X1 X0 1
//   位移 8 位补码；按键高有效（0 = 抬起，1 = 按下）。
//
// 帧对齐（空隙驱动）：
//   帧内位间隔约 130us，帧间空隙约 16.4ms，相差两个数量级，故以 >3ms
//   的 STROBE 间隔判为帧边界：
//     · 帧首定位：只在跨过空隙后的第一个 STROBE 上开帧（对应 MSB）；
//     · 位序错位自愈：帧内某位间隔 >3ms = 已跨帧 → 丢弃本帧余下各位，
//       下一帧重新定位，错位最多持续一帧。
//
// 位移语义：
//   裕兴约 50Hz 轮询一帧，鼠标报告约 125Hz。core0 把每份报告的 dx/dy
//   累加进共享变量，core1 每帧起始一次性取走并清零；本帧作废则原样还回。
//   累积超过 ±127 时按最大分量等比缩放，保留方向。
//
// 日志（CDC，Type-C 插电脑看）：
//   [MOUSE]  USB 鼠标位移
//   [FRAME]  裕兴每帧发出的数据
//   [ERR]    超时 / 帧不完整
//   [STAT]   每 200 帧汇总
// ================================================================

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"

#include "pio_usb.h"
#include "tusb.h"

// ------------------------------------------------------------------
// 引脚
// ------------------------------------------------------------------
#define PIN_CLK       2     // GP2 — CLK，输入
#define PIN_STROBE    3     // GP3 — STROBE，输入
#define PIN_DATA      4     // GP4 — DATA，输出（开漏）
#define PIN_USB_DP   12     // USB-A 母口 D+，D- = GP13

// ------------------------------------------------------------------
// 时序常量（微秒）
// ------------------------------------------------------------------
#define TIMEOUT_FRAME_US   25000u   // 等帧首 STROBE 的超时（主机停摆判据）
#define TIMEOUT_HOLD_US   100000u   // 等帧内 STROBE / CLK 的超时
#define GAP_FRAME_US        3000u   // >3ms 判为帧间空隙（帧边界）
#define ACC_LIMIT           8192    // 累积器上限（防溢出，非输出限幅）

// ------------------------------------------------------------------
// core1 → core0 日志队列（CDC 只能由 core0 写，core1 只投递）
// ------------------------------------------------------------------
typedef struct { char msg[96]; } log_item_t;
static queue_t log_queue;

static void log_put(const char *fmt, ...) {
    log_item_t item;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(item.msg, sizeof(item.msg), fmt, ap);
    va_end(ap);
    (void)queue_try_add(&log_queue, &item);   // 队列满则丢，绝不阻塞
}

// ------------------------------------------------------------------
// 鼠标共享状态（core0 写，core1 取）
// ------------------------------------------------------------------
static critical_section_t g_mouse_cs;
static int32_t g_acc_dx    = 0;
static int32_t g_acc_dy    = 0;
static uint8_t g_buttons   = 0;
static uint8_t g_btn_latch = 0;
static bool    g_mouse_present = false;

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void mouse_accumulate(uint8_t buttons, int32_t dx, int32_t dy) {
    critical_section_enter_blocking(&g_mouse_cs);
    g_acc_dx    = clamp_i32(g_acc_dx + dx, -ACC_LIMIT, ACC_LIMIT);
    g_acc_dy    = clamp_i32(g_acc_dy + dy, -ACC_LIMIT, ACC_LIMIT);
    g_buttons   = (uint8_t)(buttons & 0x07);
    g_btn_latch |= (uint8_t)(buttons & 0x07);   // 短按锁存，避免被采样漏掉
    critical_section_exit(&g_mouse_cs);
}

static void mouse_take(int32_t *dx, int32_t *dy, uint8_t *btn) {
    critical_section_enter_blocking(&g_mouse_cs);
    *dx  = g_acc_dx;
    *dy  = g_acc_dy;
    *btn = (uint8_t)(g_buttons | g_btn_latch);
    g_acc_dx    = 0;
    g_acc_dy    = 0;
    g_btn_latch = 0;
    critical_section_exit(&g_mouse_cs);
}

static void mouse_give_back(int32_t dx, int32_t dy) {
    critical_section_enter_blocking(&g_mouse_cs);
    g_acc_dx = clamp_i32(g_acc_dx + dx, -ACC_LIMIT, ACC_LIMIT);
    g_acc_dy = clamp_i32(g_acc_dy + dy, -ACC_LIMIT, ACC_LIMIT);
    critical_section_exit(&g_mouse_cs);
}

// ------------------------------------------------------------------
// DATA 开漏输出
//   1 → 释放总线（靠内部上拉拉高）
//   0 → 强下拉
// ------------------------------------------------------------------
static inline void data_put(uint8_t bit) {
    if (bit) {
        gpio_set_dir(PIN_DATA, GPIO_IN);
    } else {
        gpio_put(PIN_DATA, 0);
        gpio_set_dir(PIN_DATA, GPIO_OUT);
    }
}

// ------------------------------------------------------------------
// 等一个低电平脉冲（先等回到高，再等变低）。false = 超时
// ------------------------------------------------------------------
static bool wait_low_pulse(uint pin, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (gpio_get(pin) == 0) {
        if (time_reached(deadline)) return false;
    }
    while (gpio_get(pin) != 0) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

// ------------------------------------------------------------------
// 组装 27 位帧
// ------------------------------------------------------------------
static uint32_t build_frame(uint8_t buttons, int8_t dx, int8_t dy) {
    uint8_t ux = (uint8_t)dx;               // 8 位补码
    uint8_t uy = (uint8_t)dy;

    uint8_t low_x  = (uint8_t)(ux & 0x3F);
    uint8_t low_y  = (uint8_t)(uy & 0x3F);
    uint8_t high_x = (uint8_t)((ux >> 6) & 0x03);
    uint8_t high_y = (uint8_t)((uy >> 6) & 0x03);

    uint8_t lb = (buttons & 0x01) ? 1 : 0;
    uint8_t rb = (buttons & 0x02) ? 1 : 0;

    uint8_t pkt1 = (uint8_t)((1u << 7) | (lb << 6) | (rb << 5) |
                             (high_x << 3) | (high_y << 1) | 1u);
    uint8_t pkt2 = (uint8_t)((low_y << 1) | 1u);
    uint8_t pkt3 = (uint8_t)((low_x << 1) | 1u);

    return ((uint32_t)pkt1 << 18) | ((uint32_t)pkt2 << 9) | (uint32_t)pkt3;
}

// ------------------------------------------------------------------
// STROBE 间隔（返回与上一个 STROBE 的间隔；首个沿返回 0）
// ------------------------------------------------------------------
static uint32_t g_last_s = 0;

static uint32_t strobe_gap(uint32_t ts) {
    uint32_t g = (g_last_s == 0) ? 0u : (ts - g_last_s);
    g_last_s = ts;
    return g;
}

// ==================================================================
// 从机应答主循环（core1）
// ==================================================================
static void yuxing_slave_loop(void) {
    uint32_t frame_ok  = 0;
    uint32_t frame_err = 0;

    int32_t  last_dx  = 0x7FFFFFFF;   // 初值保证首帧必打
    int32_t  last_dy  = 0x7FFFFFFF;
    uint32_t last_btn = 0xFFFFFFFFu;
    uint32_t next_frame_log = 0;

    data_put(1);
    g_last_s = 0;

    while (true) {
        int32_t dx, dy;
        uint8_t btn;
        mouse_take(&dx, &dy, &btn);

        // 超过 ±127 时按最大分量等比缩放，保留方向
        int32_t out_dx = dx, out_dy = dy;
        int32_t adx = (dx < 0 ? -dx : dx);
        int32_t ady = (dy < 0 ? -dy : dy);
        int32_t mx  = (ady > adx) ? ady : adx;
        if (mx > 127) {
            int32_t sc = 12700 / mx;            // 百分比缩放系数
            out_dx = (adx * sc) / 100;
            out_dy = (ady * sc) / 100;
            if (dx < 0) out_dx = -out_dx;
            if (dy < 0) out_dy = -out_dy;
        }

        uint32_t frame = build_frame(btn, (int8_t)out_dx, (int8_t)out_dy);

        bool     ok      = true;
        int      sent    = 0;
        uint32_t bit_gap = 0;
        const char *fail = NULL;

        // ---- ① 帧首定位：只在跨过 >3ms 空隙后的第一个 STROBE 上开帧 ----
        int skip_head = 0;
        while (true) {
            if (!wait_low_pulse(PIN_STROBE, TIMEOUT_FRAME_US)) {
                ok = false;
                fail = "STROBE timeout (no frame)";
                break;
            }
            uint32_t ts = time_us_32();
            bool     had_prev = (g_last_s != 0);
            uint32_t g = strobe_gap(ts);
            if (!had_prev || g > GAP_FRAME_US) break;    // 帧首，开帧
            if (++skip_head > 64) {                      // 主机持续乱脉，放弃本帧
                ok = false;
                fail = "STROBE burst";
                break;
            }
        }

        // ---- ② 逐位应答（27 位，MSB 先出）----
        for (int i = 26; ok && i >= 0; i--) {
            if (i != 26) {
                if (!wait_low_pulse(PIN_STROBE, TIMEOUT_HOLD_US)) {
                    ok = false;
                    fail = "STROBE timeout (in frame)";
                    break;
                }
                uint32_t ts = time_us_32();
                uint32_t g  = strobe_gap(ts);
                bit_gap = g;
                if (g > GAP_FRAME_US) {              // 跨帧 = 位序错位，本帧作废
                    ok = false;
                    fail = "bit misalign";
                    break;
                }
            }

            data_put((uint8_t)((frame >> i) & 1u));

            if (!wait_low_pulse(PIN_CLK, TIMEOUT_HOLD_US)) {
                ok = false;
                fail = "CLK timeout (in frame)";
                break;
            }
            sent++;
        }

        data_put(1);                                 // 帧间释放总线

        if (ok) {
            frame_ok++;

            // 裕兴帧数据：内容变化时打印，限流 50ms
            uint32_t now = time_us_32();
            bool changed = (out_dx != last_dx) || (out_dy != last_dy) ||
                           ((uint32_t)btn != last_btn);
            if (changed && now >= next_frame_log) {
                log_put("[FRAME] #%lu dx=%ld dy=%ld btn=%u\n",
                        (unsigned long)frame_ok, (long)out_dx, (long)out_dy,
                        (unsigned)btn);
                last_dx  = out_dx;
                last_dy  = out_dy;
                last_btn = btn;
                next_frame_log = now + 50000u;
            }

            // 每 200 帧一条汇总
            if ((frame_ok % 200u) == 0u) {
                log_put("[STAT] frames=%lu err=%lu raw=%ld/%ld out=%ld/%ld\n",
                        (unsigned long)frame_ok, (unsigned long)frame_err,
                        (long)dx, (long)dy, (long)out_dx, (long)out_dy);
            }
        } else {
            frame_err++;

            // 前 10 条全报，之后每 50 条一条，避免刷屏
            if (frame_err <= 10u || (frame_err % 50u) == 0u) {
                log_put("[ERR] %s (sent %d/27, err=%lu, gap=%luus)\n",
                        fail, sent, (unsigned long)frame_err,
                        (unsigned long)bit_gap);
            }

            mouse_give_back(out_dx, out_dy);         // 本帧没送出去，数据还回
        }
    }
}

static void core1_main(void) {
    sleep_ms(10);
    yuxing_slave_loop();
}

// ==================================================================
// TinyUSB 主机回调（core0）
// ==================================================================
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE) {
        g_mouse_present = true;
        log_put("[USB] mouse mounted %04X:%04X\n", (unsigned)vid, (unsigned)pid);
        (void)tuh_hid_receive_report(dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    (void)instance;
    g_mouse_present = false;
    log_put("[USB] mouse unmounted\n");
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE
        && len >= 3) {

        int32_t dx, dy;
        if (len >= 5) {
            // 16 位小端：[btn, dxL, dxH, dyL, dyH]
            dx = (int16_t)((uint16_t)report[1] | ((uint16_t)report[2] << 8));
            dy = (int16_t)((uint16_t)report[3] | ((uint16_t)report[4] << 8));
        } else {
            // boot 8 位：[btn, dx, dy]
            dx = (int8_t)report[1];
            dy = (int8_t)report[2];
        }

        mouse_accumulate(report[0], dx, dy);

        // USB 鼠标移动数据：有动作时打印，限流 80ms
        static uint32_t next_mouse_log = 0;
        uint32_t now = time_us_32();
        if ((dx || dy || (report[0] & 0x07)) && now >= next_mouse_log) {
            log_put("[MOUSE] dx=%ld dy=%ld btn=%u\n",
                    (long)dx, (long)dy, (unsigned)(report[0] & 0x07));
            next_mouse_log = now + 80000u;
        }
    }

    // 必须续订，否则只能收到第一份报告
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        log_put("[ERR] cannot request report\n");
    }
}

// ==================================================================
// GPIO 初始化（裕兴三信号线）
// ==================================================================
static void init_gpio(void) {
    // GP2 = CLK，输入（裕兴驱动），内部上拉应对开漏场景
    gpio_init(PIN_CLK);
    gpio_set_dir(PIN_CLK, GPIO_IN);
    gpio_set_pulls(PIN_CLK, true, false);

    // GP3 = STROBE，输入（裕兴驱动）
    gpio_init(PIN_STROBE);
    gpio_set_dir(PIN_STROBE, GPIO_IN);
    gpio_set_pulls(PIN_STROBE, true, false);

    // GP4 = DATA，输出（Pico 驱动），开漏语义
    gpio_init(PIN_DATA);
    gpio_put(PIN_DATA, 1);
    gpio_set_dir(PIN_DATA, GPIO_IN);        // 初始释放（高）
    gpio_set_pulls(PIN_DATA, true, false);  // 内部上拉（约 50k）兜底
}

// ==================================================================
// core0 主程序：USB 设备栈（CDC 日志）+ USB 主机栈（鼠标）
// ==================================================================
int main(void) {
    // PIO-USB 要求 sysclk 为 12MHz 整数倍（RP2350 默认 150MHz 不满足）
    bool clk_ok = set_sys_clock_khz(120000, true);

    stdio_init_all();   // UART 调试（GP0/GP1）
    sleep_ms(10);

    init_gpio();
    critical_section_init(&g_mouse_cs);
    queue_init(&log_queue, sizeof(log_item_t), 64);

    printf("\n=== RP2350 USB Mouse -> YuXing V9.2 Bridge (slim) ===\n");
    printf("sysclk=%u kHz (%s)\n",
           (unsigned)(clock_get_hz(clk_sys) / 1000), clk_ok ? "ok" : "FAIL");
    printf("YuXing: GP2=CLK(in) GP3=STROBE(in) GP4=DATA(out)\n");
    printf("USB   : port0 native device -> CDC | port1 PIO-USB host D+=GP12 D-=GP13\n\n");

    // 先起 core1 的实时从机循环，再初始化 USB
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    // port0 = 原生 USB 设备栈（Type-C → CDC 虚拟串口）
    tud_init(0);

    // port1 = Pico-PIO-USB 主机栈（USB-A → HID 鼠标）
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;

    // 【关键】PIO-USB 默认 DP 引脚是 GP0（PIO_USB_DP_PIN_DEFAULT = 0），
    // 而本板 USB-A 母口 D+ 走 GP12、D- 走 GP13。不显式指定的话，
    // PIO 主机会跑到 GP0/GP1 上（那里还是 UART0），永远看不到鼠标。
    pio_cfg.pin_dp = PIN_USB_DP;

    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    bool host_ok = tuh_init(1);

    log_put("[INIT] PIO-USB host %s  D+=GP%u D-=GP%u\n",
            host_ok ? "OK" : "FAIL",
            (unsigned)pio_cfg.pin_dp, (unsigned)(pio_cfg.pin_dp + 1));

    bool cdc_was_connected = false;
    log_item_t item;

    while (true) {
        tud_task();   // 设备栈：CDC 收发
        tuh_task();   // 主机栈：枚举 + HID 报告

        // 把 core1 / 回调投递的日志写到 CDC
        while (queue_try_remove(&log_queue, &item)) {
            tud_cdc_write(item.msg, (uint32_t)strlen(item.msg));
        }
        tud_cdc_write_flush();

        // 主机刚打开串口时打一条横幅
        bool connected = tud_cdc_connected();
        if (connected && !cdc_was_connected) {
            static const char banner[] =
                "\r\n=== USB Mouse -> YuXing V9.2 Bridge ===\r\n"
                "GP2=CLK(in) GP3=STROBE(in) GP4=DATA(out) | USB host D+=GP12\r\n\r\n";
            tud_cdc_write(banner, (uint32_t)(sizeof(banner) - 1));
            tud_cdc_write_flush();
        }
        cdc_was_connected = connected;
    }
}
