package dev.vcd.controller

import android.app.Activity
import android.os.Build
import android.os.PowerManager
import android.view.Display
import android.view.WindowManager

/**
 * Window-level settings for the gamepad screen. Apply in onResume, clear in
 * onPause. The per-thread and per-socket ones (sender thread priority, timer
 * slack, Wi-Fi low-latency lock, DSCP marking) are applied by
 * [ControllerLink] and the C engine.
 */
object LatencyTuning {

    /**
     * - **Sustained Performance mode** (Android 7+, where the device supports
     *   it): the CPU and GPU run at clocks they can hold indefinitely instead
     *   of boosting and then throttling when hot. Peak speed is lower, but
     *   frame and wake-up timing stay steady through a long session, which
     *   is what an input device needs.
     * - **Highest refresh rate** at the current resolution: touches are
     *   sampled and delivered per display frame, so 120 Hz halves the
     *   input-side delay compared with 60 Hz.
     * - **Screen kept on**: the low-latency Wi-Fi lock only works while the
     *   screen is on.
     *
     * Returns what was applied, for logging.
     */
    fun apply(activity: Activity): String {
        val window = activity.window
        val applied = mutableListOf<String>()

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applied += "screen on"

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            val power = activity.getSystemService(PowerManager::class.java)
            if (power != null && power.isSustainedPerformanceModeSupported) {
                window.setSustainedPerformanceMode(true)
                applied += "sustained performance"
            }
        }

        val best = highestRefreshMode(activity)
        if (best != null) {
            window.attributes = window.attributes.also { it.preferredDisplayModeId = best.modeId }
            applied += "display mode ${best.modeId} at ${best.refreshRate} Hz"
        }
        return applied.joinToString()
    }

    /** Undoes [apply]; call in onPause. */
    fun clear(activity: Activity) {
        val window = activity.window
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            window.setSustainedPerformanceMode(false)
        }
        window.attributes = window.attributes.also { it.preferredDisplayModeId = 0 }
    }

    @Suppress("DEPRECATION")   // defaultDisplay, for Android 8-10
    private fun highestRefreshMode(activity: Activity): Display.Mode? {
        val display: Display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            activity.display ?: return null
        } else {
            activity.windowManager.defaultDisplay
        }
        val current = display.mode
        return display.supportedModes
            .filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
            .maxByOrNull { it.refreshRate }
    }
}
