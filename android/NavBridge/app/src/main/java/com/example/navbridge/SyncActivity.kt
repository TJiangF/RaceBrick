package com.example.navbridge

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class SyncActivity : AppCompatActivity() {
    companion object {
        const val EXTRA_BASE = "base"
        const val EXTRA_UID = "uid"
    }

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val db by lazy { SyncLocalDb(this) }
    private val cloud = CloudApi()
    private var base = ""
    private var uid: String? = null
    private var deviceStatus = "未检测"
    private var deviceCounts = "—"
    private var cloudStatus = "未登录"
    private var cloudCounts = "—"
    private var busy: AlertDialog? = null
    private lateinit var content: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        base = intent.getStringExtra(EXTRA_BASE) ?: RaceApi.baseUrl(this)
        uid = intent.getStringExtra(EXTRA_UID) ?: SyncLocalDb(this).summary().uid
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF080C11.toInt())
            setPadding(dp(16), dp(24), dp(16), dp(16))
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        header.addView(TextView(this).apply {
            text = "SYNC"
            setTextColor(0xFF66E0D2.toInt())
            textSize = 22f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        header.addView(Button(this).apply {
            text = "BACK"
            setOnClickListener { finish() }
        })
        root.addView(header)
        content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(content) }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
        ))
        setContentView(root)
        renderLocal()
        refresh()
    }

    private fun refresh() {
        renderLocal()
        worker.execute {
            var dev = "不可达"
            var dc = "—"
            try {
                val st = RaceApi.status(base)
                val u = st.optString("uid", "")
                if (u.isNotBlank()) uid = u
                val t = RaceApi.tracks(base).size
                val s = RaceApi.sessions(base).size
                val l = RaceApi.logs(base).size
                dev = "在线 · ${st.optString("ip", base)}"
                dc = "$t tracks · $s sessions · $l logs"
            } catch (e: Exception) {
                dev = "离线 · ${e.message}"
            }
            var cs = "未登录"
            var cc = "—"
            val token = CloudStore.token(this)
            if (token != null && uid != null) {
                try {
                    val st = cloud.state(uid!!, token)
                    cs = "在线 · 版本 ${st.dataVersion}"
                    cc = "${st.tracks} tracks · ${st.sessions} sessions · ${st.logs} logs"
                } catch (e: Exception) {
                    cs = "错误 · ${e.message}"
                }
            }
            main.post {
                deviceStatus = dev
                deviceCounts = dc
                cloudStatus = cs
                cloudCounts = cc
                renderLocal()
            }
        }
    }

    private fun renderLocal() {
        if (!::content.isInitialized) return
        content.removeAllViews()
        val local = db.summary()
        val dirty = if (local.dirtyDevice) "dirtyDevice" else if (local.dirtyCloud) "dirtyCloud" else "clean"
        content.addView(panel("RACEBRICK DEVICE", deviceStatus, deviceCounts, "last modified: unknown\nstable IDs: unavailable"))
        content.addView(connector("device ⇄ local: manifest API pending"))
        content.addView(panel("LOCAL APP", "SQLite · $dirty", "${local.tracks} tracks · ${local.sessions} sessions · ${local.logs} logs",
            "dataset v${local.datasetRevision}\nlast local update: ${date(local.updatedAt)}"))
        content.addView(connector("local ⇄ cloud: manual backup only"))
        content.addView(panel("CLOUD", cloudStatus, cloudCounts,
            "last upload: ${date(CloudStore.lastSyncAt(this, uid ?: ""))}\nmanual restore only"))
        content.addView(TextView(this).apply {
            text = "ID/版本说明：设备数据当前还没有稳定 UUID/版本，页面只显示计数与状态；不会显示伪造的逐项 diff。"
            setTextColor(0xFFE0A040.toInt())
            textSize = 12f
            setPadding(0, dp(8), 0, dp(8))
        })
        content.addView(button("VIEW LOCAL DATA（雷达图）") {
            startActivity(android.content.Intent(this, LocalRecordsActivity::class.java))
        })
        content.addView(button("Load RACEBRICK -> LOCAL APP") { loadDevice() })
        val dirtyCount = db.dirtyTracks().size
        content.addView(button("PUSH LOCAL TRACK CHANGES -> RACEBRICK (${dirtyCount})") { pushLocalTracks() })
        content.addView(button("BACKUP LOCAL -> CLOUD（待增量 API）") { toast("增量云备份尚未实现") })
        content.addView(button("RESTORE CLOUD -> LOCAL（待版本校验）") { toast("云端恢复尚未实现") })
        content.addView(button(if (CloudStore.token(this) == null) "LOGIN CLOUD" else "REFRESH CLOUD") {
            if (CloudStore.token(this) == null) loginDialog() else refresh()
        })
    }

    private fun panel(title: String, status: String, counts: String, detail: String): LinearLayout {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF111923.toInt())
            setPadding(dp(14), dp(12), dp(14), dp(12))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8) }
        }
        box.addView(TextView(this).apply {
            text = title
            setTextColor(0xFF66E0D2.toInt())
            textSize = 16f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        })
        box.addView(label(status, 0xFFF0F6F4.toInt()))
        box.addView(label(counts, 0xFFA9C4BE.toInt()))
        box.addView(label(detail, 0xFF7F93A5.toInt()))
        return box
    }

    private fun connector(text: String): TextView = label("  ↕  $text", 0xFF8FA2B5.toInt()).apply {
        textSize = 11f
        setPadding(dp(8), dp(5), 0, dp(5))
    }

    private fun label(text: String, color: Int): TextView = TextView(this).apply {
        this.text = text
        setTextColor(color)
        textSize = 13f
        setPadding(0, dp(3), 0, 0)
    }

    private fun button(text: String, action: () -> Unit): Button = Button(this).apply {
        this.text = text
        textSize = 12f
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(8) }
        setOnClickListener { action() }
    }

    private fun loadDevice() {
        if (busy != null) return
        val dialog = AlertDialog.Builder(this)
            .setTitle("Load device data")
            .setMessage("读取设备并写入 Local App 工作副本。不会修改设备或云端。")
            .setNegativeButton("取消", null)
            .setPositiveButton("继续") { _, _ -> doLoad() }
            .create()
        dialog.show()
        Lang.localizeDialog(dialog)
    }

    private fun doLoad() {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(12), dp(20), dp(16))
        }
        val msg = TextView(this).apply { text = "准备读取设备…"; setTextColor(0xFFA9C4BE.toInt()) }
        box.addView(msg)
        box.addView(ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            isIndeterminate = true
        })
        busy = AlertDialog.Builder(this).setTitle("Device -> Local").setView(box)
            .setCancelable(false).create().also { it.show() }
        worker.execute {
            try {
                val status = RaceApi.status(base)
                val deviceUid = status.optString("uid", uid ?: "")
                if (deviceUid.isBlank()) throw IllegalStateException("设备未返回 UID")
                val snapshot = DeviceSnapshot.build(base) { m, _ ->
                    main.post { msg.text = m }
                }
                val s = db.importDeviceSnapshot(deviceUid, snapshot.body)
                main.post {
                    busy?.dismiss(); busy = null
                    uid = deviceUid
                    toast("Local App 已载入：${s.tracks} tracks / ${s.sessions} sessions / ${s.logs} logs")
                    renderLocal()
                }
            } catch (e: Exception) {
                main.post {
                    busy?.dismiss(); busy = null
                    toast("读取失败：${e.message}")
                }
            }
        }
    }

    private fun pushLocalTracks() {
        if (busy != null) return
        val tracks = db.dirtyTracks()
        if (tracks.isEmpty()) { toast("没有本地 Track 修改"); return }
        AlertDialog.Builder(this)
            .setTitle("Push local tracks")
            .setMessage("将 ${tracks.size} 条本地 Track 修改写回 RaceBrick。不会上传云端。")
            .setNegativeButton("取消", null)
            .setPositiveButton("推送") { _, _ -> doPushTracks(tracks) }
            .create().also { it.show(); Lang.localizeDialog(it) }
    }

    private fun doPushTracks(tracks: List<SyncLocalDb.LocalEntity>) {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(12), dp(20), dp(16))
        }
        val msg = TextView(this).apply { text = "正在推送 Track…"; setTextColor(0xFFA9C4BE.toInt()) }
        box.addView(msg)
        busy = AlertDialog.Builder(this).setTitle("Local -> Device").setView(box)
            .setCancelable(false).create().also { it.show() }
        worker.execute {
            var ok = 0
            val failed = ArrayList<String>()
            for ((i, e) in tracks.withIndex()) {
                val p = e.payload
                try {
                    RaceApi.updateTrack(
                        base, e.localKey.toInt(), p.optString("name", ""),
                        p.optDouble("start_f", 0.0), p.optBoolean("reverse", false),
                        p.optDouble("start_width", p.optDouble("width", 5.0)),
                        p.optDouble("start_offset", 0.0),
                    )
                    db.markTrackDeviceClean(e.uuid)
                    ok++
                } catch (ex: Exception) {
                    failed += e.name.ifBlank { e.localKey }
                }
                main.post { msg.text = "推送 ${i + 1}/${tracks.size}" }
            }
            main.post {
                busy?.dismiss(); busy = null
                toast(if (failed.isEmpty()) "已推送 $ok 条 Track" else "成功 $ok，失败 ${failed.size}：${failed.first()}")
                refresh()
            }
        }
    }

    private fun loginDialog() {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
        }
        val user = EditText(this).apply { hint = "RaceBrick Cloud 用户名" }
        val pass = EditText(this).apply {
            hint = "密码"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        box.addView(user)
        box.addView(pass)
        val d = AlertDialog.Builder(this).setTitle("LOGIN CLOUD").setView(box)
            .setNegativeButton("取消", null).setPositiveButton("登录", null).create()
        d.setOnShowListener {
            d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                worker.execute {
                    try {
                        val a = cloud.login(user.text.toString().trim(), pass.text.toString())
                        CloudStore.save(this, a.token, a.username)
                        main.post { d.dismiss(); toast("已登录 ${a.username}"); refresh() }
                    } catch (e: Exception) {
                        main.post { toast("登录失败：${e.message}") }
                    }
                }
            }
        }
        d.show()
        Lang.localizeDialog(d)
    }

    private fun date(ts: Long): String =
        if (ts <= 0) "—" else SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(ts * 1000L))

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    override fun onDestroy() {
        worker.shutdownNow()
        busy?.dismiss()
        super.onDestroy()
    }
}
