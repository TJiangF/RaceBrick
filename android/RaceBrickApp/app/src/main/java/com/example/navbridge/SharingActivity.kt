package com.example.navbridge

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.util.concurrent.Executors

class SharingActivity : AppCompatActivity() {
    companion object {
        const val EXTRA_UID = "uid"
    }

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val api = CloudApi()
    private lateinit var uid: String
    private lateinit var status: TextView
    private lateinit var content: LinearLayout
    private var busy = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        uid = intent.getStringExtra(EXTRA_UID).orEmpty()
        val token = CloudStore.token(this)
        if (uid.isBlank() || token.isNullOrBlank()) {
            Toast.makeText(this, "请先登录云端并绑定设备", Toast.LENGTH_SHORT).show()
            finish()
            return
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF080C11.toInt())
            setPadding(dp(16), dp(24), dp(16), dp(16))
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(TextView(this).apply {
            text = "公开与 RAW 授权"
            setTextColor(0xFF66E0D2.toInt())
            textSize = 22f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        header.addView(Button(this).apply {
            text = "返回"
            setOnClickListener { finish() }
        })
        root.addView(header)
        status = TextView(this).apply {
            text = "正在读取分享状态…"
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 12f
            setPadding(0, dp(6), 0, dp(4))
        }
        root.addView(status)
        content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(content) }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f,
        ))
        setContentView(root)
        load()
    }

    override fun onDestroy() {
        worker.shutdownNow()
        super.onDestroy()
    }

    private fun load() {
        val token = CloudStore.token(this) ?: return
        status.text = "正在读取分享状态…"
        worker.execute {
            runCatching { api.deviceSharing(uid, token) }
                .onSuccess { sharing -> main.post { render(sharing) } }
                .onFailure { e -> main.post { status.text = "读取失败：${e.message}" } }
        }
    }

    private fun render(sharing: DeviceSharing) {
        status.text = "设备 ${uid} · ${sharing.tracks.size} Tracks / ${sharing.sessions.size} Sessions"
        content.removeAllViews()
        content.addView(TextView(this).apply {
            text = "赛道公开会暴露坐标和起始线位置；圈速、RAW 是两个独立授权。"
            setTextColor(0xFFE0A040.toInt())
            textSize = 11f
            setPadding(0, 0, 0, dp(8))
        })
        if (sharing.tracks.isEmpty()) {
            content.addView(label("没有 Track"))
        }
        for (t in sharing.tracks) {
            val box = box()
            box.addView(label(t.name.ifBlank { "Track ${t.localId}" }))
            val public = CheckBox(this).apply {
                text = "公开赛道（会显示在公开目录）"
                isChecked = t.isPublic
                setOnCheckedChangeListener { _, checked ->
                    if (!checked) {
                        updateTrack(t, false)
                    } else {
                        confirm(
                            "公开赛道",
                            "公开后其他用户可以看到赛道坐标和起始线。确定公开？",
                            yes = { updateTrack(t, true) },
                            no = { isChecked = false },
                        )
                    }
                }
            }
            box.addView(public)
            content.addView(box)
        }
        for (s in sharing.sessions) {
            val box = box()
            box.addView(label("${s.trackName.ifBlank { "Session ${s.localSlot}" }} · L${s.localSlot}"))
            val laps = CheckBox(this).apply {
                text = "公开圈速榜单"
                isChecked = s.lapsPublic
                setOnCheckedChangeListener { _, checked -> updateSession(s, checked, null) }
            }
            val raw = CheckBox(this).apply {
                text = "授权登录用户下载 RAW"
                isChecked = s.rawPublic
                setOnCheckedChangeListener { _, checked ->
                    if (!checked) {
                        updateSession(s, null, false)
                    } else {
                        confirm(
                            "RAW 下载授权",
                            "RAW 包含完整轨迹、速度和圈信息。授权后其他登录用户可下载。确定授权？",
                            yes = { updateSession(s, null, true) },
                            no = { isChecked = false },
                        )
                    }
                }
            }
            box.addView(laps)
            box.addView(raw)
            content.addView(box)
        }
    }

    private fun updateTrack(track: SharingTrack, public: Boolean) {
        val token = CloudStore.token(this) ?: return
        if (busy) return
        busy = true
        worker.execute {
            runCatching { api.setTrackSharing(uid, track.trackUuid, public, token) }
                .onSuccess { main.post { busy = false; status.text = "Track 已${if (public) "公开" else "撤回"}"; load() } }
                .onFailure { e -> main.post { busy = false; toast("更新失败：${e.message}"); load() } }
        }
    }

    private fun updateSession(
        session: SharingSession,
        lapsPublic: Boolean?,
        rawPublic: Boolean?,
    ) {
        val token = CloudStore.token(this) ?: return
        if (busy) return
        busy = true
        worker.execute {
            runCatching { api.setSessionSharing(uid, session.sessionUuid, lapsPublic, rawPublic, token) }
                .onSuccess { main.post { busy = false; status.text = "Session 授权已更新"; load() } }
                .onFailure { e -> main.post { busy = false; toast("更新失败：${e.message}"); load() } }
        }
    }

    private fun box(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(0xFF111923.toInt())
        setPadding(dp(12), dp(10), dp(12), dp(10))
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = dp(8) }
    }

    private fun label(text: String): TextView = TextView(this).apply {
        this.text = text
        setTextColor(0xFFA9C4BE.toInt())
        textSize = 13f
    }

    private fun confirm(title: String, message: String, yes: () -> Unit, no: () -> Unit) {
        val d = AlertDialog.Builder(this)
            .setTitle(Lang.tr(title))
            .setMessage(Lang.tr(message))
            .setNegativeButton(Lang.tr("取消")) { _, _ -> no() }
            .setPositiveButton(Lang.tr("确认")) { _, _ -> yes() }
            .create()
        d.show()
        Lang.localizeDialog(d)
    }

    private fun toast(text: String) {
        Toast.makeText(this, Lang.tr(text), Toast.LENGTH_SHORT).show()
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
