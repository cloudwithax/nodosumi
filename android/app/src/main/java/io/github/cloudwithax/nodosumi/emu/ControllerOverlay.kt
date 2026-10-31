package io.github.cloudwithax.nodosumi.emu

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import androidx.core.content.res.ResourcesCompat
import io.github.cloudwithax.nodosumi.NativeLibrary
import io.github.cloudwithax.nodosumi.R
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt

/** On-screen dreamcast controller: stick, d-pad, ABXY, triggers and start. */
class ControllerOverlay(context: Context) : View(context) {
    private sealed class Control(var x: Float = 0f, var y: Float = 0f, var r: Float = 0f) {
        class Button(val bit: Int, val label: String, val color: Int) : Control()
        class Pill(val bit: Int, val label: String, var w: Float = 0f, var h: Float = 0f) : Control()
        class Dpad : Control()
        class Stick : Control()
    }

    private val a = Control.Button(NativeLibrary.A, "A", Color.rgb(224, 64, 62))
    private val b = Control.Button(NativeLibrary.B, "B", Color.rgb(54, 132, 218))
    private val xb = Control.Button(NativeLibrary.X, "X", Color.rgb(240, 192, 52))
    private val yb = Control.Button(NativeLibrary.Y, "Y", Color.rgb(66, 170, 96))
    private val l = Control.Pill(NativeLibrary.L, "L")
    private val r = Control.Pill(NativeLibrary.R, "R")
    private val start = Control.Pill(NativeLibrary.START, "START")
    private val dpad = Control.Dpad()
    private val stick = Control.Stick()
    private val controls = listOf(a, b, xb, yb, l, r, start, dpad, stick)

    /** 50-150, how big the controls are drawn. */
    var scale = 100
        set(value) { field = value; layoutControls(); invalidate() }
    var haptics = true

    private val pointers = HashMap<Int, Control>()
    private var buttons = 0
    private var stickX = 0f
    private var stickY = 0f

    private val density = resources.displayMetrics.density
    private val ink = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(150, 20, 17, 16) }
    private val shu = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(220, 232, 71, 10) }
    private val edge = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        color = Color.argb(170, 246, 241, 231)
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = ResourcesCompat.getFont(context, R.font.oswald_medium)
        color = Color.argb(230, 246, 241, 231)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = layoutControls()

    /** Lays out in units of a hundredth of the short side, anchored to the corners. */
    private fun layoutControls() {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w == 0f || h == 0f) return
        val s = min(w, h) / 100f * scale / 100f
        val portrait = h > w

        fun left(c: Control, dx: Float, dy: Float, rad: Float) { c.x = dx * s; c.y = h - dy * s; c.r = rad * s }
        fun right(c: Control, dx: Float, dy: Float, rad: Float) { c.x = w - dx * s; c.y = h - dy * s; c.r = rad * s }

        left(stick, 22f, 58f, 15f)
        left(dpad, 40f, 21f, 15f)
        right(a, 24f, 26f, 8f)
        right(b, 11f, 39f, 8f)
        right(xb, 37f, 39f, 8f)
        right(yb, 24f, 52f, 8f)

        // in landscape the triggers sit up top, in portrait above the rest
        val trigY = if (portrait) h - 82f * s else 10f * s
        for ((c, x) in listOf(l to 16f * s, r to w - 16f * s)) {
            c.x = x; c.y = trigY; c.w = 24f * s; c.h = 10f * s; c.r = 12f * s
        }
        start.x = w / 2f; start.y = h - 7f * s; start.w = 20f * s; start.h = 7f * s; start.r = 10f * s
    }

    private fun hit(px: Float, py: Float): Control? = controls
        .map { it to hypot(px - it.x, py - it.y) / it.r }
        .filter { (c, d) -> d < if (c is Control.Pill) 1.4f else 1.3f }
        .minByOrNull { it.second }
        ?.first

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                hit(event.getX(i), event.getY(i))?.let { pointers[event.getPointerId(i)] = it }
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    val id = event.getPointerId(i)
                    // the stick keeps its finger, everything else can be slid between
                    if (pointers[id] is Control.Stick) continue
                    val c = hit(event.getX(i), event.getY(i))
                    if (c == null || c is Control.Stick) pointers.remove(id) else pointers[id] = c
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP ->
                pointers.remove(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> pointers.clear()
        }
        update(event)
        return true
    }

    private fun update(event: MotionEvent) {
        var bits = 0
        var sx = 0f
        var sy = 0f
        for (i in 0 until event.pointerCount) {
            val c = pointers[event.getPointerId(i)] ?: continue
            val dx = event.getX(i) - c.x
            val dy = event.getY(i) - c.y
            when (c) {
                is Control.Button -> bits = bits or (1 shl c.bit)
                is Control.Pill -> bits = bits or (1 shl c.bit)
                is Control.Dpad -> bits = bits or dpadBits(dx, dy, c.r)
                is Control.Stick -> {
                    val len = hypot(dx, dy)
                    val k = if (len > c.r) c.r / len else 1f
                    sx = dx * k / c.r
                    sy = dy * k / c.r
                }
            }
        }

        if (haptics && bits and buttons.inv() != 0) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
        }
        buttons = bits
        stickX = sx
        stickY = sy
        InputHub.touch.buttons = bits
        InputHub.touch.lx = (sx * Short.MAX_VALUE).roundToInt()
        InputHub.touch.ly = (sy * Short.MAX_VALUE).roundToInt()
        invalidate()
    }

    /** 8-way, with a small dead middle. */
    private fun dpadBits(dx: Float, dy: Float, r: Float): Int {
        if (hypot(dx, dy) < r * 0.2f) return 0
        val deg = Math.toDegrees(atan2(dy.toDouble(), dx.toDouble())).let { if (it < 0) it + 360 else it }
        val sector = ((deg + 22.5) / 45).toInt() % 8
        val dirs = arrayOf(
            listOf(NativeLibrary.RIGHT),
            listOf(NativeLibrary.RIGHT, NativeLibrary.DOWN),
            listOf(NativeLibrary.DOWN),
            listOf(NativeLibrary.DOWN, NativeLibrary.LEFT),
            listOf(NativeLibrary.LEFT),
            listOf(NativeLibrary.LEFT, NativeLibrary.UP),
            listOf(NativeLibrary.UP),
            listOf(NativeLibrary.UP, NativeLibrary.RIGHT),
        )
        return dirs[sector].fold(0) { acc, bit -> acc or (1 shl bit) }
    }

    /** Lets go of everything, when the menu opens over the controls. */
    fun release() {
        pointers.clear()
        buttons = 0
        stickX = 0f
        stickY = 0f
        InputHub.touch.apply { buttons = 0; lx = 0; ly = 0 }
        invalidate()
    }

    private fun pressed(bit: Int) = buttons and (1 shl bit) != 0

    private val rect = RectF()

    override fun onDraw(canvas: Canvas) {
        for (c in controls) {
            when (c) {
                is Control.Button -> {
                    canvas.drawCircle(c.x, c.y, c.r, if (pressed(c.bit)) shu else ink)
                    canvas.drawCircle(c.x, c.y, c.r, edge)
                    text.textSize = c.r
                    text.color = if (pressed(c.bit)) Color.WHITE else c.color
                    canvas.drawText(c.label, c.x, c.y + c.r * 0.36f, text)
                }
                is Control.Pill -> {
                    rect.set(c.x - c.w / 2, c.y - c.h / 2, c.x + c.w / 2, c.y + c.h / 2)
                    canvas.drawRoundRect(rect, c.h / 2, c.h / 2, if (pressed(c.bit)) shu else ink)
                    canvas.drawRoundRect(rect, c.h / 2, c.h / 2, edge)
                    text.textSize = c.h * 0.5f
                    text.color = Color.argb(230, 246, 241, 231)
                    canvas.drawText(c.label, c.x, c.y + c.h * 0.18f, text)
                }
                is Control.Dpad -> drawDpad(canvas, c)
                is Control.Stick -> {
                    canvas.drawCircle(c.x, c.y, c.r, ink)
                    canvas.drawCircle(c.x, c.y, c.r, edge)
                    val active = stickX != 0f || stickY != 0f
                    val tx = c.x + stickX * c.r * 0.6f
                    val ty = c.y + stickY * c.r * 0.6f
                    canvas.drawCircle(tx, ty, c.r * 0.45f, if (active) shu else ink)
                    canvas.drawCircle(tx, ty, c.r * 0.45f, edge)
                }
            }
        }
    }

    private fun drawDpad(canvas: Canvas, c: Control) {
        val arm = c.r * 0.36f
        val corner = arm * 0.4f
        // a plus, then each pressed arm lit on top
        rect.set(c.x - c.r, c.y - arm, c.x + c.r, c.y + arm)
        canvas.drawRoundRect(rect, corner, corner, ink)
        rect.set(c.x - arm, c.y - c.r, c.x + arm, c.y + c.r)
        canvas.drawRoundRect(rect, corner, corner, ink)
        val arms = listOf(
            NativeLibrary.UP to RectF(c.x - arm, c.y - c.r, c.x + arm, c.y - arm),
            NativeLibrary.DOWN to RectF(c.x - arm, c.y + arm, c.x + arm, c.y + c.r),
            NativeLibrary.LEFT to RectF(c.x - c.r, c.y - arm, c.x - arm, c.y + arm),
            NativeLibrary.RIGHT to RectF(c.x + arm, c.y - arm, c.x + c.r, c.y + arm),
        )
        for ((bit, armRect) in arms) {
            if (pressed(bit)) canvas.drawRoundRect(armRect, corner, corner, shu)
        }
        // the outline, traced around the plus
        val p = android.graphics.Path().apply {
            moveTo(c.x - arm, c.y - c.r)
            lineTo(c.x + arm, c.y - c.r); lineTo(c.x + arm, c.y - arm)
            lineTo(c.x + c.r, c.y - arm); lineTo(c.x + c.r, c.y + arm)
            lineTo(c.x + arm, c.y + arm); lineTo(c.x + arm, c.y + c.r)
            lineTo(c.x - arm, c.y + c.r); lineTo(c.x - arm, c.y + arm)
            lineTo(c.x - c.r, c.y + arm); lineTo(c.x - c.r, c.y - arm)
            lineTo(c.x - arm, c.y - arm); close()
        }
        edge.pathEffect = android.graphics.CornerPathEffect(corner)
        canvas.drawPath(p, edge)
        edge.pathEffect = null
    }
}
