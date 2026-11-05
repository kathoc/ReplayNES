package io.github.replaynes

import android.view.Surface

/** JNI entry points of libreplaynes_android.so (src/main/cpp/replaynes_jni.cpp). */
object NativeBridge {
    init {
        System.loadLibrary("replaynes_android")
    }

    /** NES controller bits (replaynes.h RN_BTN_*). */
    const val BTN_A = 0x01
    const val BTN_B = 0x02
    const val BTN_SELECT = 0x04
    const val BTN_START = 0x08
    const val BTN_UP = 0x10
    const val BTN_DOWN = 0x20
    const val BTN_LEFT = 0x40
    const val BTN_RIGHT = 0x80

    external fun engineVersion(): String

    /** Writes the ReplayNES Test Cartridge (CC0 sample ROM). Returns an error message or null. */
    external fun writeTestCartridge(path: String): String?

    /** Opens (or creates) the project of [romPath] and starts emulating. Error message or null. */
    external fun start(romPath: String, projectDir: String): String?

    /** Stops the emulation thread, saves the project and closes the session. */
    external fun stop()
    external fun isRunning(): Boolean
    external fun setPaused(paused: Boolean)
    external fun setInput(touch: Int, keys: Int)
    external fun setRewind(on: Boolean)
    external fun frameCount(): Long
    external fun setSurface(surface: Surface?, width: Int, height: Int)
}
