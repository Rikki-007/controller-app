package dev.vcd.controller

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View

/**
 * The on-screen gamepad. Touches go straight to the shared C layout
 * (include/layout.h), which does the hit-testing, deadzones and floating
 * stick and publishes the resulting state on [link]'s sender, so the
 * controller state never round-trips through Kotlin.
 *
 * - Touch events are requested unbuffered: Android otherwise batches
 *   MotionEvents to the next vsync, up to a frame of added latency.
 * - Labels, colours and which buttons exist follow [controllerType]'s
 *   profile, so changing it relabels the pad (A on Xbox, Cross on
 *   PlayStation, B on Switch for the same bottom button). The type a host
 *   emulates is fixed at HELLO, so pair a change with a new ControllerLink.
 * - Drawing reads one float array per frame; nothing allocates while
 *   touching or drawing.
 *
 * Main thread only, like every View.
 */
class GamepadView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    /** Where input goes. May be set or replaced at any time; null drops input. */
    var link: ControllerLink? = null

    var controllerType: ControllerType = ControllerType.XBOX
        set(value) {
            if (field != value) {
                field = value
                rebuild()
            }
        }

    /** Radial deadzone and saturation (fractions of the stick radius) per side. */
    fun setStick(side: Int, deadzone: Float, saturation: Float) {
        stickSettings[side.coerceIn(0, 1)] = floatArrayOf(deadzone, saturation)
        if (layout != 0L) NativeCore.layoutSetStick(layout, side, deadzone, saturation)
    }

    var hapticsEnabled = true

    // Native layout and what we know about its controls.
    private var layout = 0L
    private var count = 0
    private var geometry = FloatArray(0)
    private var visuals = FloatArray(0)
    private var labels = emptyArray<String>()
    private var colors = IntArray(0)
    private var pressedMask = 0
    private val stickSettings = arrayOf(floatArrayOf(0.12f, 0.85f), floatArrayOf(0.10f, 0.90f))

    // Preallocated drawing state.
    private val basePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0x33FFFFFF }
    private val pressedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0x99FFFFFF.toInt() }
    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = 0x88FFFFFF.toInt()
        strokeWidth = 3f
    }
    private val knobPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0xCCFFFFFF.toInt() }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val rect = RectF()

    init {
        isHapticFeedbackEnabled = true
    }

    private fun rebuild() {
        if (layout != 0L) NativeCore.layoutDestroy(layout)
        layout = 0L
        if (width == 0 || height == 0) return
        layout = NativeCore.layoutCreate(controllerType.wire, width.toFloat() / height)
        for (side in 0..1) NativeCore.layoutSetStick(layout, side, stickSettings[side][0], stickSettings[side][1])
        count = NativeCore.layoutCount(layout)
        geometry = FloatArray(count * NativeCore.GEOMETRY_STRIDE)
        visuals = FloatArray(count * NativeCore.VISUAL_STRIDE)
        NativeCore.layoutGeometry(layout, geometry)
        labels = Array(count) { NativeCore.layoutLabel(layout, it) }
        colors = IntArray(count) { NativeCore.layoutColor(layout, it) }
        contentDescription = "Gamepad: " + (0 until count).joinToString { NativeCore.layoutName(layout, it) }
        pressedMask = 0
        publish()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        rebuild()
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        if (layout != 0L) {
            NativeCore.layoutReset(layout)
            publish()                       // release everything this view held
            NativeCore.layoutDestroy(layout)
            layout = 0L
        }
    }

    // ---- Touch ------------------------------------------------------------------

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (layout == 0L) return false
        val unit = height.toFloat()
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // Deliver this gesture's events as they happen, not batched per frame.
                requestUnbufferedDispatch(event)
                feed(event, event.actionIndex, PHASE_DOWN, unit)
            }
            MotionEvent.ACTION_POINTER_DOWN -> feed(event, event.actionIndex, PHASE_DOWN, unit)
            MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) feed(event, i, PHASE_MOVE, unit)
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP -> feed(event, event.actionIndex, PHASE_UP, unit)
            MotionEvent.ACTION_CANCEL -> for (i in 0 until event.pointerCount) feed(event, i, PHASE_CANCEL, unit)
            else -> return true
        }
        publish()
        return true
    }

    private fun feed(event: MotionEvent, index: Int, phase: Int, unit: Float) {
        NativeCore.layoutTouch(
            layout, event.getPointerId(index).toLong(), phase,
            event.getX(index) / unit, event.getY(index) / unit,
        )
    }

    /** State to the sender (no-op without a running link), then haptics and redraw. */
    private fun publish() {
        val l = layout
        if (l == 0L) return
        link?.nativeHandle?.let { if (it != 0L) NativeCore.layoutApply(l, it) }

        NativeCore.layoutVisuals(l, visuals)
        var mask = 0
        for (i in 0 until count) {
            if (visuals[i * NativeCore.VISUAL_STRIDE] > 0.5f) mask = mask or (1 shl i)
        }
        if (hapticsEnabled && (mask and pressedMask.inv()) != 0) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
        }
        pressedMask = mask
        postInvalidateOnAnimation()
    }

    // ---- Drawing ----------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val unit = height.toFloat()
        textPaint.textSize = unit * 0.045f
        for (i in 0 until count) {
            val g = i * NativeCore.GEOMETRY_STRIDE
            val v = i * NativeCore.VISUAL_STRIDE
            val kind = geometry[g].toInt()
            val w = geometry[g + 3] * unit
            val h = geometry[g + 4] * unit
            val round = geometry[g + 5] > 0.5f
            val pressed = visuals[v] > 0.5f
            val x = visuals[v + 1] * unit
            val y = visuals[v + 2] * unit
            val fill = if (pressed) pressedPaint else basePaint

            when (kind) {
                KIND_STICK -> {
                    val r = w / 2
                    canvas.drawCircle(x, y, r, basePaint)
                    canvas.drawCircle(x, y, r, outlinePaint)
                    canvas.drawCircle(x + visuals[v + 3] * r, y + visuals[v + 4] * r, r * 0.42f, knobPaint)
                }
                KIND_DPAD -> {
                    val r = w / 2
                    val arm = r * 0.36f
                    rect.set(x - arm, y - r, x + arm, y + r)
                    canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, basePaint)
                    rect.set(x - r, y - arm, x + r, y + arm)
                    canvas.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, basePaint)
                    if (pressed) {
                        canvas.drawCircle(x + visuals[v + 3] * r * 0.7f, y + visuals[v + 4] * r * 0.7f, arm, pressedPaint)
                    }
                }
                else -> {
                    if (round) {
                        canvas.drawCircle(x, y, w / 2, fill)
                    } else {
                        rect.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2)
                        canvas.drawRoundRect(rect, h * 0.3f, h * 0.3f, fill)
                    }
                    val accent = colors[i]
                    outlinePaint.color = if (accent != 0) (0xFF shl 24) or accent else 0x88FFFFFF.toInt()
                    if (round) canvas.drawCircle(x, y, w / 2, outlinePaint) else canvas.drawRoundRect(rect, h * 0.3f, h * 0.3f, outlinePaint)
                    outlinePaint.color = 0x88FFFFFF.toInt()
                    textPaint.color = if (accent != 0) (0xFF shl 24) or accent else Color.WHITE
                    val label = labels[i]
                    if (label.isNotEmpty()) {
                        canvas.drawText(label, x, y - (textPaint.ascent() + textPaint.descent()) / 2, textPaint)
                    }
                }
            }
        }
    }

    private companion object {
        // vc_touch_phase_t
        const val PHASE_DOWN = 0
        const val PHASE_MOVE = 1
        const val PHASE_UP = 2
        const val PHASE_CANCEL = 3

        // vc_control_kind_t
        const val KIND_STICK = 4
        const val KIND_DPAD = 3
    }
}
