/*
 * hid_output.c - turns a vc_input_state_t into virtual gamepad input.
 * See hid_output.h for the contract and docs/BUILD.md for the backends.
 */

/* Backend headers. Feature macros must come before any system header. */
#if defined(VC_HID_BACKEND_VIGEM) && defined(VC_HID_BACKEND_UINPUT)
#  error "define at most one of VC_HID_BACKEND_VIGEM and VC_HID_BACKEND_UINPUT"
#endif

#if defined(VC_HID_BACKEND_VIGEM)
#  include <windows.h>
#  include <ViGEm/Client.h>
#elif defined(VC_HID_BACKEND_UINPUT)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200112L
#  endif
#  include <errno.h>
#  include <fcntl.h>
#  include <linux/uinput.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_output.h"

struct vc_hid_device {
    vc_controller_type_t type;
    vc_hid_target_t      target;
    uint64_t             submits;
#if defined(VC_HID_BACKEND_VIGEM)
    PVIGEM_TARGET        pad;
#elif defined(VC_HID_BACKEND_UINPUT)
    int                  fd;
#endif
};

#ifdef __GNUC__
static void hid_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

static void hid_log(const char *fmt, ...)
{
    va_list args;

    fputs("[hid] error: ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------------ */
/* Translation                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t from;   /* VC_BTN_* */
    uint16_t to;
} button_map_t;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const button_map_t xusb_buttons[] = {
    { VC_BTN_SOUTH,      VC_XUSB_A },
    { VC_BTN_EAST,       VC_XUSB_B },
    { VC_BTN_WEST,       VC_XUSB_X },
    { VC_BTN_NORTH,      VC_XUSB_Y },
    { VC_BTN_L1,         VC_XUSB_LEFT_SHOULDER },
    { VC_BTN_R1,         VC_XUSB_RIGHT_SHOULDER },
    { VC_BTN_L3,         VC_XUSB_LEFT_THUMB },
    { VC_BTN_R3,         VC_XUSB_RIGHT_THUMB },
    { VC_BTN_START,      VC_XUSB_START },
    { VC_BTN_SELECT,     VC_XUSB_BACK },
    { VC_BTN_HOME,       VC_XUSB_GUIDE },
    { VC_BTN_DPAD_UP,    VC_XUSB_DPAD_UP },
    { VC_BTN_DPAD_DOWN,  VC_XUSB_DPAD_DOWN },
    { VC_BTN_DPAD_LEFT,  VC_XUSB_DPAD_LEFT },
    { VC_BTN_DPAD_RIGHT, VC_XUSB_DPAD_RIGHT }
};

static const button_map_t ds4_buttons[] = {
    { VC_BTN_SOUTH,  VC_DS4_CROSS },
    { VC_BTN_EAST,   VC_DS4_CIRCLE },
    { VC_BTN_WEST,   VC_DS4_SQUARE },
    { VC_BTN_NORTH,  VC_DS4_TRIANGLE },
    { VC_BTN_L1,     VC_DS4_SHOULDER_LEFT },
    { VC_BTN_R1,     VC_DS4_SHOULDER_RIGHT },
    { VC_BTN_L3,     VC_DS4_THUMB_LEFT },
    { VC_BTN_R3,     VC_DS4_THUMB_RIGHT },
    { VC_BTN_START,  VC_DS4_OPTIONS },
    { VC_BTN_SELECT, VC_DS4_SHARE }
};

static const button_map_t evdev_buttons[] = {
    { VC_BTN_SOUTH,  VC_BTN_CODE_SOUTH },
    { VC_BTN_EAST,   VC_BTN_CODE_EAST },
    { VC_BTN_WEST,   VC_BTN_CODE_WEST },
    { VC_BTN_NORTH,  VC_BTN_CODE_NORTH },
    { VC_BTN_L1,     VC_BTN_CODE_TL },
    { VC_BTN_R1,     VC_BTN_CODE_TR },
    { VC_BTN_L3,     VC_BTN_CODE_THUMBL },
    { VC_BTN_R3,     VC_BTN_CODE_THUMBR },
    { VC_BTN_START,  VC_BTN_CODE_START },
    { VC_BTN_SELECT, VC_BTN_CODE_SELECT },
    { VC_BTN_HOME,   VC_BTN_CODE_MODE },
    { VC_BTN_MISC,   VC_KEY_RECORD }
};

static uint16_t map_buttons(uint32_t buttons, const button_map_t *map, size_t count)
{
    uint16_t out = 0;
    size_t   i;

    for (i = 0; i < count; i++) {
        if (buttons & map[i].from) {
            out |= map[i].to;
        }
    }
    return out;
}

/* D-pad as -1/0/+1 per axis; opposite directions held together cancel out. */
static int dpad_x(uint32_t buttons)
{
    return ((buttons & VC_BTN_DPAD_RIGHT) != 0) - ((buttons & VC_BTN_DPAD_LEFT) != 0);
}

static int dpad_y_up(uint32_t buttons)
{
    return ((buttons & VC_BTN_DPAD_UP) != 0) - ((buttons & VC_BTN_DPAD_DOWN) != 0);
}

/* Our sticks have +y up; DS4 and evdev have +y down. -32768 has no positive
 * counterpart, so it becomes 32767. */
static int16_t flip_axis(int16_t value)
{
    return value == INT16_MIN ? INT16_MAX : (int16_t)-value;
}

/* -32768..32767 to 0..255, with 0 landing on 128. */
static uint8_t axis_to_u8(int16_t value)
{
    return (uint8_t)(((int32_t)value + 32768) >> 8);
}

static int16_t saturate_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

void vc_hid_to_xusb(const vc_input_state_t *state, vc_hid_xusb_report_t *out)
{
    memset(out, 0, sizeof *out);
    out->buttons = map_buttons(state->buttons, xusb_buttons, ARRAY_LEN(xusb_buttons));
    out->left_trigger = state->trigger_left;
    out->right_trigger = state->trigger_right;
    out->thumb_lx = state->left.x;
    out->thumb_ly = state->left.y;
    out->thumb_rx = state->right.x;
    out->thumb_ry = state->right.y;
}

void vc_hid_to_ds4(const vc_input_state_t *state, vc_hid_ds4_report_t *out)
{
    /* Indexed [up/down + 1][left/right + 1]. */
    static const uint8_t hats[3][3] = {
        { VC_DS4_HAT_SOUTHWEST, VC_DS4_HAT_SOUTH, VC_DS4_HAT_SOUTHEAST },
        { VC_DS4_HAT_WEST,      VC_DS4_HAT_NONE,  VC_DS4_HAT_EAST },
        { VC_DS4_HAT_NORTHWEST, VC_DS4_HAT_NORTH, VC_DS4_HAT_NORTHEAST }
    };
    int i;

    memset(out, 0, sizeof *out);

    out->thumb_lx = axis_to_u8(state->left.x);
    out->thumb_ly = axis_to_u8(flip_axis(state->left.y));
    out->thumb_rx = axis_to_u8(state->right.x);
    out->thumb_ry = axis_to_u8(flip_axis(state->right.y));

    out->buttons = map_buttons(state->buttons, ds4_buttons, ARRAY_LEN(ds4_buttons));
    out->buttons |= hats[dpad_y_up(state->buttons) + 1][dpad_x(state->buttons) + 1];
    if (state->trigger_left > 0) {
        out->buttons |= VC_DS4_TRIGGER_LEFT;
    }
    if (state->trigger_right > 0) {
        out->buttons |= VC_DS4_TRIGGER_RIGHT;
    }
    out->trigger_l = state->trigger_left;
    out->trigger_r = state->trigger_right;

    if (state->buttons & VC_BTN_HOME) {
        out->special |= VC_DS4_SPECIAL_PS;
    }
    if (state->buttons & VC_BTN_TOUCHPAD) {
        out->special |= VC_DS4_SPECIAL_TOUCHPAD;
    }

    if (state->flags & VC_INPUT_HAS_MOTION) {
        out->has_motion = 1;
        for (i = 0; i < 3; i++) {
            /* Gyro scale already matches; accel goes from 4096 to 8192 per g. */
            out->gyro[i] = state->motion.gyro[i];
            out->accel[i] = saturate_i16((int32_t)state->motion.accel[i] * 2);
        }
    }
    if (state->flags & VC_INPUT_HAS_TOUCH) {
        out->has_touch = 1;
        for (i = 0; i < VC_TOUCH_POINTS; i++) {
            const vc_touch_point_t *point = &state->touch[i];
            out->touch[i].active = point->active;
            out->touch[i].id = point->id;
            out->touch[i].x = (uint16_t)(((uint32_t)point->x * VC_DS4_TOUCH_WIDTH) >> 16);
            out->touch[i].y = (uint16_t)(((uint32_t)point->y * VC_DS4_TOUCH_HEIGHT) >> 16);
        }
    }
}

static int put_event(vc_hid_evdev_event_t *out, int count, uint16_t type,
                     uint16_t code, int32_t value)
{
    out[count].type = type;
    out[count].code = code;
    out[count].value = value;
    return count + 1;
}

int vc_hid_to_evdev(const vc_input_state_t *state, vc_hid_evdev_event_t *out)
{
    int    count = 0;
    size_t i;

    for (i = 0; i < ARRAY_LEN(evdev_buttons); i++) {
        count = put_event(out, count, VC_EV_KEY, evdev_buttons[i].to,
                          (state->buttons & evdev_buttons[i].from) != 0);
    }
    count = put_event(out, count, VC_EV_KEY, VC_BTN_CODE_TL2, state->trigger_left > 0);
    count = put_event(out, count, VC_EV_KEY, VC_BTN_CODE_TR2, state->trigger_right > 0);

    count = put_event(out, count, VC_EV_ABS, VC_ABS_X, state->left.x);
    count = put_event(out, count, VC_EV_ABS, VC_ABS_Y, flip_axis(state->left.y));
    count = put_event(out, count, VC_EV_ABS, VC_ABS_RX, state->right.x);
    count = put_event(out, count, VC_EV_ABS, VC_ABS_RY, flip_axis(state->right.y));
    count = put_event(out, count, VC_EV_ABS, VC_ABS_Z, state->trigger_left);
    count = put_event(out, count, VC_EV_ABS, VC_ABS_RZ, state->trigger_right);
    count = put_event(out, count, VC_EV_ABS, VC_ABS_HAT0X, dpad_x(state->buttons));
    count = put_event(out, count, VC_EV_ABS, VC_ABS_HAT0Y, -dpad_y_up(state->buttons));

    count = put_event(out, count, VC_EV_SYN, VC_SYN_REPORT, 0);
    return count;
}

/* 12 mapped buttons + 2 trigger buttons + 8 axes + 1 sync. */
VC_STATIC_ASSERT(ARRAY_LEN(evdev_buttons) + 2 + 8 + 1 <= VC_HID_EVDEV_MAX_EVENTS,
                 evdev_event_capacity);

/* ------------------------------------------------------------------------ */
/* Backends                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Each backend provides:
 *   BACKEND_NAME
 *   backend_target_for(type)   device a controller type becomes
 *   backend_init / backend_shutdown
 *   backend_create(dev)        dev->type and dev->target are already set
 *   backend_submit(dev, state)
 *   backend_destroy(dev)
 */

#if defined(VC_HID_BACKEND_VIGEM)

/* ---- ViGEmBus (Windows) -------------------------------------------------- */

#define BACKEND_NAME "vigem"

/* The translation above uses its own constants so it builds without ViGEm.
 * These fail the build if they ever disagree with the real headers. */
VC_STATIC_ASSERT(VC_XUSB_DPAD_UP == XUSB_GAMEPAD_DPAD_UP, xusb_dpad_up);
VC_STATIC_ASSERT(VC_XUSB_START == XUSB_GAMEPAD_START, xusb_start);
VC_STATIC_ASSERT(VC_XUSB_BACK == XUSB_GAMEPAD_BACK, xusb_back);
VC_STATIC_ASSERT(VC_XUSB_LEFT_THUMB == XUSB_GAMEPAD_LEFT_THUMB, xusb_left_thumb);
VC_STATIC_ASSERT(VC_XUSB_LEFT_SHOULDER == XUSB_GAMEPAD_LEFT_SHOULDER, xusb_left_shoulder);
VC_STATIC_ASSERT(VC_XUSB_GUIDE == XUSB_GAMEPAD_GUIDE, xusb_guide);
VC_STATIC_ASSERT(VC_XUSB_A == XUSB_GAMEPAD_A, xusb_a);
VC_STATIC_ASSERT(VC_XUSB_Y == XUSB_GAMEPAD_Y, xusb_y);
VC_STATIC_ASSERT(VC_DS4_SQUARE == DS4_BUTTON_SQUARE, ds4_square);
VC_STATIC_ASSERT(VC_DS4_CROSS == DS4_BUTTON_CROSS, ds4_cross);
VC_STATIC_ASSERT(VC_DS4_TRIGGER_LEFT == DS4_BUTTON_TRIGGER_LEFT, ds4_trigger_left);
VC_STATIC_ASSERT(VC_DS4_SHARE == DS4_BUTTON_SHARE, ds4_share);
VC_STATIC_ASSERT(VC_DS4_THUMB_RIGHT == DS4_BUTTON_THUMB_RIGHT, ds4_thumb_right);
VC_STATIC_ASSERT(VC_DS4_HAT_NONE == DS4_BUTTON_DPAD_NONE, ds4_hat_none);
VC_STATIC_ASSERT(VC_DS4_HAT_NORTHWEST == DS4_BUTTON_DPAD_NORTHWEST, ds4_hat_northwest);
VC_STATIC_ASSERT(VC_DS4_SPECIAL_PS == DS4_SPECIAL_BUTTON_PS, ds4_special_ps);
VC_STATIC_ASSERT(VC_DS4_SPECIAL_TOUCHPAD == DS4_SPECIAL_BUTTON_TOUCHPAD, ds4_special_touchpad);

static PVIGEM_CLIENT g_client;

static vc_hid_target_t backend_target_for(vc_controller_type_t type)
{
    return type == VC_CONTROLLER_XBOX ? VC_HID_TARGET_X360 : VC_HID_TARGET_DS4;
}

static int backend_init(void)
{
    VIGEM_ERROR err;

    g_client = vigem_alloc();
    if (g_client == NULL) {
        hid_log("vigem_alloc() failed: out of memory");
        return -1;
    }
    err = vigem_connect(g_client);
    if (!VIGEM_SUCCESS(err)) {
        hid_log("could not connect to ViGEmBus (0x%08lX); is the driver installed?",
                (unsigned long)err);
        vigem_free(g_client);
        g_client = NULL;
        return -1;
    }
    return 0;
}

static void backend_shutdown(void)
{
    if (g_client != NULL) {
        vigem_disconnect(g_client);
        vigem_free(g_client);
        g_client = NULL;
    }
}

static int backend_create(vc_hid_device_t *dev)
{
    VIGEM_ERROR err;

    dev->pad = dev->target == VC_HID_TARGET_X360 ? vigem_target_x360_alloc()
                                                 : vigem_target_ds4_alloc();
    if (dev->pad == NULL) {
        hid_log("could not allocate a ViGEm target: out of memory");
        return -1;
    }
    err = vigem_target_add(g_client, dev->pad);
    if (!VIGEM_SUCCESS(err)) {
        hid_log("vigem_target_add() failed (0x%08lX)", (unsigned long)err);
        vigem_target_free(dev->pad);
        dev->pad = NULL;
        return -1;
    }
    return 0;
}

static int backend_submit(vc_hid_device_t *dev, const vc_input_state_t *state)
{
    VIGEM_ERROR err;

    if (dev->target == VC_HID_TARGET_X360) {
        vc_hid_xusb_report_t in;
        XUSB_REPORT          report;

        vc_hid_to_xusb(state, &in);
        memset(&report, 0, sizeof report);
        report.wButtons = in.buttons;
        report.bLeftTrigger = in.left_trigger;
        report.bRightTrigger = in.right_trigger;
        report.sThumbLX = in.thumb_lx;
        report.sThumbLY = in.thumb_ly;
        report.sThumbRX = in.thumb_rx;
        report.sThumbRY = in.thumb_ry;
        err = vigem_target_x360_update(g_client, dev->pad, report);
    } else {
        vc_hid_ds4_report_t in;
        DS4_REPORT          report;

        /* Basic report only. Gyro, accelerometer and touch are translated
         * into `in` but not sent yet: that needs DS4_REPORT_EX and
         * vigem_target_ds4_update_ex(). */
        vc_hid_to_ds4(state, &in);
        memset(&report, 0, sizeof report);
        report.bThumbLX = in.thumb_lx;
        report.bThumbLY = in.thumb_ly;
        report.bThumbRX = in.thumb_rx;
        report.bThumbRY = in.thumb_ry;
        report.wButtons = in.buttons;
        report.bSpecial = in.special;
        report.bTriggerL = in.trigger_l;
        report.bTriggerR = in.trigger_r;
        err = vigem_target_ds4_update(g_client, dev->pad, report);
    }

    if (!VIGEM_SUCCESS(err)) {
        hid_log("ViGEm report update failed (0x%08lX)", (unsigned long)err);
        return -1;
    }
    return 0;
}

static void backend_destroy(vc_hid_device_t *dev)
{
    if (dev->pad != NULL) {
        vigem_target_remove(g_client, dev->pad);
        vigem_target_free(dev->pad);
        dev->pad = NULL;
    }
}

#elif defined(VC_HID_BACKEND_UINPUT)

/* ---- uinput (Linux) ------------------------------------------------------ */

#define BACKEND_NAME "uinput"

/* The translation above uses its own constants so it builds on any OS.
 * These fail the build if they ever disagree with the kernel headers. */
VC_STATIC_ASSERT(VC_EV_KEY == EV_KEY && VC_EV_ABS == EV_ABS && VC_EV_SYN == EV_SYN, ev_types);
VC_STATIC_ASSERT(VC_KEY_RECORD == KEY_RECORD, key_record);
VC_STATIC_ASSERT(VC_BTN_CODE_SOUTH == BTN_SOUTH && VC_BTN_CODE_EAST == BTN_EAST, btn_south_east);
VC_STATIC_ASSERT(VC_BTN_CODE_NORTH == BTN_NORTH && VC_BTN_CODE_WEST == BTN_WEST, btn_north_west);
VC_STATIC_ASSERT(VC_BTN_CODE_TL == BTN_TL && VC_BTN_CODE_TR == BTN_TR, btn_shoulders);
VC_STATIC_ASSERT(VC_BTN_CODE_TL2 == BTN_TL2 && VC_BTN_CODE_TR2 == BTN_TR2, btn_triggers);
VC_STATIC_ASSERT(VC_BTN_CODE_SELECT == BTN_SELECT && VC_BTN_CODE_START == BTN_START, btn_select_start);
VC_STATIC_ASSERT(VC_BTN_CODE_MODE == BTN_MODE, btn_mode);
VC_STATIC_ASSERT(VC_BTN_CODE_THUMBL == BTN_THUMBL && VC_BTN_CODE_THUMBR == BTN_THUMBR, btn_thumbs);
VC_STATIC_ASSERT(VC_ABS_X == ABS_X && VC_ABS_Y == ABS_Y && VC_ABS_Z == ABS_Z, abs_left);
VC_STATIC_ASSERT(VC_ABS_RX == ABS_RX && VC_ABS_RY == ABS_RY && VC_ABS_RZ == ABS_RZ, abs_right);
VC_STATIC_ASSERT(VC_ABS_HAT0X == ABS_HAT0X && VC_ABS_HAT0Y == ABS_HAT0Y, abs_hat);

#define UINPUT_PATH "/dev/uinput"

typedef struct {
    uint16_t    vendor;
    uint16_t    product;
    const char *name;
} usb_identity_t;

/* Indexed by vc_controller_type_t. */
static const usb_identity_t identities[VC_CONTROLLER_TYPE_COUNT] = {
    { 0, 0, NULL },
    { 0x045E, 0x028E, "Microsoft X-Box 360 pad" },
    { 0x054C, 0x0CE6, "Sony Interactive Entertainment DualSense Wireless Controller" },
    { 0x057E, 0x2009, "Nintendo Switch Pro Controller" },
    { 0x054C, 0x09CC, "Sony Interactive Entertainment Wireless Controller" }
};

static vc_hid_target_t backend_target_for(vc_controller_type_t type)
{
    (void)type;
    return VC_HID_TARGET_EVDEV;
}

static int backend_init(void)
{
    /* Fail early, with a useful message, instead of on the first create. */
    if (access(UINPUT_PATH, W_OK) != 0) {
        hid_log("cannot write to %s: %s (load the uinput module and grant access, "
                "see docs/BUILD.md)", UINPUT_PATH, strerror(errno));
        return -1;
    }
    return 0;
}

static void backend_shutdown(void)
{
}

static int setup_axis(int fd, uint16_t code, int32_t min, int32_t max, int32_t flat)
{
    struct uinput_abs_setup axis;

    memset(&axis, 0, sizeof axis);
    axis.code = code;
    axis.absinfo.minimum = min;
    axis.absinfo.maximum = max;
    axis.absinfo.flat = flat;
    return ioctl(fd, UI_ABS_SETUP, &axis);
}

static int backend_create(vc_hid_device_t *dev)
{
    const usb_identity_t *identity = &identities[dev->type];
    struct uinput_setup   setup;
    size_t                i;
    int                   fd;
    int                   failed = 0;

    fd = open(UINPUT_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        hid_log("open(%s) failed: %s", UINPUT_PATH, strerror(errno));
        return -1;
    }

    failed |= ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0;
    for (i = 0; i < ARRAY_LEN(evdev_buttons); i++) {
        failed |= ioctl(fd, UI_SET_KEYBIT, (int)evdev_buttons[i].to) < 0;
    }
    failed |= ioctl(fd, UI_SET_KEYBIT, BTN_TL2) < 0;
    failed |= ioctl(fd, UI_SET_KEYBIT, BTN_TR2) < 0;

    failed |= ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0;
    failed |= setup_axis(fd, ABS_X, INT16_MIN, INT16_MAX, 128) < 0;
    failed |= setup_axis(fd, ABS_Y, INT16_MIN, INT16_MAX, 128) < 0;
    failed |= setup_axis(fd, ABS_RX, INT16_MIN, INT16_MAX, 128) < 0;
    failed |= setup_axis(fd, ABS_RY, INT16_MIN, INT16_MAX, 128) < 0;
    failed |= setup_axis(fd, ABS_Z, 0, VC_TRIGGER_MAX, 0) < 0;
    failed |= setup_axis(fd, ABS_RZ, 0, VC_TRIGGER_MAX, 0) < 0;
    failed |= setup_axis(fd, ABS_HAT0X, -1, 1, 0) < 0;
    failed |= setup_axis(fd, ABS_HAT0Y, -1, 1, 0) < 0;

    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = identity->vendor;
    setup.id.product = identity->product;
    setup.id.version = 1;
    strncpy(setup.name, identity->name, sizeof setup.name - 1);
    failed |= ioctl(fd, UI_DEV_SETUP, &setup) < 0;

    if (failed || ioctl(fd, UI_DEV_CREATE) < 0) {
        hid_log("could not configure the uinput device: %s", strerror(errno));
        close(fd);
        return -1;
    }
    dev->fd = fd;
    return 0;
}

static int backend_submit(vc_hid_device_t *dev, const vc_input_state_t *state)
{
    vc_hid_evdev_event_t in[VC_HID_EVDEV_MAX_EVENTS];
    struct input_event   events[VC_HID_EVDEV_MAX_EVENTS];
    size_t               bytes;
    int                  count;
    int                  i;

    /* The kernel drops events whose value did not change, so sending the
     * full state every time is fine. */
    count = vc_hid_to_evdev(state, in);
    memset(events, 0, sizeof events);
    for (i = 0; i < count; i++) {
        events[i].type = in[i].type;
        events[i].code = in[i].code;
        events[i].value = in[i].value;
    }

    bytes = (size_t)count * sizeof events[0];
    if (write(dev->fd, events, bytes) != (ssize_t)bytes) {
        hid_log("write to uinput failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void backend_destroy(vc_hid_device_t *dev)
{
    ioctl(dev->fd, UI_DEV_DESTROY);
    close(dev->fd);
}

#else

/* ---- Stub (any platform) ------------------------------------------------- */

/* Creates no device. It runs the same translation the ViGEm backend would,
 * so the rest of the daemon can be built and tested without a driver. */

#define BACKEND_NAME "stub"

static vc_hid_target_t backend_target_for(vc_controller_type_t type)
{
    return type == VC_CONTROLLER_XBOX ? VC_HID_TARGET_X360 : VC_HID_TARGET_DS4;
}

static int backend_init(void)
{
    return 0;
}

static void backend_shutdown(void)
{
}

static int backend_create(vc_hid_device_t *dev)
{
    (void)dev;
    return 0;
}

static int backend_submit(vc_hid_device_t *dev, const vc_input_state_t *state)
{
    if (dev->target == VC_HID_TARGET_X360) {
        vc_hid_xusb_report_t report;
        vc_hid_to_xusb(state, &report);
    } else {
        vc_hid_ds4_report_t report;
        vc_hid_to_ds4(state, &report);
    }
    return 0;
}

static void backend_destroy(vc_hid_device_t *dev)
{
    (void)dev;
}

#endif

/* ------------------------------------------------------------------------ */
/* Device wrapper                                                           */
/* ------------------------------------------------------------------------ */

static int g_initialized;

static int type_is_valid(vc_controller_type_t type)
{
    return (int)type > VC_CONTROLLER_NONE && (int)type < VC_CONTROLLER_TYPE_COUNT;
}

const char *vc_hid_backend_name(void)
{
    return BACKEND_NAME;
}

vc_hid_target_t vc_hid_target_for(vc_controller_type_t type)
{
    return type_is_valid(type) ? backend_target_for(type) : VC_HID_TARGET_NONE;
}

int vc_hid_init(void)
{
    if (g_initialized) {
        return 0;
    }
    if (backend_init() != 0) {
        return -1;
    }
    g_initialized = 1;
    return 0;
}

void vc_hid_shutdown(void)
{
    if (g_initialized) {
        backend_shutdown();
        g_initialized = 0;
    }
}

int vc_hid_create(vc_controller_type_t type, vc_hid_device_t **out)
{
    vc_hid_device_t *dev;

    if (out == NULL) {
        return -1;
    }
    *out = NULL;
    if (!g_initialized) {
        hid_log("vc_hid_create() called before vc_hid_init()");
        return -1;
    }
    if (!type_is_valid(type)) {
        hid_log("%d is not a controller type", (int)type);
        return -1;
    }

    dev = (vc_hid_device_t *)calloc(1, sizeof *dev);
    if (dev == NULL) {
        hid_log("out of memory allocating a device");
        return -1;
    }
    dev->type = type;
    dev->target = backend_target_for(type);
    if (backend_create(dev) != 0) {
        free(dev);
        return -1;
    }
    *out = dev;
    return 0;
}

int vc_hid_submit(vc_hid_device_t *device, const vc_input_state_t *state)
{
    if (device == NULL || state == NULL) {
        return -1;
    }
    if (backend_submit(device, state) != 0) {
        return -1;
    }
    device->submits++;
    return 0;
}

void vc_hid_get_info(const vc_hid_device_t *device, vc_hid_device_info_t *out)
{
    if (device != NULL && out != NULL) {
        out->type = device->type;
        out->target = device->target;
        out->submits = device->submits;
    }
}

void vc_hid_destroy(vc_hid_device_t *device)
{
    if (device != NULL) {
        backend_destroy(device);
        free(device);
    }
}
