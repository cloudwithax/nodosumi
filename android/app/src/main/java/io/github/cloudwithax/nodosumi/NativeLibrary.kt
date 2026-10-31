package io.github.cloudwithax.nodosumi

/** The libretro frontend in frontend.c. Everything but setInput runs on the GL thread. */
object NativeLibrary {
    init {
        System.loadLibrary("nodosumi_android")
    }

    // libretro joypad ids, used as bit positions in setInput's buttons
    const val B = 0
    const val Y = 1
    const val START = 3
    const val UP = 4
    const val DOWN = 5
    const val LEFT = 6
    const val RIGHT = 7
    const val A = 8
    const val X = 9
    const val L = 12
    const val R = 13

    external fun setOption(key: String, value: String)
    external fun start(systemDir: String, gamePath: String): Boolean
    external fun stop()
    external fun surfaceCreated()
    external fun surfaceChanged(width: Int, height: Int)
    external fun frame()
    external fun setPaused(paused: Boolean)
    external fun setInput(port: Int, buttons: Int, lx: Int, ly: Int)
    external fun rumble(port: Int): Int
    external fun reset()
    external fun saveState(path: String): Boolean
    external fun loadState(path: String): Boolean
    external fun discCount(): Int
    external fun discIndex(): Int
    external fun swapDisc(index: Int): Boolean
}
