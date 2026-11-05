package io.github.replaynes

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.min

/**
 * On-screen NES pad: 8-way D-pad, A, B, SELECT, START and a hold-to-rewind button. Multi-touch:
 * the state is recomputed from every active pointer on each event, so sliding a finger between
 * buttons works and a lifted finger releases only its own button.
 */
class GamepadOverlayView(context: Context) : View(context) {
    /** Called with the NES button bits and the rewind state whenever they change. */
    var listener: ((buttons: Int, rewind: Boolean) -> Unit)? = null

    private var buttons = 0
    private var rewinding = false

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(70, 255, 255, 255) }
    private val pressed = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(150, 255, 255, 255) }
    private val red = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(150, 230, 30, 30) }
    private val redPressed = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(230, 255, 60, 60) }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(220, 255, 255, 255); textAlign = Paint.Align.CENTER; isFakeBoldText = true
    }

    // Geometry (px), computed in onSizeChanged.
    private var dpadX = 0f; private var dpadY = 0f; private var dpadR = 0f
    private var aX = 0f; private var aY = 0f; private var bX = 0f; private var bY = 0f; private var abR = 0f
    private val selectRect = RectF(); private val startRect = RectF(); private val rewindRect = RectF()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        val u = min(w, h) / 100f  // layout unit
        val portrait = h > w
        val margin = 6 * u
        dpadR = 17 * u
        abR = 9 * u
        val baseY = h - margin - dpadR
        dpadX = margin + dpadR
        dpadY = baseY
        aX = w - margin - abR
        aY = baseY - 5 * u
        bX = aX - 2.4f * abR
        bY = baseY + 5 * u
        val pillW = 16 * u; val pillH = 6 * u
        val pillY = if (portrait) baseY + dpadR - pillH else h - margin - pillH
        selectRect.set(w / 2f - pillW - 2 * u, pillY, w / 2f - 2 * u, pillY + pillH)
        startRect.set(w / 2f + 2 * u, pillY, w / 2f + pillW + 2 * u, pillY + pillH)
        val rwTop = if (portrait) baseY - dpadR - 12 * u else margin
        rewindRect.set(w - margin - 22 * u, rwTop, w - margin, rwTop + 8 * u)
        text.textSize = 4 * u
    }

    override fun onDraw(canvas: Canvas) {
        // D-pad: a cross with the pressed arms highlighted.
        val arm = dpadR * 0.36f
        fun armPaint(bit: Int) = if (buttons and bit != 0) pressed else fill
        canvas.drawRect(dpadX - arm, dpadY - dpadR, dpadX + arm, dpadY - arm, armPaint(NativeBridge.BTN_UP))
        canvas.drawRect(dpadX - arm, dpadY + arm, dpadX + arm, dpadY + dpadR, armPaint(NativeBridge.BTN_DOWN))
        canvas.drawRect(dpadX - dpadR, dpadY - arm, dpadX - arm, dpadY + arm, armPaint(NativeBridge.BTN_LEFT))
        canvas.drawRect(dpadX + arm, dpadY - arm, dpadX + dpadR, dpadY + arm, armPaint(NativeBridge.BTN_RIGHT))
        canvas.drawRect(dpadX - arm, dpadY - arm, dpadX + arm, dpadY + arm, fill)

        canvas.drawCircle(aX, aY, abR, if (buttons and NativeBridge.BTN_A != 0) redPressed else red)
        canvas.drawCircle(bX, bY, abR, if (buttons and NativeBridge.BTN_B != 0) redPressed else red)
        val ty = (text.descent() + text.ascent()) / 2
        canvas.drawText("A", aX, aY - ty, text)
        canvas.drawText("B", bX, bY - ty, text)

        pill(canvas, selectRect, "SELECT", buttons and NativeBridge.BTN_SELECT != 0)
        pill(canvas, startRect, "START", buttons and NativeBridge.BTN_START != 0)
        pill(canvas, rewindRect, "◀◀ REWIND", rewinding)
    }

    private fun pill(canvas: Canvas, r: RectF, label: String, on: Boolean) {
        canvas.drawRoundRect(r, r.height() / 2, r.height() / 2, if (on) pressed else fill)
        canvas.drawText(label, r.centerX(), r.centerY() - (text.descent() + text.ascent()) / 2, text)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        var b = 0
        var rw = false
        val ended = e.actionMasked == MotionEvent.ACTION_UP || e.actionMasked == MotionEvent.ACTION_CANCEL
        if (!ended) {
            for (i in 0 until e.pointerCount) {
                if (e.actionMasked == MotionEvent.ACTION_POINTER_UP && i == e.actionIndex) continue
                val x = e.getX(i); val y = e.getY(i)
                b = b or hit(x, y)
                if (inflate(rewindRect).contains(x, y)) rw = true
            }
        }
        if (b != buttons || rw != rewinding) {
            buttons = b; rewinding = rw
            listener?.invoke(b, rw)
            invalidate()
        }
        return true
    }

    private fun hit(x: Float, y: Float): Int {
        val dx = x - dpadX; val dy = y - dpadY
        val d = hypot(dx, dy)
        if (d < dpadR * 1.4f) {
            if (d < dpadR * 0.2f) return 0  // dead zone in the centre
            // 8 sectors of 45 degrees, starting at "right"; diagonals press two arms.
            val deg = ((Math.toDegrees(atan2(-dy, dx).toDouble()) + 360.0 + 22.5) % 360.0)
            return when ((deg / 45).toInt()) {
                0 -> NativeBridge.BTN_RIGHT
                1 -> NativeBridge.BTN_RIGHT or NativeBridge.BTN_UP
                2 -> NativeBridge.BTN_UP
                3 -> NativeBridge.BTN_UP or NativeBridge.BTN_LEFT
                4 -> NativeBridge.BTN_LEFT
                5 -> NativeBridge.BTN_LEFT or NativeBridge.BTN_DOWN
                6 -> NativeBridge.BTN_DOWN
                else -> NativeBridge.BTN_DOWN or NativeBridge.BTN_RIGHT
            }
        }
        var r = 0
        // Generous A/B areas; the gap between them presses both (a common NES-pad habit).
        val da = hypot(x - aX, y - aY); val db = hypot(x - bX, y - bY)
        if (da < abR * 1.35f) r = r or NativeBridge.BTN_A
        if (db < abR * 1.35f) r = r or NativeBridge.BTN_B
        if (inflate(selectRect).contains(x, y)) r = r or NativeBridge.BTN_SELECT
        if (inflate(startRect).contains(x, y)) r = r or NativeBridge.BTN_START
        return r
    }

    private val tmp = RectF()
    private fun inflate(r: RectF): RectF {
        val m = r.height() * 0.4f
        tmp.set(r.left - m, r.top - m, r.right + m, r.bottom + m)
        return tmp
    }
}
