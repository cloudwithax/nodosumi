package io.github.cloudwithax.nodosumi.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.selection.selectable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeTopAppBar
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import io.github.cloudwithax.nodosumi.data.CoreOption
import io.github.cloudwithax.nodosumi.data.Settings
import kotlin.math.roundToInt

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(onBack: () -> Unit) {
    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        topBar = {
            LargeTopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Rounded.ArrowBack, "Back")
                    }
                },
                scrollBehavior = scroll,
            )
        },
    ) { padding ->
        LazyColumn(contentPadding = PaddingValues(bottom = 24.dp), modifier = Modifier.padding(padding)) {
            item { Section("System") }
            item { ChoiceSetting(Settings.REGION, "The console's region, games from other regions may refuse to boot") }
            item { ChoiceSetting(Settings.LANGUAGE, "The language games that support several will use") }
            item { ChoiceSetting(Settings.BROADCAST, "The TV standard the console outputs") }
            item { ChoiceSetting(Settings.BIOS, "Start at the BIOS menu instead of the game, needs boot.bin") }

            item { Section("Graphics") }
            item { ChoiceSetting(Settings.ASPECT, "16:9 squeezes the picture for games with a widescreen mode") }
            item { ChoiceSetting(Settings.ACCURATE_DEPTH, "Slower, but less z-fighting in some games") }

            item { Section("Controls") }
            item {
                SwitchSetting(
                    "Touch controls", "Hidden on their own once a controller is used",
                    Settings.showOverlay.value, Settings::setShowOverlay,
                )
            }
            item {
                SliderSetting("Touch controls opacity", Settings.overlayOpacity.intValue, 10..100, "%") {
                    Settings.setOverlayOpacity(it)
                }
            }
            item {
                SliderSetting("Touch controls size", Settings.overlayScale.intValue, 50..150, "%") {
                    Settings.setOverlayScale(it)
                }
            }
            item {
                SwitchSetting(
                    "Haptic feedback", "Buzz when a touch button is pressed",
                    Settings.haptics.value, Settings::setHaptics,
                )
            }
            item {
                SwitchSetting(
                    "Rumble", "Use the Puru Puru Pack, on the controller or the phone",
                    Settings.rumble.value, Settings::setRumble,
                )
            }
        }
    }
}

@Composable
private fun Section(title: String) {
    Text(
        title.uppercase(),
        style = SectionStyle,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp, bottom = 8.dp),
    )
}

@Composable
private fun SettingRow(
    title: String,
    description: String,
    onClick: (() -> Unit)? = null,
    trailing: @Composable () -> Unit = {},
) {
    Row(
        Modifier
            .fillMaxWidth()
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(horizontal = 24.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(
                description,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.width(16.dp))
        trailing()
    }
}

@Composable
private fun ChoiceSetting(option: CoreOption, description: String) {
    var open by remember { mutableStateOf(false) }
    val value = Settings.core(option).value
    val label = option.labels[option.values.indexOf(value).coerceAtLeast(0)]

    SettingRow(option.title, description, onClick = { open = true }) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
    }

    if (open) {
        AlertDialog(
            onDismissRequest = { open = false },
            title = { Text(option.title) },
            text = {
                Column {
                    option.values.forEachIndexed { i, v ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .selectable(selected = v == value, role = Role.RadioButton) {
                                    Settings.setCore(option, v)
                                    open = false
                                }
                                .padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = v == value, onClick = null)
                            Spacer(Modifier.width(12.dp))
                            Text(option.labels[i], style = MaterialTheme.typography.bodyLarge)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { open = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun SwitchSetting(title: String, description: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    SettingRow(title, description, onClick = { onChange(!checked) }) {
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun SliderSetting(title: String, value: Int, range: IntRange, unit: String, onChange: (Int) -> Unit) {
    // only saved once the finger lifts
    var live by remember(value) { mutableFloatStateOf(value.toFloat()) }
    Column(Modifier.padding(horizontal = 24.dp, vertical = 10.dp)) {
        Row {
            Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            Text(
                "${live.roundToInt()}$unit",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        Slider(
            value = live,
            onValueChange = { live = it },
            onValueChangeFinished = { onChange(live.roundToInt()) },
            valueRange = range.first.toFloat()..range.last.toFloat(),
        )
    }
}
