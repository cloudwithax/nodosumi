package io.github.cloudwithax.nodosumi

import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Process
import android.util.Log
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import io.github.cloudwithax.nodosumi.data.Settings
import java.io.File
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/** ADB entry point in the benchmark APK only. Normal app data is read-only. */
class BenchmarkActivity : ComponentActivity() {
    private lateinit var view: GLSurfaceView
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Settings.init(this)
        val game = intent.getStringExtra("game") ?: return finish()
        val directory = File(filesDir, "benchmark").apply { mkdirs() }
        val system = File(directory, "system").apply { mkdirs() }
        val bios = File(system, "dc").apply { mkdirs() }
        for (name in listOf("boot.bin", "flash.bin")) {
            val source = File(Settings.biosDir(this), name)
            if (source.exists()) source.copyTo(File(bios, name), overwrite = true)
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).hide(WindowInsetsCompat.Type.systemBars())
        view = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            setEGLConfigChooser(8, 8, 8, 8, 0, 0)
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                    NativeLibrary.configureBenchmark(File(directory, "bot.txt").path, File(directory, "trace.bin").path, intent.getBooleanExtra("baseline", false))
                    for (option in Settings.CORE_OPTIONS) NativeLibrary.setOption(option.key, option.default)
                    NativeLibrary.setOption("nodosumi_internal_resolution", intent.getStringExtra("resolution") ?: "1280x960")
                    NativeLibrary.setOption("nodosumi_aspect", intent.getStringExtra("aspect") ?: "4:3")
                    NativeLibrary.setOption("nodosumi_widescreen", if (intent.getBooleanExtra("wide", false)) "enabled" else "disabled")
                    check(NativeLibrary.start(system.path, game)) { "Benchmark game could not start" }
                    NativeLibrary.surfaceCreated()
                    Log.i("NodosumiBench", "Started $game at ${intent.getStringExtra("resolution")}")
                }
                override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) = NativeLibrary.surfaceChanged(width, height)
                override fun onDrawFrame(gl: GL10?) = NativeLibrary.frame()
            })
        }
        setContentView(view)
    }
    override fun onPause() {
        if (::view.isInitialized) {
            view.queueEvent { NativeLibrary.stop() }
            view.onPause()
        }
        super.onPause()
        Process.killProcess(Process.myPid())
    }
}
