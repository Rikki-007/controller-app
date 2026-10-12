package dev.vcd.controller

/**
 * JNI surface of libvccore (src/main/cpp/vccore_jni.c), which wraps the
 * shared C engine in include/vc_client.h. Internal: apps use [ControllerLink].
 *
 * Every method takes primitives only, so the per-touch calls cost no
 * allocation. A handle is a native pointer: never use it after [destroy].
 */
internal object NativeCore {
    init {
        System.loadLibrary("vccore")
    }

    /**
     * Called from native code on the sender thread, inside [run]. The C side
     * looks these methods up by name and signature, so consumer-rules.pro
     * keeps them from being renamed.
     */
    interface EventSink {
        fun onConnected(slot: Int, caps: Int, timeoutMs: Int)
        fun onFeedback(rumbleLow: Int, rumbleHigh: Int, red: Int, green: Int, blue: Int, player: Int)
        fun onClosed(end: Int, error: Int, message: String)
    }

    /** Returns 0 if [host] is not a dotted IPv4 address. */
    @JvmStatic external fun create(host: String, port: Int, type: Int, caps: Int, name: String, rateHz: Int): Long

    /** Blocks for the whole session; returns a vc_client_end_t. Sender thread only. */
    @JvmStatic external fun run(handle: Long, sink: EventSink): Int

    // Input: one thread (the main thread) only. Never block.
    @JvmStatic external fun setState(handle: Long, buttons: Int, lx: Short, ly: Short, rx: Short, ry: Short, lt: Int, rt: Int)
    @JvmStatic external fun setMotion(handle: Long, enabled: Boolean, gx: Short, gy: Short, gz: Short, ax: Short, ay: Short, az: Short)
    @JvmStatic external fun setTouch(handle: Long, index: Int, active: Boolean, id: Int, x: Int, y: Int)

    /** Any thread. */
    @JvmStatic external fun stop(handle: Long)

    /** Fills [out] with sent, send errors, round trip in microseconds. */
    @JvmStatic external fun stats(handle: Long, out: IntArray)

    /** After [run] has returned. */
    @JvmStatic external fun destroy(handle: Long)

    // ---- On-screen layout (include/layout.h); main thread only. -------------

    const val GEOMETRY_STRIDE = 8   // kind, x, y, w, h, round, side, floating
    const val VISUAL_STRIDE = 5     // pressed, baseX, baseY, knobX, knobY

    @JvmStatic external fun layoutCreate(type: Int, aspect: Float): Long
    @JvmStatic external fun layoutDestroy(layout: Long)
    @JvmStatic external fun layoutSetStick(layout: Long, side: Int, deadzone: Float, saturation: Float)

    /** [phase]: 0 down, 1 move, 2 up, 3 cancel (vc_touch_phase_t). */
    @JvmStatic external fun layoutTouch(layout: Long, id: Long, phase: Int, x: Float, y: Float)
    @JvmStatic external fun layoutReset(layout: Long)

    /** Computes the state from the touches and publishes it on [link] (a link handle). */
    @JvmStatic external fun layoutApply(layout: Long, link: Long)

    @JvmStatic external fun layoutCount(layout: Long): Int
    @JvmStatic external fun layoutGeometry(layout: Long, out: FloatArray)
    @JvmStatic external fun layoutVisuals(layout: Long, out: FloatArray)
    @JvmStatic external fun layoutLabel(layout: Long, index: Int): String
    @JvmStatic external fun layoutName(layout: Long, index: Int): String
    @JvmStatic external fun layoutColor(layout: Long, index: Int): Int
}
