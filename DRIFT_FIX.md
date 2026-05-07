# Pixel Drift Fix for USB HID Drawing on Switch

[English](#english) | [中文](#中文)

---

## English

### The Problem

When using TomodachiDrawer (or similar RP2040-based drawing scripts) over USB to simulate a Pro Controller on Switch, the cursor would start drawing perfectly aligned, but partway through the drawing it would shift by a few pixels. Every pixel after that point inherited the offset, ruining the image.

The same image drawn over Bluetooth controller emulation works perfectly — but Bluetooth is too slow for practical use.

### What Did NOT Work

These were tried and ruled out:

- Changing USB cables (tried 2 different ones)
- Changing USB ports (front and back of dock)
- Removing all paired controllers
- Original power adapter
- No screen recording active
- Maxing out the TSP solver time limit (30s)
- **Increasing the inter-input delay** (tried 30ms / 35ms / 40ms / 45ms / 50ms / 60ms / 80ms — all still drifted, and 80ms was slower than Bluetooth)
- **Continuously resending the current state during waits** (no improvement)

### The Actual Root Cause

The original firmware uses **time-based delays (milliseconds)** to control button hold length. With Switch's 8ms USB polling interval, the number of times Switch actually sees a button press is non-deterministic:

- 25ms hold → Switch polls ~3 times, but jitter means sometimes 2, sometimes 4
- An extra poll = an extra D-pad input registered = 1 pixel of drift
- These errors accumulate over thousands of inputs

Bluetooth doesn't have this problem because the BT HID protocol synchronizes state continuously regardless of timing jitter.

### The Fix

Two changes:

**1. Reduce USB polling interval from 8ms to 1ms**

In `usb_descriptors.c`, change the HID descriptor's `bInterval` from 8 to 1:

```c
TUD_HID_DESCRIPTOR(0, 1, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, 64, 1)
//                                                                                       ^ was 8
```

**2. Replace time-based delays with poll-count-based delays**

Instead of "wait N milliseconds", do "send N reports, waiting for each to be acknowledged before sending the next". This makes the number of times Switch sees the input deterministic.

In `TomodachiDrawer.Firmware.c`:

```c
#define TAP_HOLD_POLLS    22
#define TAP_RELEASE_POLLS 3

static void wait_reports(uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        while (!tud_hid_ready()) {
            tud_task();
        }
        tud_hid_report(0, current_report, sizeof(current_report));
    }
}
```

And replace the `delay_ms_usb(25)` calls inside `OPCODE_TAP_BUTTON` and `OPCODE_TAP_DPAD` with `wait_reports(TAP_HOLD_POLLS)` and `wait_reports(TAP_RELEASE_POLLS)`.

### Tuning Process

The two values were found empirically by binary search on a real Switch:

| HOLD value | Result |
|---|---|
| 8 | Drifted after ~1 minute |
| 12 | Drifted |
| 15 | Drifted |
| 18 | Drifted |
| 19 | Drifted |
| 20 | OK on full image (10 min) |
| 22 | OK with safety margin |

`RELEASE` was kept at 3 throughout — the release phase only needs to be long enough to register as a release, no game-logic interpretation needed.

### Final Settings

```c
#define TAP_HOLD_POLLS    22  // ~22ms with 1ms polling
#define TAP_RELEASE_POLLS 3   // ~3ms
```

This gives roughly **2x the speed of the original 25ms time-based version** while being completely stable.

### Notes

- These values are tuned for one specific Switch + Pico setup. Your mileage may vary; if drift returns, increase `TAP_HOLD_POLLS` by 2 and rebuild.
- If you want maximum speed and don't mind risk, try lowering `TAP_HOLD_POLLS` to 20.
- If you want extra safety, use 25.

---

## 中文

### 问题描述

用 TomodachiDrawer（或类似的基于 RP2040 的画图脚本）通过 USB 在 Switch 上模拟 Pro 手柄画图时，画笔一开始位置完全正确，但画到一半会突然偏移几个像素。从偏移那一刻开始，后面所有像素全都跟着错位，整张图毁了。

同一张图用蓝牙模拟手柄完全没问题，但蓝牙速度太慢，没法实用。

### 试过但**没用**的方法

以下方法都排除了：

- 换 USB 线（试了 2 根）
- 换 USB 口（底座前后都试过）
- 删除所有已配对的手柄
- 使用原装电源适配器
- 关闭录像功能
- TSP 路径优化时间拉满（30 秒）
- **增加按键间延迟**（试过 30 / 35 / 40 / 45 / 50 / 60 / 80 毫秒，全都偏，而且 80ms 比蓝牙还慢）
- **在等待期间持续重发当前状态**（没用）

### 真正的原因

原版固件用**毫秒数（时间）**来控制按键按下时长。但 Switch 的 USB 轮询间隔是 8ms，这导致 Switch 实际"看到"按键的次数是不确定的：

- 按下 25ms → Switch 轮询大约 3 次，但因为时钟抖动，有时是 2 次，有时是 4 次
- 多被读到 1 次 = 多走了 1 格方向 = 偏移 1 像素
- 几千次操作累积下来必然出错

蓝牙不会有这个问题，因为蓝牙 HID 协议会持续同步状态，不受轮询抖动影响。

### 解决方案

改两个地方：

**1. 把 USB 轮询间隔从 8ms 降到 1ms**

在 `usb_descriptors.c` 中，把 HID 描述符的 `bInterval` 从 8 改成 1：

```c
TUD_HID_DESCRIPTOR(0, 1, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, 64, 1)
//                                                                                       ^ 原来是 8
```

**2. 把基于时间的等待改成基于"轮询次数"的等待**

不再"等 N 毫秒"，而是"发送 N 次报告，每次都等 Switch 确认收到后再发下一次"。这样 Switch 看到该输入的次数就是确定的，不受时钟抖动影响。

在 `TomodachiDrawer.Firmware.c` 中：

```c
#define TAP_HOLD_POLLS    22
#define TAP_RELEASE_POLLS 3

static void wait_reports(uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        while (!tud_hid_ready()) {
            tud_task();
        }
        tud_hid_report(0, current_report, sizeof(current_report));
    }
}
```

然后把 `OPCODE_TAP_BUTTON` 和 `OPCODE_TAP_DPAD` 里原来的 `delay_ms_usb(25)` 改成 `wait_reports(TAP_HOLD_POLLS)` 和 `wait_reports(TAP_RELEASE_POLLS)`。

### 参数调试过程

这两个值是在真机上二分法测出来的：

| HOLD 值 | 结果 |
|---|---|
| 8 | 约 1 分钟后开始偏 |
| 12 | 偏 |
| 15 | 偏 |
| 18 | 偏 |
| 19 | 偏 |
| 20 | 完整画完 10 分钟无偏移 |
| 22 | 留余量后稳定使用 |

`RELEASE` 一直保持 3 不动 —— 松开阶段只需要让 Switch 识别"按键释放"即可，不需要游戏逻辑处理时间。

### 最终参数

```c
#define TAP_HOLD_POLLS    22  // 1ms 轮询下约等于 22ms
#define TAP_RELEASE_POLLS 3   // 约 3ms
```

实测速度**大约是原版（25ms 时间制）的 2 倍**，且完全稳定。

### 注意事项

- 这套参数是在我自己的 Switch + Pico 上调出来的。你的设备可能略有不同，如果还是偏，就把 `TAP_HOLD_POLLS` 加 2 重新编译
- 想追求极速可以试着把 `TAP_HOLD_POLLS` 降到 20
- 想更保险就用 25
```

▸ 翻到页面底部，点绿色 **Commit changes...** → **Commit changes**
