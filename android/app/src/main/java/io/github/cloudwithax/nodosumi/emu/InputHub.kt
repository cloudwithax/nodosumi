package io.github.cloudwithax.nodosumi.emu

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import io.github.cloudwithax.nodosumi.NativeLibrary
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Collects the touch controls and up to four controllers into dreamcast ports.
 * The UI thread writes, the GL thread reads in push().
 */
object InputHub {
    const val PORTS = 4

    class Pad {
        @Volatile var buttons = 0
        @Volatile var lx = 0
        @Volatile var ly = 0
        // the hat and the triggers' axes share bits with keys, keep them apart
        @Volatile var axisButtons = 0
    }

    val touch = Pad()
    private val pads = Array(PORTS) { Pad() }
    private val ports = LinkedHashMap<Int, Int>()

    /** The port a controller plays on, handed out in the order they're first used. */
    fun portOf(device: InputDevice?): Int? {
        device ?: return null
        synchronized(ports) {
            ports[device.id]?.let { return it }
            if (ports.size >= PORTS) return null
            val port = (0 until PORTS).first { it !in ports.values }
            ports[device.id] = port
            return port
        }
    }

    fun deviceOnPort(port: Int): Int? = synchronized(ports) {
        ports.entries.firstOrNull { it.value == port }?.key
    }

    fun removeDevice(id: Int) {
        val port = synchronized(ports) { ports.remove(id) } ?: return
        pads[port].apply { buttons = 0; axisButtons = 0; lx = 0; ly = 0 }
    }

    private fun bitOf(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> NativeLibrary.A
        KeyEvent.KEYCODE_BUTTON_B -> NativeLibrary.B
        KeyEvent.KEYCODE_BUTTON_X -> NativeLibrary.X
        KeyEvent.KEYCODE_BUTTON_Y -> NativeLibrary.Y
        KeyEvent.KEYCODE_BUTTON_START -> NativeLibrary.START
        KeyEvent.KEYCODE_DPAD_UP -> NativeLibrary.UP
        KeyEvent.KEYCODE_DPAD_DOWN -> NativeLibrary.DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> NativeLibrary.LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> NativeLibrary.RIGHT
        // the dreamcast only has triggers, the shoulders double as them
        KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_L2 -> NativeLibrary.L
        KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_R2 -> NativeLibrary.R
        else -> null
    }

    fun isController(device: InputDevice?): Boolean {
        device ?: return false
        if (device.isVirtual) return false
        val s = device.sources
        return s and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            s and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
    }

    /** Returns true if the key belonged to a controller and was taken. */
    fun onKey(event: KeyEvent): Boolean {
        if (!isController(event.device)) return false
        val bit = bitOf(event.keyCode) ?: return false
        val port = portOf(event.device) ?: return true
        val pad = pads[port]
        pad.buttons = when (event.action) {
            KeyEvent.ACTION_DOWN -> pad.buttons or (1 shl bit)
            KeyEvent.ACTION_UP -> pad.buttons and (1 shl bit).inv()
            else -> pad.buttons
        }
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        val device = event.device
        if (!isController(device) || event.action != MotionEvent.ACTION_MOVE) return false
        val port = portOf(device) ?: return true
        val pad = pads[port]

        fun axis(a: Int): Float {
            val v = event.getAxisValue(a)
            val flat = device.getMotionRange(a, event.source)?.flat ?: 0f
            return if (abs(v) <= flat) 0f else v
        }

        pad.lx = (axis(MotionEvent.AXIS_X) * Short.MAX_VALUE).roundToInt()
        pad.ly = (axis(MotionEvent.AXIS_Y) * Short.MAX_VALUE).roundToInt()

        val hx = axis(MotionEvent.AXIS_HAT_X)
        val hy = axis(MotionEvent.AXIS_HAT_Y)
        val lt = maxOf(axis(MotionEvent.AXIS_LTRIGGER), axis(MotionEvent.AXIS_BRAKE))
        val rt = maxOf(axis(MotionEvent.AXIS_RTRIGGER), axis(MotionEvent.AXIS_GAS))
        var bits = 0
        if (hx < -0.5f) bits = bits or (1 shl NativeLibrary.LEFT)
        if (hx > 0.5f) bits = bits or (1 shl NativeLibrary.RIGHT)
        if (hy < -0.5f) bits = bits or (1 shl NativeLibrary.UP)
        if (hy > 0.5f) bits = bits or (1 shl NativeLibrary.DOWN)
        if (lt > 0.5f) bits = bits or (1 shl NativeLibrary.L)
        if (rt > 0.5f) bits = bits or (1 shl NativeLibrary.R)
        pad.axisButtons = bits
        return true
    }

    /** Hands the current state to the core, on the GL thread. */
    fun push() {
        for (port in 0 until PORTS) {
            val pad = pads[port]
            var buttons = pad.buttons or pad.axisButtons
            var lx = pad.lx
            var ly = pad.ly
            // the touch controls play on port 1, alongside its controller
            if (port == 0) {
                buttons = buttons or touch.buttons
                lx = (lx + touch.lx).coerceIn(-Short.MAX_VALUE.toInt(), Short.MAX_VALUE.toInt())
                ly = (ly + touch.ly).coerceIn(-Short.MAX_VALUE.toInt(), Short.MAX_VALUE.toInt())
            }
            NativeLibrary.setInput(port, buttons, lx, ly)
        }
    }
}
