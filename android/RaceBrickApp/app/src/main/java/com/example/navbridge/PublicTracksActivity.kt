package com.example.navbridge

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class PublicTracksActivity : AppCompatActivity() {
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val api = CloudApi()
    private val db by lazy { SyncLocalDb(this) }
    private lateinit var list: LinearLayout
    private lateinit var status: TextView
    private lateinit var more: Button
    private var nextCursor: String? = null
    private var loading = false
    private var imported = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
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
            text = "公开赛道"
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
            text = "正在读取公开赛道…"
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 12f
            setPadding(0, dp(6), 0, dp(4))
        }
        root.addView(status)
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(list) }, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f,
        ))
        more = Button(this).apply {
            text = "加载更多"
            visibility = android.view.View.GONE
            setOnClickListener { load(nextCursor, append = true) }
        }
        root.addView(more)
        setContentView(root)
        load(null, append = false)
    }

    override fun finish() {
        if (imported) setResult(RESULT_OK)
        super.finish()
    }

    override fun onDestroy() {
        worker.shutdownNow()
        super.onDestroy()
    }

    private fun load(cursor: String?, append: Boolean) {
        if (loading) return
        loading = true
        status.text = "正在读取公开赛道…"
        worker.execute {
            runCatching { api.publicTracks(20, cursor) }
                .onSuccess { page ->
                    main.post {
                        loading = false
                        if (!append) list.removeAllViews()
                        nextCursor = page.nextCursor
                        more.visibility = if (nextCursor != null) android.view.View.VISIBLE else android.view.View.GONE
                        if (page.tracks.isEmpty() && !append) {
                            status.text = "没有公开赛道"
                        } else {
                            status.text = "公开赛道 ${if (append) "已加载更多" else "${page.tracks.size} 条"}"
                            for (t in page.tracks) list.addView(card(t))
                        }
                        if (page.tracks.isEmpty() && !append) {
                            list.addView(TextView(this).apply {
                                text = "暂时没有公开赛道。"
                                setTextColor(0xFF8A9A9A.toInt())
                                setPadding(dp(4), dp(12), dp(4), dp(12))
                            })
                        }
                    }
                }
                .onFailure { e ->
                    main.post {
                        loading = false
                        status.text = "读取失败：${e.message}"
                    }
                }
        }
    }

    private fun card(t: PublicTrackSummary): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(0xFF111923.toInt())
        setPadding(dp(14), dp(12), dp(14), dp(12))
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = dp(8) }
        addView(TextView(this@PublicTracksActivity).apply {
            text = t.name.ifBlank { "未命名赛道" }
            setTextColor(0xFFF0F6F4.toInt())
            textSize = 16f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        })
        addView(TextView(this@PublicTracksActivity).apply {
            text = String.format(Locale.US, "%.0f m  ·  %s", t.lengthM, if (t.geo) "GPS" else "本地")
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 12f
            setPadding(0, dp(4), 0, 0)
        })
        addView(TextView(this@PublicTracksActivity).apply {
            text = "更新 ${fmt(t.updatedAt)}"
            setTextColor(0xFF6D817D.toInt())
            textSize = 11f
            setPadding(0, dp(2), 0, 0)
        })
        addView(Button(this@PublicTracksActivity).apply {
            text = "详情 / 下载"
            setOnClickListener { openDetail(t) }
        })
    }

    private fun openDetail(summary: PublicTrackSummary) {
        worker.execute {
            runCatching {
                val track = api.publicTrack(summary.publicId)
                val board = runCatching { api.publicLeaderboard(summary.publicId) }.getOrNull()
                track to board
            }
                .onSuccess { (track, board) -> main.post { showDetail(track, board) } }
                .onFailure { e -> main.post { toast("读取详情失败：${e.message}") } }
        }
    }

    private fun showDetail(track: PublicTrack, board: PublicLeaderboardPage?) {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(8))
        }
        val points = pointsOf(track.payload)
        if (points.size >= 2) {
            val map = TrackMapView(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(260),
                )
                setLayers(listOf(TrackMapView.Layer(points, 0xFF66E0D2.toInt(), track.name)))
                startF = track.payload.optDouble("start_f", 0.0).toFloat()
                widthM = track.payload.optDouble("start_width", 10.0).toFloat()
                trackLengthM = track.payload.optDouble("length_m", 0.0).toFloat()
                startOffsetM = track.payload.optDouble("start_offset", 0.0).toFloat()
                reverse = track.payload.optBoolean("reverse", false)
            }
            box.addView(map)
        }
        box.addView(TextView(this).apply {
            text = "${track.name.ifBlank { "未命名赛道" }}\n" +
                String.format(Locale.US, "长度 %.0f m  ·  %s  ·  公开 ID %s",
                    track.payload.optDouble("length_m", 0.0),
                    if (track.payload.optBoolean("geo", false)) "GPS" else "本地",
                    track.publicId)
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 12f
            setPadding(0, dp(6), 0, 0)
        })
        box.addView(TextView(this).apply {
            text = "导入后会保留公开赛道 UUID；同名赛道不会覆盖，重复下载不会创建副本。"
            setTextColor(0xFF6D817D.toInt())
            textSize = 11f
            setPadding(0, dp(6), 0, 0)
        })
        box.addView(TextView(this).apply {
            text = "圈速榜单"
            setTextColor(0xFF66E0D2.toInt())
            textSize = 14f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            setPadding(0, dp(10), 0, dp(2))
        })
        if (board == null) {
            box.addView(TextView(this).apply {
                text = "榜单读取失败"
                setTextColor(0xFFE0A040.toInt())
                textSize = 12f
            })
        } else if (board.entries.isEmpty()) {
            box.addView(TextView(this).apply {
                text = "暂无公开圈速"
                setTextColor(0xFF8A9A9A.toInt())
                textSize = 12f
            })
        } else {
            for (e in board.entries) {
                box.addView(TextView(this).apply {
                    text = "P${e.rank}  ${e.driver}  ·  L${e.lapNumber}  ${formatLapMs(e.lapTimeMs)}"
                    setTextColor(0xFFE8F3F0.toInt())
                    textSize = 12f
                    setPadding(0, dp(3), 0, 0)
                })
            }
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("公开赛道")
            .setView(ScrollView(this).apply { addView(box) })
            .setNegativeButton("取消", null)
            .setPositiveButton("导入 Local", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                worker.execute {
                    val result = db.importSharedTrack(track.payload)
                    main.post {
                        when (result.status) {
                            "imported" -> {
                                imported = true
                                toast("已导入：${result.name}")
                                dialog.dismiss()
                            }
                            "exists" -> toast("本地已有相同 UUID 的赛道，未覆盖")
                            else -> toast("赛道数据无效，无法导入")
                        }
                    }
                }
            }
        }
        dialog.show()
        Lang.localizeDialog(dialog)
    }

    private fun pointsOf(payload: JSONObject): List<android.graphics.PointF> {
        val arr = payload.optJSONArray("points") ?: return emptyList()
        val out = ArrayList<android.graphics.PointF>()
        if (arr.length() > 0 && arr.opt(0) is JSONArray) {
            for (i in 0 until arr.length()) {
                val p = arr.optJSONArray(i) ?: continue
                if (p.length() >= 2) out += android.graphics.PointF(
                    p.optDouble(0).toFloat(), p.optDouble(1).toFloat(),
                )
            }
        } else {
            var i = 0
            while (i + 1 < arr.length()) {
                out += android.graphics.PointF(
                    arr.optDouble(i).toFloat(), arr.optDouble(i + 1).toFloat(),
                )
                i += 2
            }
        }
        return out
    }

    private fun formatLapMs(ms: Long): String {
        val total = ms.coerceAtLeast(0L)
        val m = total / 60000
        val s = (total % 60000) / 1000
        val millis = total % 1000
        return String.format(Locale.US, "%d:%02d.%03d", m, s, millis)
    }

    private fun fmt(epoch: Long): String =
        if (epoch <= 0) "未知" else SimpleDateFormat("MM-dd HH:mm", Locale.getDefault())
            .format(Date(epoch * 1000L))

    private fun toast(text: String) {
        Toast.makeText(this, Lang.tr(text), Toast.LENGTH_SHORT).show()
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
