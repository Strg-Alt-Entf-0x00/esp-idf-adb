package de.esp32adb.companion

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import android.database.sqlite.SQLiteException
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class AdbExportReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_EXPORT_CHANNELS) return

        val requestId = intent.getStringExtra(EXTRA_REQUEST_ID).orEmpty()
        if (!REQUEST_ID_PATTERN.matches(requestId)) {
            setResult(Activity.RESULT_CANCELED, "ERROR:InvalidRequestId", null)
            return
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            setResult(Activity.RESULT_CANCELED, "ERROR:DownloadsUnavailable", null)
            return
        }
        if (!exportInProgress.compareAndSet(false, true)) {
            setResult(Activity.RESULT_CANCELED, "ERROR:ExportAlreadyRunning", null)
            return
        }

        val pendingResult = goAsync()
        executor.execute {
            try {
                val channels = TvChannelRepository.read(context)
                val timestamp = SimpleDateFormat(
                    "yyyyMMdd-HHmmss",
                    Locale.ROOT,
                ).format(Date())
                val fileName = "tv-channel-list-$timestamp-$requestId.csv"
                ChannelCsvExporter.export(context, fileName, channels)
                pendingResult.setResultCode(Activity.RESULT_OK)
                pendingResult.setResultData(fileName)
                Log.i(TAG, "ADB channel export completed: $fileName rows=${channels.size}")
            } catch (error: SecurityException) {
                reportFailure(pendingResult, error)
            } catch (error: SQLiteException) {
                reportFailure(pendingResult, error)
            } catch (error: IllegalArgumentException) {
                reportFailure(pendingResult, error)
            } catch (error: IllegalStateException) {
                reportFailure(pendingResult, error)
            } catch (error: IOException) {
                reportFailure(pendingResult, error)
            } finally {
                exportInProgress.set(false)
                pendingResult.finish()
            }
        }
    }

    private fun reportFailure(
        pendingResult: PendingResult,
        error: Exception,
    ) {
        pendingResult.setResultCode(Activity.RESULT_CANCELED)
        pendingResult.setResultData("ERROR:${error.javaClass.simpleName}")
        Log.e(TAG, "ADB channel export failed", error)
    }

    companion object {
        const val ACTION_EXPORT_CHANNELS =
            "de.esp32adb.companion.action.EXPORT_CHANNELS"
        const val EXTRA_REQUEST_ID = "request_id"

        private const val TAG = "AdbExportReceiver"
        private val REQUEST_ID_PATTERN = Regex("[a-f0-9]{32}")
        private val exportInProgress = AtomicBoolean(false)
        private val executor = Executors.newSingleThreadExecutor()
    }
}
