package io.github.cloudwithax.nodosumi.data

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import java.io.File

/** A choice the core takes as a libretro variable. */
class CoreOption(
    val key: String,
    val title: String,
    val values: List<String>,
    val labels: List<String>,
    val default: String,
)

object Settings {
    val REGION = CoreOption(
        "nodosumi_region", "Region",
        listOf("japan", "usa", "europe"), listOf("Japan", "USA", "Europe"), "usa",
    )
    val LANGUAGE = CoreOption(
        "nodosumi_language", "Language",
        listOf("japanese", "english", "german", "french", "spanish", "italian"),
        listOf("Japanese", "English", "German", "French", "Spanish", "Italian"), "english",
    )
    val BROADCAST = CoreOption(
        "nodosumi_broadcast", "Broadcast",
        listOf("ntsc", "pal", "pal_m", "pal_n"), listOf("NTSC", "PAL", "PAL-M", "PAL-N"), "ntsc",
    )
    val BIOS = CoreOption(
        "nodosumi_bios", "Boot to BIOS",
        listOf("disabled", "enabled"), listOf("Off", "On"), "disabled",
    )
    val ASPECT = CoreOption(
        "nodosumi_aspect", "Aspect ratio",
        listOf("4:3", "16:9", "stretch"), listOf("4:3", "16:9", "Stretch"), "4:3",
    )
    val ACCURATE_DEPTH = CoreOption(
        "nodosumi_accurate_depth", "Accurate depth",
        listOf("disabled", "enabled"), listOf("Off", "On"), "disabled",
    )

    val CORE_OPTIONS = listOf(REGION, LANGUAGE, BROADCAST, BIOS, ASPECT, ACCURATE_DEPTH)

    private lateinit var prefs: SharedPreferences

    // compose reads these, so changes show up without a reload
    val setupDone = mutableStateOf(false)
    val gamesDir = mutableStateOf<String?>(null)
    val showOverlay = mutableStateOf(true)
    val overlayOpacity = mutableIntStateOf(60)
    val overlayScale = mutableIntStateOf(100)
    val haptics = mutableStateOf(true)
    val rumble = mutableStateOf(true)
    private val core = mutableMapOf<String, androidx.compose.runtime.MutableState<String>>()

    fun init(context: Context) {
        if (::prefs.isInitialized) return
        prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        setupDone.value = prefs.getBoolean("setup_done", false)
        gamesDir.value = prefs.getString("games_dir", null)
        showOverlay.value = prefs.getBoolean("show_overlay", true)
        overlayOpacity.intValue = prefs.getInt("overlay_opacity", 60)
        overlayScale.intValue = prefs.getInt("overlay_scale", 100)
        haptics.value = prefs.getBoolean("haptics", true)
        rumble.value = prefs.getBoolean("rumble", true)
        for (opt in CORE_OPTIONS) {
            core[opt.key] = mutableStateOf(prefs.getString(opt.key, opt.default)!!)
        }
    }

    fun core(opt: CoreOption) = core.getValue(opt.key)

    fun setCore(opt: CoreOption, value: String) {
        core.getValue(opt.key).value = value
        prefs.edit().putString(opt.key, value).apply()
    }

    fun setSetupDone() {
        setupDone.value = true
        prefs.edit().putBoolean("setup_done", true).apply()
    }

    fun setGamesDir(path: String) {
        gamesDir.value = path
        prefs.edit().putString("games_dir", path).apply()
    }

    fun setShowOverlay(value: Boolean) {
        showOverlay.value = value
        prefs.edit().putBoolean("show_overlay", value).apply()
    }

    fun setOverlayOpacity(value: Int) {
        overlayOpacity.intValue = value
        prefs.edit().putInt("overlay_opacity", value).apply()
    }

    fun setOverlayScale(value: Int) {
        overlayScale.intValue = value
        prefs.edit().putInt("overlay_scale", value).apply()
    }

    fun setHaptics(value: Boolean) {
        haptics.value = value
        prefs.edit().putBoolean("haptics", value).apply()
    }

    fun setRumble(value: Boolean) {
        rumble.value = value
        prefs.edit().putBoolean("rumble", value).apply()
    }

    /** The libretro system directory, the core keeps its files under dc/ in it. */
    fun systemDir(context: Context) = File(context.filesDir, "system").apply { mkdirs() }

    fun biosDir(context: Context) = File(systemDir(context), "dc").apply { mkdirs() }

    fun statesDir(context: Context) = File(context.filesDir, "states").apply { mkdirs() }
}
