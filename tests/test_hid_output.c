/*
 * test_hid_output.c - report translation and the stub device wrapper.
 *
 * Build:  cc -std=c99 -Iinclude src/hid_output.c tests/test_hid_output.c
 * (no backend flag, so the stub backend is used and no driver is needed)
 */
#include "hid_output.h"

#define VC_TEST_NO_PACKETS
#include "test_util.h"

static vc_input_state_t neutral(void)
{
    vc_input_state_t state;
    memset(&state, 0, sizeof state);
    return state;
}

/* ------------------------------------------------------------------------ */
/* Xbox 360                                                                 */
/* ------------------------------------------------------------------------ */

static void test_xusb(void)
{
    vc_input_state_t     s = neutral();
    vc_hid_xusb_report_t r;

    vc_hid_to_xusb(&s, &r);
    CHECK(r.buttons == 0);
    CHECK(r.left_trigger == 0 && r.right_trigger == 0);
    CHECK(r.thumb_lx == 0 && r.thumb_ly == 0 && r.thumb_rx == 0 && r.thumb_ry == 0);

    /* Face buttons are positional: SOUTH is A, EAST is B, and so on. */
    s.buttons = VC_BTN_SOUTH;
    vc_hid_to_xusb(&s, &r);
    CHECK(r.buttons == VC_XUSB_A);
    s.buttons = VC_BTN_EAST | VC_BTN_WEST | VC_BTN_NORTH;
    vc_hid_to_xusb(&s, &r);
    CHECK(r.buttons == (VC_XUSB_B | VC_XUSB_X | VC_XUSB_Y));

    s.buttons = VC_BTN_L1 | VC_BTN_R1 | VC_BTN_L3 | VC_BTN_R3 | VC_BTN_START |
                VC_BTN_SELECT | VC_BTN_HOME;
    vc_hid_to_xusb(&s, &r);
    CHECK(r.buttons == (VC_XUSB_LEFT_SHOULDER | VC_XUSB_RIGHT_SHOULDER |
                        VC_XUSB_LEFT_THUMB | VC_XUSB_RIGHT_THUMB | VC_XUSB_START |
                        VC_XUSB_BACK | VC_XUSB_GUIDE));

    s.buttons = VC_BTN_DPAD_UP | VC_BTN_DPAD_DOWN | VC_BTN_DPAD_LEFT | VC_BTN_DPAD_RIGHT;
    vc_hid_to_xusb(&s, &r);
    CHECK(r.buttons == 0x000F);

    /* No Xbox 360 equivalent. */
    s.buttons = VC_BTN_MISC | VC_BTN_TOUCHPAD;
    vc_hid_to_xusb(&s, &r);
    CHECK(r.buttons == 0);

    /* Sticks and triggers pass through unchanged, including the y direction. */
    s.left.x = -32768;
    s.left.y = 32767;
    s.right.x = 1234;
    s.right.y = -1;
    s.trigger_left = 255;
    s.trigger_right = 9;
    vc_hid_to_xusb(&s, &r);
    CHECK(r.thumb_lx == -32768 && r.thumb_ly == 32767);
    CHECK(r.thumb_rx == 1234 && r.thumb_ry == -1);
    CHECK(r.left_trigger == 255 && r.right_trigger == 9);
}

/* ------------------------------------------------------------------------ */
/* DualShock 4                                                              */
/* ------------------------------------------------------------------------ */

static unsigned ds4_hat(uint32_t dpad)
{
    vc_input_state_t    s = neutral();
    vc_hid_ds4_report_t r;

    s.buttons = dpad;
    vc_hid_to_ds4(&s, &r);
    return r.buttons & VC_DS4_HAT_MASK;
}

static void test_ds4(void)
{
    vc_input_state_t    s = neutral();
    vc_hid_ds4_report_t r;

    /* Neutral: sticks centred at 128, hat released, nothing pressed. */
    vc_hid_to_ds4(&s, &r);
    CHECK(r.thumb_lx == 128 && r.thumb_ly == 128);
    CHECK(r.thumb_rx == 128 && r.thumb_ry == 128);
    CHECK(r.buttons == VC_DS4_HAT_NONE);
    CHECK(r.special == 0);
    CHECK(r.trigger_l == 0 && r.trigger_r == 0);
    CHECK(r.has_motion == 0 && r.has_touch == 0);

    /* Stick extremes; y is flipped because DS4 y grows downwards. */
    s.left.x = -32768;
    s.left.y = 32767;      /* fully up */
    s.right.x = 32767;
    s.right.y = -32768;    /* fully down */
    vc_hid_to_ds4(&s, &r);
    CHECK(r.thumb_lx == 0 && r.thumb_ly == 0);
    CHECK(r.thumb_rx == 255 && r.thumb_ry == 255);

    s = neutral();
    s.buttons = VC_BTN_SOUTH | VC_BTN_EAST | VC_BTN_WEST | VC_BTN_NORTH;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.buttons == (VC_DS4_CROSS | VC_DS4_CIRCLE | VC_DS4_SQUARE |
                        VC_DS4_TRIANGLE | VC_DS4_HAT_NONE));

    s.buttons = VC_BTN_L1 | VC_BTN_R1 | VC_BTN_L3 | VC_BTN_R3 | VC_BTN_START |
                VC_BTN_SELECT | VC_BTN_MISC;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.buttons == (VC_DS4_SHOULDER_LEFT | VC_DS4_SHOULDER_RIGHT |
                        VC_DS4_THUMB_LEFT | VC_DS4_THUMB_RIGHT | VC_DS4_OPTIONS |
                        VC_DS4_SHARE | VC_DS4_HAT_NONE));
    CHECK(r.special == 0);

    s.buttons = VC_BTN_HOME | VC_BTN_TOUCHPAD;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.special == (VC_DS4_SPECIAL_PS | VC_DS4_SPECIAL_TOUCHPAD));
    CHECK(r.buttons == VC_DS4_HAT_NONE);

    /* A pulled trigger also sets its digital button. */
    s = neutral();
    s.trigger_left = 1;
    s.trigger_right = 255;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.trigger_l == 1 && r.trigger_r == 255);
    CHECK(r.buttons == (VC_DS4_TRIGGER_LEFT | VC_DS4_TRIGGER_RIGHT | VC_DS4_HAT_NONE));

    /* All eight hat directions, plus opposite directions cancelling. */
    CHECK(ds4_hat(VC_BTN_DPAD_UP) == VC_DS4_HAT_NORTH);
    CHECK(ds4_hat(VC_BTN_DPAD_UP | VC_BTN_DPAD_RIGHT) == VC_DS4_HAT_NORTHEAST);
    CHECK(ds4_hat(VC_BTN_DPAD_RIGHT) == VC_DS4_HAT_EAST);
    CHECK(ds4_hat(VC_BTN_DPAD_DOWN | VC_BTN_DPAD_RIGHT) == VC_DS4_HAT_SOUTHEAST);
    CHECK(ds4_hat(VC_BTN_DPAD_DOWN) == VC_DS4_HAT_SOUTH);
    CHECK(ds4_hat(VC_BTN_DPAD_DOWN | VC_BTN_DPAD_LEFT) == VC_DS4_HAT_SOUTHWEST);
    CHECK(ds4_hat(VC_BTN_DPAD_LEFT) == VC_DS4_HAT_WEST);
    CHECK(ds4_hat(VC_BTN_DPAD_UP | VC_BTN_DPAD_LEFT) == VC_DS4_HAT_NORTHWEST);
    CHECK(ds4_hat(VC_BTN_DPAD_UP | VC_BTN_DPAD_DOWN) == VC_DS4_HAT_NONE);
    CHECK(ds4_hat(VC_BTN_DPAD_LEFT | VC_BTN_DPAD_RIGHT | VC_BTN_DPAD_UP) == VC_DS4_HAT_NORTH);

    /* Motion: gyro unchanged, accel doubled and saturated. */
    s = neutral();
    s.flags = VC_INPUT_HAS_MOTION;
    s.motion.gyro[0] = -100;
    s.motion.gyro[2] = 32767;
    s.motion.accel[0] = 4096;      /* 1 g */
    s.motion.accel[1] = 20000;     /* would overflow when doubled */
    s.motion.accel[2] = -32768;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.has_motion == 1 && r.has_touch == 0);
    CHECK(r.gyro[0] == -100 && r.gyro[2] == 32767);
    CHECK(r.accel[0] == 8192);
    CHECK(r.accel[1] == 32767);
    CHECK(r.accel[2] == -32768);

    /* Touch: 0..65535 scaled onto the 1920 x 943 pad. */
    s = neutral();
    s.flags = VC_INPUT_HAS_TOUCH;
    s.touch[0].active = 1;
    s.touch[0].id = 9;
    s.touch[0].x = 65535;
    s.touch[0].y = 65535;
    s.touch[1].active = 1;
    s.touch[1].x = 32768;
    s.touch[1].y = 0;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.has_touch == 1 && r.has_motion == 0);
    CHECK(r.touch[0].active == 1 && r.touch[0].id == 9);
    CHECK(r.touch[0].x == VC_DS4_TOUCH_WIDTH - 1);
    CHECK(r.touch[0].y == VC_DS4_TOUCH_HEIGHT - 1);
    CHECK(r.touch[1].x == VC_DS4_TOUCH_WIDTH / 2 && r.touch[1].y == 0);

    /* Motion and touch data without the flag is not carried over. */
    s.flags = 0;
    vc_hid_to_ds4(&s, &r);
    CHECK(r.has_touch == 0 && r.touch[0].x == 0);
}

/* ------------------------------------------------------------------------ */
/* evdev                                                                    */
/* ------------------------------------------------------------------------ */

/* Value of the event with this type and code, or -9999 if absent or repeated. */
static int32_t ev_value(const vc_hid_evdev_event_t *events, int count,
                        unsigned type, unsigned code)
{
    int32_t value = -9999;
    int     found = 0;
    int     i;

    for (i = 0; i < count; i++) {
        if (events[i].type == type && events[i].code == code) {
            value = events[i].value;
            found++;
        }
    }
    return found == 1 ? value : -9999;
}

static void test_evdev(void)
{
    vc_input_state_t     s = neutral();
    vc_hid_evdev_event_t ev[VC_HID_EVDEV_MAX_EVENTS];
    int                  n;

    n = vc_hid_to_evdev(&s, ev);
    CHECK(n == 23);
    CHECK(n <= VC_HID_EVDEV_MAX_EVENTS);
    /* The batch ends with a sync, and only there. */
    CHECK(ev[n - 1].type == VC_EV_SYN && ev[n - 1].code == VC_SYN_REPORT);
    CHECK(ev_value(ev, n, VC_EV_SYN, VC_SYN_REPORT) == 0);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_SOUTH) == 0);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_X) == 0);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_HAT0Y) == 0);

    s.buttons = VC_BTN_SOUTH | VC_BTN_NORTH | VC_BTN_HOME | VC_BTN_MISC |
                VC_BTN_DPAD_UP | VC_BTN_DPAD_LEFT | VC_BTN_TOUCHPAD;
    s.left.x = 1000;
    s.left.y = 32767;       /* up */
    s.right.x = -32768;
    s.right.y = -32768;     /* down */
    s.trigger_left = 0;
    s.trigger_right = 200;
    n = vc_hid_to_evdev(&s, ev);
    CHECK(n == 23);   /* always the full state */

    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_SOUTH) == 1);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_NORTH) == 1);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_EAST) == 0);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_WEST) == 0);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_MODE) == 1);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_KEY_RECORD) == 1);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_TL2) == 0);
    CHECK(ev_value(ev, n, VC_EV_KEY, VC_BTN_CODE_TR2) == 1);

    /* evdev y grows downwards, and -32768 saturates when flipped. */
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_X) == 1000);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_Y) == -32767);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_RX) == -32768);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_RY) == 32767);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_Z) == 0);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_RZ) == 200);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_HAT0X) == -1);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_HAT0Y) == -1);

    s = neutral();
    s.buttons = VC_BTN_DPAD_DOWN | VC_BTN_DPAD_RIGHT;
    n = vc_hid_to_evdev(&s, ev);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_HAT0X) == 1);
    CHECK(ev_value(ev, n, VC_EV_ABS, VC_ABS_HAT0Y) == 1);
}

/* ------------------------------------------------------------------------ */
/* Device wrapper (stub backend)                                            */
/* ------------------------------------------------------------------------ */

static void test_device(void)
{
    vc_hid_device_t     *dev = NULL;
    vc_hid_device_info_t info;
    vc_input_state_t     s = neutral();
    int                  type;

    CHECK(strcmp(vc_hid_backend_name(), "stub") == 0);

    CHECK(vc_hid_target_for(VC_CONTROLLER_XBOX) == VC_HID_TARGET_X360);
    CHECK(vc_hid_target_for(VC_CONTROLLER_PS4) == VC_HID_TARGET_DS4);
    CHECK(vc_hid_target_for(VC_CONTROLLER_PS5) == VC_HID_TARGET_DS4);
    CHECK(vc_hid_target_for(VC_CONTROLLER_SWITCH) == VC_HID_TARGET_DS4);
    CHECK(vc_hid_target_for(VC_CONTROLLER_NONE) == VC_HID_TARGET_NONE);
    CHECK(vc_hid_target_for((vc_controller_type_t)42) == VC_HID_TARGET_NONE);

    /* Creating before init is refused (and logged). */
    printf("  expect two [hid] errors below\n");
    fflush(stdout);
    CHECK(vc_hid_create(VC_CONTROLLER_XBOX, &dev) == -1 && dev == NULL);

    CHECK(vc_hid_init() == 0);
    CHECK(vc_hid_init() == 0);   /* harmless when repeated */
    CHECK(vc_hid_create(VC_CONTROLLER_NONE, &dev) == -1 && dev == NULL);
    CHECK(vc_hid_create(VC_CONTROLLER_XBOX, NULL) == -1);

    for (type = VC_CONTROLLER_XBOX; type < VC_CONTROLLER_TYPE_COUNT; type++) {
        CHECK(vc_hid_create((vc_controller_type_t)type, &dev) == 0);
        CHECK(dev != NULL);
        if (dev == NULL) {
            continue;
        }
        s.buttons = VC_BTN_SOUTH;
        CHECK(vc_hid_submit(dev, &s) == 0);
        CHECK(vc_hid_submit(dev, &s) == 0);
        CHECK(vc_hid_submit(dev, NULL) == -1);

        memset(&info, 0, sizeof info);
        vc_hid_get_info(dev, &info);
        CHECK((int)info.type == type);
        CHECK(info.target == vc_hid_target_for((vc_controller_type_t)type));
        CHECK(info.submits == 2);

        vc_hid_destroy(dev);
        dev = NULL;
    }

    CHECK(vc_hid_submit(NULL, &s) == -1);
    vc_hid_destroy(NULL);
    vc_hid_shutdown();
    vc_hid_shutdown();   /* harmless when repeated */
}

int main(void)
{
    test_xusb();
    test_ds4();
    test_evdev();
    test_device();
    return test_summary();
}
