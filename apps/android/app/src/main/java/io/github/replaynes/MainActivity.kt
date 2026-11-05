package io.github.replaynes

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.text.method.LinkMovementMethod
import android.text.util.Linkify
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.io.IOException
import kotlin.math.min

/**
 * Single activity: a ROM library screen and the game screen (SurfaceView + touch pad overlay).
 * The emulation itself runs on the native emulation thread (NativeBridge).
 */
class MainActivity : Activity() {
    private lateinit var library: RomLibrary
    private var currentRom: File? = null
    private var started = false
    private var resumed = false

    private var touchButtons = 0
    private var keyButtons = 0
    private var axisButtons = 0
    private var keyRewind = false
    private var touchRewind = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        library = RomLibrary(this)
        currentRom = savedInstanceState?.getString(STATE_ROM)?.let { File(it) }?.takeIf { it.exists() }
        if (!handleDebugIntent(intent)) {
            if (currentRom != null) showGame() else showLibrary()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleDebugIntent(intent)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        currentRom?.let { outState.putString(STATE_ROM, it.path) }
    }

    override fun onStart() {
        super.onStart()
        started = true
        startSessionIfNeeded()
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        NativeBridge.setPaused(false)
        if (currentRom != null) hideSystemBars()
    }

    override fun onPause() {
        resumed = false
        NativeBridge.setPaused(true)
        super.onPause()
    }

    override fun onStop() {
        started = false
        NativeBridge.stop()  // saves the project and closes the session
        super.onStop()
    }

    // ------------------------------------------------------------------------------- library

    private fun showLibrary() {
        currentRom = null
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val dp = resources.displayMetrics.density
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((16 * dp).toInt(), (24 * dp).toInt(), (16 * dp).toInt(), (16 * dp).toInt())
        }
        root.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            textSize = 28f
            setTextColor(Color.WHITE)
        })
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        fun button(label: String, onClick: () -> Unit) = Button(this).apply {
            text = label
            setOnClickListener { onClick() }
            buttons.addView(this)
        }
        button(getString(R.string.open_rom)) { openRomPicker() }
        button(getString(R.string.test_cartridge)) {
            try {
                startGame(library.testCartridge())
            } catch (e: IOException) {
                toast(e.message ?: "error")
            }
        }
        button(getString(R.string.about)) { showAbout() }
        root.addView(buttons)

        val roms = library.roms()
        root.addView(TextView(this).apply {
            text = if (roms.isEmpty()) getString(R.string.no_roms) else getString(R.string.recent_roms)
            setTextColor(Color.LTGRAY)
            setPadding(0, (16 * dp).toInt(), 0, (8 * dp).toInt())
        })
        val list = ListView(this)
        val adapter = object : ArrayAdapter<String>(this, android.R.layout.simple_list_item_1,
            roms.map { it.nameWithoutExtension }) {
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View =
                (super.getView(position, convertView, parent) as TextView).apply { setTextColor(Color.WHITE) }
        }
        list.adapter = adapter
        list.setOnItemClickListener { _, _, pos, _ -> startGame(roms[pos]) }
        root.addView(list, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        // The outer frame takes the system-bar insets (edge-to-edge on API 35), the inner keeps its padding.
        setContentView(FrameLayout(this).apply {
            fitsSystemWindows = true
            setBackgroundColor(Color.rgb(24, 24, 28))
            addView(root)
        })
        showSystemBars()  // needs the decor view (after setContentView)
    }

    private fun openRomPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"  // .nes files have no reliable MIME type; the iNES header is checked instead
        }
        @Suppress("DEPRECATION")
        startActivityForResult(intent, REQUEST_OPEN_ROM)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_OPEN_ROM || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            startGame(library.import(uri))
        } catch (e: IOException) {
            toast(getString(R.string.import_failed, e.message ?: ""))
        }
    }

    private fun showAbout() {
        val dp = resources.displayMetrics.density
        val body = TextView(this).apply {
            text = getString(R.string.about_text, BuildConfig.VERSION_NAME, NativeBridge.engineVersion())
            setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
            Linkify.addLinks(this, Linkify.WEB_URLS)
            movementMethod = LinkMovementMethod.getInstance()
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.about)
            .setView(ScrollView(this).apply { addView(body) })
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    // ---------------------------------------------------------------------------------- game

    private fun startGame(rom: File) {
        NativeBridge.stop()
        currentRom = rom
        showGame()
        startSessionIfNeeded()
    }

    private fun startSessionIfNeeded() {
        val rom = currentRom ?: return
        if (!started || NativeBridge.isRunning()) return
        val err = NativeBridge.start(rom.path, library.projectFor(rom).path)
        if (err != null) {
            Log.e(TAG, "start failed: $err")
            AlertDialog.Builder(this)
                .setTitle(R.string.open_failed)
                .setMessage(err)
                .setPositiveButton(android.R.string.ok, null)
                .show()
            showLibrary()
            return
        }
        NativeBridge.setPaused(!resumed)
        pushInput()
    }

    private fun showGame() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        touchButtons = 0; keyButtons = 0; axisButtons = 0; keyRewind = false; touchRewind = false
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        val surface = SurfaceView(this)
        surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {}
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                NativeBridge.setSurface(holder.surface, width, height)
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                NativeBridge.setSurface(null, 0, 0)
            }
        })
        root.addView(LetterboxLayout(this, surface))
        val pad = GamepadOverlayView(this).apply {
            listener = { b, rw -> touchButtons = b; touchRewind = rw; pushInput() }
        }
        root.addView(pad, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(root)
        hideSystemBars()
    }

    private fun pushInput() {
        NativeBridge.setInput(touchButtons, keyButtons or axisButtons)
        NativeBridge.setRewind(touchRewind || keyRewind)
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (currentRom != null) {
            NativeBridge.stop()
            showLibrary()
        } else {
            @Suppress("DEPRECATION")
            super.onBackPressed()
        }
    }

    // ---------------------------------------------------------------- hardware gamepad / keys

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (currentRom == null) return super.dispatchKeyEvent(event)
        val down = when (event.action) {
            KeyEvent.ACTION_DOWN -> true
            KeyEvent.ACTION_UP -> false
            else -> return super.dispatchKeyEvent(event)
        }
        if (event.keyCode in REWIND_KEYS) {
            keyRewind = down; pushInput(); return true
        }
        val bit = KEY_MAP[event.keyCode] ?: return super.dispatchKeyEvent(event)
        keyButtons = if (down) keyButtons or bit else keyButtons and bit.inv()
        pushInput()
        return true
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (currentRom == null || event.action != MotionEvent.ACTION_MOVE ||
            (event.source and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
        ) return super.dispatchGenericMotionEvent(event)
        // Hat (D-pad reported as axes) and the left stick.
        val x = event.getAxisValue(MotionEvent.AXIS_HAT_X).let { if (it != 0f) it else event.getAxisValue(MotionEvent.AXIS_X) }
        val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y).let { if (it != 0f) it else event.getAxisValue(MotionEvent.AXIS_Y) }
        var b = 0
        if (x < -0.5f) b = b or NativeBridge.BTN_LEFT
        if (x > 0.5f) b = b or NativeBridge.BTN_RIGHT
        if (y < -0.5f) b = b or NativeBridge.BTN_UP
        if (y > 0.5f) b = b or NativeBridge.BTN_DOWN
        axisButtons = b
        pushInput()
        return true
    }

    // ---------------------------------------------------------------------------------- misc

    /** Debug builds: `am start ... --es rom /path/to/x.nes` (or `--es rom testcart`). */
    private fun handleDebugIntent(intent: Intent?): Boolean {
        if (!BuildConfig.DEBUG) return false
        val rom = intent?.getStringExtra(EXTRA_ROM) ?: return false
        intent.removeExtra(EXTRA_ROM)
        return try {
            startGame(if (rom == "testcart") library.testCartridge() else library.importPath(rom))
            true
        } catch (e: IOException) {
            Log.e(TAG, "debug ROM $rom: $e")
            toast(getString(R.string.import_failed, e.message ?: ""))
            showLibrary()
            true
        }
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
    }

    private fun showSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.show(WindowInsets.Type.systemBars())
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = 0
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()

    /** Centers its single child at the NES display aspect (256x240 with 8:7 pixels), letterboxed. */
    private class LetterboxLayout(context: android.content.Context, child: View) : ViewGroup(context) {
        init {
            addView(child)
        }

        override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
            val w = MeasureSpec.getSize(widthMeasureSpec)
            val h = MeasureSpec.getSize(heightMeasureSpec)
            val aspect = 256f * 8f / 7f / 240f
            var cw = w; var ch = (w / aspect).toInt()
            if (ch > h) { ch = h; cw = (h * aspect).toInt() }
            getChildAt(0).measure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY))
            setMeasuredDimension(w, h)
        }

        override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
            val c = getChildAt(0)
            val w = r - l; val h = b - t
            val x = (w - c.measuredWidth) / 2
            // Portrait: game at the top, the touch pad gets the space below.
            val y = if (h > w) min((h - c.measuredHeight) / 2, h / 10) else (h - c.measuredHeight) / 2
            c.layout(x, y, x + c.measuredWidth, y + c.measuredHeight)
        }
    }

    companion object {
        private const val TAG = "ReplayNES"
        private const val STATE_ROM = "rom"
        private const val EXTRA_ROM = "rom"
        private const val REQUEST_OPEN_ROM = 1

        private val KEY_MAP = mapOf(
            KeyEvent.KEYCODE_DPAD_UP to NativeBridge.BTN_UP,
            KeyEvent.KEYCODE_DPAD_DOWN to NativeBridge.BTN_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT to NativeBridge.BTN_LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT to NativeBridge.BTN_RIGHT,
            // Positional like the other frontends: NES A = east face button, NES B = south.
            KeyEvent.KEYCODE_BUTTON_B to NativeBridge.BTN_A,
            KeyEvent.KEYCODE_BUTTON_A to NativeBridge.BTN_B,
            KeyEvent.KEYCODE_BUTTON_Y to NativeBridge.BTN_A,
            KeyEvent.KEYCODE_BUTTON_X to NativeBridge.BTN_B,
            KeyEvent.KEYCODE_BUTTON_START to NativeBridge.BTN_START,
            KeyEvent.KEYCODE_BUTTON_SELECT to NativeBridge.BTN_SELECT,
            // Keyboard: arrows, X = A, Z = B, Enter = Start, Space / right Shift = Select.
            KeyEvent.KEYCODE_X to NativeBridge.BTN_A,
            KeyEvent.KEYCODE_Z to NativeBridge.BTN_B,
            KeyEvent.KEYCODE_ENTER to NativeBridge.BTN_START,
            KeyEvent.KEYCODE_SPACE to NativeBridge.BTN_SELECT,
            KeyEvent.KEYCODE_SHIFT_RIGHT to NativeBridge.BTN_SELECT,
        )
        private val REWIND_KEYS = setOf(
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_DEL,
        )
    }
}
