package io.github.cloudwithax.nodosumi.ui

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Folder
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.SdStorage
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material.icons.rounded.SportsEsports
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SuggestionChip
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.cloudwithax.nodosumi.BuildConfig
import io.github.cloudwithax.nodosumi.R
import io.github.cloudwithax.nodosumi.data.Game
import io.github.cloudwithax.nodosumi.data.Games
import io.github.cloudwithax.nodosumi.data.Settings
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(onPlay: (Game) -> Unit, onSettings: () -> Unit) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var searching by rememberSaveable { mutableStateOf(false) }
    var query by rememberSaveable { mutableStateOf("") }
    var refresh by remember { mutableIntStateOf(0) }
    var games by remember { mutableStateOf<List<Game>?>(null) }
    val dir = Settings.gamesDir.value
    val scroll = TopAppBarDefaults.pinnedScrollBehavior()

    LaunchedEffect(dir, refresh) {
        games = null
        games = if (dir == null) emptyList() else withContext(Dispatchers.IO) { Games.scan(dir) }
    }

    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        topBar = {
            TopAppBar(
                scrollBehavior = scroll,
                title = {
                    if (searching && tab == 0) {
                        SearchField(query, { query = it })
                    } else {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Image(painterResource(R.drawable.logo), null, Modifier.size(36.dp))
                            Spacer(Modifier.width(10.dp))
                            Text(if (tab == 0) "Games" else "Options")
                        }
                    }
                },
                actions = {
                    if (tab == 0) {
                        if (searching) {
                            IconButton(onClick = { searching = false; query = "" }) {
                                Icon(Icons.Rounded.Close, "Close search")
                            }
                        } else {
                            IconButton(onClick = { searching = true }) {
                                Icon(Icons.Rounded.Search, "Search")
                            }
                            IconButton(onClick = { refresh++ }) {
                                Icon(Icons.Rounded.Refresh, "Rescan")
                            }
                        }
                    }
                },
            )
        },
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == 0,
                    onClick = { tab = 0 },
                    icon = { Icon(Icons.Rounded.SportsEsports, null) },
                    label = { Text("Games") },
                )
                NavigationBarItem(
                    selected = tab == 1,
                    onClick = { tab = 1; searching = false },
                    icon = { Icon(Icons.Rounded.Tune, null) },
                    label = { Text("Options") },
                )
            }
        },
    ) { padding ->
        Box(Modifier.padding(padding).fillMaxSize()) {
            if (tab == 0) {
                val shown = games?.let { list ->
                    if (query.isBlank()) list
                    else list.filter { it.title.contains(query.trim(), ignoreCase = true) }
                }
                GamesTab(shown, searching = query.isNotBlank(), onPlay = onPlay)
            } else {
                OptionsTab(onSettings)
            }
        }
    }
}

@Composable
private fun SearchField(query: String, onChange: (String) -> Unit) {
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    TextField(
        value = query,
        onValueChange = onChange,
        placeholder = { Text("Search games") },
        singleLine = true,
        keyboardOptions = KeyboardOptions.Default,
        colors = TextFieldDefaults.colors(
            focusedContainerColor = Color.Transparent,
            unfocusedContainerColor = Color.Transparent,
            focusedIndicatorColor = Color.Transparent,
            unfocusedIndicatorColor = Color.Transparent,
        ),
        modifier = Modifier.fillMaxWidth().focusRequester(focus),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GamesTab(games: List<Game>?, searching: Boolean, onPlay: (Game) -> Unit) {
    var details by remember { mutableStateOf<Game?>(null) }

    when {
        games == null -> LinearProgressIndicator(Modifier.fillMaxWidth())
        games.isEmpty() -> EmptyGames(searching)
        else -> LazyVerticalGrid(
            columns = GridCells.Adaptive(152.dp),
            contentPadding = PaddingValues(12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            modifier = Modifier.fillMaxSize(),
        ) {
            items(games, key = { it.path }) { game ->
                GameCard(game, onClick = { onPlay(game) }, onLongClick = { details = game })
            }
        }
    }

    details?.let { game ->
        ModalBottomSheet(onDismissRequest = { details = null }) {
            GameDetails(game, onPlay = { details = null; onPlay(game) })
        }
    }
}

@Composable
private fun EmptyGames(searching: Boolean) {
    val pick = rememberFolderPicker()
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Image(painterResource(R.drawable.logo), null, Modifier.size(96.dp).alpha(0.35f))
        Spacer(Modifier.height(16.dp))
        Text(
            if (searching) "No games match" else "No games found",
            style = MaterialTheme.typography.headlineSmall,
        )
        if (!searching) {
            Spacer(Modifier.height(8.dp))
            Text(
                Settings.gamesDir.value ?: "No games folder picked",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(20.dp))
            OutlinedButton(onClick = pick) { Text("Pick games folder") }
        }
    }
}

/** No box art to go on, so each game gets an ink tile with its initial and a turned ensō. */
@Composable
private fun GameCover(game: Game, modifier: Modifier = Modifier) {
    val turn = (game.title.hashCode() % 360).toFloat()
    Box(
        modifier
            .aspectRatio(1f)
            .clip(RoundedCornerShape(20.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh),
        contentAlignment = Alignment.Center,
    ) {
        Image(
            painterResource(R.drawable.logo),
            null,
            Modifier.fillMaxSize().padding(10.dp).rotate(turn).alpha(0.18f),
        )
        Text(
            game.title.firstOrNull { it.isLetterOrDigit() }?.uppercase() ?: "?",
            fontFamily = Oswald,
            fontSize = 52.sp,
            color = MaterialTheme.colorScheme.primary,
        )
        Text(
            game.format,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(10.dp)
                .clip(RoundedCornerShape(6.dp))
                .background(MaterialTheme.colorScheme.surfaceContainerLowest.copy(alpha = 0.7f))
                .padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameCard(game: Game, onClick: () -> Unit, onLongClick: () -> Unit) {
    Column(
        Modifier
            .clip(RoundedCornerShape(20.dp))
            .combinedClickable(onClick = onClick, onLongClick = onLongClick)
            .padding(4.dp),
    ) {
        GameCover(game)
        Spacer(Modifier.height(8.dp))
        Text(
            game.title,
            style = MaterialTheme.typography.titleSmall,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 4.dp),
        )
        if (game.tags.isNotEmpty()) {
            Text(
                game.tags.joinToString(" · "),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(horizontal = 4.dp),
            )
        }
    }
}

@Composable
private fun GameDetails(game: Game, onPlay: () -> Unit) {
    Column(Modifier.padding(start = 24.dp, end = 24.dp, bottom = 32.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            GameCover(game, Modifier.size(96.dp))
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(game.title, style = MaterialTheme.typography.headlineSmall)
                Spacer(Modifier.height(6.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    (game.tags + game.format).take(4).forEach {
                        SuggestionChip(onClick = {}, label = { Text(it) })
                    }
                }
            }
        }
        Spacer(Modifier.height(20.dp))
        Text(
            game.path,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            formatSize(game.size),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(20.dp))
        Button(onClick = onPlay, modifier = Modifier.fillMaxWidth()) {
            Icon(Icons.Rounded.PlayArrow, null)
            Spacer(Modifier.width(8.dp))
            Text("Play")
        }
    }
}

private fun formatSize(bytes: Long): String {
    val mb = bytes / (1024.0 * 1024.0)
    return if (mb >= 1024) String.format(Locale.US, "%.2f GB", mb / 1024)
    else String.format(Locale.US, "%.0f MB", mb)
}

@Composable
private fun OptionsTab(onSettings: () -> Unit) {
    val context = LocalContext.current
    val pickFolder = rememberFolderPicker()
    var bios by remember { mutableStateOf(installedBios(context)) }
    val pickBios = rememberBiosPicker { bios = installedBios(context) }
    val storage = rememberStorageAccess()
    var about by remember { mutableStateOf(false) }

    LazyColumn(contentPadding = PaddingValues(vertical = 8.dp)) {
        item {
            OptionRow(Icons.Rounded.Settings, "Settings", "System, graphics and controls", onSettings)
        }
        item {
            OptionRow(
                Icons.Rounded.Folder, "Games folder",
                Settings.gamesDir.value ?: "None picked", pickFolder,
            )
        }
        item {
            OptionRow(
                Icons.Rounded.Memory, "Install BIOS",
                if (bios.isEmpty()) "Using the built-in BIOS" else bios.joinToString(", ") + " installed",
                pickBios,
            )
        }
        if (!storage) {
            item {
                OptionRow(Icons.Rounded.SdStorage, "Storage access", "Needed to open your games") {
                    context.startActivity(io.github.cloudwithax.nodosumi.storageAccessIntent(context))
                }
            }
        }
        item {
            OptionRow(Icons.Rounded.Info, "About", "Nodosumi ${BuildConfig.VERSION_NAME}") { about = true }
        }
    }

    if (about) {
        AlertDialog(
            onDismissRequest = { about = false },
            icon = { Image(painterResource(R.drawable.logo), null, Modifier.size(72.dp)) },
            title = { Text("Nodosumi") },
            text = {
                Text(
                    "Version ${BuildConfig.VERSION_NAME}\n\n" +
                        "A Dreamcast emulator, descended from redream. GPLv3 or later.\n\n" +
                        "SEGA and Dreamcast are trademarks of SEGA Corporation. " +
                        "Nodosumi isn't affiliated with SEGA.",
                )
            },
            confirmButton = { TextButton(onClick = { about = false }) { Text("Close") } },
            dismissButton = {
                TextButton(onClick = {
                    context.startActivity(
                        Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/cloudwithax/nodosumi")),
                    )
                }) { Text("GitHub") }
            },
        )
    }
}

@Composable
fun OptionRow(icon: ImageVector, title: String, description: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Surface(
            shape = RoundedCornerShape(14.dp),
            color = MaterialTheme.colorScheme.primaryContainer,
            modifier = Modifier.size(44.dp),
        ) {
            Box(contentAlignment = Alignment.Center) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.onPrimaryContainer)
            }
        }
        Spacer(Modifier.width(16.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(
                description,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}
