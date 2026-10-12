/*
 * vccore_jni.c - JNI bridge between NativeCore.kt and the shared C engine
 * (include/vc_client.h).
 *
 * Per-touch calls (setState, setMotion, setTouch) take primitives only: no
 * arrays, no objects, no allocation, so a call costs well under a
 * microsecond. They edit a working copy owned by the UI thread and publish
 * it through the engine's lock-free handoff; they never block.
 *
 * Events come back on the sender thread, inside NativeCore.run(), whose
 * JNIEnv and sink object are valid for exactly that long.
 */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "vc_client.h"

/* Floats per control in layoutGeometry() and layoutVisuals(); GamepadLayout.kt
 * reads them with the same strides. */
#define GEOMETRY_STRIDE 8   /* kind, x, y, w, h, round, side, floating */
#define VISUAL_STRIDE   5   /* pressed, base_x, base_y, knob_x, knob_y */

typedef struct {
    vc_client_t     *client;
    vc_input_state_t state;          /* UI thread's working copy */
    /* Sender thread, valid only while run() is executing. */
    JNIEnv          *env;
    jobject          sink;
    jmethodID        on_connected;
    jmethodID        on_feedback;
    jmethodID        on_closed;
} link_t;

static link_t *link_from(jlong handle)
{
    return (link_t *)(intptr_t)handle;
}

static uint8_t clamp_u8(jint v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static uint16_t clamp_u16(jint v)
{
    return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

/* Engine callback, on the sender thread. */
static void on_event(const vc_client_event_t *event, void *user)
{
    link_t *link = (link_t *)user;
    JNIEnv *env = link->env;

    if (env == NULL || link->sink == NULL) {
        return;
    }
    switch (event->type) {
    case VC_CLIENT_EVENT_CONNECTED:
        (*env)->CallVoidMethod(env, link->sink, link->on_connected, (jint)event->slot,
                               (jint)event->caps, (jint)event->timeout_ms);
        break;
    case VC_CLIENT_EVENT_FEEDBACK:
        (*env)->CallVoidMethod(env, link->sink, link->on_feedback,
                               (jint)event->feedback.rumble_low, (jint)event->feedback.rumble_high,
                               (jint)event->feedback.led[0], (jint)event->feedback.led[1],
                               (jint)event->feedback.led[2], (jint)event->feedback.player);
        break;
    case VC_CLIENT_EVENT_CLOSED: {
        jstring message = (*env)->NewStringUTF(env, event->message != NULL ? event->message : "");
        if (message != NULL) {
            (*env)->CallVoidMethod(env, link->sink, link->on_closed, (jint)event->end,
                                   (jint)event->error, message);
            (*env)->DeleteLocalRef(env, message);
        }
        break;
    }
    }
    /* A throwing listener must not take the send loop down with it. */
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

JNIEXPORT jlong JNICALL
Java_dev_vcd_controller_NativeCore_create(JNIEnv *env, jclass cls, jstring host, jint port,
                                          jint type, jint caps, jstring name, jint rate_hz)
{
    vc_client_config_t config;
    link_t            *link;
    const char        *host_chars;
    const char        *name_chars;

    (void)cls;
    link = (link_t *)calloc(1, sizeof *link);
    if (link == NULL) {
        return 0;
    }
    host_chars = (*env)->GetStringUTFChars(env, host, NULL);
    name_chars = (*env)->GetStringUTFChars(env, name, NULL);
    if (host_chars != NULL && name_chars != NULL) {
        memset(&config, 0, sizeof config);
        config.host = host_chars;
        config.port = clamp_u16(port);
        config.controller_type = clamp_u8(type);
        config.caps = clamp_u8(caps);
        config.name = name_chars;
        config.rate_hz = rate_hz > 0 ? (unsigned)rate_hz : 0u;
        config.on_event = on_event;
        config.user = link;
        link->client = vc_client_create(&config);   /* copies the strings */
    }
    if (host_chars != NULL) {
        (*env)->ReleaseStringUTFChars(env, host, host_chars);
    }
    if (name_chars != NULL) {
        (*env)->ReleaseStringUTFChars(env, name, name_chars);
    }
    if (link->client == NULL) {
        free(link);
        return 0;
    }
    return (jlong)(intptr_t)link;
}

JNIEXPORT jint JNICALL
Java_dev_vcd_controller_NativeCore_run(JNIEnv *env, jclass cls, jlong handle, jobject sink)
{
    link_t         *link = link_from(handle);
    jclass          sink_class;
    vc_client_end_t end;

    (void)cls;
    if (link == NULL || sink == NULL) {
        return (jint)VC_CLIENT_END_SYSTEM;
    }
    sink_class = (*env)->GetObjectClass(env, sink);
    link->on_connected = (*env)->GetMethodID(env, sink_class, "onConnected", "(III)V");
    link->on_feedback = (*env)->GetMethodID(env, sink_class, "onFeedback", "(IIIIII)V");
    link->on_closed = (*env)->GetMethodID(env, sink_class, "onClosed", "(IILjava/lang/String;)V");
    (*env)->DeleteLocalRef(env, sink_class);
    if (link->on_connected == NULL || link->on_feedback == NULL || link->on_closed == NULL) {
        /* NoSuchMethodError is pending and surfaces in Kotlin: R8 renamed
         * the sink (see consumer-rules.pro) or the signatures drifted. */
        return (jint)VC_CLIENT_END_SYSTEM;
    }

    link->env = env;
    link->sink = sink;            /* a local ref, valid for this call */
    end = vc_client_run(link->client);
    link->env = NULL;
    link->sink = NULL;
    return (jint)end;
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_setState(JNIEnv *env, jclass cls, jlong handle, jint buttons,
                                            jshort lx, jshort ly, jshort rx, jshort ry,
                                            jint lt, jint rt)
{
    link_t *link = link_from(handle);

    (void)env;
    (void)cls;
    if (link == NULL) {
        return;
    }
    link->state.buttons = (uint32_t)buttons & VC_BTN_VALID_MASK;
    link->state.left.x = lx;
    link->state.left.y = ly;
    link->state.right.x = rx;
    link->state.right.y = ry;
    link->state.trigger_left = clamp_u8(lt);
    link->state.trigger_right = clamp_u8(rt);
    vc_client_set_state(link->client, &link->state);
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_setMotion(JNIEnv *env, jclass cls, jlong handle,
                                             jboolean enabled, jshort gx, jshort gy, jshort gz,
                                             jshort ax, jshort ay, jshort az)
{
    link_t *link = link_from(handle);

    (void)env;
    (void)cls;
    if (link == NULL) {
        return;
    }
    if (enabled) {
        link->state.flags |= VC_INPUT_HAS_MOTION;
        link->state.motion.gyro[0] = gx;
        link->state.motion.gyro[1] = gy;
        link->state.motion.gyro[2] = gz;
        link->state.motion.accel[0] = ax;
        link->state.motion.accel[1] = ay;
        link->state.motion.accel[2] = az;
    } else {
        link->state.flags &= (uint8_t)~VC_INPUT_HAS_MOTION;
    }
    vc_client_set_state(link->client, &link->state);
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_setTouch(JNIEnv *env, jclass cls, jlong handle, jint index,
                                            jboolean active, jint id, jint x, jint y)
{
    link_t           *link = link_from(handle);
    vc_touch_point_t *point;

    (void)env;
    (void)cls;
    if (link == NULL || index < 0 || index >= VC_TOUCH_POINTS) {
        return;
    }
    point = &link->state.touch[index];
    point->active = active ? 1 : 0;
    point->id = (uint8_t)(id & VC_TOUCH_STATE_ID_MASK);
    point->x = clamp_u16(x);
    point->y = clamp_u16(y);
    link->state.flags |= VC_INPUT_HAS_TOUCH;
    vc_client_set_state(link->client, &link->state);
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_stop(JNIEnv *env, jclass cls, jlong handle)
{
    link_t *link = link_from(handle);

    (void)env;
    (void)cls;
    if (link != NULL) {
        vc_client_stop(link->client);
    }
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_stats(JNIEnv *env, jclass cls, jlong handle, jintArray out)
{
    link_t           *link = link_from(handle);
    vc_client_stats_t stats;
    jint              values[3];

    (void)cls;
    if (link == NULL || out == NULL || (*env)->GetArrayLength(env, out) < 3) {
        return;
    }
    vc_client_get_stats(link->client, &stats);
    values[0] = (jint)stats.sent;
    values[1] = (jint)stats.send_errors;
    values[2] = (jint)stats.rtt_us;
    (*env)->SetIntArrayRegion(env, out, 0, 3, values);
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_destroy(JNIEnv *env, jclass cls, jlong handle)
{
    link_t *link = link_from(handle);

    (void)env;
    (void)cls;
    if (link != NULL) {
        vc_client_destroy(link->client);
        free(link);
    }
}

/* ------------------------------------------------------------------------ */
/* On-screen layout (include/layout.h)                                      */
/* ------------------------------------------------------------------------ */

static vc_layout_t *layout_from(jlong handle)
{
    return (vc_layout_t *)(intptr_t)handle;
}

JNIEXPORT jlong JNICALL
Java_dev_vcd_controller_NativeCore_layoutCreate(JNIEnv *env, jclass cls, jint type, jfloat aspect)
{
    vc_layout_t *layout = (vc_layout_t *)malloc(sizeof *layout);

    (void)env;
    (void)cls;
    if (layout == NULL) {
        return 0;
    }
    vc_layout_init_default(layout, (vc_controller_type_t)type, aspect);
    return (jlong)(intptr_t)layout;
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutDestroy(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env;
    (void)cls;
    free(layout_from(handle));
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutSetStick(JNIEnv *env, jclass cls, jlong handle,
                                                  jint side, jfloat deadzone, jfloat saturation)
{
    (void)env;
    (void)cls;
    vc_layout_set_stick(layout_from(handle), side, deadzone, saturation);
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutTouch(JNIEnv *env, jclass cls, jlong handle, jlong id,
                                               jint phase, jfloat x, jfloat y)
{
    (void)env;
    (void)cls;
    if (phase < VC_TOUCH_DOWN || phase > VC_TOUCH_CANCEL) {
        return;
    }
    vc_layout_touch(layout_from(handle), (uint64_t)id, (vc_touch_phase_t)phase, x, y);
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutReset(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env;
    (void)cls;
    vc_layout_reset(layout_from(handle));
}

/* Touches -> controller state -> the link's sender, without the state ever
 * crossing into Kotlin. Motion set through setMotion() is kept. */
JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutApply(JNIEnv *env, jclass cls, jlong layout_handle,
                                               jlong link_handle)
{
    vc_layout_t *layout = layout_from(layout_handle);
    link_t      *link = link_from(link_handle);

    (void)env;
    (void)cls;
    if (layout == NULL || link == NULL) {
        return;
    }
    vc_layout_state(layout, &link->state);
    vc_client_set_state(link->client, &link->state);
}

JNIEXPORT jint JNICALL
Java_dev_vcd_controller_NativeCore_layoutCount(JNIEnv *env, jclass cls, jlong handle)
{
    vc_layout_t *layout = layout_from(handle);

    (void)env;
    (void)cls;
    return layout != NULL ? layout->count : 0;
}

JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutGeometry(JNIEnv *env, jclass cls, jlong handle,
                                                  jfloatArray out)
{
    vc_layout_t *layout = layout_from(handle);
    jfloat       values[VC_LAYOUT_MAX_CONTROLS * GEOMETRY_STRIDE];
    int          i;

    (void)cls;
    if (layout == NULL || out == NULL ||
        (*env)->GetArrayLength(env, out) < layout->count * GEOMETRY_STRIDE) {
        return;
    }
    for (i = 0; i < layout->count; i++) {
        const vc_control_t *c = &layout->controls[i];
        jfloat             *v = values + i * GEOMETRY_STRIDE;

        v[0] = (jfloat)c->kind;
        v[1] = c->x;
        v[2] = c->y;
        v[3] = c->w;
        v[4] = c->h;
        v[5] = (jfloat)c->round;
        v[6] = (jfloat)c->side;
        v[7] = (jfloat)c->floating;
    }
    (*env)->SetFloatArrayRegion(env, out, 0, layout->count * GEOMETRY_STRIDE, values);
}

/* Called every drawn frame: one array copy, no allocation. */
JNIEXPORT void JNICALL
Java_dev_vcd_controller_NativeCore_layoutVisuals(JNIEnv *env, jclass cls, jlong handle,
                                                 jfloatArray out)
{
    vc_layout_t        *layout = layout_from(handle);
    jfloat              values[VC_LAYOUT_MAX_CONTROLS * VISUAL_STRIDE];
    vc_control_visual_t visual;
    int                 i;

    (void)cls;
    if (layout == NULL || out == NULL ||
        (*env)->GetArrayLength(env, out) < layout->count * VISUAL_STRIDE) {
        return;
    }
    for (i = 0; i < layout->count; i++) {
        jfloat *v = values + i * VISUAL_STRIDE;

        vc_layout_visual(layout, i, &visual);
        v[0] = (jfloat)visual.pressed;
        v[1] = visual.base_x;
        v[2] = visual.base_y;
        v[3] = visual.knob_x;
        v[4] = visual.knob_y;
    }
    (*env)->SetFloatArrayRegion(env, out, 0, layout->count * VISUAL_STRIDE, values);
}

JNIEXPORT jstring JNICALL
Java_dev_vcd_controller_NativeCore_layoutLabel(JNIEnv *env, jclass cls, jlong handle, jint index)
{
    (void)cls;
    /* Labels are ASCII or BMP symbols, where modified UTF-8 is plain UTF-8. */
    return (*env)->NewStringUTF(env, vc_layout_label(layout_from(handle), index));
}

JNIEXPORT jstring JNICALL
Java_dev_vcd_controller_NativeCore_layoutName(JNIEnv *env, jclass cls, jlong handle, jint index)
{
    (void)cls;
    return (*env)->NewStringUTF(env, vc_layout_name(layout_from(handle), index));
}

JNIEXPORT jint JNICALL
Java_dev_vcd_controller_NativeCore_layoutColor(JNIEnv *env, jclass cls, jlong handle, jint index)
{
    (void)env;
    (void)cls;
    return (jint)vc_layout_color(layout_from(handle), index);
}
