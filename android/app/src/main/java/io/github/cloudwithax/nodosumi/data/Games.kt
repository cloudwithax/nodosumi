package io.github.cloudwithax.nodosumi.data

import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import java.io.File

data class Game(
    val title: String,
    val tags: List<String>,
    val path: String,
    val format: String,
    val size: Long,
)

object Games {
    val EXTENSIONS = setOf("cdi", "chd", "gdi", "cue", "m3u")

    // gdi dumps are often a folder with a generic disc.gdi in it
    private val GENERIC_NAMES = setOf("disc", "game", "track", "image")

    /** The filesystem path behind a document tree the user picked, for the core to open directly. */
    fun treeToPath(uri: Uri): String? {
        val id = DocumentsContract.getTreeDocumentId(uri) ?: return null
        val volume = id.substringBefore(':')
        val rest = id.substringAfter(':', "")
        val root = if (volume == "primary") {
            Environment.getExternalStorageDirectory().path
        } else {
            "/storage/$volume"
        }
        return if (rest.isEmpty()) root else "$root/$rest"
    }

    fun scan(dir: String): List<Game> {
        val files = File(dir).walkTopDown()
            .maxDepth(4)
            .onEnter { !it.name.startsWith(".") }
            .filter { it.isFile && it.extension.lowercase() in EXTENSIONS }
            .toList()

        // a playlist stands in for the discs it lists
        val inPlaylists = files.filter { it.extension.equals("m3u", true) }
            .flatMap { m3u ->
                runCatching { m3u.readLines() }.getOrDefault(emptyList())
                    .map { it.trim() }
                    .filter { it.isNotEmpty() && !it.startsWith("#") }
                    .map { File(if (it.startsWith("/")) it else "${m3u.parent}/$it").canonicalPath }
            }
            .toSet()

        return files
            .filter { it.canonicalPath !in inPlaylists }
            .map { toGame(it) }
            .sortedBy { it.title.lowercase() }
    }

    /** The game's name before its tags are taken off, which the core also names its card by. */
    fun cardName(path: String): String {
        val file = File(path)
        val name = file.nameWithoutExtension
        return if (name.lowercase() in GENERIC_NAMES) file.parentFile?.name ?: name else name
    }

    /** "Title (USA) (Disc 1) [!]" -> Title */
    fun titleOf(name: String) =
        name.replace(Regex("""\s*[(\[][^)\]]*[)\]]"""), "").trim().ifEmpty { name }

    private fun toGame(file: File): Game {
        val name = cardName(file.path)
        // "Title (USA) (Disc 1) [!]" -> Title, [USA, Disc 1, !]
        val tags = Regex("""[(\[]([^)\]]+)[)\]]""").findAll(name).map { it.groupValues[1] }.toList()
        return Game(titleOf(name), tags, file.path, file.extension.uppercase(), file.length())
    }
}
