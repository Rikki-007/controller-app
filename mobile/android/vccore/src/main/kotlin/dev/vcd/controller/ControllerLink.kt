package dev.vcd.controller

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import kotlin.math.roundToInt

/**
 * One controller's link to vcd.
 *
 * The protocol work (TCP handshake, keepalive, the UDP send loop, the wire
 * format) happens in the shared C engine, src/vc_client.c, the same code the
 * iOS app runs and the desktop tests exercise. This class adds what is
 * specific to Android:
 *
 * - **The sender thread.** A dedicated thread at
 *   THREAD_PRIORITY_URGENT_DISPLAY runs the engine for the whole session.
 *   It never touches the main thread, so a busy UI cannot delay input.
 * - **A low-latency Wi-Fi lock** (WIFI_MODE_FULL_LOW_LATENCY on Android 10+)
 *   while connected, which turns off Wi-Fi power save: without it, radio
 *   sleep adds tens of milliseconds of jitter.
 * - **A non-blocking input API.** [setState] and friends hand the state to
 *   the sender through a lock-free triple buffer in native code. A change is
 *   on the wire within 2 ms; the current state is re-sent at [Config.rateHz]
 *   regardless.
 * - **Events on the main thread**, through [Listener].
 *
 * Use it from the main thread only. Stop it in onPause/onStop: Android
 * restricts network access for background apps.
 */
class ControllerLink(context: Context, val config: Config, private val listener: Listener) {

    data class Config(
        /** vcd's dotted IPv4 address. */
        val host: String,
        val port: Int = DEFAULT_PORT,
        val type: ControllerType,
        /** [Capabilities] bits this phone can provide. */
        val capabilities: Int = 0,
        /** Shown in vcd's log; at most 32 bytes are sent. */
        val name: String = Build.MODEL,
        val rateHz: Int = 120,
    )

    /** Why a session ended; mirrors vc_client_end_t. */
    enum class End { STOPPED, CONNECT_FAILED, REFUSED, HOST_ERROR, HOST_BYE, CONNECTION_LOST, PROTOCOL, TIMEOUT, SYSTEM }

    /** Called on the main thread. */
    interface Listener {
        fun onConnected(slot: Int, grantedCapabilities: Int) {}
        fun onFeedback(rumbleLow: Int, rumbleHigh: Int, red: Int, green: Int, blue: Int, player: Int) {}

        /** Always the last call for a [start]. [hostError] is vc_error_code_t or 0. */
        fun onClosed(end: End, hostError: Int, message: String)
    }

    data class Statistics(val datagramsSent: Int, val sendErrors: Int, val roundTripMicros: Int)

    private val main = Handler(Looper.getMainLooper())
    private val wifiLock: WifiManager.WifiLock? = createWifiLock(context)

    // Main thread only.
    private var handle = 0L
    private var thread: Thread? = null

    val isRunning: Boolean get() = thread != null

    /** Native handle for [GamepadView]; 0 while not running. Main thread. */
    internal val nativeHandle: Long get() = handle

    /** Connects and starts sending. Returns at once; onConnected or onClosed follows. */
    fun start() {
        checkMainThread()
        if (thread != null) return
        val h = NativeCore.create(
            config.host, config.port, config.type.wire, config.capabilities,
            config.name.take(32), config.rateHz,
        )
        require(h != 0L) { "not a dotted IPv4 address: ${config.host}" }
        handle = h
        wifiLock?.acquire()
        thread = Thread({ senderMain(h) }, "vcd-sender").also { it.start() }
    }

    /** Sends BYE and ends the session; onClosed(STOPPED) follows. */
    fun stop() {
        checkMainThread()
        if (handle != 0L) NativeCore.stop(handle)
    }

    // ---- Input (main thread). Ignored while not running. ----------------------

    /** Buttons ([Buttons] bits), sticks in -1..1 with +y up, triggers in 0..1. */
    fun setState(
        buttons: Int,
        leftX: Float, leftY: Float,
        rightX: Float, rightY: Float,
        leftTrigger: Float, rightTrigger: Float,
    ) {
        checkMainThread()
        val h = handle
        if (h == 0L) return
        NativeCore.setState(
            h, buttons,
            axis(leftX), axis(leftY), axis(rightX), axis(rightY),
            trigger(leftTrigger), trigger(rightTrigger),
        )
    }

    /**
     * Gyro in degrees per second and acceleration in g, controller axes
     * (+x right, +y up, +z toward the player). Null [gyroDps] stops sending
     * motion. Register the sensor listener on the main looper: input has a
     * single writer.
     */
    fun setMotion(gyroDps: FloatArray?, accelG: FloatArray) {
        checkMainThread()
        val h = handle
        if (h == 0L) return
        if (gyroDps == null) {
            NativeCore.setMotion(h, false, 0, 0, 0, 0, 0, 0)
            return
        }
        NativeCore.setMotion(
            h, true,
            gyro(gyroDps[0]), gyro(gyroDps[1]), gyro(gyroDps[2]),
            accel(accelG[0]), accel(accelG[1]), accel(accelG[2]),
        )
    }

    /** Touchpad contact [index] (0 or 1); x and y in 0..1, origin top-left. */
    fun setTouch(index: Int, active: Boolean, id: Int, x: Float, y: Float) {
        checkMainThread()
        val h = handle
        if (h == 0L) return
        NativeCore.setTouch(h, index, active, id, touch(x), touch(y))
    }

    val statistics: Statistics?
        get() {
            checkMainThread()
            val h = handle
            if (h == 0L) return null
            val out = IntArray(3)
            NativeCore.stats(h, out)
            return Statistics(out[0], out[1], out[2])
        }

    // ---- Sender thread ----------------------------------------------------------

    private fun senderMain(h: Long) {
        raisePriority()
        NativeCore.run(h, sink)   // blocks for the whole session
        main.post { finished(h) } // after the onClosed post, so after onClosed
    }

    /** Native events, on the sender thread: hop to the main thread. */
    private val sink = object : NativeCore.EventSink {
        override fun onConnected(slot: Int, caps: Int, timeoutMs: Int) {
            main.post { listener.onConnected(slot, caps) }
        }

        override fun onFeedback(rumbleLow: Int, rumbleHigh: Int, red: Int, green: Int, blue: Int, player: Int) {
            main.post { listener.onFeedback(rumbleLow, rumbleHigh, red, green, blue, player) }
        }

        override fun onClosed(end: Int, error: Int, message: String) {
            val reason = End.entries.getOrElse(end) { End.SYSTEM }
            main.post { listener.onClosed(reason, error, message) }
        }
    }

    private fun finished(h: Long) {
        if (handle == h) handle = 0L
        NativeCore.destroy(h)
        thread = null
        wifiLock?.let { if (it.isHeld) it.release() }
    }

    private companion object {
        const val TAG = "vcd.ControllerLink"

        fun checkMainThread() {
            check(Looper.myLooper() == Looper.getMainLooper()) { "ControllerLink is main-thread only" }
        }

        /** URGENT_DISPLAY is what the UI's render thread gets; fall back if refused. */
        fun raisePriority() {
            for (priority in intArrayOf(Process.THREAD_PRIORITY_URGENT_DISPLAY, Process.THREAD_PRIORITY_DISPLAY)) {
                try {
                    Process.setThreadPriority(priority)
                    return
                } catch (e: SecurityException) {
                    Log.w(TAG, "thread priority $priority refused", e)
                } catch (e: IllegalArgumentException) {
                    Log.w(TAG, "thread priority $priority refused", e)
                }
            }
        }

        @Suppress("DEPRECATION")   // WIFI_MODE_FULL_HIGH_PERF, for Android 8-9
        fun createWifiLock(context: Context): WifiManager.WifiLock? {
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                ?: return null
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            return wifi.createWifiLock(mode, "vcd:controller").apply { setReferenceCounted(false) }
        }

        // Unit conversion; see include/controller.h.
        fun axis(v: Float): Short = (v.coerceIn(-1f, 1f) * 32767f).roundToInt().toShort()
        fun trigger(v: Float): Int = (v.coerceIn(0f, 1f) * 255f).roundToInt()
        fun gyro(dps: Float): Short = (dps.coerceIn(-2000f, 2000f) * (32767f / 2000f)).roundToInt().toShort()
        fun accel(g: Float): Short = (g.coerceIn(-7.99f, 7.99f) * 4096f).roundToInt().toShort()
        fun touch(v: Float): Int = (v.coerceIn(0f, 1f) * 65535f).roundToInt()
    }
}
