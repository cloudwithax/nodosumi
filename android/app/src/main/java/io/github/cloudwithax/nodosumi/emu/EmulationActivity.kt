package io.github.cloudwithax.nodosumi.emu

import android.content.Context
import android.hardware.input.InputManager
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.addCallback
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ExitToApp
import androidx.compose.material.icons.rounded.Album
import androidx.compose.material.icons.rounded.Download
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.RestartAlt
import androidx.compose.material.icons.rounded.Save
import androidx.compose.material.icons.rounded.TouchApp
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import io.github.cloudwithax.nodosumi.NativeLibrary
import io.github.cloudwithax.nodosumi.R
import io.github.cloudwithax.nodosumi.data.Settings
import io.github.cloudwithax.nodosumi.ui.NodosumiTheme
import java.io.File
import java.text.DateFormat
import java.util.Date
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class EmulationActivity : ComponentActivity() {
    companion object {
        const val EXTRA_PATH = "path"
        const val EXTRA_TITLE = "title"
        private const val SLOTS = 5

        /** The game the core in this process was started with. */
        private var loaded: String? = null
    }

    private lateinit var path: String
    private lateinit var title: String
    private lateinit var glView: GLSurfaceView
    private lateinit var overlay: ControllerOverlay
    private val main = Handler(Looper.getMainLooper())

    private val menuOpen = mutableStateOf(false)
    // hidden once a controller is used, until asked for again from the menu
    private val padInUse = mutableStateOf(false)
    private val discs = mutableStateOf(0 to 0)

    private var rumbleLast = 0
    private var exiting = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Settings.init(this)

        path = intent.getStringExtra(EXTRA_PATH) ?: return finish()
        title = intent.getStringExtra(EXTRA_TITLE) ?: File(path).nameWithoutExtension
        if (loaded != null && loaded != path) {
            // a core can only run one game a process, the next tap gets a fresh one
            finish()
            Process.killProcess(Process.myPid())
            return
        }

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        overlay = ControllerOverlay(this)
        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            setEGLConfigChooser(8, 8, 8, 8, 0, 0)
            preserveEGLContextOnPause = true
            setRenderer(Renderer())
            holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(holder: SurfaceHolder) {
                    // the core runs at 60hz, ask high refresh screens to match
                    holder.surface.setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                }
                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
                override fun surfaceDestroyed(holder: SurfaceHolder) {}
            })
        }

        onBackPressedDispatcher.addCallback(this) { menuOpen.value = !menuOpen.value }
        getSystemService(InputManager::class.java).registerInputDeviceListener(devices, main)

        setContent { NodosumiTheme { EmulationScreen() } }
        Toast.makeText(this, "Press back for the menu", Toast.LENGTH_SHORT).show()
    }

    private inner class Renderer : GLSurfaceView.Renderer {
        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            if (loaded == null) {
                for (opt in Settings.CORE_OPTIONS) {
                    NativeLibrary.setOption(opt.key, Settings.core(opt).value)
                }
                if (!NativeLibrary.start(Settings.systemDir(this@EmulationActivity).path, path)) {
                    main.post {
                        Toast.makeText(this@EmulationActivity, "Couldn't start $title", Toast.LENGTH_LONG).show()
                        finish()
                    }
                    return
                }
                loaded = path
            }
            NativeLibrary.surfaceCreated()
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            NativeLibrary.surfaceChanged(width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            InputHub.push()
            NativeLibrary.frame()
            val strength = if (Settings.rumble.value) NativeLibrary.rumble(0) else 0
            if (strength != rumbleLast) {
                rumbleLast = strength
                main.post { rumble(strength) }
            }
        }
    }

    /** The first controller's motor if it has one, else the phone's. */
    private fun rumble(strength: Int) {
        val device = InputHub.deviceOnPort(0)?.let { InputDevice.getDevice(it) }
        @Suppress("DEPRECATION")
        val vibrator = device?.vibrator?.takeIf { it.hasVibrator() }
            ?: getSystemService(Vibrator::class.java)
            ?: return
        if (strength == 0) {
            vibrator.cancel()
        } else {
            // the core says when to stop, so ask for longer than any one effect
            val amp = (strength * 255 / 65535).coerceIn(1, 255)
            vibrator.vibrate(VibrationEffect.createOneShot(2000, amp))
        }
    }

    private val devices = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(id: Int) {}
        override fun onInputDeviceChanged(id: Int) {}
        override fun onInputDeviceRemoved(id: Int) {
            InputHub.removeDevice(id)
            if (InputHub.deviceOnPort(0) == null) padInUse.value = false
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (InputHub.isController(event.device)) {
            // select or the home-ish button opens the menu on a controller
            if (event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT || event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
                if (event.action == KeyEvent.ACTION_UP) menuOpen.value = !menuOpen.value
                return true
            }
            // let the controller drive the menu while it's open
            if (!menuOpen.value && InputHub.onKey(event)) {
                padInUse.value = true
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (!menuOpen.value && InputHub.onMotion(event)) {
            padInUse.value = true
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    private fun setPaused(paused: Boolean) {
        glView.queueEvent { NativeLibrary.setPaused(paused) }
    }

    override fun onResume() {
        super.onResume()
        if (!::glView.isInitialized) return
        glView.onResume()
        setPaused(menuOpen.value)
    }

    override fun onPause() {
        if (::glView.isInitialized) {
            // queued ahead of the pause, so the core stops before the GL thread does
            setPaused(true)
            glView.onPause()
            main.post { rumble(0) }
        }
        super.onPause()
    }

    override fun onDestroy() {
        if (::glView.isInitialized) {
            getSystemService(InputManager::class.java).unregisterInputDeviceListener(devices)
        }
        super.onDestroy()
        if (exiting) Process.killProcess(Process.myPid())
    }

    private fun exit() {
        if (exiting) return
        exiting = true
        // unloading writes the vmu and flash back to disk
        glView.queueEvent {
            NativeLibrary.stop()
            main.post { finish() }
        }
    }

    private fun stateFile(slot: Int) = File(Settings.statesDir(this), "${File(path).nameWithoutExtension}.$slot.state")

    private fun toast(text: String) = main.post { Toast.makeText(this, text, Toast.LENGTH_SHORT).show() }

    private fun saveState(slot: Int) = glView.queueEvent {
        toast(if (NativeLibrary.saveState(stateFile(slot).path)) "Saved to slot $slot" else "Couldn't save")
    }

    private fun loadState(slot: Int) = glView.queueEvent {
        toast(if (NativeLibrary.loadState(stateFile(slot).path)) "Loaded slot $slot" else "Couldn't load slot $slot")
    }

    @Composable
    private fun EmulationScreen() {
        val drawer = rememberDrawerState(DrawerValue.Closed)
        var dialog by remember { mutableStateOf<String?>(null) }

        // back, select and the drawer's own scrim all go through menuOpen
        LaunchedEffect(menuOpen.value) {
            if (menuOpen.value) drawer.open() else drawer.close()
        }
        LaunchedEffect(drawer) {
            snapshotFlow { drawer.targetValue }.collect { target ->
                val open = target == DrawerValue.Open
                menuOpen.value = open
                setPaused(open)
                if (open) {
                    overlay.release()
                    glView.queueEvent {
                        val d = NativeLibrary.discCount() to NativeLibrary.discIndex()
                        main.post { discs.value = d }
                    }
                }
            }
        }

        ModalNavigationDrawer(
            drawerState = drawer,
            gesturesEnabled = drawer.isOpen,
            drawerContent = {
                ModalDrawerSheet(Modifier.width(300.dp)) {
                    Menu(
                        onResume = { menuOpen.value = false },
                        onDialog = { dialog = it },
                    )
                }
            },
        ) {
            AndroidView({ glView }, Modifier.fillMaxSize())
            if (Settings.showOverlay.value && !padInUse.value) {
                AndroidView(
                    { overlay },
                    Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.displayCutout),
                    update = {
                        it.alpha = Settings.overlayOpacity.intValue / 100f
                        it.scale = Settings.overlayScale.intValue
                        it.haptics = Settings.haptics.value
                    },
                )
            }
        }

        when (dialog) {
            "save", "load" -> SlotDialog(save = dialog == "save") { dialog = null }
            "disc" -> DiscDialog { dialog = null }
            "reset" -> AlertDialog(
                onDismissRequest = { dialog = null },
                title = { Text("Reset?") },
                text = { Text("Anything since your last save will be lost.") },
                confirmButton = {
                    TextButton(onClick = {
                        dialog = null
                        menuOpen.value = false
                        glView.queueEvent { NativeLibrary.reset() }
                    }) { Text("Reset") }
                },
                dismissButton = { TextButton(onClick = { dialog = null }) { Text("Cancel") } },
            )
        }
    }

    @Composable
    private fun Menu(onResume: () -> Unit, onDialog: (String) -> Unit) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(12.dp)) {
            Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                Image(painterResource(R.drawable.logo), null, Modifier.size(48.dp))
                Spacer(Modifier.width(12.dp))
                Text(title, style = MaterialTheme.typography.titleLarge, maxLines = 2)
            }
            HorizontalDivider(Modifier.padding(vertical = 8.dp))

            @Composable
            fun item(icon: androidx.compose.ui.graphics.vector.ImageVector, label: String, onClick: () -> Unit) =
                NavigationDrawerItem(
                    icon = { Icon(icon, null) },
                    label = { Text(label) },
                    selected = false,
                    onClick = onClick,
                )

            item(Icons.Rounded.PlayArrow, "Resume", onResume)
            item(Icons.Rounded.Save, "Save state") { onDialog("save") }
            item(Icons.Rounded.Download, "Load state") { onDialog("load") }
            if (discs.value.first > 1) {
                item(Icons.Rounded.Album, "Swap disc") { onDialog("disc") }
            }
            val showing = Settings.showOverlay.value && !padInUse.value
            item(Icons.Rounded.TouchApp, if (showing) "Hide touch controls" else "Show touch controls") {
                if (showing) {
                    Settings.setShowOverlay(false)
                } else {
                    Settings.setShowOverlay(true)
                    padInUse.value = false
                }
            }
            item(Icons.Rounded.RestartAlt, "Reset") { onDialog("reset") }
            HorizontalDivider(Modifier.padding(vertical = 8.dp))
            item(Icons.AutoMirrored.Rounded.ExitToApp, "Exit") { exit() }
        }
    }

    @Composable
    private fun SlotDialog(save: Boolean, onDismiss: () -> Unit) {
        val format = remember { DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT) }
        AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(if (save) "Save state" else "Load state") },
            text = {
                Column {
                    for (slot in 1..SLOTS) {
                        val file = stateFile(slot)
                        val exists = file.exists()
                        TextButton(
                            onClick = {
                                onDismiss()
                                menuOpen.value = false
                                if (save) saveState(slot) else loadState(slot)
                            },
                            enabled = save || exists,
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text("Slot $slot", Modifier.weight(1f))
                            Text(
                                if (exists) format.format(Date(file.lastModified())) else "Empty",
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        )
    }

    @Composable
    private fun DiscDialog(onDismiss: () -> Unit) {
        val (count, current) = discs.value
        AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text("Swap disc") },
            text = {
                Column {
                    for (i in 0 until count) {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .height(48.dp)
                                .selectable(selected = i == current, role = Role.RadioButton) {
                                    onDismiss()
                                    menuOpen.value = false
                                    glView.queueEvent {
                                        toast(if (NativeLibrary.swapDisc(i)) "Disc ${i + 1} in" else "Couldn't swap disc")
                                    }
                                },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = i == current, onClick = null)
                            Spacer(Modifier.width(12.dp))
                            Text("Disc ${i + 1}")
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        )
    }
}
