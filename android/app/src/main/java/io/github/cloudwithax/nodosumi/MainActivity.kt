package io.github.cloudwithax.nodosumi

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.getValue
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import io.github.cloudwithax.nodosumi.data.Game
import io.github.cloudwithax.nodosumi.data.Settings
import io.github.cloudwithax.nodosumi.emu.EmulationActivity
import io.github.cloudwithax.nodosumi.ui.HomeScreen
import io.github.cloudwithax.nodosumi.ui.NodosumiTheme
import io.github.cloudwithax.nodosumi.ui.SettingsScreen
import io.github.cloudwithax.nodosumi.ui.SetupScreen
import java.io.File

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        Settings.init(this)

        setContent {
            NodosumiTheme {
                val nav = rememberNavController()
                val start = if (Settings.setupDone.value) "home" else "setup"
                NavHost(nav, startDestination = start) {
                    composable("setup") {
                        SetupScreen(onDone = {
                            Settings.setSetupDone()
                            nav.navigate("home") { popUpTo("setup") { inclusive = true } }
                        })
                    }
                    composable("home") {
                        HomeScreen(
                            onPlay = { launch(it) },
                            onSettings = { nav.navigate("settings") },
                        )
                    }
                    composable("settings") {
                        SettingsScreen(onBack = { nav.popBackStack() })
                    }
                }
            }
        }
    }

    private fun launch(game: Game) {
        startActivity(
            Intent(this, EmulationActivity::class.java)
                .putExtra(EmulationActivity.EXTRA_PATH, game.path)
                .putExtra(EmulationActivity.EXTRA_TITLE, game.title),
        )
    }
}

fun hasStorageAccess() = Environment.isExternalStorageManager()

fun storageAccessIntent(context: Context) =
    Intent(
        android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
        Uri.parse("package:${context.packageName}"),
    )

/** BIOS files the core looks for in system/dc. */
val BIOS_FILES = listOf("boot.bin", "flash.bin")

/**
 * Copies a picked file into the core's BIOS folder. It's filed by its own name
 * when that's one the core knows, else by size: the boot rom is 2MB, flash 128KB.
 */
fun importBios(context: Context, uri: Uri): String? {
    val resolver = context.contentResolver
    var name: String? = null
    var size = -1L
    resolver.query(uri, null, null, null, null)?.use {
        if (it.moveToFirst()) {
            name = it.getString(it.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME))
            size = it.getLong(it.getColumnIndexOrThrow(OpenableColumns.SIZE))
        }
    }
    val target = name?.lowercase()?.takeIf { it in BIOS_FILES }
        ?: when (size) {
            2L * 1024 * 1024 -> "boot.bin"
            128L * 1024 -> "flash.bin"
            else -> return null
        }
    val out = File(Settings.biosDir(context), target)
    resolver.openInputStream(uri)?.use { input -> out.outputStream().use { input.copyTo(it) } }
        ?: return null
    return target
}
