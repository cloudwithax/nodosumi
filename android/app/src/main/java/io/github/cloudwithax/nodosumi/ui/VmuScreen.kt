package io.github.cloudwithax.nodosumi.ui

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.FileDownload
import androidx.compose.material.icons.rounded.FileUpload
import androidx.compose.material.icons.rounded.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.github.cloudwithax.nodosumi.data.Games
import io.github.cloudwithax.nodosumi.data.VmuCard
import io.github.cloudwithax.nodosumi.data.VmuSave
import io.github.cloudwithax.nodosumi.emu.EmulationActivity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.format.DateTimeFormatter
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

// the browser keeps to the night it floats in, whatever the app's theme
private val Glow = Color(0xFF6F8BFF)
private val Text1 = Color(0xFFEFF2FF)
private val Text2 = Color(0xFF9AA3C7)

/**
 * The memory card browser, after the ps2's: each game's card floats in the
 * dark, and opening one shows the saves on it.
 */
@Composable
fun VmuScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var cards by remember { mutableStateOf<List<VmuCard>?>(null) }
    var open by remember { mutableStateOf<VmuCard?>(null) }
    // bumped after an edit, for the saves to be read again
    var version by remember { mutableIntStateOf(0) }

    LaunchedEffect(version) {
        cards = withContext(Dispatchers.IO) { VmuCard.cards(context) }
        open = open?.let { o -> cards?.find { it.file == o.file } }
    }

    BackHandler { if (open != null) open = null else onBack() }

    fun edit(card: VmuCard, done: String?, block: (VmuCard) -> String?) = scope.launch {
        val error = withContext(Dispatchers.IO) {
            block(card).also { if (it == null) card.save() }
        }
        toast(context, error ?: done ?: return@launch)
        version++
    }

    Box(Modifier.fillMaxSize()) {
        Night()
        AnimatedContent(
            targetState = open,
            transitionSpec = { (fadeIn(tween(350)) + scaleIn(tween(350), 0.94f)) togetherWith fadeOut(tween(200)) },
            label = "card",
        ) { card ->
            if (card == null) {
                CardsView(cards, onBack, onOpen = { open = it })
            } else {
                SavesView(card, cards.orEmpty(), version, onBack = { open = null }, onEdit = ::edit)
            }
        }
    }
}

@Composable
private fun CardsView(cards: List<VmuCard>?, onBack: () -> Unit, onOpen: (VmuCard) -> Unit) {
    var selected by remember { mutableIntStateOf(0) }
    val card = cards?.getOrNull(selected)

    Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
        Header("Memory Cards", "Visual Memory · A1", onBack) {}
        Caption(
            card?.title ?: "",
            card?.let { "${plural(it.saves.size, "save")} · ${plural(it.freeBlocks, "block")} free" } ?: "",
        )
        when {
            cards == null -> Unit
            cards.isEmpty() -> Empty("No memory cards yet", "Each game makes its own card the first time it saves.")
            else -> FloatGrid(cards.size, selected, { selected = it }, onClick = { onOpen(cards[it]) }) { i, active ->
                val c = cards[i]
                val first = remember(c) { c.saves.firstOrNull() }
                Floating(i, active, label = c.title) { Vmu(first?.icons.orEmpty(), first?.frameMs ?: 0, it) }
            }
        }
    }
}

@Composable
private fun SavesView(
    card: VmuCard,
    cards: List<VmuCard>,
    version: Int,
    onBack: () -> Unit,
    onEdit: (VmuCard, String?, (VmuCard) -> String?) -> Unit,
) {
    val context = LocalContext.current
    val saves = remember(card, version) { card.saves }
    var selected by remember(card) { mutableIntStateOf(0) }
    val save = saves.getOrNull(selected.coerceAtMost(saves.lastIndex))
    var dialog by remember { mutableStateOf<String?>(null) }
    var menu by remember { mutableStateOf(false) }
    // the running game reads its card as it goes, so it's left alone
    val inUse = EmulationActivity.loaded?.let { Games.cardName(it) == card.name && card.name.isNotEmpty() } == true

    val exportSave = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null && save != null) write(context, uri, card.export(save), "Exported ${save.title}")
    }
    val importSave = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) onEdit(card, "Imported") { it.import(read(context, uri) ?: return@onEdit "Couldn't read that file") }
    }
    val backup = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) write(context, uri, card.bytes(), "Backed up ${card.title}")
    }
    val restore = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) onEdit(card, "Restored ${card.title}") {
            if (it.restore(read(context, uri) ?: ByteArray(0))) null else "That isn't a VMU image, they're 128KB"
        }
    }

    Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
        Header(card.title, "${plural(card.freeBlocks, "block")} free of ${VmuCard.USER_BLOCKS}", onBack) {
            Meter(VmuCard.USER_BLOCKS - card.freeBlocks, VmuCard.USER_BLOCKS)
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Rounded.MoreVert, "More", tint = Text1) }
                DropdownMenu(menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem({ Text("Import save") }, enabled = !inUse, onClick = { menu = false; importSave.launch(arrayOf("*/*")) })
                    DropdownMenuItem({ Text("Back up card") }, onClick = { menu = false; backup.launch("${card.name.ifEmpty { "shared" }}.bin") })
                    DropdownMenuItem({ Text("Restore card") }, enabled = !inUse, onClick = { menu = false; dialog = "restore" })
                    DropdownMenuItem({ Text("Format card") }, enabled = !inUse, onClick = { menu = false; dialog = "format" })
                }
            }
        }
        Caption(
            save?.title ?: "",
            save?.let { listOfNotNull(it.description, plural(it.blocks, "block"), it.date?.format(DATES)).joinToString(" · ") } ?: "",
        )
        if (inUse) {
            Text(
                "In use by the running game, exit it to make changes",
                color = Color(0xFFFFB38F),
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(horizontal = 28.dp),
            )
        }

        Box(Modifier.weight(1f)) {
            if (saves.isEmpty()) {
                Empty("This card is empty", "Saves show up here once ${card.title} makes one.")
            } else {
                FloatGrid(saves.size, selected, { selected = it }, onClick = { selected = it }) { i, active ->
                    val s = saves[i]
                    Floating(i, active, label = s.title) { size -> Icon(s.icons, s.frameMs, size) }
                }
            }
        }

        if (save != null) {
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.End),
            ) {
                Action(Icons.Rounded.ContentCopy, "Copy", enabled = cards.size > 1) { dialog = "copy" }
                Action(Icons.Rounded.FileUpload, "Export") { exportSave.launch("${save.filename}.dci") }
                Action(Icons.Rounded.Delete, "Delete", enabled = !inUse) { dialog = "delete" }
            }
        }
    }

    when (dialog) {
        "delete" -> save?.let {
            Confirm("Delete ${it.title}?", "It can't be brought back unless you've exported it.", "Delete", { dialog = null }) {
                onEdit(card, "Deleted ${it.title}") { c -> c.delete(it); null }
            }
        }
        "format" -> Confirm("Format ${card.title}?", "Every save on it is erased. Back it up first if you might want them.", "Format", { dialog = null }) {
            onEdit(card, "Formatted ${card.title}") { it.format(); null }
        }
        "restore" -> Confirm("Restore ${card.title}?", "The card is replaced with a backup, its saves now are lost.", "Pick backup", { dialog = null }) {
            restore.launch(arrayOf("*/*"))
        }
        "copy" -> save?.let { CopyDialog(it, card, cards, { dialog = null }, onEdit) }
    }
}

@Composable
private fun CopyDialog(
    save: VmuSave,
    from: VmuCard,
    cards: List<VmuCard>,
    onDismiss: () -> Unit,
    onEdit: (VmuCard, String?, (VmuCard) -> String?) -> Unit,
) {
    val running = EmulationActivity.loaded?.let { Games.cardName(it) }
    val targets = cards.filter { it.file != from.file && it.name != running }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Copy ${save.title} to") },
        text = {
            Column {
                for (to in targets) {
                    Row(
                        Modifier.fillMaxWidth().heightIn(min = 52.dp).selectable(false, role = Role.RadioButton) {
                            onDismiss()
                            val dci = from.export(save)
                            onEdit(to, "Copied to ${to.title}") { it.import(dci) }
                        },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(false, onClick = null)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(to.title)
                            val replaces = to.saves.any { it.filename == save.filename }
                            Text(
                                if (replaces) "Replaces the save there" else "${plural(to.freeBlocks, "block")} free",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/*
 * pieces
 */
@Composable
private fun Header(title: String, subtitle: String, onBack: () -> Unit, actions: @Composable () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Rounded.ArrowBack, "Back", tint = Text1) }
        Spacer(Modifier.width(8.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.headlineMedium, color = Text1, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(subtitle, style = MaterialTheme.typography.labelLarge, color = Text2)
        }
        actions()
    }
}

/** What the selected item is, in the space above the grid. */
@Composable
private fun Caption(title: String, detail: String) {
    AnimatedContent(title to detail, transitionSpec = { fadeIn(tween(220)) togetherWith fadeOut(tween(120)) }, label = "caption") { (t, d) ->
        Column(Modifier.fillMaxWidth().padding(horizontal = 28.dp, vertical = 14.dp).heightIn(min = 56.dp)) {
            Text(t, style = MaterialTheme.typography.titleLarge, color = Text1, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(d, style = MaterialTheme.typography.bodyMedium, color = Text2, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun Meter(used: Int, total: Int) {
    Box(
        Modifier.padding(horizontal = 12.dp).size(120.dp, 6.dp)
            .background(Color.White.copy(alpha = 0.1f), RoundedCornerShape(3.dp)),
    ) {
        Box(
            Modifier.fillMaxWidth(used / total.toFloat()).height(6.dp)
                .background(Brush.horizontalGradient(listOf(Glow, Color(0xFFB7C6FF))), RoundedCornerShape(3.dp)),
        )
    }
}

@Composable
private fun Action(icon: androidx.compose.ui.graphics.vector.ImageVector, label: String, enabled: Boolean = true, onClick: () -> Unit) {
    FilledTonalButton(onClick = onClick, enabled = enabled) {
        Icon(icon, null, Modifier.size(18.dp))
        Spacer(Modifier.width(8.dp))
        Text(label)
    }
}

@Composable
private fun Empty(title: String, detail: String) {
    Column(Modifier.fillMaxSize().padding(32.dp), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(title, style = MaterialTheme.typography.titleLarge, color = Text1)
        Spacer(Modifier.height(6.dp))
        Text(detail, style = MaterialTheme.typography.bodyMedium, color = Text2, textAlign = TextAlign.Center)
    }
}

@Composable
private fun Confirm(title: String, text: String, confirm: String, onDismiss: () -> Unit, onConfirm: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = { TextButton(onClick = { onDismiss(); onConfirm() }) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Items that follow focus, so a controller's d-pad moves the selection. */
@Composable
private fun FloatGrid(
    count: Int,
    selected: Int,
    onSelect: (Int) -> Unit,
    onClick: (Int) -> Unit,
    item: @Composable (Int, Boolean) -> Unit,
) {
    // a controller starts on the items rather than the back button
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    LazyVerticalGrid(
        columns = GridCells.Adaptive(150.dp),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
        modifier = Modifier.fillMaxSize(),
    ) {
        itemsIndexed(List(count) { it }) { i, _ ->
            Box(
                Modifier
                    .then(if (i == selected) Modifier.focusRequester(focus) else Modifier)
                    .onFocusChanged { if (it.isFocused) onSelect(i) }
                    .clickable(remember { MutableInteractionSource() }, indication = null) {
                        if (i == selected) onClick(i) else onSelect(i)
                    }
                    .focusable()
                    .padding(vertical = 8.dp),
                contentAlignment = Alignment.Center,
            ) { item(i, i == selected) }
        }
    }
}

/** An icon bobbing over its reflection, brighter and nearer when selected. */
@Composable
private fun Floating(index: Int, active: Boolean, label: String, icon: @Composable (Dp) -> Unit) {
    val t by rememberInfiniteTransition(label = "bob").animateFloat(
        0f, 1f, infiniteRepeatable(tween(4200, easing = LinearEasing)), label = "t",
    )
    val scale by animateFloatAsState(if (active) 1.14f else 0.9f, tween(260), label = "scale")
    val alpha by animateFloatAsState(if (active) 1f else 0.5f, tween(260), label = "alpha")
    val bob = sin((t + index * 0.17f) * 2 * PI).toFloat() * if (active) 6f else 3f
    val size = 96.dp

    Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.graphicsLayer { this.alpha = alpha }) {
        Box(
            Modifier.size(size + 24.dp).graphicsLayer { scaleX = scale; scaleY = scale; translationY = bob * density },
            contentAlignment = Alignment.Center,
        ) {
            if (active) {
                Canvas(Modifier.fillMaxSize()) {
                    drawCircle(Brush.radialGradient(listOf(Glow.copy(alpha = 0.45f), Color.Transparent)), radius = this.size.minDimension / 1.6f)
                }
            }
            icon(size)
        }
        // the floor's reflection, mirrored and fading out
        Box(Modifier.size(size + 24.dp, 30.dp).clipToBounds()) {
            Box(
                Modifier
                    .size(size + 24.dp)
                    .graphicsLayer {
                        scaleX = scale
                        scaleY = -scale
                        translationY = -bob * density - 12.dp.toPx()
                        this.alpha = 0.4f
                        compositingStrategy = CompositingStrategy.Offscreen
                    }
                    .drawWithContent {
                        drawContent()
                        drawRect(
                            Brush.verticalGradient(0.4f to Color.Transparent, 0.9f to Color.Black),
                            blendMode = BlendMode.DstIn,
                        )
                    },
                contentAlignment = Alignment.Center,
            ) { icon(size) }
        }
        Text(
            label,
            style = MaterialTheme.typography.labelLarge,
            color = if (active) Text1 else Text2,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center,
            modifier = Modifier.width(140.dp),
        )
    }
}

/** A save's icon, its frames played in turn, kept to its pixels. */
@Composable
private fun Icon(frames: List<android.graphics.Bitmap>, frameMs: Long, size: Dp) {
    val images = remember(frames) { frames.map { it.asImageBitmap() } }
    var frame by remember(frames) { mutableIntStateOf(0) }
    LaunchedEffect(images) {
        while (images.size > 1) {
            delay(frameMs)
            frame = (frame + 1) % images.size
        }
    }
    val image = images.getOrNull(frame)
    if (image == null) {
        Box(Modifier.size(size), contentAlignment = Alignment.Center) {
            Text("?", style = MaterialTheme.typography.displaySmall, color = Text2)
        }
    } else {
        Pixels(image, Modifier.size(size))
    }
}

@Composable
private fun Pixels(image: ImageBitmap, modifier: Modifier) =
    Image(image, null, modifier, contentScale = ContentScale.Fit, filterQuality = FilterQuality.None)

/** A visual memory unit, its screen showing the card's first save. */
@Composable
private fun Vmu(frames: List<android.graphics.Bitmap>, frameMs: Long, size: Dp) {
    Box(Modifier.size(size * 0.78f, size), contentAlignment = Alignment.TopCenter) {
        Canvas(Modifier.fillMaxSize()) { drawVmu() }
        Box(
            Modifier.padding(top = size * 0.16f).size(size * 0.5f, size * 0.38f),
            contentAlignment = Alignment.Center,
        ) {
            if (frames.isNotEmpty()) Icon(frames, frameMs, size * 0.36f)
        }
    }
}

private fun DrawScope.drawVmu() {
    val w = size.width
    val h = size.height
    val r = CornerRadius(w * 0.16f)
    drawRoundRect(Brush.verticalGradient(listOf(Color(0xFFF4F5FA), Color(0xFFBFC3D2))), cornerRadius = r)
    drawRoundRect(Color.White.copy(alpha = 0.6f), Offset(w * 0.04f, h * 0.02f), Size(w * 0.92f, h * 0.05f), CornerRadius(w * 0.03f))
    // the screen's bezel and glass
    drawRoundRect(Color(0xFF2A2E3D), Offset(w * 0.14f, h * 0.12f), Size(w * 0.72f, h * 0.46f), CornerRadius(w * 0.06f))
    drawRoundRect(
        Brush.verticalGradient(listOf(Color(0xFFB9D3C0), Color(0xFF8FB39C))),
        Offset(w * 0.18f, h * 0.15f), Size(w * 0.64f, h * 0.40f), CornerRadius(w * 0.03f),
    )
    // d-pad and the a and b buttons
    val pad = Color(0xFF3B4052)
    val cx = w * 0.3f
    val cy = h * 0.76f
    val arm = w * 0.09f
    drawRoundRect(pad, Offset(cx - arm * 1.5f, cy - arm / 2), Size(arm * 3, arm), CornerRadius(arm / 3))
    drawRoundRect(pad, Offset(cx - arm / 2, cy - arm * 1.5f), Size(arm, arm * 3), CornerRadius(arm / 3))
    drawCircle(pad, w * 0.075f, Offset(w * 0.67f, h * 0.80f))
    drawCircle(pad, w * 0.075f, Offset(w * 0.82f, h * 0.71f))
}

/** The dark the cards float in, lit by a few slow drifting glows. */
@Composable
private fun Night() {
    val t by rememberInfiniteTransition(label = "night").animateFloat(
        0f, 1f, infiniteRepeatable(tween(40000, easing = LinearEasing)), label = "t",
    )
    Canvas(Modifier.fillMaxSize()) {
        drawRect(Brush.verticalGradient(listOf(Color(0xFF04050C), Color(0xFF0A1030), Color(0xFF03040A))))
        val a = t * 2 * PI.toFloat()
        fun glow(x: Float, y: Float, radius: Float, color: Color) {
            val c = Offset(size.width * x, size.height * y)
            drawCircle(Brush.radialGradient(listOf(color, Color.Transparent), c, size.maxDimension * radius), size.maxDimension * radius, c)
        }
        glow(0.25f + 0.1f * sin(a), 0.3f + 0.08f * cos(a), 0.45f, Color(0xFF2B3E9E).copy(alpha = 0.35f))
        glow(0.78f + 0.08f * cos(a * 2), 0.6f + 0.1f * sin(a), 0.4f, Color(0xFF3E2B8E).copy(alpha = 0.25f))
        glow(0.55f + 0.12f * sin(a * 3), 0.95f, 0.35f, Shu.copy(alpha = 0.08f))
        // the floor's horizon
        drawRect(
            Brush.verticalGradient(0f to Color.Transparent, 0.5f to Glow.copy(alpha = 0.05f), 1f to Color.Transparent),
            topLeft = Offset(0f, size.height * 0.62f),
            size = Size(size.width, size.height * 0.2f),
        )
    }
}

/*
 * helpers
 */
private val DATES = DateTimeFormatter.ofPattern("d MMM yyyy, HH:mm")

private fun plural(n: Int, word: String) = "$n $word${if (n == 1) "" else "s"}"

private fun toast(context: Context, text: String) = Toast.makeText(context, text, Toast.LENGTH_SHORT).show()

private fun read(context: Context, uri: Uri): ByteArray? =
    runCatching { context.contentResolver.openInputStream(uri)?.use { it.readBytes() } }.getOrNull()

private fun write(context: Context, uri: Uri, bytes: ByteArray, done: String) {
    val ok = runCatching { context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(bytes) } != null }.getOrDefault(false)
    toast(context, if (ok) done else "Couldn't write the file")
}
