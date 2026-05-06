#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

#define NEOPIXEL_PIN 16
#define NEOPIXEL_PIO pio0
#define NEOPIXEL_SM  0

#define NEOPIXEL_BRIGHT 127
#define RAINBOW_DIVISOR 4

#define FLASH_TARGET_OFFSET (1 * 1024 * 1024)

// === 恢复原速 25ms，靠持续发送来保证可靠性 ===
#define TAP_HOLD_MS    25
#define TAP_RELEASE_MS 25
// =============================================

typedef uint16_t gamepad_button_t;

#define BTN_Y       ((gamepad_button_t)0x0001)
#define BTN_B       ((gamepad_button_t)0x0002)
#define BTN_A       ((gamepad_button_t)0x0004)
#define BTN_X       ((gamepad_button_t)0x0008)
#define BTN_L       ((gamepad_button_t)0x0010)
#define BTN_R       ((gamepad_button_t)0x0020)
#define BTN_ZL      ((gamepad_button_t)0x0040)
#define BTN_ZR      ((gamepad_button_t)0x0080)
#define BTN_MINUS   ((gamepad_button_t)0x0101)
#define BTN_PLUS    ((gamepad_button_t)0x0102)
#define BTN_LCLICK  ((gamepad_button_t)0x0104)
#define BTN_RCLICK  ((gamepad_button_t)0x0108)
#define BTN_HOME    ((gamepad_button_t)0x0110)
#define BTN_CAPTURE ((gamepad_button_t)0x0120)

#define DPAD_UP        0
#define DPAD_UPRIGHT   1
#define DPAD_RIGHT     2
#define DPAD_DOWNRIGHT 3
#define DPAD_DOWN      4
#define DPAD_DOWNLEFT  5
#define DPAD_LEFT      6
#define DPAD_UPLEFT    7
#define DPAD_NEUTRAL   8

#define STICK_LX     3
#define STICK_LY     4
#define STICK_RX     5
#define STICK_RY     6
#define STICK_CENTER 128

static uint8_t current_report[8] = {0x00, 0x00, 0x08, 128, 128, 128, 128, 0x00};

const uint8_t *flash_contents = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

const gamepad_button_t button_map[] = {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R,
    BTN_ZL, BTN_ZR, BTN_MINUS, BTN_PLUS, BTN_LCLICK,
    BTN_RCLICK, BTN_HOME, BTN_CAPTURE
};

const uint8_t stick_axis_map[] = {
    STICK_LX, STICK_LY, STICK_RX, STICK_RY
};

#define TDLD_VERSION 0x03

#define OPCODE_INVALID          0x0
#define OPCODE_PRESS_BUTTON     0x1
#define OPCODE_RELEASE_BUTTON   0x2
#define OPCODE_PRESS_DPAD       0x3
#define OPCODE_RELEASE_DPAD     0x4
#define OPCODE_RELEASE_ALL      0x5
#define OPCODE_DELAY            0x6
#define OPCODE_SET_STICK        0x7
#define OPCODE_TAP_BUTTON       0x8
#define OPCODE_TAP_DPAD         0x9

#define OPCODE_REPEAT_LAST_1    0xE
#define OPCODE_REPEAT_LAST_2    0xF

static inline void hid_press(gamepad_button_t btn) {
    current_report[btn >> 8] |= (btn & 0xFF);
}

static inline void hid_release(gamepad_button_t btn) {
    current_report[btn >> 8] &= ~(btn & 0xFF);
}

static inline void hid_release_all(void) {
    current_report[0] = 0x00;
    current_report[1] = 0x00;
    current_report[2] = DPAD_NEUTRAL;
    current_report[3] = STICK_CENTER;
    current_report[4] = STICK_CENTER;
    current_report[5] = STICK_CENTER;
    current_report[6] = STICK_CENTER;
    current_report[7] = 0x00;
}

static inline void hid_set_dpad(uint8_t direction) {
    current_report[2] = direction;
}

static inline void hid_set_stick(uint8_t axis, uint8_t value) {
    current_report[axis] = value;
}

static void neopixel_init(void) {
    uint offset = pio_add_program(NEOPIXEL_PIO, &ws2812_program);
    ws2812_program_init(NEOPIXEL_PIO, NEOPIXEL_SM, offset, NEOPIXEL_PIN, 800000, false);
}

static void neopixel_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    pio_sm_put_blocking(NEOPIXEL_PIO, NEOPIXEL_SM, grb);
}

static void boringpixel_init(void) {
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
}

static void boringpixel_set(bool on)
{
    gpio_put(25, on);
}

// ========== 核心修改：等待期间持续重发当前状态 ==========
// 原版只发1次就沉默，Switch漏掉就永远丢了
// 现在每次Switch来轮询都能拿到当前状态，不会丢
static void delay_ms_usb(uint32_t ms) {
    absolute_time_t end = make_timeout_time_ms(ms);
    while (!time_reached(end)) {
        tud_task();
        if (tud_hid_ready()) {
            tud_hid_report(0, current_report, sizeof(current_report));
        }
    }
}
// ======================================================

static void send_report_raw(void) {
    while (!tud_hid_ready()) {
        tud_task();
    }
    tud_hid_report(0, current_report, sizeof(current_report));
}

static void push_report(void) {
    send_report_raw();
    if (current_report[0] != 0 || current_report[1] != 0) {
        neopixel_set_rgb(0, NEOPIXEL_BRIGHT, 0);
        boringpixel_set(true);
    } else {
        neopixel_set_rgb(10, 10, 10);
        boringpixel_set(false);
    }
}

static void error_flash(int interval_ms) {
    while (true) {
        neopixel_set_rgb(NEOPIXEL_BRIGHT, 0, 0);
        boringpixel_set(true);
        delay_ms_usb(interval_ms);
        neopixel_set_rgb(0, 0, 0);
        boringpixel_set(false);
        delay_ms_usb(interval_ms);
    }
}

void get_good_rainbow(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (hue < 85) {
        *r = 255 - hue * 3;
        *g = hue * 3;
        *b = 0;
    } else if (hue < 170) {
        hue -= 85;
        *r = 0;
        *g = 255 - hue * 3;
        *b = hue * 3;
    } else {
        hue -= 170;
        *r = hue * 3;
        *g = 0;
        *b = 255 - hue * 3;
    }
}

static void done_rainbow(void) {
    uint8_t hue = 0;
    uint8_t r, g, b;
    boringpixel_set(true);
    while (true) {
        get_good_rainbow(hue, &r, &g, &b);
        neopixel_set_rgb(r/RAINBOW_DIVISOR,g/RAINBOW_DIVISOR,b/RAINBOW_DIVISOR);
        sleep_ms(10);
        hue++;
    }
}

static void run_single_byte_opcode(uint8_t record) {
    uint8_t opcode = record >> 4;
    uint8_t nibble = record & 0xF;
    switch (opcode) {
        case OPCODE_PRESS_BUTTON:
            hid_press(button_map[nibble]);
            push_report();
            break;
        case OPCODE_RELEASE_BUTTON:
            hid_release(button_map[nibble]);
            push_report();
            break;
        case OPCODE_PRESS_DPAD:
            hid_set_dpad(nibble);
            push_report();
            break;
        case OPCODE_RELEASE_DPAD:
            hid_set_dpad(DPAD_NEUTRAL);
            push_report();
            break;
        case OPCODE_RELEASE_ALL:
            hid_release_all();
            push_report();
            break;
        case OPCODE_TAP_BUTTON:
            hid_press(button_map[nibble]);
            push_report();
            delay_ms_usb(TAP_HOLD_MS);
            hid_release(button_map[nibble]);
            push_report();
            delay_ms_usb(TAP_RELEASE_MS);
            break;
        case OPCODE_TAP_DPAD:
            hid_set_dpad(nibble);
            push_report();
            delay_ms_usb(TAP_HOLD_MS);
            hid_set_dpad(DPAD_NEUTRAL);
            push_report();
            delay_ms_usb(TAP_RELEASE_MS);
            break;
        default:
            error_flash(5000);
            break;
    }
}

int main(void) {
    board_init();
    tusb_init();
    neopixel_init();
    boringpixel_init();

    while (!tud_mounted()) {
        tud_task();
    }

    for (int i = 0; i < 3; i++) {
        neopixel_set_rgb(NEOPIXEL_BRIGHT, NEOPIXEL_BRIGHT, 0);
        send_report_raw();
        delay_ms_usb(500);
        neopixel_set_rgb(0, 0, 0);
        send_report_raw();
        delay_ms_usb(500);
    }

    const uint8_t *ptr = flash_contents;

    if (ptr[0] != 'T' || ptr[1] != 'D' || ptr[2] != 'L' || ptr[3] != 'D') {
        error_flash(250);
        return 0;
    }
    if (ptr[4] != TDLD_VERSION) {
        error_flash(1000);
        return 0;
    }
    ptr += 6;

    uint8_t last_1byte_record = 0;
    bool working = true;
    while (working) {
        tud_task();
        uint8_t record = *ptr++;
        uint8_t opcode = record >> 4;
        uint8_t nibble  = record & 0x0F;

        switch (opcode) {
            case OPCODE_INVALID:
                working = false;
                break;
            case OPCODE_DELAY: {
                uint8_t data     = *ptr++;
                uint16_t delayMs = (nibble << 8) | data;
                delay_ms_usb(delayMs);
                break;
            }
            case OPCODE_SET_STICK: {
                uint8_t axis_value = *ptr++;
                hid_set_stick(stick_axis_map[nibble], axis_value);
                push_report();
                break;
            }
            case OPCODE_REPEAT_LAST_1: {
                uint8_t count = nibble;
                for (int i = 0; i < count; i++) {
                    run_single_byte_opcode(last_1byte_record);
                }
                break;
            }
            case OPCODE_REPEAT_LAST_2: {
                uint8_t data      = *ptr++;
                uint16_t count    = (nibble << 8) | data;
                for (int i = 0; i < count; i++) {
                    run_single_byte_opcode(last_1byte_record);
                }
                break;
            }
            default:
                run_single_byte_opcode(record);
                last_1byte_record = record;
                break;
        }
    }

    done_rainbow();
    return 0;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t len) { return 0; }
void     tud_hid_set_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t const *buf, uint16_t len) {}
