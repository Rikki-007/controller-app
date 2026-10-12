/*
 * layout.c - the on-screen gamepad: geometry, touch tracking, touch-to-state.
 * See layout.h for the contract.
 */
#include <math.h>
#include <string.h>

#include "layout.h"
#include "parser.h"

/* The default layout is designed for this aspect (a phone in landscape) and
 * scaled down for narrower views. */
#define REFERENCE_ASPECT  2.0f
/* Buttons accept touches this much (fraction of their size) outside the
 * drawn shape: fingers are bigger than they look on glass. */
#define DEFAULT_HIT_SLOP  0.15f
/* tan(22.5 degrees): splits the D-pad into 8 equal sectors. */
#define DPAD_SECTOR       0.41421356f

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------------ */
/* Geometry                                                                 */
/* ------------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int inside(const vc_control_t *c, float x, float y, float slop)
{
    float grow = 1.0f + slop;

    if (c->round) {
        float r = c->w * 0.5f * grow;
        float dx = x - c->x;
        float dy = y - c->y;
        return dx * dx + dy * dy <= r * r;
    }
    return fabsf(x - c->x) <= c->w * 0.5f * grow && fabsf(y - c->y) <= c->h * 0.5f * grow;
}

static int in_zone(const vc_control_t *c, float x, float y)
{
    return x >= c->zone_x0 && x <= c->zone_x1 && y >= c->zone_y0 && y <= c->zone_y1;
}

/* Touches currently captured by control `index`. */
static int captures(const vc_layout_t *l, int index)
{
    int count = 0;
    int i;

    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES; i++) {
        count += l->touches[i].active && l->touches[i].control == index;
    }
    return count;
}

/* Stick deflection from the finger's offset: radial deadzone, rescaled so
 * output starts at 0 at the deadzone's edge and reaches 1 at `saturation`.
 * Radial (not per-axis) keeps diagonals and fine aim intact. y is screen
 * direction (down). */
static void stick_value(const vc_control_t *c, float dx, float dy, float *out_x, float *out_y)
{
    float r = c->w * 0.5f;
    float deadzone = clampf(c->deadzone, 0.0f, 0.9f);
    float saturation = c->saturation;
    float nx;
    float ny;
    float magnitude;
    float scaled;

    *out_x = 0.0f;
    *out_y = 0.0f;
    if (r <= 0.0f) {
        return;
    }
    if (saturation <= deadzone || saturation > 1.0f) {
        saturation = 1.0f;
    }
    nx = dx / r;
    ny = dy / r;
    magnitude = sqrtf(nx * nx + ny * ny);
    if (magnitude <= deadzone) {
        return;
    }
    scaled = (magnitude - deadzone) / (saturation - deadzone);
    if (scaled > 1.0f) {
        scaled = 1.0f;
    }
    *out_x = nx / magnitude * scaled;
    *out_y = ny / magnitude * scaled;
}

/* D-pad bits from the finger's offset from the centre (screen y down). */
static uint32_t dpad_buttons(const vc_control_t *c, float dx, float dy)
{
    float    r = c->w * 0.5f;
    float    deadzone = clampf(c->deadzone, 0.0f, 0.9f);
    float    ax;
    float    ay;
    uint32_t buttons = 0;

    if (r <= 0.0f || dx * dx + dy * dy <= (deadzone * r) * (deadzone * r)) {
        return 0;
    }
    ax = fabsf(dx);
    ay = fabsf(dy);
    if (ax >= ay * DPAD_SECTOR) {
        buttons |= dx > 0.0f ? VC_BTN_DPAD_RIGHT : VC_BTN_DPAD_LEFT;
    }
    if (ay >= ax * DPAD_SECTOR) {
        buttons |= dy > 0.0f ? VC_BTN_DPAD_DOWN : VC_BTN_DPAD_UP;
    }
    return buttons;
}

static int16_t to_axis(float v)
{
    v = clampf(v, -1.0f, 1.0f) * 32767.0f;
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static uint16_t to_unit16(float v)
{
    return (uint16_t)(clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

/* ------------------------------------------------------------------------ */
/* Building layouts                                                         */
/* ------------------------------------------------------------------------ */

void vc_layout_init_empty(vc_layout_t *l, vc_controller_type_t type, float aspect)
{
    int i;

    if (l == NULL) {
        return;
    }
    memset(l, 0, sizeof *l);
    l->type = type;
    l->aspect = aspect > 0.1f ? aspect : REFERENCE_ASPECT;
    l->hit_slop = DEFAULT_HIT_SLOP;
    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES; i++) {
        l->touches[i].control = -1;
    }
}

int vc_layout_add(vc_layout_t *l, const vc_control_t *control)
{
    if (l == NULL || control == NULL || l->count >= VC_LAYOUT_MAX_CONTROLS) {
        return -1;
    }
    l->controls[l->count] = *control;
    return l->count++;
}

const vc_control_t *vc_layout_control(const vc_layout_t *l, int index)
{
    return l != NULL && index >= 0 && index < l->count ? &l->controls[index] : NULL;
}

void vc_layout_set_stick(vc_layout_t *l, int side, float deadzone, float saturation)
{
    int i;

    if (l == NULL) {
        return;
    }
    for (i = 0; i < l->count; i++) {
        vc_control_t *c = &l->controls[i];
        if (c->kind == VC_CONTROL_STICK && c->side == side) {
            c->deadzone = clampf(deadzone, 0.0f, 0.9f);
            c->saturation = clampf(saturation, c->deadzone + 0.01f, 1.0f);
        }
    }
}

/* Positions in the default layout are given for REFERENCE_ASPECT, relative
 * to an anchor, and scaled by `scale` towards the bottom edge on narrower
 * views. */
typedef enum { ANCHOR_LEFT, ANCHOR_CENTER, ANCHOR_RIGHT } anchor_t;

typedef struct {
    vc_layout_t *l;
    float        scale;
} builder_t;

static float place_x(const builder_t *b, anchor_t anchor, float offset)
{
    switch (anchor) {
    case ANCHOR_LEFT:   return offset * b->scale;
    case ANCHOR_RIGHT:  return b->l->aspect - offset * b->scale;
    default:            return b->l->aspect * 0.5f + offset * b->scale;
    }
}

static float place_y(const builder_t *b, float y)
{
    return 1.0f - (1.0f - y) * b->scale;
}

static vc_control_t *add(builder_t *b, vc_control_kind_t kind, anchor_t anchor,
                         float x, float y, float w, float h, int round)
{
    vc_control_t c;
    int          index;

    memset(&c, 0, sizeof c);
    c.kind = kind;
    c.x = place_x(b, anchor, x);
    c.y = place_y(b, y);
    c.w = w * b->scale;
    c.h = (round ? w : h) * b->scale;
    c.round = round;
    index = vc_layout_add(b->l, &c);
    return index >= 0 ? &b->l->controls[index] : NULL;
}

/* A button, unless the controller type has no such button. */
static void add_button(builder_t *b, uint32_t button, anchor_t anchor,
                       float x, float y, float w, float h, int round)
{
    vc_control_t *c;

    if (vc_parser_button_name(b->l->type, button) == NULL) {
        return;
    }
    c = add(b, VC_CONTROL_BUTTON, anchor, x, y, w, h, round);
    if (c != NULL) {
        c->button = button;
    }
}

void vc_layout_init_default(vc_layout_t *l, vc_controller_type_t type, float aspect)
{
    builder_t     b;
    vc_control_t *c;
    const float   face_x = 0.30f;   /* face cluster centre, from the right edge */
    const float   face_y = 0.55f;
    const float   face_d = 0.12f;   /* centre to each face button */
    const float   face_w = 0.115f;

    vc_layout_init_empty(l, type, aspect);
    if (l == NULL) {
        return;
    }
    b.l = l;
    b.scale = l->aspect < REFERENCE_ASPECT ? l->aspect / REFERENCE_ASPECT : 1.0f;

    /* Top corners: triggers, then shoulders. */
    c = add(&b, VC_CONTROL_TRIGGER, ANCHOR_LEFT, 0.20f, 0.09f, 0.30f, 0.12f, 0);
    if (c != NULL) {
        c->side = 0;
    }
    c = add(&b, VC_CONTROL_TRIGGER, ANCHOR_RIGHT, 0.20f, 0.09f, 0.30f, 0.12f, 0);
    if (c != NULL) {
        c->side = 1;
    }
    add_button(&b, VC_BTN_L1, ANCHOR_LEFT, 0.20f, 0.23f, 0.30f, 0.10f, 0);
    add_button(&b, VC_BTN_R1, ANCHOR_RIGHT, 0.20f, 0.23f, 0.30f, 0.10f, 0);

    /* Left: floating stick over most of the left half, D-pad below-inside. */
    c = add(&b, VC_CONTROL_STICK, ANCHOR_LEFT, 0.30f, 0.58f, 0.30f, 0.30f, 1);
    if (c != NULL) {
        c->side = 0;
        c->deadzone = 0.12f;
        c->saturation = 0.85f;
        c->floating = 1;
        c->zone_x0 = 0.0f;
        c->zone_y0 = place_y(&b, 0.30f);
        c->zone_x1 = place_x(&b, ANCHOR_CENTER, -0.25f);
        c->zone_y1 = 1.0f;
    }
    c = add(&b, VC_CONTROL_DPAD, ANCHOR_LEFT, 0.62f, 0.80f, 0.28f, 0.28f, 1);
    if (c != NULL) {
        c->deadzone = 0.25f;
    }
    add_button(&b, VC_BTN_L3, ANCHOR_LEFT, 0.10f, 0.90f, 0.11f, 0.11f, 1);

    /* Right: face diamond, fixed stick below-inside. */
    add_button(&b, VC_BTN_NORTH, ANCHOR_RIGHT, face_x, face_y - face_d, face_w, face_w, 1);
    add_button(&b, VC_BTN_SOUTH, ANCHOR_RIGHT, face_x, face_y + face_d, face_w, face_w, 1);
    add_button(&b, VC_BTN_WEST, ANCHOR_RIGHT, face_x + face_d, face_y, face_w, face_w, 1);
    add_button(&b, VC_BTN_EAST, ANCHOR_RIGHT, face_x - face_d, face_y, face_w, face_w, 1);
    c = add(&b, VC_CONTROL_STICK, ANCHOR_RIGHT, 0.62f, 0.80f, 0.28f, 0.28f, 1);
    if (c != NULL) {
        c->side = 1;
        c->deadzone = 0.10f;
        c->saturation = 0.90f;
    }
    add_button(&b, VC_BTN_R3, ANCHOR_RIGHT, 0.10f, 0.90f, 0.11f, 0.11f, 1);

    /* Centre: system buttons; the touchpad where the controller has one. */
    add_button(&b, VC_BTN_SELECT, ANCHOR_CENTER, -0.15f, 0.08f, 0.13f, 0.07f, 0);
    add_button(&b, VC_BTN_HOME, ANCHOR_CENTER, 0.0f, 0.08f, 0.08f, 0.08f, 1);
    add_button(&b, VC_BTN_START, ANCHOR_CENTER, 0.15f, 0.08f, 0.13f, 0.07f, 0);
    add_button(&b, VC_BTN_MISC, ANCHOR_CENTER, 0.0f, 0.19f, 0.065f, 0.065f, 1);
    if (vc_parser_button_name(type, VC_BTN_TOUCHPAD) != NULL) {
        add(&b, VC_CONTROL_TOUCHPAD, ANCHOR_CENTER, 0.0f, 0.45f, 0.42f, 0.24f, 0);
        add_button(&b, VC_BTN_TOUCHPAD, ANCHOR_CENTER, 0.0f, 0.65f, 0.16f, 0.07f, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Touches                                                                  */
/* ------------------------------------------------------------------------ */

static int find_touch(const vc_layout_t *l, uint64_t id)
{
    int i;

    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES; i++) {
        if (l->touches[i].active && l->touches[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* What a new touch at (x, y) captures; sets the stick origin for it.
 * Returns the control index, or -1 for a free (button) touch. */
static int capture_for(const vc_layout_t *l, float x, float y, vc_layout_touch_t *t)
{
    int i;

    t->origin_x = x;
    t->origin_y = y;

    /* 1. Fixed capturing controls, exactly under the finger. */
    for (i = 0; i < l->count; i++) {
        const vc_control_t *c = &l->controls[i];

        if (c->kind == VC_CONTROL_BUTTON || (c->kind == VC_CONTROL_STICK && c->floating)) {
            continue;
        }
        if (!inside(c, x, y, 0.0f)) {
            continue;
        }
        if ((c->kind == VC_CONTROL_STICK || c->kind == VC_CONTROL_DPAD) && captures(l, i) > 0) {
            continue;   /* already held by another finger */
        }
        if (c->kind == VC_CONTROL_TOUCHPAD && captures(l, i) >= VC_TOUCH_POINTS) {
            continue;
        }
        t->origin_x = c->x;
        t->origin_y = c->y;
        return i;
    }

    /* 2. A button under the finger: stay free so the finger can slide. */
    for (i = 0; i < l->count; i++) {
        if (l->controls[i].kind == VC_CONTROL_BUTTON && inside(&l->controls[i], x, y, l->hit_slop)) {
            return -1;
        }
    }

    /* 3. A floating stick's zone: the stick centres on the finger, kept
     * whole on screen. */
    for (i = 0; i < l->count; i++) {
        const vc_control_t *c = &l->controls[i];
        float               r = c->w * 0.5f;

        if (c->kind == VC_CONTROL_STICK && c->floating && in_zone(c, x, y) && captures(l, i) == 0) {
            t->origin_x = clampf(x, r, l->aspect - r);
            t->origin_y = clampf(y, r, 1.0f - r);
            return i;
        }
    }
    return -1;
}

void vc_layout_touch(vc_layout_t *l, uint64_t id, vc_touch_phase_t phase, float x, float y)
{
    int                index;
    vc_layout_touch_t *t;

    if (l == NULL) {
        return;
    }
    index = find_touch(l, id);

    if (phase == VC_TOUCH_UP || phase == VC_TOUCH_CANCEL) {
        if (index >= 0) {
            l->touches[index].active = 0;
            l->touches[index].control = -1;
        }
        return;
    }
    if (phase == VC_TOUCH_MOVE && index >= 0) {
        l->touches[index].x = x;
        l->touches[index].y = y;
        return;
    }

    /* DOWN, or a MOVE for a touch whose DOWN we missed. */
    if (index < 0) {
        for (index = 0; index < VC_LAYOUT_MAX_TOUCHES; index++) {
            if (!l->touches[index].active) {
                break;
            }
        }
        if (index == VC_LAYOUT_MAX_TOUCHES) {
            return;   /* more fingers than we track */
        }
    }
    t = &l->touches[index];
    t->active = 0;   /* not counted while choosing its capture */
    t->control = -1;
    t->id = id;
    t->x = x;
    t->y = y;
    t->control = capture_for(l, x, y, t);
    t->active = 1;
}

void vc_layout_reset(vc_layout_t *l)
{
    int i;

    if (l == NULL) {
        return;
    }
    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES; i++) {
        l->touches[i].active = 0;
        l->touches[i].control = -1;
    }
}

/* ------------------------------------------------------------------------ */
/* Touches to state                                                         */
/* ------------------------------------------------------------------------ */

static int has_touchpad(const vc_layout_t *l)
{
    int i;

    for (i = 0; i < l->count; i++) {
        if (l->controls[i].kind == VC_CONTROL_TOUCHPAD) {
            return 1;
        }
    }
    return 0;
}

void vc_layout_state(const vc_layout_t *l, vc_input_state_t *state)
{
    uint32_t buttons = 0;
    int      contacts = 0;
    int      i;
    int      j;

    if (l == NULL || state == NULL) {
        return;
    }
    memset(&state->left, 0, sizeof state->left);
    memset(&state->right, 0, sizeof state->right);
    memset(state->touch, 0, sizeof state->touch);
    state->trigger_left = 0;
    state->trigger_right = 0;

    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES; i++) {
        const vc_layout_touch_t *t = &l->touches[i];
        const vc_control_t      *c;
        float                    vx;
        float                    vy;

        if (!t->active) {
            continue;
        }
        if (t->control < 0) {
            /* Free touch: every button under it, slop included, so a finger
             * between two face buttons presses both. */
            for (j = 0; j < l->count; j++) {
                const vc_control_t *b = &l->controls[j];
                if (b->kind == VC_CONTROL_BUTTON && inside(b, t->x, t->y, l->hit_slop)) {
                    buttons |= b->button;
                }
            }
            continue;
        }

        c = &l->controls[t->control];
        switch (c->kind) {
        case VC_CONTROL_STICK:
            stick_value(c, t->x - t->origin_x, t->y - t->origin_y, &vx, &vy);
            /* Screen y grows down; the wire's +y is up. */
            if (c->side == 0) {
                state->left.x = to_axis(vx);
                state->left.y = to_axis(-vy);
            } else {
                state->right.x = to_axis(vx);
                state->right.y = to_axis(-vy);
            }
            break;
        case VC_CONTROL_DPAD:
            buttons |= dpad_buttons(c, t->x - c->x, t->y - c->y);
            break;
        case VC_CONTROL_TRIGGER:
            if (c->side == 0) {
                state->trigger_left = VC_TRIGGER_MAX;
            } else {
                state->trigger_right = VC_TRIGGER_MAX;
            }
            break;
        case VC_CONTROL_TOUCHPAD:
            if (contacts < VC_TOUCH_POINTS) {
                vc_touch_point_t *p = &state->touch[contacts++];
                p->active = 1;
                p->id = (uint8_t)(i & VC_TOUCH_STATE_ID_MASK);   /* stable while down */
                p->x = to_unit16((t->x - (c->x - c->w * 0.5f)) / c->w);
                p->y = to_unit16((t->y - (c->y - c->h * 0.5f)) / c->h);
            }
            break;
        default:
            break;
        }
    }

    state->buttons = buttons;
    if (has_touchpad(l)) {
        state->flags |= VC_INPUT_HAS_TOUCH;   /* "no contacts" is news too */
    } else {
        state->flags &= (uint8_t)~VC_INPUT_HAS_TOUCH;
    }
}

void vc_layout_visual(const vc_layout_t *l, int index, vc_control_visual_t *out)
{
    const vc_control_t *c;
    int                 i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (l == NULL || index < 0 || index >= l->count) {
        return;
    }
    c = &l->controls[index];
    out->base_x = c->x;
    out->base_y = c->y;

    for (i = 0; i < VC_LAYOUT_MAX_TOUCHES; i++) {
        const vc_layout_touch_t *t = &l->touches[i];

        if (!t->active) {
            continue;
        }
        if (t->control == index) {
            out->pressed = 1;
            if (c->kind == VC_CONTROL_STICK || c->kind == VC_CONTROL_DPAD) {
                float r = c->w * 0.5f;
                float dx;
                float dy;
                float magnitude;

                if (c->kind == VC_CONTROL_STICK) {
                    out->base_x = t->origin_x;
                    out->base_y = t->origin_y;
                }
                dx = (t->x - out->base_x) / r;
                dy = (t->y - out->base_y) / r;
                magnitude = sqrtf(dx * dx + dy * dy);
                if (magnitude > 1.0f) {
                    dx /= magnitude;
                    dy /= magnitude;
                }
                out->knob_x = dx;
                out->knob_y = dy;
            }
        } else if (t->control < 0 && c->kind == VC_CONTROL_BUTTON &&
                   inside(c, t->x, t->y, l->hit_slop)) {
            out->pressed = 1;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Labels                                                                   */
/* ------------------------------------------------------------------------ */

/* Glyphs for profile names that are words on the box but symbols on the
 * controller. Everything else is drawn as the profile name. */
static const struct {
    const char *name;
    const char *glyph;
} glyphs[] = {
    { "Cross",    "✕" },
    { "Circle",   "○" },
    { "Square",   "□" },
    { "Triangle", "△" },
    { "Plus",     "+" },
    { "Minus",    "−" },
    { "Touchpad", "Pad" }
};

/* Triggers are not button bits, so the profiles have no names for them. */
static const char *trigger_label(vc_controller_type_t type, int side)
{
    if (type == VC_CONTROLLER_SWITCH) {
        return side == 0 ? "ZL" : "ZR";
    }
    if (type == VC_CONTROLLER_PS4 || type == VC_CONTROLLER_PS5) {
        return side == 0 ? "L2" : "R2";
    }
    return side == 0 ? "LT" : "RT";
}

const char *vc_layout_name(const vc_layout_t *l, int index)
{
    const vc_control_t *c;
    const char         *name;

    if (l == NULL || index < 0 || index >= l->count) {
        return "";
    }
    c = &l->controls[index];
    switch (c->kind) {
    case VC_CONTROL_BUTTON:
        name = vc_parser_button_name(l->type, c->button);
        return name != NULL ? name : "";
    case VC_CONTROL_TRIGGER:
        return c->side == 0 ? "Left trigger" : "Right trigger";
    case VC_CONTROL_STICK:
        return c->side == 0 ? "Left stick" : "Right stick";
    case VC_CONTROL_DPAD:
        return "D-pad";
    case VC_CONTROL_TOUCHPAD:
        return "Touchpad";
    }
    return "";
}

const char *vc_layout_label(const vc_layout_t *l, int index)
{
    const vc_control_t *c;
    const char         *name;
    size_t              i;

    if (l == NULL || index < 0 || index >= l->count) {
        return "";
    }
    c = &l->controls[index];
    if (c->kind == VC_CONTROL_TRIGGER) {
        return trigger_label(l->type, c->side);
    }
    if (c->kind != VC_CONTROL_BUTTON) {
        return "";
    }
    name = vc_layout_name(l, index);
    for (i = 0; i < ARRAY_LEN(glyphs); i++) {
        if (strcmp(name, glyphs[i].name) == 0) {
            return glyphs[i].glyph;
        }
    }
    return name;
}

uint32_t vc_layout_color(const vc_layout_t *l, int index)
{
    /* SOUTH, EAST, WEST, NORTH */
    static const uint32_t xbox[4] = { 0x5DB33Bu, 0xE0383Eu, 0x2B7FD9u, 0xF2B71Fu };
    static const uint32_t playstation[4] = { 0x7AA7D9u, 0xE36C6Cu, 0xD58EC4u, 0x4FB89Eu };
    const uint32_t       *palette;
    uint32_t              button;
    int                   bit;

    if (l == NULL || index < 0 || index >= l->count ||
        l->controls[index].kind != VC_CONTROL_BUTTON) {
        return 0;
    }
    switch (l->type) {
    case VC_CONTROLLER_XBOX: palette = xbox; break;
    case VC_CONTROLLER_PS4:
    case VC_CONTROLLER_PS5:  palette = playstation; break;
    default:                 return 0;   /* Switch face buttons are plain */
    }
    button = l->controls[index].button;
    for (bit = 0; bit < 4; bit++) {
        if (button == (UINT32_C(1) << bit)) {
            return palette[bit];
        }
    }
    return 0;
}
