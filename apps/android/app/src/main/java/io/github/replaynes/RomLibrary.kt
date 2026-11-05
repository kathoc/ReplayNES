package io.github.replaynes

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException

/** ROMs live in filesDir/roms/, their ReplayNES projects in filesDir/projects/<name>.nesrec/. */
class RomLibrary(private val context: Context) {
    val romDir = File(context.filesDir, "roms").apply { mkdirs() }
    private val projectDir = File(context.filesDir, "projects").apply { mkdirs() }

    fun roms(): List<File> =
        romDir.listFiles { f -> f.isFile && f.name.endsWith(".nes", ignoreCase = true) }
            ?.sortedBy { it.name.lowercase() } ?: emptyList()

    fun projectFor(rom: File): File = File(projectDir, rom.nameWithoutExtension + ".nesrec")

    /** Copies a document picked with the Storage Access Framework into the library. */
    @Throws(IOException::class)
    fun import(uri: Uri): File {
        val name = displayName(uri) ?: "ROM.nes"
        val bytes = context.contentResolver.openInputStream(uri)?.use { it.readBytes(MAX_ROM + 1) }
            ?: throw IOException("Cannot read the file")
        return add(name, bytes)
    }

    /** Debug launches: copies a ROM from a file path into the library. */
    @Throws(IOException::class)
    fun importPath(path: String): File {
        val f = File(path)
        return add(f.name, f.inputStream().use { it.readBytes(MAX_ROM + 1) })
    }

    fun testCartridge(): File {
        val f = File(romDir, TEST_CARTRIDGE)
        if (!f.exists()) {
            NativeBridge.writeTestCartridge(f.path)?.let { throw IOException(it) }
        }
        return f
    }

    private fun add(rawName: String, bytes: ByteArray): File {
        if (bytes.size > MAX_ROM) throw IOException("File too large for an NES ROM")
        if (bytes.size < 16 || bytes[0] != 'N'.code.toByte() || bytes[1] != 'E'.code.toByte() ||
            bytes[2] != 'S'.code.toByte() || bytes[3] != 0x1A.toByte()
        ) throw IOException("Not an iNES (.nes) ROM")
        var base = rawName.substringBeforeLast('.').replace(Regex("[/\\\\:*?\"<>|\\x00-\\x1f]"), "_")
            .trim().ifEmpty { "ROM" }
        var dest = File(romDir, "$base.nes")
        var n = 2
        // Same name, different content: keep both (a project belongs to exactly one ROM).
        while (dest.exists() && !dest.readBytes().contentEquals(bytes)) {
            dest = File(romDir, "$base ($n).nes"); n++
        }
        if (!dest.exists()) {
            val tmp = File(romDir, dest.name + ".tmp")
            tmp.writeBytes(bytes)
            if (!tmp.renameTo(dest)) throw IOException("Cannot store the ROM")
        }
        return dest
    }

    private fun displayName(uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }

    private fun java.io.InputStream.readBytes(limit: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buf = ByteArray(64 * 1024)
        while (out.size() < limit) {
            val r = read(buf)
            if (r < 0) break
            out.write(buf, 0, r)
        }
        return out.toByteArray()
    }

    companion object {
        const val MAX_ROM = 8 * 1024 * 1024
        const val TEST_CARTRIDGE = "ReplayNES Test Cartridge.nes"
    }
}
