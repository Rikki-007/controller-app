/*
 * hid_output.h - turns a vc_input_state_t into virtual gamepad input.
 *
 * Two parts:
 *
 *   1. Translation (portable, no OS calls): converts the unified state into
 *      the report formats the system drivers expect - an Xbox 360 report and
 *      a DualShock 4 report for ViGEmBus, or an evdev event list for uinput.
 *
 *   2. Device wrapper: creates a virtual gamepad and submits states to it
 *      through one backend chosen at compile time:
 *
 *        (no flag)                 stub   - translates and counts, creates no
 *                                           device; builds everywhere
 *        -DVC_HID_BACKEND_VIGEM    ViGEm  - Windows, needs ViGEmBus + ViGEmClient
 *        -DVC_HID_BACKEND_UINPUT   uinput - Linux, needs /dev/uinput
 *
 * Build instructions for each backend are in docs/BUILD.md.
 *
 * Which device a controller type becomes:
 *
 *              stub / ViGEm      uinput (USB vendor:product)
 *   Xbox       Xbox 360          evdev pad 045e:028e
 *   PS4        DualShock 4       evdev pad 054c:09cc
 *   PS5        DualShock 4       evdev pad 054c:0ce6
 *   Switch     DualShock 4       evdev pad 057e:2009
 *
 * ViGEmBus can only present Xbox 360 and DualShock 4 pads, so PS5 and Switch
 * appear as a DualShock 4 on Windows.
 *
 * Not thread-safe: call everything from one thread.
 */
#ifndef VC_HID_OUTPUT_H
#define VC_HID_OUTPUT_H

#include <stdint.h>

#include "controller.h"

/* ------------------------------------------------------------------------ */
/* Translation                                                              */
/* ------------------------------------------------------------------------ */

typedef enum {
    VC_HID_TARGET_NONE = 0,
    VC_HID_TARGET_X360,    /* Xbox 360 pad (XInput) */
    VC_HID_TARGET_DS4,     /* DualShock 4 */
    VC_HID_TARGET_EVDEV    /* Linux evdev gamepad */
} vc_hid_target_t;

/* --- Xbox 360: same fields and bit values as ViGEm's XUSB_REPORT. --- */

#define VC_XUSB_DPAD_UP         0x0001u
#define VC_XUSB_DPAD_DOWN       0x0002u
#define VC_XUSB_DPAD_LEFT       0x0004u
#define VC_XUSB_DPAD_RIGHT      0x0008u
#define VC_XUSB_START           0x0010u
#define VC_XUSB_BACK            0x0020u
#define VC_XUSB_LEFT_THUMB      0x0040u
#define VC_XUSB_RIGHT_THUMB     0x0080u
#define VC_XUSB_LEFT_SHOULDER   0x0100u
#define VC_XUSB_RIGHT_SHOULDER  0x0200u
#define VC_XUSB_GUIDE           0x0400u
#define VC_XUSB_A               0x1000u
#define VC_XUSB_B               0x2000u
#define VC_XUSB_X               0x4000u
#define VC_XUSB_Y               0x8000u

typedef struct {
    uint16_t buttons;         /* VC_XUSB_* */
    uint8_t  left_trigger;    /* 0..255 */
    uint8_t  right_trigger;
    int16_t  thumb_lx;        /* +x right, +y up, as in vc_stick_t */
    int16_t  thumb_ly;
    int16_t  thumb_rx;
    int16_t  thumb_ry;
} vc_hid_xusb_report_t;

/* VC_BTN_MISC and VC_BTN_TOUCHPAD have no Xbox 360 equivalent and are dropped. */
void vc_hid_to_xusb(const vc_input_state_t *state, vc_hid_xusb_report_t *out);

/* --- DualShock 4: basic fields match ViGEm's DS4_REPORT. --- */

/* Low nibble of `buttons` is the d-pad as a hat value. */
#define VC_DS4_HAT_NORTH        0x0u
#define VC_DS4_HAT_NORTHEAST    0x1u
#define VC_DS4_HAT_EAST         0x2u
#define VC_DS4_HAT_SOUTHEAST    0x3u
#define VC_DS4_HAT_SOUTH        0x4u
#define VC_DS4_HAT_SOUTHWEST    0x5u
#define VC_DS4_HAT_WEST         0x6u
#define VC_DS4_HAT_NORTHWEST    0x7u
#define VC_DS4_HAT_NONE         0x8u
#define VC_DS4_HAT_MASK         0xFu

#define VC_DS4_SQUARE           0x0010u
#define VC_DS4_CROSS            0x0020u
#define VC_DS4_CIRCLE           0x0040u
#define VC_DS4_TRIANGLE         0x0080u
#define VC_DS4_SHOULDER_LEFT    0x0100u
#define VC_DS4_SHOULDER_RIGHT   0x0200u
#define VC_DS4_TRIGGER_LEFT     0x0400u   /* L2 as a button */
#define VC_DS4_TRIGGER_RIGHT    0x0800u
#define VC_DS4_SHARE            0x1000u
#define VC_DS4_OPTIONS          0x2000u
#define VC_DS4_THUMB_LEFT       0x4000u
#define VC_DS4_THUMB_RIGHT      0x8000u

#define VC_DS4_SPECIAL_PS       0x01u
#define VC_DS4_SPECIAL_TOUCHPAD 0x02u

#define VC_DS4_TOUCH_WIDTH      1920
#define VC_DS4_TOUCH_HEIGHT     943

typedef struct {
    uint8_t  active;
    uint8_t  id;
    uint16_t x;   /* 0..VC_DS4_TOUCH_WIDTH - 1 */
    uint16_t y;   /* 0..VC_DS4_TOUCH_HEIGHT - 1 */
} vc_hid_ds4_touch_t;

typedef struct {
    /* Basic report, as in DS4_REPORT. Sticks are 0..255 with 128 centred and
     * y growing downwards. */
    uint8_t  thumb_lx;
    uint8_t  thumb_ly;
    uint8_t  thumb_rx;
    uint8_t  thumb_ry;
    uint16_t buttons;     /* hat nibble | VC_DS4_* */
    uint8_t  special;     /* VC_DS4_SPECIAL_* */
    uint8_t  trigger_l;
    uint8_t  trigger_r;
    /* Extended data, for DS4_REPORT_EX. Zero unless the state carries it. */
    uint8_t  has_motion;
    uint8_t  has_touch;
    int16_t  gyro[3];     /* 2000 deg/s full scale */
    int16_t  accel[3];    /* 8192 per g */
    vc_hid_ds4_touch_t touch[VC_TOUCH_POINTS];
} vc_hid_ds4_report_t;

/* VC_BTN_MISC has no DualShock 4 equivalent and is dropped. */
void vc_hid_to_ds4(const vc_input_state_t *state, vc_hid_ds4_report_t *out);

/* --- Linux evdev: one event per control, ending with a sync event. --- */

/* Same numeric values as <linux/input-event-codes.h>. */
#define VC_EV_SYN          0x00
#define VC_EV_KEY          0x01
#define VC_EV_ABS          0x03
#define VC_SYN_REPORT      0x00
#define VC_KEY_RECORD      0xA7
#define VC_BTN_CODE_SOUTH  0x130
#define VC_BTN_CODE_EAST   0x131
#define VC_BTN_CODE_NORTH  0x133
#define VC_BTN_CODE_WEST   0x134
#define VC_BTN_CODE_TL     0x136
#define VC_BTN_CODE_TR     0x137
#define VC_BTN_CODE_TL2    0x138
#define VC_BTN_CODE_TR2    0x139
#define VC_BTN_CODE_SELECT 0x13A
#define VC_BTN_CODE_START  0x13B
#define VC_BTN_CODE_MODE   0x13C
#define VC_BTN_CODE_THUMBL 0x13D
#define VC_BTN_CODE_THUMBR 0x13E
#define VC_ABS_X           0x00   /* left stick */
#define VC_ABS_Y           0x01
#define VC_ABS_Z           0x02   /* left trigger */
#define VC_ABS_RX          0x03   /* right stick */
#define VC_ABS_RY          0x04
#define VC_ABS_RZ          0x05   /* right trigger */
#define VC_ABS_HAT0X       0x10   /* d-pad: -1 left, +1 right */
#define VC_ABS_HAT0Y       0x11   /* d-pad: -1 up, +1 down */

typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} vc_hid_evdev_event_t;

#define VC_HID_EVDEV_MAX_EVENTS 24

/* Writes the full state as events into out[VC_HID_EVDEV_MAX_EVENTS] and
 * returns how many were written. Sticks are -32768..32767 with y growing
 * downwards; triggers 0..255. Motion, touch and VC_BTN_TOUCHPAD are not
 * included: on Linux those belong to separate sensor and touchpad devices,
 * which this version does not create. */
int vc_hid_to_evdev(const vc_input_state_t *state, vc_hid_evdev_event_t *out);

/* ------------------------------------------------------------------------ */
/* Device wrapper                                                           */
/* ------------------------------------------------------------------------ */

typedef struct vc_hid_device vc_hid_device_t;

typedef struct {
    vc_controller_type_t type;      /* what the client asked for */
    vc_hid_target_t      target;    /* what it is emulated as */
    uint64_t             submits;   /* states sent successfully */
} vc_hid_device_info_t;

/* "stub", "vigem" or "uinput". */
const char *vc_hid_backend_name(void);

/* Device a controller type becomes under the compiled-in backend;
 * VC_HID_TARGET_NONE for an invalid type. */
vc_hid_target_t vc_hid_target_for(vc_controller_type_t type);

/* Connects to the system driver. Call once before creating devices.
 * Returns 0, or -1 after logging the reason to stderr. */
int vc_hid_init(void);

/* Disconnects from the system driver. Destroy all devices first. */
void vc_hid_shutdown(void);

/* Plugs in a virtual gamepad. Returns 0 and sets *out, or -1 after logging. */
int vc_hid_create(vc_controller_type_t type, vc_hid_device_t **out);

/* Sends one state to the virtual gamepad. Returns 0 or -1. */
int vc_hid_submit(vc_hid_device_t *device, const vc_input_state_t *state);

void vc_hid_get_info(const vc_hid_device_t *device, vc_hid_device_info_t *out);

/* Unplugs the gamepad and frees it. NULL is ignored. */
void vc_hid_destroy(vc_hid_device_t *device);

#endif /* VC_HID_OUTPUT_H */
