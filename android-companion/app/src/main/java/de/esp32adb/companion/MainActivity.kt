package de.esp32adb.companion

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.pm.PackageManager
import android.database.sqlite.SQLiteException
import android.media.tv.TvInputInfo
import android.media.tv.TvInputManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.ListView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

private data class ChannelScanResult(
    val summary: String,
    val channels: List<TvChannel>,
)

private data class ScanResult(
    val report: String,
    val channels: List<TvChannel>,
)

private sealed class CsvExportResult {
    data class Saved(val location: String) : CsvExportResult()
    data class Failed(val errorType: String) : CsvExportResult()
}

private enum class ChannelSort {
    NUMBER,
    NAME,
    PROVIDER_ORDER,
}

class MainActivity : Activity() {
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private lateinit var reportView: TextView
    private lateinit var scanButton: Button
    private lateinit var channelListButton: Button
    private lateinit var exportButton: Button
    private lateinit var channelsPermissionButton: Button
    private var channelRows: List<TvChannel> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(20), dp(24), dp(20))
            setBackgroundColor(0xFF101418.toInt())
        }
        applySystemInsets(root)

        val title = TextView(this).apply {
            text = getString(R.string.screen_title)
            textSize = 26f
            setTextColor(0xFFFFFFFF.toInt())
        }
        root.addView(title, matchWidth())

        val subtitle = TextView(this).apply {
            text = getString(R.string.screen_subtitle)
            textSize = 16f
            setTextColor(0xFFB7C4CC.toInt())
            setPadding(0, dp(8), 0, dp(16))
        }
        root.addView(subtitle, matchWidth())

        scanButton = Button(this).apply {
            text = getString(R.string.scan_again)
            setOnClickListener { scan() }
        }
        root.addView(scanButton, matchWidth())

        channelListButton = Button(this).apply {
            text = getString(R.string.open_channel_list, 0)
            isEnabled = false
            setOnClickListener { showChannelList() }
        }
        root.addView(channelListButton, matchWidth())

        exportButton = Button(this).apply {
            text = getString(R.string.export_channel_csv)
            isEnabled = false
            setOnClickListener { exportChannels() }
        }
        root.addView(exportButton, matchWidth())

        channelsPermissionButton = Button(this).apply {
            text = getString(R.string.request_channels_permission)
            setOnClickListener { requestChannelsPermission() }
        }
        root.addView(channelsPermissionButton, matchWidth())

        reportView = TextView(this).apply {
            textSize = 16f
            setTextColor(0xFFE7EDF0.toInt())
            setLineSpacing(dp(4).toFloat(), 1f)
            setPadding(0, dp(20), 0, dp(24))
            setTextIsSelectable(true)
        }
        val scrollView = ScrollView(this).apply {
            isFillViewport = true
            isFocusable = false
            isFocusableInTouchMode = false
            addView(reportView, ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ))
        }
        root.addView(
            scrollView,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f,
            ).apply {
                topMargin = dp(8)
            },
        )

        setContentView(root)
        updateChannelsPermissionButton()
        root.requestApplyInsets()
        scanButton.requestFocus()
        scan()
    }

    override fun onDestroy() {
        worker.shutdownNow()
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_TV_LISTINGS_PERMISSION) {
            updateChannelsPermissionButton()
            scan()
        }
    }

    private fun scan() {
        scanButton.isEnabled = false
        channelListButton.isEnabled = false
        exportButton.isEnabled = false
        reportView.text = getString(R.string.scanning)
        worker.execute {
            val result = createReport()
            mainHandler.post {
                if (!isFinishing && !isDestroyed) {
                    reportView.text = result.report
                    channelRows = result.channels
                    channelListButton.text =
                        getString(R.string.open_channel_list, channelRows.size)
                    channelListButton.isEnabled = channelRows.isNotEmpty()
                    exportButton.isEnabled = channelRows.isNotEmpty()
                    scanButton.isEnabled = true
                    updateChannelsPermissionButton()
                }
            }
        }
    }

    private fun showChannelList() {
        val channels = channelRows
        if (channels.isEmpty()) return

        var sort = ChannelSort.NUMBER
        val filter = EditText(this).apply {
            hint = getString(R.string.channel_filter_hint)
            setSingleLine(true)
        }
        val sortButton = Button(this)
        val resultCount = TextView(this).apply {
            textSize = 14f
            setTextColor(0xFFB7C4CC.toInt())
            setPadding(0, dp(4), 0, dp(8))
        }
        val adapter = ArrayAdapter(
            this,
            android.R.layout.simple_list_item_1,
            ArrayList<String>(),
        )
        val list = ListView(this).apply {
            this.adapter = adapter
            isFocusable = true
            isFocusableInTouchMode = true
        }
        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(filter, matchWidth())
            addView(sortButton, matchWidth())
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), 0, dp(20), 0)
            addView(controls, matchWidth())
            addView(resultCount, matchWidth())
            addView(
                TextView(this@MainActivity).apply {
                    text = getString(R.string.channel_source_note)
                    textSize = 14f
                    setTextColor(0xFFB7C4CC.toInt())
                    setPadding(0, 0, 0, dp(8))
                },
                matchWidth(),
            )
            addView(
                list,
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    dp(420),
                ),
            )
        }

        fun refreshList() {
            sortButton.text = getString(
                R.string.channel_sort_by,
                getString(
                    when (sort) {
                        ChannelSort.NUMBER -> R.string.channel_sort_number
                        ChannelSort.NAME -> R.string.channel_sort_name
                        ChannelSort.PROVIDER_ORDER -> R.string.channel_sort_provider
                    },
                ),
            )
            val query = filter.text.toString().trim().lowercase(Locale.ROOT)
            val matches = channels.filter { channel ->
                query.isEmpty() ||
                    listOf(
                        channel.displayNumber.orEmpty(),
                        channel.displayName.orEmpty(),
                        channel.inputId.orEmpty(),
                        channel.broadcastType.orEmpty(),
                        channel.serviceType.orEmpty(),
                        channel.broadcastVisibilityType.orEmpty(),
                        channel.browsable?.toString().orEmpty(),
                        channel.locked?.toString().orEmpty(),
                    ).any { it.lowercase(Locale.ROOT).contains(query) }
            }
            val ordered = when (sort) {
                ChannelSort.NUMBER -> matches.sortedWith { first, second ->
                    compareChannelNumbers(
                        first.displayNumber.orEmpty(),
                        second.displayNumber.orEmpty(),
                    )
                }
                ChannelSort.NAME -> matches.sortedWith(
                    compareBy<TvChannel> {
                        it.displayName.orEmpty().lowercase(Locale.ROOT)
                    }.thenBy { it.displayNumber.orEmpty().lowercase(Locale.ROOT) },
                )
                ChannelSort.PROVIDER_ORDER -> matches
            }
            adapter.clear()
            adapter.addAll(ordered.map { channel ->
                getString(
                    R.string.channel_list_item,
                    channel.displayNumber
                        ?.takeIf(String::isNotBlank)
                        ?: getString(R.string.unnumbered),
                    channel.displayName
                        ?.takeIf(String::isNotBlank)
                        ?: getString(R.string.unnamed),
                    channel.inputId
                        ?.takeIf(String::isNotBlank)
                        ?: getString(R.string.unknown_input),
                    channel.broadcastType
                        ?.takeIf(String::isNotBlank)
                        ?: getString(R.string.value_unknown),
                    channel.browsable?.let {
                        getString(if (it) R.string.yes else R.string.no)
                    } ?: getString(R.string.value_unknown),
                    channel.locked?.let {
                        getString(if (it) R.string.yes else R.string.no)
                    } ?: getString(R.string.value_unknown),
                )
            })
            resultCount.text = getString(
                R.string.channel_list_result_count,
                ordered.size,
                channels.size,
            )
        }

        sortButton.setOnClickListener {
            sort = ChannelSort.values()[(sort.ordinal + 1) % ChannelSort.values().size]
            refreshList()
        }
        filter.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) =
                refreshList()
            override fun afterTextChanged(s: Editable?) = Unit
        })
        refreshList()

        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.channel_list_title)
            .setView(content)
            .setPositiveButton(R.string.close, null)
            .create()
        dialog.setOnShowListener {
            dialog.window?.setLayout(
                (resources.displayMetrics.widthPixels * 0.9f).toInt(),
                (resources.displayMetrics.heightPixels * 0.85f).toInt(),
            )
        }
        dialog.show()
    }

    private fun exportChannels() {
        val channels = channelRows.toList()
        if (channels.isEmpty()) return
        exportButton.isEnabled = false
        worker.execute {
            val timestamp = SimpleDateFormat(
                "yyyyMMdd-HHmmss",
                Locale.ROOT,
            ).format(Date())
            val fileName = "tv-channel-list-$timestamp.csv"
            val result = try {
                CsvExportResult.Saved(ChannelCsvExporter.export(this, fileName, channels))
            } catch (error: IOException) {
                CsvExportResult.Failed(error.javaClass.simpleName)
            } catch (error: SecurityException) {
                CsvExportResult.Failed(error.javaClass.simpleName)
            }
            mainHandler.post {
                if (!isFinishing && !isDestroyed) {
                    exportButton.isEnabled = channelRows.isNotEmpty()
                    val message = when (result) {
                        is CsvExportResult.Saved -> getString(
                            R.string.csv_export_success,
                            channels.size,
                            result.location,
                        )
                        is CsvExportResult.Failed -> getString(
                            R.string.csv_export_failed,
                            result.errorType,
                        )
                    }
                    Toast.makeText(this, message, Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun compareChannelNumbers(first: String, second: String): Int {
        if (first.isBlank() || second.isBlank()) {
            return when {
                first.isBlank() && second.isBlank() -> 0
                first.isBlank() -> 1
                else -> -1
            }
        }

        val partsPattern = Regex("\\d+|\\D+")
        val firstParts = partsPattern.findAll(first).map { it.value }.toList()
        val secondParts = partsPattern.findAll(second).map { it.value }.toList()
        for (index in 0 until minOf(firstParts.size, secondParts.size)) {
            val left = firstParts[index]
            val right = secondParts[index]
            val comparison = if (left.all(Char::isDigit) && right.all(Char::isDigit)) {
                val normalizedLeft = left.trimStart('0').ifEmpty { "0" }
                val normalizedRight = right.trimStart('0').ifEmpty { "0" }
                normalizedLeft.length.compareTo(normalizedRight.length)
                    .takeIf { it != 0 }
                    ?: normalizedLeft.compareTo(normalizedRight)
            } else {
                left.compareTo(right, ignoreCase = true)
            }
            if (comparison != 0) return comparison
        }
        return firstParts.size.compareTo(secondParts.size)
            .takeIf { it != 0 }
            ?: first.compareTo(second, ignoreCase = true)
    }

    private fun requestChannelsPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            requestPermissions(
                arrayOf(READ_TV_LISTINGS_PERMISSION),
                REQUEST_TV_LISTINGS_PERMISSION,
            )
        }
    }

    private fun updateChannelsPermissionButton() {
        val permissionNeeded = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            checkSelfPermission(READ_TV_LISTINGS_PERMISSION) !=
            PackageManager.PERMISSION_GRANTED
        channelsPermissionButton.visibility = if (permissionNeeded) View.VISIBLE else View.GONE
    }

    private fun createReport(): ScanResult {
        val channels = readChannels()
        val report = buildString {
            appendLine(getString(R.string.device_section))
            appendLine(getString(R.string.device_format, Build.MANUFACTURER, Build.MODEL))
            appendLine(
                getString(
                    R.string.device_profile_format,
                    Build.BRAND,
                    Build.DEVICE,
                    Build.PRODUCT,
                ),
            )
            appendLine(getString(R.string.build_fingerprint_format, Build.FINGERPRINT))
            appendLine(getString(R.string.android_format, Build.VERSION.RELEASE, Build.VERSION.SDK_INT))
            appendLine()

            appendLine(getString(R.string.feature_section))
            appendFeature(
                R.string.feature_leanback,
                packageManager.hasSystemFeature(PackageManager.FEATURE_LEANBACK),
            )
            appendFeature(
                R.string.feature_live_tv,
                packageManager.hasSystemFeature("android.software.live_tv"),
            )
            appendFeature(
                R.string.feature_touchscreen,
                packageManager.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN),
            )
            appendLine()

            appendLine(getString(R.string.inputs_section))
            appendLine(readInputs())
            appendLine()

            appendLine(getString(R.string.channels_section))
            appendLine(channels.summary)
            appendLine()
            append(getString(R.string.diagnostic_note))
        }
        return ScanResult(report, channels.channels)
    }

    private fun StringBuilder.appendFeature(label: Int, available: Boolean) {
        appendLine(
            getString(
                R.string.feature_format,
                getString(label),
                getString(if (available) R.string.yes else R.string.no),
            ),
        )
    }

    private fun readInputs(): String {
        val manager = getSystemService(Context.TV_INPUT_SERVICE) as? TvInputManager
            ?: return getString(R.string.inputs_unavailable)

        return try {
            val inputs = manager.tvInputList.sortedBy {
                it.loadLabel(this@MainActivity).toString().lowercase(Locale.ROOT)
            }
            if (inputs.isEmpty()) {
                return getString(R.string.inputs_empty)
            }

            buildString {
                appendLine(getString(R.string.input_count, inputs.size))
                inputs.forEach { input ->
                    val state = try {
                        inputStateLabel(manager.getInputState(input.id))
                    } catch (_: IllegalArgumentException) {
                        getString(R.string.state_unavailable)
                    }
                    appendLine(
                        getString(
                            R.string.input_format,
                            input.loadLabel(this@MainActivity),
                            inputTypeLabel(input.type),
                            state,
                            input.id,
                        ),
                    )
                }
            }.trimEnd()
        } catch (_: SecurityException) {
            getString(R.string.inputs_denied)
        }
    }

    private fun readChannels(): ChannelScanResult {
        if (
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            checkSelfPermission(READ_TV_LISTINGS_PERMISSION) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return ChannelScanResult(
                getString(R.string.channels_permission_required),
                emptyList(),
            )
        }

        val rows = try {
            TvChannelRepository.read(this)
        } catch (_: SecurityException) {
            return ChannelScanResult(getString(R.string.channels_denied), emptyList())
        } catch (error: SQLiteException) {
            return ChannelScanResult(
                getString(R.string.channels_provider_error, error.javaClass.simpleName),
                emptyList(),
            )
        } catch (error: IllegalArgumentException) {
            return ChannelScanResult(
                getString(R.string.channels_schema_error, error.javaClass.simpleName),
                emptyList(),
            )
        } catch (_: IllegalStateException) {
            return ChannelScanResult(getString(R.string.channels_no_cursor), emptyList())
        }

        val rowsByInput = linkedMapOf<String, Int>()
        val rowsByBroadcastType = linkedMapOf<String, Int>()

        rows.forEach { channel ->
            val inputKey = channel.inputId
                ?.takeIf(String::isNotBlank)
                ?: getString(R.string.unknown_input)
            rowsByInput[inputKey] = (rowsByInput[inputKey] ?: 0) + 1
            val typeKey = channel.broadcastType
                ?.takeIf(String::isNotBlank)
                ?: getString(R.string.value_unknown)
            rowsByBroadcastType[typeKey] = (rowsByBroadcastType[typeKey] ?: 0) + 1
        }

        val summary = buildString {
            appendLine(getString(R.string.channels_count, rows.size))
            if (rowsByInput.isNotEmpty()) {
                appendLine()
                appendLine(getString(R.string.channels_by_input))
                rowsByInput.entries.sortedByDescending { it.value }.forEach { (inputId, rowsForInput) ->
                    appendLine(
                        getString(
                            R.string.channels_input_count,
                            inputId,
                            rowsForInput,
                        ),
                    )
                }
                appendLine()
                appendLine(getString(R.string.channels_by_broadcast_type))
                rowsByBroadcastType.entries.sortedByDescending { it.value }
                    .forEach { (type, rowsForType) ->
                        appendLine(getString(R.string.channels_input_count, type, rowsForType))
                    }
                appendLine()
                appendLine(
                    getString(
                        R.string.channels_browsable_summary,
                        rows.count { it.browsable == true },
                        rows.count { it.browsable == false },
                        rows.count { it.browsable == null },
                    ),
                )
            }
            if (rows.isEmpty()) {
                append(getString(R.string.channels_empty))
            }
        }.trimEnd()
        return ChannelScanResult(summary, rows)
    }

    private fun inputTypeLabel(type: Int): String = when (type) {
        TvInputInfo.TYPE_HDMI -> getString(R.string.type_hdmi)
        TvInputInfo.TYPE_TUNER -> getString(R.string.type_tuner)
        TvInputInfo.TYPE_COMPONENT -> getString(R.string.type_component)
        TvInputInfo.TYPE_COMPOSITE -> getString(R.string.type_composite)
        TvInputInfo.TYPE_DISPLAY_PORT -> getString(R.string.type_display_port)
        TvInputInfo.TYPE_DVI -> getString(R.string.type_dvi)
        TvInputInfo.TYPE_SCART -> getString(R.string.type_scart)
        TvInputInfo.TYPE_SVIDEO -> getString(R.string.type_svideo)
        TvInputInfo.TYPE_OTHER -> getString(R.string.type_other)
        TvInputInfo.TYPE_VGA -> getString(R.string.type_vga)
        else -> getString(R.string.type_vendor, type)
    }

    private fun inputStateLabel(state: Int): String = when (state) {
        TvInputManager.INPUT_STATE_CONNECTED -> getString(R.string.state_connected)
        TvInputManager.INPUT_STATE_CONNECTED_STANDBY -> getString(R.string.state_standby)
        TvInputManager.INPUT_STATE_DISCONNECTED -> getString(R.string.state_disconnected)
        else -> getString(R.string.state_unknown, state)
    }

    private fun matchWidth() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    )

    @Suppress("DEPRECATION")
    private fun applySystemInsets(view: View) {
        val initialLeft = view.paddingLeft
        val initialTop = view.paddingTop
        val initialRight = view.paddingRight
        val initialBottom = view.paddingBottom
        view.setOnApplyWindowInsetsListener { currentView, insets ->
            val left: Int
            val top: Int
            val right: Int
            val bottom: Int
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val systemInsets = insets.getInsets(WindowInsets.Type.systemBars())
                left = systemInsets.left
                top = systemInsets.top
                right = systemInsets.right
                bottom = systemInsets.bottom
            } else {
                left = insets.systemWindowInsetLeft
                top = insets.systemWindowInsetTop
                right = insets.systemWindowInsetRight
                bottom = insets.systemWindowInsetBottom
            }
            currentView.setPadding(
                initialLeft + left,
                initialTop + top,
                initialRight + right,
                initialBottom + bottom,
            )
            insets
        }
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private companion object {
        const val READ_TV_LISTINGS_PERMISSION = "android.permission.READ_TV_LISTINGS"
        const val REQUEST_TV_LISTINGS_PERMISSION = 1
    }
}
