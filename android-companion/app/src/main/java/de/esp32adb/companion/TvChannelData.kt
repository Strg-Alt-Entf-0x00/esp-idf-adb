package de.esp32adb.companion

import android.annotation.TargetApi
import android.content.ContentValues
import android.content.Context
import android.media.tv.TvContract
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.io.OutputStreamWriter

internal data class TvChannel(
    val displayName: String?,
    val displayNumber: String?,
    val inputId: String?,
    val broadcastType: String?,
    val serviceType: String?,
    val broadcastVisibilityType: String?,
    val browsable: Boolean?,
    val locked: Boolean?,
)

internal object TvChannelRepository {
    private const val READ_TV_LISTINGS_PERMISSION = "android.permission.READ_TV_LISTINGS"

    fun read(context: Context): List<TvChannel> {
        if (
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            context.checkSelfPermission(READ_TV_LISTINGS_PERMISSION) !=
            android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            throw SecurityException("READ_TV_LISTINGS permission is not granted")
        }

        val projection = arrayOf(
            TvContract.Channels.COLUMN_DISPLAY_NAME,
            TvContract.Channels.COLUMN_DISPLAY_NUMBER,
            TvContract.Channels.COLUMN_INPUT_ID,
            TvContract.Channels.COLUMN_TYPE,
            TvContract.Channels.COLUMN_SERVICE_TYPE,
            TvContract.Channels.COLUMN_BROADCAST_VISIBILITY_TYPE,
            TvContract.Channels.COLUMN_BROWSABLE,
            TvContract.Channels.COLUMN_LOCKED,
        )
        val cursor = context.contentResolver.query(
            TvContract.Channels.CONTENT_URI,
            projection,
            null,
            null,
            null,
        ) ?: throw IllegalStateException("TV provider returned no query cursor")

        return cursor.use {
            val nameColumn = it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_DISPLAY_NAME)
            val numberColumn = it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_DISPLAY_NUMBER)
            val inputColumn = it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_INPUT_ID)
            val typeColumn = it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_TYPE)
            val serviceTypeColumn =
                it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_SERVICE_TYPE)
            val visibilityColumn = it.getColumnIndexOrThrow(
                TvContract.Channels.COLUMN_BROADCAST_VISIBILITY_TYPE,
            )
            val browsableColumn =
                it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_BROWSABLE)
            val lockedColumn = it.getColumnIndexOrThrow(TvContract.Channels.COLUMN_LOCKED)
            val channels = ArrayList<TvChannel>()

            while (it.moveToNext()) {
                channels += TvChannel(
                    it.getString(nameColumn),
                    it.getString(numberColumn),
                    it.getString(inputColumn),
                    it.getString(typeColumn),
                    it.getString(serviceTypeColumn),
                    it.getString(visibilityColumn),
                    readNullableBoolean(it, browsableColumn),
                    readNullableBoolean(it, lockedColumn),
                )
            }
            channels
        }
    }

    private fun readNullableBoolean(
        cursor: android.database.Cursor,
        columnIndex: Int,
    ): Boolean? {
        if (cursor.isNull(columnIndex)) return null
        return when (cursor.getString(columnIndex)) {
            "1", "true" -> true
            "0", "false" -> false
            else -> throw IllegalArgumentException("Invalid TV channel boolean value")
        }
    }
}

internal object ChannelCsvExporter {
    fun export(context: Context, fileName: String, channels: List<TvChannel>): String {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            writeCsvToDownloads(context, fileName, channels)
            "Downloads/$fileName"
        } else {
            val directory = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
                ?: throw IOException("External app documents directory is unavailable")
            if (!directory.exists() && !directory.mkdirs()) {
                throw IOException("Could not create the app documents directory")
            }
            val file = File(directory, fileName)
            writeCsv(FileOutputStream(file), channels)
            file.absolutePath
        }
    }

    @TargetApi(Build.VERSION_CODES.Q)
    private fun writeCsvToDownloads(
        context: Context,
        fileName: String,
        channels: List<TvChannel>,
    ) {
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
            put(MediaStore.MediaColumns.MIME_TYPE, "text/csv")
            put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: throw IOException("Could not create the CSV in Downloads")
        try {
            val output = context.contentResolver.openOutputStream(uri, "wt")
                ?: throw IOException("Could not open the CSV in Downloads")
            writeCsv(output, channels)
            val published = ContentValues().apply {
                put(MediaStore.MediaColumns.IS_PENDING, 0)
            }
            if (context.contentResolver.update(uri, published, null, null) != 1) {
                throw IOException("Could not publish the CSV in Downloads")
            }
        } catch (error: IOException) {
            discardPendingCsv(context, uri, error)
            throw error
        } catch (error: SecurityException) {
            discardPendingCsv(context, uri, error)
            throw error
        }
    }

    private fun discardPendingCsv(
        context: Context,
        uri: android.net.Uri,
        exportError: Exception,
    ) {
        try {
            if (context.contentResolver.delete(uri, null, null) == 0) {
                exportError.addSuppressed(IOException("Could not remove incomplete CSV"))
            }
        } catch (error: SecurityException) {
            exportError.addSuppressed(error)
        }
    }

    private fun writeCsv(output: OutputStream, channels: List<TvChannel>) {
        OutputStreamWriter(output, Charsets.UTF_8).use { writer ->
            writer.write("\uFEFF")
            writer.write(
                "channel_number,channel_name,input_id,broadcast_type,service_type," +
                    "broadcast_visibility_type,browsable,locked\r\n",
            )
            channels.forEach { channel ->
                writer.write(csvField(channel.displayNumber))
                writer.write(",")
                writer.write(csvField(channel.displayName))
                writer.write(",")
                writer.write(csvField(channel.inputId))
                writer.write(",")
                writer.write(csvField(channel.broadcastType))
                writer.write(",")
                writer.write(csvField(channel.serviceType))
                writer.write(",")
                writer.write(csvField(channel.broadcastVisibilityType))
                writer.write(",")
                writer.write(csvField(channel.browsable?.toString()))
                writer.write(",")
                writer.write(csvField(channel.locked?.toString()))
                writer.write("\r\n")
            }
        }
    }

    private fun csvField(value: String?): String {
        val field = value.orEmpty()
        val firstNonWhitespace = field.firstOrNull { !it.isWhitespace() }
        val safeField = if (firstNonWhitespace in setOf('=', '+', '-', '@')) {
            "'$field"
        } else {
            field
        }
        return "\"${safeField.replace("\"", "\"\"")}\""
    }
}
