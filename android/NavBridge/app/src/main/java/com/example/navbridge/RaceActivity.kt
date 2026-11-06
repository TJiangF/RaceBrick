package com.example.navbridge

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.graphics.Typeface
import android.text.InputFilter
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class RaceActivity : AppCompatActivity() {
    private lateinit var tvStatus: TextView
    private lateinit var etUrl: EditText
    private lateinit var llList: LinearLayout

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var direct: WifiDirectConnector
    private var generation = 0

    private var tracks: List<RaceTrack> = emptyList()
    private var sessions: List<RaceSession> = emptyList()
    private var filterTrack: Int? = null
    private var showingSessions = false
    private var directPending = false
    private var awaitingWriteSettings = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_race)

        tvStatus = findViewById(R.id.tvRaceStatus)
        etUrl = findViewById(R.id.etRaceUrl)
        llList = findViewById(R.id.llRaceList)
        direct = WifiDirectConnector(this)
        etUrl.setText(RaceApi.baseUrl(this))

        findViewById<Button>(R.id.btnRaceNav).setOnClickListener { finish() }
        findViewById<Button>(R.id.btnRaceSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.btnRaceDirect).setOnClickListener { directWithPermission() }
        findViewById<Button>(R.id.btnRaceLoad).setOnClickListener { loadAll() }
        findViewById<Button>(R.id.btnRaceTracks).setOnClickListener {
            showingSessions = false
            renderTracks()
        }
        findViewById<Button>(R.id.btnRaceSessions).setOnClickListener {
            showingSessions = true
            renderSessions()
        }

        loadAll()
    }

    private fun directWithPermission() {
        if (!Settings.System.canWrite(this)) {
            awaitingWriteSettings = true
            status("直连需要“修改系统设置”权限，请在系统页允许后返回")
            val intent = Intent(
                Settings.ACTION_MANAGE_WRITE_SETTINGS,
                Uri.parse("package:$packageName"),
            )
            try {
                startActivity(intent)
            } catch (_: Exception) {
                startActivity(Intent(Settings.ACTION_SETTINGS))
            }
            return
        }
        val needed = ArrayList<String>()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED
        ) needed += Manifest.permission.ACCESS_FINE_LOCATION
        if (android.os.Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.NEARBY_WIFI_DEVICES)
            != PackageManager.PERMISSION_GRANTED
        ) needed += Manifest.permission.NEARBY_WIFI_DEVICES
        if (needed.isEmpty()) {
            toggleDirect()
            return
        }
        directPending = true
        status("直连需要定位/附近 WiFi 权限")
        ActivityCompat.requestPermissions(this, needed.toTypedArray(), 101)
    }

    override fun onResume() {
        super.onResume()
        if (awaitingWriteSettings && Settings.System.canWrite(this)) {
            awaitingWriteSettings = false
            directWithPermission()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != 101) return
        val granted = grantResults.isNotEmpty() &&
            grantResults.all { it == PackageManager.PERMISSION_GRANTED }
        if (granted && directPending) {
            directPending = false
            directWithPermission()
        } else {
            directPending = false
            fail("缺少定位/附近 WiFi 权限，无法直连")
        }
    }

    private fun toggleDirect() {
        val btn = findViewById<Button>(R.id.btnRaceDirect)
        if (direct.isConnected) {
            direct.disconnect()
            btn.text = "直连 ESP WiFi"
            status("已断开 ESP 直连")
            return
        }
        status("正在请求直连 RaceBrick-XXXX…")
        direct.connect { ok, message ->
            status(message)
            if (ok) {
                etUrl.setText(WifiDirectConnector.AP_URL)
                RaceApi.saveBaseUrl(this, WifiDirectConnector.AP_URL)
                btn.text = "断开 ESP 直连"
                loadAll()
            } else {
                btn.text = "直连 ESP WiFi"
            }
        }
    }

    private fun base(): String = RaceApi.saveBaseUrl(this, etUrl.text.toString())

    private fun loadAll() {
        direct.bindCurrentEsp()
        val gen = ++generation
        val b = base()
        status("正在读取赛道和场次…")
        worker.execute {
            runCatching {
                val t = RaceApi.tracks(b)
                val s = RaceApi.sessions(b)
                t to s
            }.onSuccess { (t, s) ->
                main.post {
                    if (gen != generation) return@post
                    tracks = t
                    sessions = s
                    status("赛道 ${t.size} 条，场次 ${s.size} 场")
                    if (showingSessions) renderSessions() else renderTracks()
                }
            }.onFailure { e ->
                main.post {
                    if (gen != generation) return@post
                    fail("连接失败：${e.message}")
                    renderTracks()
                }
            }
        }
    }

    private fun renderTracks() {
        llList.removeAllViews()
        if (tracks.isEmpty()) {
            empty("没有赛道。设备首次启动会写入一条模拟赛道；也可以先在设备端保存赛道。")
            return
        }
        for (t in tracks) {
            val card = card()
            card.addView(title(t.name.ifBlank { "赛道 #${t.id}" }))
            card.addView(
                meta(
                    "${formatLength(t.lengthM)}  ·  ${t.sections} 个分段  ·  " +
                        if (t.geo) "有 GPS 基准" else "无 GPS 基准"
                )
            )
            card.addView(
                row(
                    button("改名") { renameTrack(t) },
                    button("查看场次") {
                        filterTrack = t.id
                        showingSessions = true
                        renderSessions()
                    },
                    button("删除", danger = true) { confirmDeleteTrack(t) },
                )
            )
            llList.addView(card)
        }
    }

    private fun renderSessions() {
        llList.removeAllViews()
        val filter = filterTrack
        val visible = if (filter == null) sessions else sessions.filter { it.trackSlot == filter }
        if (filter != null) {
            val header = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val name = tracks.firstOrNull { it.id == filter }?.name ?: "赛道 #$filter"
            header.addView(
                TextView(this).apply {
                    text = "筛选：$name"
                    setTextColor(0xFFF2B95C.toInt())
                    textSize = 13f
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }
            )
            header.addView(
                button("显示全部") {
                    filterTrack = null
                    renderSessions()
                }
            )
            llList.addView(header)
        }
        if (visible.isEmpty()) {
            empty(
                if (filter == null) "没有比赛场次。设备端进入 RACE 并在 GPS 有定位后开始比赛，结束后会自动保存。"
                else "这条赛道还没有场次。"
            )
            return
        }
        for (s in visible.sortedByDescending { it.startEpoch }) {
            val card = card()
            card.addView(title(s.trackName.ifBlank { "赛道 #${s.trackSlot}" }))
            card.addView(
                meta(
                    "${formatTime(s.startEpoch)}  ·  ${formatDuration(s.durationMs)}  ·  " +
                        "${s.laps} 圈  ·  ${s.samples} 点"
                )
            )
            if (s.deletedLapMask != 0) {
                card.addView(meta("已删除圈：${deletedLaps(s)}", warning = true))
            }
            card.addView(
                row(
                    button("详情") { showSessionDetail(s) },
                    button("导出 CSV") { exportSession(s) },
                    button("删除", danger = true) { confirmDeleteSession(s) },
                )
            )
            llList.addView(card)
        }
    }

    private fun showSessionDetail(summary: RaceSession) {
        status("正在读取场次详情…")
        val b = base()
        worker.execute {
            runCatching { RaceApi.session(b, summary.slot) }
                .onSuccess { s -> main.post { if (!isFinishing) buildDetailDialog(s).show() } }
                .onFailure { e -> main.post { fail("读取详情失败：${e.message}") } }
        }
    }

    private fun buildDetailDialog(s: RaceSession): AlertDialog {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        content.addView(meta("${formatTime(s.startEpoch)}  ·  ${formatDuration(s.durationMs)}  ·  ${s.samples} 点"))
        if (s.lapTimes.isEmpty()) {
            content.addView(meta("没有有效圈速。"))
        } else {
            for ((index, t) in s.lapTimes.withIndex()) {
                val lap = index + 1
                val deleted = (s.deletedLapMask and (1 shl (lap - 1))) != 0
                val row = LinearLayout(this).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(0, dp(6), 0, dp(6))
                }
                row.addView(
                    TextView(this).apply {
                        text = "第 $lap 圈   ${formatLap(t)}${if (deleted) "  (已删除)" else ""}"
                        setTextColor(if (deleted) 0xFF8A9A9A.toInt() else 0xFFF0F6F4.toInt())
                        textSize = 14f
                        layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                    }
                )
                if (!deleted) {
                    row.addView(
                        button("删除") { confirmDeleteLap(s, lap) }
                    )
                }
                content.addView(row)
            }
        }
        val scroll = ScrollView(this).apply { addView(content) }
        return AlertDialog.Builder(this)
            .setTitle("场次详情")
            .setView(scroll)
            .setPositiveButton("关闭", null)
            .create()
    }

    private fun renameTrack(t: RaceTrack) {
        val input = EditText(this).apply {
            setText(t.name)
            filters = arrayOf(InputFilter.LengthFilter(7))
            setSelection(text.length)
        }
        AlertDialog.Builder(this)
            .setTitle("赛道改名")
            .setMessage("最多 7 个字符；会写回 ESP 外置 Flash。")
            .setView(input)
            .setNegativeButton("取消", null)
            .setPositiveButton("保存") { _, _ ->
                val name = input.text.toString().trim()
                if (name.isEmpty()) {
                    toast("名称不能为空")
                    return@setPositiveButton
                }
                val b = base()
                status("正在保存赛道名…")
                worker.execute {
                    runCatching { RaceApi.renameTrack(b, t.id, name) }
                        .onSuccess {
                            main.post {
                                toast("已改名")
                                loadAll()
                            }
                        }
                        .onFailure { e -> main.post { fail("改名失败：${e.message}") } }
                }
            }
            .show()
    }

    private fun confirmDeleteTrack(t: RaceTrack) {
        confirm("删除赛道", "删除「${t.name}」？赛道几何会被清空，已保存场次不会删除。") {
            val gen = ++generation
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteTrack(b, t.id) }
                    .onSuccess {
                        main.post {
                            if (gen != generation) return@post
                            toast("已删除赛道")
                            filterTrack = null
                            loadAll()
                        }
                    }
                    .onFailure { e -> main.post { fail("删除失败：${e.message}") } }
            }
        }
    }

    private fun confirmDeleteSession(s: RaceSession) {
        confirm("删除场次", "删除这整场原始数据？此操作不可恢复。") {
            val gen = ++generation
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteSession(b, s.slot) }
                    .onSuccess {
                        main.post {
                            if (gen != generation) return@post
                            toast("已删除场次")
                            loadAll()
                        }
                    }
                    .onFailure { e -> main.post { fail("删除失败：${e.message}") } }
            }
        }
    }

    private fun confirmDeleteLap(s: RaceSession, lap: Int) {
        confirm("删除单圈", "只删除第 $lap 圈的圈速/轨迹标记，原始场次保留。") {
            val b = base()
            worker.execute {
                runCatching { RaceApi.deleteLap(b, s.slot, lap) }
                    .onSuccess {
                        main.post {
                            toast("已删除第 $lap 圈")
                            loadAll()
                        }
                    }
                    .onFailure { e -> main.post { fail("删除失败：${e.message}") } }
            }
        }
    }

    private fun exportSession(s: RaceSession) {
        status("正在下载 CSV…")
        val b = base()
        worker.execute {
            runCatching {
                val bytes = RaceApi.sessionCsv(b, s.slot)
                val dir = File(cacheDir, "race_exports").apply { mkdirs() }
                val file = File(dir, "racebrick-session-${s.id}.csv")
                file.writeBytes(bytes)
                file
            }.onSuccess { file ->
                main.post {
                    status("已生成 ${file.name}")
                    val uri = FileProvider.getUriForFile(
                        this,
                        "$packageName.files",
                        file,
                    )
                    val send = Intent(Intent.ACTION_SEND).apply {
                        type = "text/csv"
                        putExtra(Intent.EXTRA_STREAM, uri)
                        putExtra(Intent.EXTRA_SUBJECT, "RaceBrick session ${s.id}")
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    startActivity(Intent.createChooser(send, "导出到 RaceChrono / 文件"))
                }
            }.onFailure { e -> main.post { fail("导出失败：${e.message}") } }
        }
    }

    private fun deletedLaps(s: RaceSession): String =
        (1..s.laps).filter { (s.deletedLapMask and (1 shl (it - 1))) != 0 }
            .joinToString("、")

    private fun card(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundResource(R.drawable.bg_card)
        setPadding(dp(12), dp(12), dp(12), dp(12))
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(10) }
    }

    private fun title(text: String): TextView = TextView(this).apply {
        this.text = text
        setTextColor(0xFFF0F6F4.toInt())
        textSize = 16f
        setTypeface(typeface, Typeface.BOLD)
    }

    private fun meta(text: String, warning: Boolean = false): TextView = TextView(this).apply {
        this.text = text
        setTextColor(if (warning) 0xFFE0A040.toInt() else 0xFFA9C4BE.toInt())
        textSize = 12f
        setPadding(0, dp(4), 0, 0)
    }

    private fun row(vararg children: Button): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        setPadding(0, dp(8), 0, 0)
        for (child in children) {
            addView(child)
        }
    }

    private fun button(text: String, danger: Boolean = false, action: () -> Unit): Button =
        Button(this).apply {
            this.text = text
            textSize = 12f
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setBackgroundResource(if (danger) R.drawable.btn_danger else R.drawable.btn_secondary)
            backgroundTintList = null
            setTextColor(if (danger) 0xFFFFDADA.toInt() else 0xFFD7E7E4.toInt())
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                .apply { marginStart = dp(4) }
            setOnClickListener { action() }
        }

    private fun empty(text: String) {
        llList.addView(TextView(this).apply {
            this.text = text
            setTextColor(0xFF6D817D.toInt())
            textSize = 13f
            setPadding(dp(4), dp(12), dp(4), dp(12))
        })
    }

    private fun status(text: String) {
        tvStatus.text = text
        tvStatus.setTextColor(0xFFA9C4BE.toInt())
    }

    private fun fail(text: String) {
        tvStatus.text = text
        tvStatus.setTextColor(0xFFFF9B9B.toInt())
    }

    private fun toast(text: String) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show()
    }

    private fun confirm(title: String, message: String, action: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setNegativeButton("取消", null)
            .setPositiveButton("确认") { _, _ -> action() }
            .show()
    }

    private fun formatTime(epoch: Long): String {
        if (epoch <= 1_600_000_000L) return "时间未同步"
        return SimpleDateFormat("MM-dd HH:mm:ss", Locale.getDefault()).format(Date(epoch * 1000L))
    }

    private fun formatDuration(ms: Long): String {
        val total = (ms / 1000L).coerceAtLeast(0L)
        return String.format(Locale.US, "%d:%02d", total / 60L, total % 60L)
    }

    private fun formatLap(seconds: Double): String {
        if (seconds <= 0.0) return "--"
        val total = seconds
        val m = (total / 60.0).toInt()
        val s = total - m * 60
        return String.format(Locale.US, "%d:%06.3f", m, s)
    }

    private fun formatLength(m: Double): String =
        if (m >= 1000.0) String.format(Locale.US, "%.2f km", m / 1000.0)
        else String.format(Locale.US, "%.0f m", m)

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    override fun onDestroy() {
        generation++
        direct.disconnect()
        worker.shutdownNow()
        super.onDestroy()
    }
}
