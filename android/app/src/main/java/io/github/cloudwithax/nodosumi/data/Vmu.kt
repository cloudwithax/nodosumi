package io.github.cloudwithax.nodosumi.data

import android.content.Context
import android.graphics.Bitmap
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.Charset
import java.time.LocalDateTime

/** A save on a card, from its directory entry and the header at its start. */
class VmuSave(
    val slot: Int,
    val game: Boolean,
    val filename: String,
    val blocks: Int,
    val date: LocalDateTime?,
    val title: String,
    val description: String,
    /** the icon's frames, 32x32, and how long each shows */
    val icons: List<Bitmap>,
    val frameMs: Long,
)

/**
 * A game's visual memory card, the 128KB image the core keeps in
 * system/dc/vmu/<game>/vmu0.bin. Edits are made to a copy in memory and written
 * back whole, so a card is never left half written.
 */
class VmuCard(val file: File, val name: String, val title: String) {
    private val data: ByteArray = file.readBytes().takeIf { it.size == SIZE } ?: blank()

    val saves: List<VmuSave> get() = entries().mapNotNull { parse(it) }
    val freeBlocks get() = (0 until USER_BLOCKS).count { fat(it) == FREE }

    // the directory, 13 blocks of 16 entries counting down from block 253
    private fun entries() = (0 until DIR_BLOCKS * 16).filter { data[entry(it)].toInt() != 0 }

    private fun entry(slot: Int) = (DIR_BLOCK - slot / 16) * BLOCK + (slot % 16) * 32

    private fun u16(off: Int) = (data[off].toInt() and 0xff) or ((data[off + 1].toInt() and 0xff) shl 8)

    private fun put16(off: Int, v: Int) {
        data[off] = v.toByte()
        data[off + 1] = (v shr 8).toByte()
    }

    private fun fat(block: Int) = u16(FAT_BLOCK * BLOCK + block * 2)

    private fun setFat(block: Int, v: Int) = put16(FAT_BLOCK * BLOCK + block * 2, v)

    /** The blocks a file's chain runs through, stopping short on a broken one. */
    private fun chain(first: Int, count: Int): List<Int> {
        val blocks = mutableListOf<Int>()
        var b = first
        while (b in 0 until FAT_BLOCK && blocks.size < count) {
            blocks += b
            b = fat(b)
        }
        return blocks
    }

    private fun contents(slot: Int): ByteArray {
        val e = entry(slot)
        val blocks = chain(u16(e + 2), u16(e + 0x18))
        val out = ByteArray(blocks.size * BLOCK)
        blocks.forEachIndexed { i, b -> data.copyInto(out, i * BLOCK, b * BLOCK, (b + 1) * BLOCK) }
        return out
    }

    private fun parse(slot: Int): VmuSave? {
        val e = entry(slot)
        val type = data[e].toInt() and 0xff
        if (type != DATA && type != GAME) return null
        val filename = text(data, e + 4, 12)
        val bytes = contents(slot)
        val h = u16(e + 0x1a) * BLOCK
        // a save too broken to have a header still shows, so it can be deleted
        val ok = h + 0x80 <= bytes.size
        val buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val numIcons = if (ok) buf.getShort(h + 0x40).toInt().coerceIn(0, 3) else 0
        val speed = if (ok) buf.getShort(h + 0x42).toInt() else 0
        val icons = (0 until numIcons).mapNotNull { icon(bytes, h, it) }
        return VmuSave(
            slot = slot,
            game = type == GAME,
            filename = filename,
            blocks = u16(e + 0x18),
            date = date(e + 0x10),
            title = if (ok) text(bytes, h, 16).ifEmpty { filename } else filename,
            description = if (ok) text(bytes, h + 0x10, 32) else "Damaged",
            icons = icons,
            frameMs = (speed.coerceIn(1, 60) * 1000L / 30).coerceAtLeast(120),
        )
    }

    // 16 argb4444 colours, then each frame's 32x32 at 4 bits a pixel, left pixel high
    private fun icon(bytes: ByteArray, h: Int, frame: Int): Bitmap? {
        val start = h + 0x80 + frame * 512
        if (start + 512 > bytes.size) return null
        val palette = IntArray(16) {
            val v = (bytes[h + 0x60 + it * 2].toInt() and 0xff) or ((bytes[h + 0x61 + it * 2].toInt() and 0xff) shl 8)
            ((v shr 12 and 0xf) * 17 shl 24) or ((v shr 8 and 0xf) * 17 shl 16) or
                ((v shr 4 and 0xf) * 17 shl 8) or ((v and 0xf) * 17)
        }
        val pixels = IntArray(32 * 32) {
            val b = bytes[start + it / 2].toInt()
            palette[if (it % 2 == 0) b shr 4 and 0xf else b and 0xf]
        }
        return Bitmap.createBitmap(pixels, 32, 32, Bitmap.Config.ARGB_8888)
    }

    private fun date(off: Int): LocalDateTime? {
        fun bcd(i: Int) = (data[off + i].toInt() and 0xff).let { (it shr 4) * 10 + (it and 0xf) }
        return runCatching {
            LocalDateTime.of(bcd(0) * 100 + bcd(1), bcd(2), bcd(3), bcd(4), bcd(5), bcd(6))
        }.getOrNull()
    }

    fun delete(save: VmuSave) {
        val e = entry(save.slot)
        for (b in chain(u16(e + 2), save.blocks)) setFat(b, FREE)
        data.fill(0, e, e + 32)
    }

    /**
     * A save in the .dci form other emulators and the dreamcast's own tools
     * use, its directory entry then its blocks with each word's bytes reversed.
     */
    fun export(save: VmuSave): ByteArray {
        val bytes = contents(save.slot)
        val out = ByteArray(32 + bytes.size)
        data.copyInto(out, 0, entry(save.slot), entry(save.slot) + 32)
        for (i in bytes.indices) out[32 + (i and 3.inv()) + 3 - (i and 3)] = bytes[i]
        return out
    }

    /** Puts a .dci on the card, replacing a save of the same name. */
    fun import(dci: ByteArray): String? {
        if (dci.size < 32 + BLOCK || (dci.size - 32) % BLOCK != 0) return "That isn't a .dci save"
        val type = dci[0].toInt() and 0xff
        if (type != DATA && type != GAME) return "That isn't a .dci save"
        val count = (dci.size - 32) / BLOCK
        val name = text(dci, 4, 12)

        val old = saves.find { it.filename == name }
        val free = freeBlocks + (old?.blocks ?: 0)
        if (count > free) return "Not enough space, it needs $count blocks and there are $free"

        old?.let { delete(it) }
        // mini-games run from the card's first blocks, saves fill in from the top
        val blocks = if (type == GAME) (0 until count).toList()
        else (USER_BLOCKS - 1 downTo 0).filter { fat(it) == FREE }.take(count)
        if (blocks.size < count || blocks.any { fat(it) != FREE }) return "There's no room for a mini-game"
        val slot = (0 until DIR_BLOCKS * 16).firstOrNull { data[entry(it)].toInt() == 0 }
            ?: return "The card's directory is full"

        blocks.forEachIndexed { i, b ->
            setFat(b, if (i == blocks.size - 1) END else blocks[i + 1])
            for (j in 0 until BLOCK) {
                val k = i * BLOCK + j
                data[b * BLOCK + j] = dci[32 + (k and 3.inv()) + 3 - (k and 3)]
            }
        }
        val e = entry(slot)
        dci.copyInto(data, e, 0, 32)
        put16(e + 2, blocks[0])
        put16(e + 0x18, count)
        return null
    }

    fun format() = blank().copyInto(data)

    fun bytes(): ByteArray = data.copyOf()

    fun restore(bytes: ByteArray): Boolean {
        if (bytes.size != SIZE) return false
        bytes.copyInto(data)
        return true
    }

    fun save() {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeBytes(data)
        tmp.renameTo(file)
    }

    companion object {
        const val SIZE = 128 * 1024
        const val USER_BLOCKS = 200
        private const val BLOCK = 512
        private const val ROOT_BLOCK = 255
        private const val FAT_BLOCK = 254
        private const val DIR_BLOCK = 253
        private const val DIR_BLOCKS = 13
        private const val FREE = 0xfffc
        private const val END = 0xfffa
        private const val DATA = 0x33
        private const val GAME = 0xcc
        private val SJIS = Charset.forName("Shift_JIS")

        // the root block of a freshly formatted card, as the core makes them
        private val ROOT = (
            "55555555555555555555555555555555 01ffffffff0000000000000000000000" +
                "00000000000000000000000000000000 1998112700005804 0000000000000000" +
                "ff000000ff00fe000100fd000d000000c8001f0000008000"
            ).replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

        private fun text(bytes: ByteArray, off: Int, len: Int) =
            String(bytes, off, len, SJIS).trimEnd(' ', '\u0000').trim().replace(Regex(" {2,}"), " ")

        private fun blank(): ByteArray {
            val d = ByteArray(SIZE)
            ROOT.copyInto(d, ROOT_BLOCK * BLOCK)
            fun fat(b: Int, v: Int) {
                d[FAT_BLOCK * BLOCK + b * 2] = v.toByte()
                d[FAT_BLOCK * BLOCK + b * 2 + 1] = (v shr 8).toByte()
            }
            for (b in 0 until 256) fat(b, FREE)
            fat(ROOT_BLOCK, END)
            fat(FAT_BLOCK, END)
            // the directory chains down from 253 to 241
            for (b in DIR_BLOCK downTo DIR_BLOCK - DIR_BLOCKS + 2) fat(b, b - 1)
            fat(DIR_BLOCK - DIR_BLOCKS + 1, END)
            return d
        }

        /** Each game's card, and the shared one from before cards were per game. */
        fun cards(context: Context): List<VmuCard> {
            val dc = Settings.biosDir(context)
            val games = File(dc, "vmu").listFiles().orEmpty()
                .filter { File(it, "vmu0.bin").isFile }
                .map { VmuCard(File(it, "vmu0.bin"), it.name, Games.titleOf(it.name)) }
                .sortedBy { it.title.lowercase() }
            val shared = File(dc, "vmu0.bin").takeIf { it.isFile }?.let { VmuCard(it, "", "Shared card") }
            return games + listOfNotNull(shared)
        }
    }
}
