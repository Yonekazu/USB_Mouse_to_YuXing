# 裕兴 USB 鼠标桥接器

**版本 0.1 alpha**

用一块 **微雪 RP2350-USB-A** 把现代 USB 鼠标变成裕兴学习机（或FC兼容机）认识的鼠标设备。

支持  **98型、F 型 9.0 ~ 9.2** 裕兴卡带的裕兴鼠标协议。其他版本（含裕兴 VCD 机）未测。
本项目在 RP2350 上实现协议转换：
**USB 鼠标 → RP2350 → 裕兴鼠标协议**，主机侧无需任何改动，插上即用。

---

## 一、硬件

| 项目 | 说明 |
| --- | --- |
| 主控板 | 微雪 RP2350-USB-A |
| USB-A | USB HID 有线或无线鼠标 |

RP2350 GPIO 容忍 5V 电平，可直接与 FC 5V 总线相连，无需电平转换。根据[微雪 RP2350-USB-A 官方文档](https://www.waveshare.net/wiki/RP2350-USB-A)，如果需要 USB 热插拔需拆除板上的 R13 电阻。

Type-C **仅调试时**供电并连接 CDC 调试串口（115200 8N1）。正式使用时建议由 FC 1P 手柄口直接供电。

### 引脚连接

| RP2350 | 方向 | 1P手柄信号 | 说明 |
| --- | --- | --- | --- |
| GP2 | 输入 | CLK | 裕兴 → Pico，锁存脉冲 |
| GP3 | 输入 | STROBE | 裕兴 → Pico，位选通脉冲 |
| GP4 | 输出（开漏） | DATA | Pico → 裕兴，数据位 |
| +5V | — | 电源 | **调试时建议断开**，仅用 USB Type-C 电脑供电 |
| GND | — | 地 | 必须共地 |

> **DATA 采用开漏语义**：写 1 时引脚置高阻（由总线侧上拉拉高），写 0 时强下拉。
> 裕兴主机在 STROBE 下降沿后约 106 µs 发出 CLK 锁存，余量充足，
> 因此无需精确控制 DATA 的释放时机。

> **仅支持 1P 手柄，不支持 2P 手柄或 15 针扩展口**。

---

## 二、协议

裕兴鼠标走三线同步串行协议，**裕兴是主机、设备是从机**，设备被动应答：

```
每一位：① 等 STROBE 低脉冲 → ② 把 1 位摆到 DATA → ③ 等 CLK 低脉冲
```

一帧 27 位 = 3 包 × 9 位，MSB 先出：

| 包 | 位序 |
| --- | --- |
| 包 1 | `0  1  L  R  X7 X6 Y7 Y6 1` |
| 包 2 | `0  0  Y5 Y4 Y3 Y2 Y1 Y0 1` |
| 包 3 | `0  0  X5 X4 X3 X2 X1 X0 1` |

- **L / R**：左键 / 右键，高有效

- **位移**：X/Y 各 8 位，由包 1 的高 2 位 + 包 2/3 的低 6 位拼成，**8 位补码**，范围 ±127
- **按键**：L / R 两位，**高有效**（0 = 抬起，1 = 按下）
- **帧频**：约 **50 Hz**（PAL 主机实测帧周期 ≈ 20 ms，帧内 27 位占 3.6 ms，帧间空隙 ≈ 16.4 ms）

### 帧对齐（本项目的关键设计）

裕兴不发送任何"帧结束"标记，帧边界只能靠时间间隔识别。实测帧内位间隔约 **130 µs**、
帧间空隙约 **16.4 ms**，相差两个数量级，因此以 **> 3 ms 的 STROBE 间隔**作为帧边界判据：

1. **帧首定位**：只在"跨过空隙后的第一个 STROBE"上开帧，该脉冲即帧首（MSB）。
   若遇到的脉冲间隔 ≤ 3 ms，说明是上一帧的残留位，跳过继续找，绝不从半途开帧。
2. **位序错位自愈**：帧内某位间隔 > 3 ms = 已跨帧 → 位序必然错位（X7/Y7 符号位串位，
   表现为光标反向猛跳），立即丢弃本帧余下各位并回到 ① 重新定位，**错位最多持续一帧**。

> 该机制是为解决"漏采帧首导致整帧错位一位、光标乱跳且无法自愈"而设计的。
> 未加此机制前，每帧都有概率出错并持续带错运行。

### 位移语义

裕兴每帧只能表达 ±127，而 USB 鼠标报告约 125 Hz、远快于裕兴 50 Hz 的轮询：

- core0 把每份报告的 dx/dy **累加**进共享变量（临界区保护），core1 每帧起始一次性取走并清零；
- 累积值超过 ±127 时**按最大分量等比缩放**到 ±127 内，保留方向（不硬限幅，避免快速移动时"卡死不跟手"）；
- 本帧作废时把取走的位移**原样还回**，下一帧继续发；
- 按键用 latch 位累积，帧间隙里按一下又松开也不会漏掉。

---

## 三、软件结构

双核分工：

| 核心 | 职责 |
| --- | --- |
| **core0** | TinyUSB 双栈：port0 = 原生 USB Device（Type-C → CDC 日志），port1 = PIO-USB Host（USB-A → HID 鼠标） |
| **core1** | 裕兴从机硬实时应答循环（等 STROBE / CLK，逐位应答），永不返回 |

> ⚠️ sysclk 必须设为 **120 MHz**（PIO-USB 要求 12 MHz 整数倍，RP2350 默认 150 MHz 不满足）。
> ⚠️ PIO-USB 默认 D+ 引脚是 GP0，本板 USB-A 口走 **GP12**，必须在代码里显式设置 `pio_cfg.pin_dp = 12`。

---

## 四、编译

### 依赖

| 依赖 | 版本 |
| --- | --- |
| Pico SDK | 2.1.0（含 tinyusb 等子模块） |
| Pico-PIO-USB | master |
| arm-none-eabi-gcc | 13.3.1 |
| CMake | ≥ 3.13（实测 3.30.5） |
| Ninja | 1.12.1 |

放置到工程根目录：`pico-sdk/`、`pico-pio-usb/`。

### 本机路径适配

`CMakeLists.txt` 中有三个指向作者本机的路径，**在其他机器上编译前请按需修改**：

```cmake
set(PICO_TOOLCHAIN_PATH "D:/pico_tc/xpack-arm-none-eabi-gcc-13.3.1-1.1" CACHE PATH "...")
set(pioasm_DIR          "D:/pico_tc/tools210/pioasm"   CACHE PATH "...")
set(picotool_DIR        "D:/pico_tc/tools210/picotool" CACHE PATH "...")
```

也可以在配置时用 `-D` 覆盖。使用 VS Code 的 **Raspberry Pi Pico** 扩展
（发布者 `raspberry-pi`）时可自动提供工具链。

> PICO_BOARD=pico2 对应微雪 RP2350-USB-A。

> ⚠️ 构建路径**必须为纯 ASCII**。实测路径含中文时 arm-gcc 的 ld 会崩溃。

### 命令行编译

```bash
mkdir build && cd build
cmake -G Ninja \
  -DCMAKE_MAKE_PROGRAM=<ninja 路径> \
  -DPICO_TOOLCHAIN_PATH=<toolchain 路径> \
  -DPICO_BOARD=pico2 \
  ../
ninja
```

产物：`build/RP2350_USB_Mouse_Test.uf2`

---

## 五、烧录与使用

1. 按住板上 **BOOTSEL** 键不放，再插上 Type-C，电脑出现 U 盘盘符
2. 把 `.uf2` 拖进盘符，板子自动复位运行
3. 用串口工具打开板子的 CDC 口查看日志
4. 把 RP2350 的 GP2/GP3/GP4/GND/5V 接到 FC 1P 手柄口对应引脚（USB 调试时建议不接 5V）

---

## 六、日志格式

```
[MOUSE] dx=12 dy=-3 btn=0                              USB 鼠标报告（限流 80ms）
[FRAME] #1523 dx=12 dy=-3 btn=0                        发出的裕兴帧（变化时，限流 50ms）
[STAT]  frames=200 err=1 raw=300/-40 out=127/-16       每 200 帧汇总
[ERR]   bit misalign (sent 18/27, err=3, gap=16402us)  异常帧
```

`[ERR]` 的四种类型：

| 消息 | 含义 |
| --- | --- |
| `STROBE timeout (no frame)` | 25 ms 内没等到帧首 STROBE，主机停摆或掉帧 |
| `STROBE timeout (in frame)` | 帧内等 STROBE 超时（100 ms 兜底） |
| `CLK timeout (in frame)` | 帧内等 CLK 超时（100 ms 兜底） |
| `bit misalign` | 帧内间隔 > 3 ms，判定跨帧位错，本帧作废并重新定位 |

`[STAT]` 中的 `raw` 是鼠标真实累积位移，`out` 是实际发给裕兴的值（超过 ±127 时被等比缩放）。

---

## 七、实现要点 / 踩坑记录

1. **必须开漏驱动 DATA**。推挽直写会在位末主动拉高，污染主机采样窗口，
   把空闲高误读为数据 1 → X7/Y7 符号位被置 1 → 指针跳变。
2. **DATA 释放时不能关内部上拉**。切回输入的同时若显式关闭上拉，总线会浮空，
   所有 '1' 位（含每包 Stop 位）都被读成 0，主机整帧丢弃，指针完全不动。
3. **超时值不能设小**。早期用 1 ms 时每帧第 0 位后就 ABORT；帧首超时取 25 ms、
   帧内超时取 100 ms 才留足余量。
4. **帧内绝不能中途放弃**。裕兴是主机，一帧要读满 27 位；设备若在第 k 位放弃，
   主机会把剩余位读成全 1 → 包 2/3 变 0x7F/0xFF → 位移巨大 → 光标乱跳、按键错乱。
5. **USB HID 报告位宽不能猜**。`tuh_hid_interface_protocol() == MOUSE` 只说明兼容 boot，
   不保证字段就是 8 位。代码按报告长度自动判断（≥5 字节按 16 位小端解析）。

---

## 八、目录结构

```
.
├── CMakeLists.txt                      构建脚本
├── RP2350_USB_Mouse_Test.c             主程序（双核：USB 栈 + 裕兴从机）
├── tusb_config.h                       TinyUSB 配置
├── usb_descriptors.c                   CDC 设备描述符
├── pico_sdk_import.cmake               SDK 导入脚本
├── .gitignore                          Git 忽略规则（build/、依赖目录、中间产物）
├── LICENSE                             MIT 许可证
├── RP2350_USB_Mouse_Test_0.1alpha.uf2       预编译固件（v0.1 alpha，可直接烧录）
├── pico-sdk/                           Pico SDK 2.1.0（依赖，自行获取）
└── pico-pio-usb/                       Pico-PIO-USB（依赖，自行获取）
```

---

## 九、参考链接

- [微雪 RP2350-USB-A 官方文档](https://www.waveshare.net/wiki/RP2350-USB-A)
- [裕兴鼠标协议（nesdev.org）](https://www.nesdev.org/wiki/Yuxing_Mouse)（非官方，仅供参考）

---

## 十、致谢

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
- [TinyUSB](https://github.com/hathach/tinyusb)

---

## 十一、许可证

本项目采用 [MIT License](LICENSE) 开源。
