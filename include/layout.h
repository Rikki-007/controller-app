/*
 * layout.h - the on-screen gamepad: control geometry, touch tracking, and
 * the mapping from touches to a vc_input_state_t.
 *
 * Shared by the iOS and Android apps, so both have the same hit-testing,
 * deadzones and floating stick; the platform views only forward raw touches
 * and draw what vc_layout_visual() reports.
 *
 * Coordinates are layout units: 1.0 is the view's height, so circles stay
 * round. x runs 0..aspect (width / height), y runs 0..1 downwards, as on
 * screen. Convert a touch with x = px / height, y = py / height.
 *
 * Touch rules:
 *   - A touch that lands on a stick, the D-pad, a trigger or the touchpad
 *     is captured by it until it lifts, wherever it slides.
 *   - A touch that lands anywhere else presses whatever button is under it
 *     now, so a finger can slide or rock between face buttons.
 *   - A floating stick has a zone: a touch landing in the zone (and on no
 *     other control) becomes the stick's centre, so the thumb never has to
 *     find a fixed spot.
 *
 * Labels and colours come from the protocol's controller profiles
 * (vc_parser_button_name), so SOUTH reads A on Xbox, Cross on PlayStation
 * and B on Switch, and buttons a controller lacks are not laid out.
 *
 * No allocation, no OS calls. C99.
 */
#ifndef VC_LAYOUT_H
#define VC_LAYOUT_H

#include <stdint.h>

#include "controller.h"

#define VC_LAYOUT_MAX_CONTROLS 24
#define VC_LAYOUT_MAX_TOUCHES  10

typedef enum {
    VC_CONTROL_BUTTON = 1,   /* one VC_BTN_* bit while pressed */
    VC_CONTROL_TRIGGER,      /* left or right trigger, full while held */
    VC_CONTROL_DPAD,         /* 8-way from the finger's direction */
    VC_CONTROL_STICK,        /* analog, radial deadzone */
    VC_CONTROL_TOUCHPAD      /* up to two contacts as touchpad data */
} vc_control_kind_t;

typedef struct {
    vc_control_kind_t kind;
    float    x, y;           /* centre */
    float    w, h;           /* size; a round control uses w as its diameter */
    int      round;
    uint32_t button;         /* BUTTON: one VC_BTN_* bit */
    int      side;           /* TRIGGER, STICK: 0 left, 1 right */
    /* STICK and DPAD: fraction of the radius around the centre that reads
     * as neutral (0..0.9). */
    float    deadzone;
    /* STICK: fraction of the radius that already reads as full deflection
     * (deadzone..1). Output is rescaled between the two, so it still starts
     * at 0 just outside the deadzone. */
    float    saturation;
    /* STICK: centre on the finger, anywhere inside the zone. */
    int      floating;
    float    zone_x0, zone_y0, zone_x1, zone_y1;
} vc_control_t;

typedef enum {
    VC_TOUCH_DOWN = 0,
    VC_TOUCH_MOVE,
    VC_TOUCH_UP,
    VC_TOUCH_CANCEL
} vc_touch_phase_t;

/* Caller-allocated; treat as private and use the functions. */
typedef struct {
    int      active;
    uint64_t id;
    float    x, y;
    int      control;          /* captured control, or -1 */
    float    origin_x;         /* STICK: centre for this touch */
    float    origin_y;
} vc_layout_touch_t;

typedef struct {
    vc_controller_type_t type;
    float                aspect;
    float                hit_slop;     /* buttons: extra hit radius, fraction of size */
    int                  count;
    vc_control_t         controls[VC_LAYOUT_MAX_CONTROLS];
    vc_layout_touch_t    touches[VC_LAYOUT_MAX_TOUCHES];
} vc_layout_t;

/* The standard landscape layout for `type` on a view of the given aspect
 * (width / height): triggers and shoulders at the top corners, a floating
 * left stick and the D-pad on the left, face buttons and a fixed right stick
 * on the right, system buttons (and the touchpad, for PlayStation) in the
 * middle. Tuned for phones (aspect ~2); narrower views get it scaled down.
 * Buttons the type's profile does not have are left out. */
void vc_layout_init_default(vc_layout_t *layout, vc_controller_type_t type, float aspect);

/* An empty layout, for custom arrangements built with vc_layout_add(). */
void vc_layout_init_empty(vc_layout_t *layout, vc_controller_type_t type, float aspect);

/* Appends a control. Returns its index, or -1 if the layout is full. */
int vc_layout_add(vc_layout_t *layout, const vc_control_t *control);

/* Control `index` (0..count-1), or NULL. For bindings that cannot index
 * the controls array directly (Swift sees it as a tuple). */
const vc_control_t *vc_layout_control(const vc_layout_t *layout, int index);

/* Sets the stick deadzones and saturation of every stick (side 0 = left). */
void vc_layout_set_stick(vc_layout_t *layout, int side, float deadzone, float saturation);

/* Feeds one touch event. `id` is any value stable for the touch's lifetime
 * (UITouch identity, MotionEvent pointer id). */
void vc_layout_touch(vc_layout_t *layout, uint64_t id, vc_touch_phase_t phase,
                     float x, float y);

/* Lifts every touch, e.g. when the view goes away or is resized. */
void vc_layout_reset(vc_layout_t *layout);

/* Writes the controller state the current touches produce: buttons, sticks,
 * triggers and touchpad contacts (setting or clearing VC_INPUT_HAS_TOUCH).
 * Motion fields and the motion flag are left as they are, so sensor data
 * merges in. */
void vc_layout_state(const vc_layout_t *layout, vc_input_state_t *state);

/* What to draw for control `index`. */
typedef struct {
    int   pressed;
    float base_x, base_y;      /* where the control is drawn now (a floating stick moves) */
    float knob_x, knob_y;      /* STICK, DPAD: finger offset, -1..1 of the radius, y down */
} vc_control_visual_t;

void vc_layout_visual(const vc_layout_t *layout, int index, vc_control_visual_t *out);

/* Short UTF-8 label to draw on control `index` for the layout's controller
 * type: "A", "✕" (Cross), "ZL", "+"... Empty for sticks and the D-pad. */
const char *vc_layout_label(const vc_layout_t *layout, int index);

/* Full name for accessibility: "Cross", "Left trigger", "D-pad"... */
const char *vc_layout_name(const vc_layout_t *layout, int index);

/* Accent colour 0xRRGGBB for the face buttons of controllers that have one
 * (Xbox, PlayStation), else 0. */
uint32_t vc_layout_color(const vc_layout_t *layout, int index);

#endif /* VC_LAYOUT_H */
