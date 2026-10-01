cmake_minimum_required(VERSION 3.13)

# =============================================================================
# RP2350-USB-A — USB 鼠标 -> FC 裕兴 V9.2 协议桥接
#
# 引脚：GP2 = CLK（输入，裕兴 -> Pico）
#       GP3 = STROBE（输入，裕兴 -> Pico）
#       GP4 = DATA（输出，开漏，内部上拉兜底，未接外部电阻）
#       GP12 = USB-A 口 D+（PIO-USB Host），D- = GP13
#
# 协议：27 位帧 = 3 个 9 位包（裕兴主机轮询驱动，帧频约 50Hz）
# 数据源：USB HID 鼠标报告（累积后按帧下发，超 ±127 等比缩放）
#
# 目录结构（本文件所在目录即工程根）：
#   ./CMakeLists.txt
#   ./RP2350_USB_Mouse_Test.c     主程序（双核：USB 栈 + 裕兴从机）
#   ./tusb_config.h               TinyUSB 配置
#   ./usb_descriptors.c           CDC 设备描述符
#   ./pico-sdk/                   Pico SDK 2.1.0（含 tinyusb 等子模块）
#   ./pico-pio-usb/               Pico-PIO-USB（PIO 实现的 USB 主机）
# =============================================================================

# 板卡：微雪 RP2350-USB-A 对应 SDK 的 pico2 板级定义
set(PICO_BOARD pico2 CACHE STRING "Pico board")

# -----------------------------------------------------------------------------
# 本机环境适配（按需修改）
# 1) pioasm / picotool 使用官方预编译二进制：未安装宿主机 C++ 编译器时，
#    SDK 会尝试现场源码构建这两个工具并失败
# 2) 工具链与构建目录都必须落在纯 ASCII 路径下：
#    路径一旦含中文，arm-gcc 的 ld 会直接崩溃（ld returned 116）
# 因此这里默认引用 ASCII 目录 D:\pico_tc，构建输出建议放在 D:\pico_build
# -----------------------------------------------------------------------------
set(PICO_TOOLCHAIN_PATH "D:/pico_tc/xpack-arm-none-eabi-gcc-13.3.1-1.1" CACHE PATH "ARM 交叉工具链路径（按需修改，或用 -DPICO_TOOLCHAIN_PATH=... 覆盖）")
set(pioasm_DIR "D:/pico_tc/tools210/pioasm" CACHE PATH "pioasm 预编译包目录（按需修改）")
set(picotool_DIR "D:/pico_tc/tools210/picotool" CACHE PATH "picotool 预编译包目录（按需修改）")

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk" CACHE PATH "Pico SDK path")
set(PICO_PIO_USB_DIR "${CMAKE_CURRENT_LIST_DIR}/pico-pio-usb")

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(RP2350_USB_Mouse_Test C CXX ASM)

pico_sdk_init()

# 引入 Pico-PIO-USB，定义 pico_pio_usb 目标
include(${PICO_PIO_USB_DIR}/CMakeLists.txt)

set(TARGET RP2350_USB_Mouse_Test)

add_executable(${TARGET}
    "${CMAKE_CURRENT_LIST_DIR}/RP2350_USB_Mouse_Test.c"
    "${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c"
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# 指示 tinyusb 使用 PIO-USB 的主机实现
target_compile_definitions(${TARGET} PRIVATE PIO_USB_USE_TINYUSB)

# tinyusb 需在本目录找到 tusb_config.h
target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(${TARGET} PRIVATE
    pico_stdlib
    pico_multicore
    pico_util
    pico_pio_usb
    tinyusb_device
    tinyusb_host
)

# 调试输出：
#   port0 原生 USB -> Device 栈 + usb_descriptors.c 提供 CDC 虚拟串口（Type-C）
#   port1 PIO-USB  -> Host 栈读取鼠标（USB-A 口）
# stdio_usb 保持关闭：它内部注册的 CDC 会与宿主栈争用 TinyUSB 初始化
pico_enable_stdio_uart(${TARGET} 1)
pico_enable_stdio_usb(${TARGET} 0)

# 生成 .uf2 / .elf / .bin / .hex 等
pico_add_extra_outputs(${TARGET})
