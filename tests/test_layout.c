/*
 * test_layout.c - the on-screen gamepad: default layouts per controller
 * type, labels from the profiles, and the touch-to-state mapping (buttons,
 * sliding, floating and fixed sticks with deadzones, D-pad sectors,
 * triggers, touchpad).
 *
 * Build:  cc -std=c99 -Iinclude -Itests src/layout.c src/parser.c tests/test_layout.c -lm
 */
#include "layout.h"

#define VC_TEST_NO_PACKETS
#include "test_util.h"

#define ASPECT 2.0f   /* the reference phone layout: positions below are exact */

static int find_button(const vc_layout_t *l, uint32_t button)
{
    int i;
    for (i = 0; i < l->count; i++) {
        if (l->controls[i].kind == VC_CONTROL_BUTTON && l->controls[i].button == button) {
            return i;
        }
    }
    return -1;
}

static int find_kind(const vc_layout_t *l, vc_control_kind_t kind, int side)
{
    int i;
    for (i = 0; i < l->count; i++) {
        if (l->controls[i].kind == kind && l->controls[i].side == side) {
            return i;
        }
    }
    return -1;
}

static vc_input_state_t state_of(const vc_layout_t *l)
{
    vc_input_state_t s;
    memset(&s, 0, sizeof s);
    vc_layout_state(l, &s);
    return s;
}

static int near(int a, int b, int tolerance)
{
    return a - b <= tolerance && b - a <= tolerance;
}

static int feq(float a, float b)
{
    return a - b < 1e-5f && b - a < 1e-5f;
}

/* ------------------------------------------------------------------------ */

static void test_profiles_and_labels(void)
{
    static vc_layout_t xbox;
    static vc_layout_t ps5;
    static vc_layout_t ps4;
    static vc_layout_t nswitch;
    int                south;

    vc_layout_init_default(&xbox, VC_CONTROLLER_XBOX, ASPECT);
    vc_layout_init_default(&ps5, VC_CONTROLLER_PS5, ASPECT);
    vc_layout_init_default(&ps4, VC_CONTROLLER_PS4, ASPECT);
    vc_layout_init_default(&nswitch, VC_CONTROLLER_SWITCH, ASPECT);

    /* Controls follow the profiles: no touchpad on Xbox or Switch, no mic
     * button on PS4. */
    CHECK(find_kind(&xbox, VC_CONTROL_TOUCHPAD, 0) < 0 && find_button(&xbox, VC_BTN_TOUCHPAD) < 0);
    CHECK(find_kind(&nswitch, VC_CONTROL_TOUCHPAD, 0) < 0);
    CHECK(find_kind(&ps5, VC_CONTROL_TOUCHPAD, 0) >= 0 && find_button(&ps5, VC_BTN_TOUCHPAD) >= 0);
    CHECK(find_button(&ps5, VC_BTN_MISC) >= 0 && find_button(&ps4, VC_BTN_MISC) < 0);
    CHECK(xbox.count == 17 && ps5.count == 19 && ps4.count == 18);

    /* The same position reads differently per controller. */
    south = find_button(&xbox, VC_BTN_SOUTH);
    CHECK(strcmp(vc_layout_label(&xbox, south), "A") == 0);
    CHECK(vc_layout_color(&xbox, south) == 0x5DB33Bu);
    south = find_button(&nswitch, VC_BTN_SOUTH);
    CHECK(strcmp(vc_layout_label(&nswitch, south), "B") == 0);
    CHECK(vc_layout_color(&nswitch, south) == 0);
    south = find_button(&ps5, VC_BTN_SOUTH);
    CHECK(strcmp(vc_layout_label(&ps5, south), "✕") == 0);
    CHECK(strcmp(vc_layout_name(&ps5, south), "Cross") == 0);
    CHECK(strcmp(vc_layout_label(&nswitch, find_button(&nswitch, VC_BTN_START)), "+") == 0);
    CHECK(strcmp(vc_layout_label(&xbox, find_button(&xbox, VC_BTN_L1)), "LB") == 0);
    CHECK(strcmp(vc_layout_label(&nswitch, find_kind(&nswitch, VC_CONTROL_TRIGGER, 0)), "ZL") == 0);
    CHECK(strcmp(vc_layout_label(&ps4, find_kind(&ps4, VC_CONTROL_TRIGGER, 1)), "R2") == 0);
    CHECK(strcmp(vc_layout_label(&xbox, find_kind(&xbox, VC_CONTROL_TRIGGER, 1)), "RT") == 0);
    CHECK(strcmp(vc_layout_label(&xbox, find_kind(&xbox, VC_CONTROL_STICK, 0)), "") == 0);
    CHECK(strcmp(vc_layout_name(&xbox, find_kind(&xbox, VC_CONTROL_STICK, 0)), "Left stick") == 0);
    CHECK(strcmp(vc_layout_label(&xbox, 99), "") == 0);
}

static void test_buttons(void)
{
    static vc_layout_t l;
    vc_input_state_t   s;
    const vc_control_t *south;
    const vc_control_t *east;

    vc_layout_init_default(&l, VC_CONTROLLER_XBOX, ASPECT);
    south = &l.controls[find_button(&l, VC_BTN_SOUTH)];
    east = &l.controls[find_button(&l, VC_BTN_EAST)];

    vc_layout_touch(&l, 1, VC_TOUCH_DOWN, south->x, south->y);
    CHECK(state_of(&l).buttons == VC_BTN_SOUTH);

    /* Sliding from one face button to the next. */
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, east->x, east->y);
    CHECK(state_of(&l).buttons == VC_BTN_EAST);
    vc_layout_touch(&l, 1, VC_TOUCH_UP, east->x, east->y);
    CHECK(state_of(&l).buttons == 0);

    /* A touch that starts on nothing presses what it slides onto. */
    vc_layout_touch(&l, 2, VC_TOUCH_DOWN, 1.0f, 0.45f);
    CHECK(state_of(&l).buttons == 0);
    vc_layout_touch(&l, 2, VC_TOUCH_MOVE, south->x, south->y);
    CHECK(state_of(&l).buttons == VC_BTN_SOUTH);

    /* Two fingers, two buttons. */
    vc_layout_touch(&l, 3, VC_TOUCH_DOWN, east->x, east->y);
    s = state_of(&l);
    CHECK(s.buttons == (VC_BTN_SOUTH | VC_BTN_EAST));

    /* reset() lifts everything. */
    vc_layout_reset(&l);
    CHECK(state_of(&l).buttons == 0);

    /* A MOVE whose DOWN was missed counts as a DOWN. */
    vc_layout_touch(&l, 4, VC_TOUCH_MOVE, south->x, south->y);
    CHECK(state_of(&l).buttons == VC_BTN_SOUTH);
    vc_layout_touch(&l, 4, VC_TOUCH_CANCEL, 0, 0);
    CHECK(state_of(&l).buttons == 0);
}

static void test_floating_stick(void)
{
    static vc_layout_t  l;
    vc_input_state_t    s;
    vc_control_visual_t v;
    int                 stick;

    vc_layout_init_default(&l, VC_CONTROLLER_XBOX, ASPECT);
    stick = find_kind(&l, VC_CONTROL_STICK, 0);
    CHECK(l.controls[stick].floating);

    /* The stick centres where the thumb lands: no deflection yet. */
    vc_layout_touch(&l, 1, VC_TOUCH_DOWN, 0.40f, 0.60f);
    s = state_of(&l);
    CHECK(s.left.x == 0 && s.left.y == 0);
    vc_layout_visual(&l, stick, &v);
    CHECK(v.pressed && feq(v.base_x, 0.40f) && feq(v.base_y, 0.60f));

    /* Full radius right: past the 0.85 saturation, so full scale. */
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, 0.55f, 0.60f);
    s = state_of(&l);
    CHECK(s.left.x == 32767 && s.left.y == 0);

    /* Up on screen is +y on the wire. */
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, 0.40f, 0.45f);
    s = state_of(&l);
    CHECK(s.left.x == 0 && s.left.y == 32767);

    /* Inside the 12% deadzone: neutral. Just past it: small, not a jump. */
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, 0.41f, 0.60f);   /* 6.7% */
    CHECK(state_of(&l).left.x == 0);
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, 0.4225f, 0.60f); /* 15% */
    s = state_of(&l);
    CHECK(s.left.x > 0 && s.left.x < 2500);

    /* A second thumb in the zone does not steal the stick. */
    vc_layout_touch(&l, 2, VC_TOUCH_DOWN, 0.30f, 0.50f);
    CHECK(l.touches[1].control == -1);
    vc_layout_touch(&l, 2, VC_TOUCH_UP, 0, 0);
    vc_layout_touch(&l, 1, VC_TOUCH_UP, 0, 0);
    CHECK(state_of(&l).left.x == 0);

    /* Landing at the edge: the base is kept whole on screen. */
    vc_layout_touch(&l, 3, VC_TOUCH_DOWN, 0.02f, 0.97f);
    vc_layout_visual(&l, stick, &v);
    CHECK(feq(v.base_x, 0.15f) && feq(v.base_y, 0.85f));
    s = state_of(&l);
    CHECK(s.left.x < 0 && s.left.y < 0);   /* the finger is down-left of the base */
    vc_layout_touch(&l, 3, VC_TOUCH_UP, 0, 0);

    /* Configurable deadzone: a bigger one swallows the same offset. */
    vc_layout_set_stick(&l, 0, 0.5f, 0.9f);
    vc_layout_touch(&l, 4, VC_TOUCH_DOWN, 0.40f, 0.60f);
    vc_layout_touch(&l, 4, VC_TOUCH_MOVE, 0.46f, 0.60f);   /* 40% */
    CHECK(state_of(&l).left.x == 0);
}

static void test_fixed_stick(void)
{
    static vc_layout_t  l;
    vc_input_state_t    s;
    const vc_control_t *stick;
    const vc_control_t *north;

    vc_layout_init_default(&l, VC_CONTROLLER_XBOX, ASPECT);
    stick = &l.controls[find_kind(&l, VC_CONTROL_STICK, 1)];
    north = &l.controls[find_button(&l, VC_BTN_NORTH)];
    CHECK(!stick->floating);

    /* Half the radius: (0.5 - 0.10) / (0.90 - 0.10) = 0.5 of full scale. */
    vc_layout_touch(&l, 1, VC_TOUCH_DOWN, stick->x, stick->y);
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, stick->x + 0.07f, stick->y);
    s = state_of(&l);
    CHECK(near(s.right.x, 16384, 8) && s.right.y == 0);

    /* Captured: sliding far off the stick keeps it at full deflection, and
     * a face button can be pressed meanwhile. */
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, stick->x - 0.6f, stick->y);
    vc_layout_touch(&l, 2, VC_TOUCH_DOWN, north->x, north->y);
    s = state_of(&l);
    CHECK(s.right.x == -32767);
    CHECK(s.buttons == VC_BTN_NORTH);
}

static void test_dpad_and_triggers(void)
{
    static vc_layout_t  l;
    const vc_control_t *dpad;
    const vc_control_t *trigger;
    vc_input_state_t    s;

    vc_layout_init_default(&l, VC_CONTROLLER_SWITCH, ASPECT);
    dpad = &l.controls[find_kind(&l, VC_CONTROL_DPAD, 0)];

    vc_layout_touch(&l, 1, VC_TOUCH_DOWN, dpad->x + 0.10f, dpad->y);
    CHECK(state_of(&l).buttons == VC_BTN_DPAD_RIGHT);
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, dpad->x + 0.07f, dpad->y - 0.07f);
    CHECK(state_of(&l).buttons == (VC_BTN_DPAD_UP | VC_BTN_DPAD_RIGHT));
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, dpad->x + 0.10f, dpad->y + 0.02f);   /* 11 degrees */
    CHECK(state_of(&l).buttons == VC_BTN_DPAD_RIGHT);
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, dpad->x, dpad->y + 0.10f);
    CHECK(state_of(&l).buttons == VC_BTN_DPAD_DOWN);
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, dpad->x + 0.02f, dpad->y);           /* deadzone */
    CHECK(state_of(&l).buttons == 0);
    vc_layout_touch(&l, 1, VC_TOUCH_MOVE, dpad->x - 0.5f, dpad->y);            /* captured */
    CHECK(state_of(&l).buttons == VC_BTN_DPAD_LEFT);
    vc_layout_touch(&l, 1, VC_TOUCH_UP, 0, 0);

    trigger = &l.controls[find_kind(&l, VC_CONTROL_TRIGGER, 0)];
    vc_layout_touch(&l, 2, VC_TOUCH_DOWN, trigger->x, trigger->y);
    s = state_of(&l);
    CHECK(s.trigger_left == 255 && s.trigger_right == 0 && s.buttons == 0);
}

static void test_touchpad_and_flags(void)
{
    static vc_layout_t  ps5;
    static vc_layout_t  xbox;
    const vc_control_t *pad;
    vc_input_state_t    s;

    vc_layout_init_default(&ps5, VC_CONTROLLER_PS5, ASPECT);
    pad = &ps5.controls[find_kind(&ps5, VC_CONTROL_TOUCHPAD, 0)];

    /* Motion set elsewhere survives; touch flag set with no contacts. */
    memset(&s, 0, sizeof s);
    s.flags = VC_INPUT_HAS_MOTION;
    s.motion.accel[2] = 4096;
    vc_layout_state(&ps5, &s);
    CHECK(s.flags == (VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH));
    CHECK(s.motion.accel[2] == 4096 && s.touch[0].active == 0);

    /* Contacts in pad coordinates, 0..65535 from the top-left corner. */
    vc_layout_touch(&ps5, 7, VC_TOUCH_DOWN, pad->x - pad->w * 0.25f, pad->y - pad->h * 0.25f);
    vc_layout_touch(&ps5, 8, VC_TOUCH_DOWN, pad->x + pad->w * 0.25f, pad->y);
    vc_layout_state(&ps5, &s);
    CHECK(s.touch[0].active && near(s.touch[0].x, 16384, 4) && near(s.touch[0].y, 16384, 4));
    CHECK(s.touch[1].active && near(s.touch[1].x, 49151, 4) && near(s.touch[1].y, 32768, 4));
    CHECK(s.touch[0].id != s.touch[1].id);
    CHECK(s.buttons == 0);   /* touching is not clicking */

    /* No touchpad: the flag is cleared even if it was set. */
    vc_layout_init_default(&xbox, VC_CONTROLLER_XBOX, ASPECT);
    s.flags = VC_INPUT_HAS_TOUCH | VC_INPUT_HAS_MOTION;
    vc_layout_state(&xbox, &s);
    CHECK(s.flags == VC_INPUT_HAS_MOTION);
}

static void test_narrow_views(void)
{
    static const float aspects[] = { 1.33f, 1.6f, 2.0f, 2.17f, 2.4f };
    static vc_layout_t l;
    size_t             a;
    int                i;
    int                fits = 1;

    /* Everything stays on screen, from tablets to tall phones. */
    for (a = 0; a < sizeof aspects / sizeof aspects[0]; a++) {
        vc_layout_init_default(&l, VC_CONTROLLER_PS5, aspects[a]);
        for (i = 0; i < l.count; i++) {
            const vc_control_t *c = &l.controls[i];
            if (c->x - c->w * 0.5f < -0.001f || c->x + c->w * 0.5f > aspects[a] + 0.001f ||
                c->y - c->h * 0.5f < -0.001f || c->y + c->h * 0.5f > 1.001f) {
                printf("  control %d off screen at aspect %.2f\n", i, (double)aspects[a]);
                fits = 0;
            }
        }
    }
    CHECK(fits);

    /* More fingers than tracked: ignored, no crash. */
    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES + 3; i++) {
        vc_layout_touch(&l, (uint64_t)(100 + i), VC_TOUCH_DOWN, 1.0f, 0.95f);
    }
    vc_layout_reset(&l);
    CHECK(state_of(&l).buttons == 0);
}

int main(void)
{
    test_profiles_and_labels();
    test_buttons();
    test_floating_stick();
    test_fixed_stick();
    test_dpad_and_triggers();
    test_touchpad_and_flags();
    test_narrow_views();
    return test_summary();
}
