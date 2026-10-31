package io.github.cloudwithax.nodosumi.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Folder
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.SdStorage
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import io.github.cloudwithax.nodosumi.BIOS_FILES
import io.github.cloudwithax.nodosumi.R
import io.github.cloudwithax.nodosumi.data.Games
import io.github.cloudwithax.nodosumi.data.Settings
import io.github.cloudwithax.nodosumi.hasStorageAccess
import io.github.cloudwithax.nodosumi.importBios
import io.github.cloudwithax.nodosumi.storageAccessIntent
import java.io.File

/** True while the screen is resumed, rechecked whenever the user comes back from system settings. */
@Composable
fun rememberStorageAccess(): Boolean {
    var granted by remember { mutableStateOf(hasStorageAccess()) }
    val owner = LocalLifecycleOwner.current
    DisposableEffect(owner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) granted = hasStorageAccess()
        }
        owner.lifecycle.addObserver(observer)
        onDispose { owner.lifecycle.removeObserver(observer) }
    }
    return granted
}

@Composable
fun rememberFolderPicker(): () -> Unit {
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        val path = uri?.let { Games.treeToPath(it) } ?: return@rememberLauncherForActivityResult
        Settings.setGamesDir(path)
    }
    return { picker.launch(null) }
}

@Composable
fun rememberBiosPicker(onImported: () -> Unit = {}): () -> Unit {
    val context = LocalContext.current
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        val name = importBios(context, uri)
        Toast.makeText(
            context,
            if (name != null) "Installed $name" else "That doesn't look like boot.bin or flash.bin",
            Toast.LENGTH_SHORT,
        ).show()
        onImported()
    }
    return { picker.launch(arrayOf("*/*")) }
}

fun installedBios(context: android.content.Context) =
    BIOS_FILES.filter { File(Settings.biosDir(context), it).exists() }

@Composable
fun SetupScreen(onDone: () -> Unit) {
    var page by rememberSaveable { mutableIntStateOf(0) }
    val pages = 4

    Surface(Modifier.fillMaxSize()) {
        Box(Modifier.safeDrawingPadding()) {
            AnimatedContent(
                targetState = page,
                transitionSpec = {
                    val dir = if (targetState > initialState) 1 else -1
                    (slideInHorizontally { it * dir / 4 } + fadeIn()) togetherWith
                        (slideOutHorizontally { -it * dir / 4 } + fadeOut())
                },
                modifier = Modifier.fillMaxSize().padding(bottom = 80.dp),
                label = "setup",
            ) { p ->
                when (p) {
                    0 -> WelcomePage()
                    1 -> StoragePage()
                    2 -> FolderPage()
                    else -> BiosPage()
                }
            }

            Row(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .padding(horizontal = 24.dp, vertical = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (page > 0) {
                    TextButton(onClick = { page-- }) { Text("Back") }
                } else {
                    Spacer(Modifier.size(1.dp))
                }
                Spacer(Modifier.weight(1f))
                PageDots(page, pages)
                Spacer(Modifier.weight(1f))
                val canContinue = when (page) {
                    1 -> rememberStorageAccess()
                    2 -> Settings.gamesDir.value != null
                    else -> true
                }
                Button(
                    onClick = { if (page == pages - 1) onDone() else page++ },
                    enabled = canContinue,
                ) {
                    Text(if (page == pages - 1) "Start playing" else "Next")
                }
            }
        }
    }
}

@Composable
private fun PageDots(page: Int, count: Int) {
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        repeat(count) {
            Box(
                Modifier
                    .size(if (it == page) 10.dp else 6.dp)
                    .clip(CircleShape)
                    .background(
                        if (it == page) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.outlineVariant,
                    ),
            )
        }
    }
}

@Composable
private fun SetupPage(
    title: String,
    body: String,
    icon: @Composable () -> Unit,
    content: @Composable () -> Unit = {},
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        // short landscape screens can't stack it all, put the icon beside the text
        val wide = maxHeight < 560.dp && maxWidth > maxHeight
        val scroll = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(32.dp)
        if (wide) {
            Row(
                scroll,
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                icon()
                Spacer(Modifier.width(48.dp))
                Column(Modifier.widthIn(max = 480.dp)) {
                    PageText(title, body, TextAlign.Start)
                    content()
                }
            }
        } else {
            Column(
                scroll,
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                Column(Modifier.widthIn(max = 480.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    icon()
                    Spacer(Modifier.height(24.dp))
                    PageText(title, body, TextAlign.Center)
                    content()
                }
            }
        }
    }
}

@Composable
private fun PageText(title: String, body: String, align: TextAlign) {
    Text(title, style = MaterialTheme.typography.headlineMedium, textAlign = align)
    Spacer(Modifier.height(12.dp))
    Text(
        body,
        style = MaterialTheme.typography.bodyLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = align,
    )
    Spacer(Modifier.height(28.dp))
}

@Composable
private fun PageIcon(icon: ImageVector) {
    Box(
        Modifier
            .size(88.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.primaryContainer),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, null, Modifier.size(44.dp), tint = MaterialTheme.colorScheme.onPrimaryContainer)
    }
}

@Composable
private fun Done(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(Icons.Rounded.CheckCircle, null, tint = MaterialTheme.colorScheme.primary)
        Spacer(Modifier.size(8.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun WelcomePage() {
    SetupPage(
        title = "Welcome to Nodosumi",
        body = "A Dreamcast emulator. A few quick steps and you're playing.",
        icon = { Image(painterResource(R.drawable.logo), null, Modifier.size(160.dp)) },
    )
}

@Composable
private fun StoragePage() {
    val context = LocalContext.current
    val granted = rememberStorageAccess()
    SetupPage(
        title = "Storage access",
        body = "Nodosumi reads disc images straight from where they live, track files and all. " +
            "Android needs you to allow access to all files for that.",
        icon = { PageIcon(Icons.Rounded.SdStorage) },
    ) {
        if (granted) {
            Done("Access granted")
        } else {
            Button(onClick = { context.startActivity(storageAccessIntent(context)) }) {
                Text("Allow access")
            }
        }
    }
}

@Composable
private fun FolderPage() {
    val pick = rememberFolderPicker()
    val dir = Settings.gamesDir.value
    SetupPage(
        title = "Games folder",
        body = "Pick the folder with your .chd, .gdi, .cdi, .cue or .m3u files. Subfolders are searched too.",
        icon = { PageIcon(Icons.Rounded.Folder) },
    ) {
        if (dir != null) {
            Done(dir)
            Spacer(Modifier.height(16.dp))
            OutlinedButton(onClick = pick) { Text("Change folder") }
        } else {
            Button(onClick = pick) { Text("Pick folder") }
        }
    }
}

@Composable
private fun BiosPage() {
    val context = LocalContext.current
    var installed by remember { mutableStateOf(installedBios(context)) }
    val pick = rememberBiosPicker { installed = installedBios(context) }
    SetupPage(
        title = "BIOS (optional)",
        body = "Games boot without one using Nodosumi's built-in BIOS. Add your console's boot.bin " +
            "for the startup animation and the BIOS menu.",
        icon = { PageIcon(Icons.Rounded.Memory) },
    ) {
        installed.forEach {
            Done("$it installed")
            Spacer(Modifier.height(8.dp))
        }
        Spacer(Modifier.height(8.dp))
        OutlinedButton(onClick = pick) { Text("Install BIOS file") }
    }
}
