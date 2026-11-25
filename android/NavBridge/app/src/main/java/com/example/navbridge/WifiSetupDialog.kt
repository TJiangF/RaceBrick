package com.example.navbridge

import android.app.Activity
import android.app.Dialog
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog

/** BLE WiFi provisioning UI. The ESP scans/connects; the phone only sends
 *  #W commands over the existing UART service and displays the replies. */
class WifiSetupDialog(
    private val activity: Activity,
    private val ble: BleClient,
) {
    private val dialog = Dialog(activity)
    private val status = TextView(activity)
    private val ssid = EditText(activity)
    private val pass = EditText(activity)
    private val list = LinearLayout(activity)
    private var rowCount = 0
    private val savedSet = HashSet<String>()
    private var scanAfterSavedList = false

    init {
        val d = activity.resources.displayMetrics.density
        fun px(v: Int) = (v * d).toInt()
        fun rounded(color: Int, radius: Float = 12f): GradientDrawable =
            GradientDrawable().apply {
                setColor(color)
                cornerRadius = radius * d
            }

        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(20), px(16), px(20), px(16))
            background = rounded(0xFF0B1316.toInt())
        }
        val title = TextView(activity).apply {
            text = "ESP WiFi 配网 / 手机热点"
            setTextColor(0xFF66E0D2.toInt())
            textSize = 20f
            setPadding(0, 0, 0, px(6))
        }
        status.apply {
            text = "正在读取设备 WiFi 状态…"
            setTextColor(0xFFA9C4BE.toInt())
            textSize = 13f
            setPadding(0, 0, 0, px(10))
        }
        root.addView(title)
        root.addView(TextView(activity).apply {
            text = "手机热点模式：先打开手机个人热点（2.4GHz），在下面输入热点名称和密码。设备连上后，RACE 页会自动发现并询问是否同步云端。"
            setTextColor(0xFF6D817D.toInt())
            textSize = 11f
            setPadding(0, 0, 0, px(8))
        })
        root.addView(status)

        val actions = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
        }
        fun action(text: String, onClick: () -> Unit): Button =
            Button(activity).apply {
                this.text = text
                textSize = 12f
                setTextColor(0xFFD7E7E4.toInt())
                background = rounded(0xFF183037.toInt(), 10f)
                setOnClickListener { onClick() }
            }
        val btnScan = action("扫描附近网络") { scan() }
        val btnSaved = action("已保存网络") { saved() }
        val btnStatus = action("刷新状态") { ble.send("#W,STATUS") }
        actions.addView(btnScan, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actions.addView(btnSaved, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actions.addView(btnStatus, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(actions)

        ssid.apply {
            hint = "WiFi / 手机热点名称（SSID）"
            setSingleLine(true)
            setTextColor(Color.WHITE)
            setHintTextColor(0xFF6D817D.toInt())
            setPadding(px(12), px(10), px(12), px(10))
            background = rounded(0xFF111E22.toInt(), 10f)
        }
        pass.apply {
            hint = "WiFi / 手机热点密码（开放网络留空）"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setSingleLine(true)
            setTextColor(Color.WHITE)
            setHintTextColor(0xFF6D817D.toInt())
            setPadding(px(12), px(10), px(12), px(10))
            background = rounded(0xFF111E22.toInt(), 10f)
        }
        root.addView(ssid, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = px(10) })
        root.addView(pass, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = px(8) })

        val connect = Button(activity).apply {
            text = "连接并保存到 ESP"
            setTextColor(0xFFF2FFFB.toInt())
            background = rounded(0xFF176B67.toInt(), 10f)
            setOnClickListener {
                val s = ssid.text.toString().trim()
                if (s.isEmpty()) {
                    Toast.makeText(activity, Lang.tr("请输入 WiFi 名称"), Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }
                status.text = "正在请求 ESP 连接 $s …"
                ble.send("#W,CONNECT,${Uri.encode(s)},${Uri.encode(pass.text.toString())}")
            }
        }
        root.addView(connect, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = px(8) })

        list.orientation = LinearLayout.VERTICAL
        val scroll = ScrollView(activity).apply {
            addView(list, ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }
        root.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, px(300)
        ).apply { topMargin = px(8) })

        dialog.setContentView(root)
        dialog.setCanceledOnTouchOutside(true)
        dialog.window?.setLayout(
            (activity.resources.displayMetrics.widthPixels * 0.92f).toInt(),
            ViewGroup.LayoutParams.WRAP_CONTENT,
        )
    }

    private fun scan() {
        // Keep the previous list visible while the ESP performs its blocking
        // scan. It is replaced only when the fresh SCAN_BEGIN arrives.
        status.text = "ESP 正在扫描附近 WiFi…"
        ble.send("#W,SCAN")
    }

    private fun saved() {
        status.text = "读取 ESP 已保存的网络…"
        scanAfterSavedList = false
        clearList()
        ble.send("#W,LIST")
    }

    fun show() {
        dialog.show()
        Lang.localizeDialog(dialog)
        ble.send("#W,STATUS")
        savedSet.clear()
        clearList()
        scanAfterSavedList = true
        ble.send("#W,LIST")
    }

    private fun connectSaved(name: String) {
        status.text = "正在连接已保存网络 $name …"
        ble.send("#W,CONNECT_SAVED,${Uri.encode(name)}")
    }

    private fun forget(name: String) {
        val d = AlertDialog.Builder(activity)
            .setTitle("忘记 WiFi")
            .setMessage("从 ESP 删除已保存的「$name」？")
            .setNegativeButton("取消", null)
            .setPositiveButton("忘记") { _, _ ->
                ble.send("#W,FORGET,${Uri.encode(name)}")
                status.text = "正在忘记 $name …"
            }
            .create()
        d.show()
        Lang.localizeDialog(d)
    }

    fun dismiss() {
        if (dialog.isShowing) dialog.dismiss()
    }

    private fun clearList() {
        list.removeAllViews()
        rowCount = 0
    }

    private fun addRow(
        text: String,
        saved: Boolean = false,
        onForget: (() -> Unit)? = null,
        onClick: () -> Unit,
    ) {
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = GradientDrawable().apply {
                setColor(if (saved) 0xFF123528.toInt() else 0xFF101B1F.toInt())
                cornerRadius = 8f * activity.resources.displayMetrics.density
            }
        }
        val b = Button(activity).apply {
            this.text = text
            textSize = 13f
            gravity = Gravity.START or Gravity.CENTER_VERTICAL
            setTextColor(if (saved) 0xFF5ED6A3.toInt() else 0xFFD7E7E4.toInt())
            background = null
            setOnClickListener { onClick() }
        }
        row.addView(b, LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f,
        ))
        if (onForget != null) {
            val forget = Button(activity).apply {
                this.text = "忘记"
                textSize = 12f
                setTextColor(0xFFFFB3C0.toInt())
                background = GradientDrawable().apply {
                    setColor(0xFF3A1720.toInt())
                    cornerRadius = 8f * activity.resources.displayMetrics.density
                }
                setOnClickListener { onForget() }
            }
            row.addView(forget, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ))
        }
        list.addView(row, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = (4 * activity.resources.displayMetrics.density).toInt() })
        rowCount++
    }

    /** Called on the UI thread by MainActivity for every #W notification. */
    fun onLine(line: String) {
        if (!line.startsWith("#W,")) return
        val body = line.substring(3)
        val parts = body.split(',')
        when (parts.firstOrNull()) {
            "STATUS" -> {
                val text = Uri.decode(body.substringAfter(',', ""))
                status.text = text.ifEmpty { "未知状态" }
                Regex("(\\d{1,3}(?:\\.\\d{1,3}){3})").find(text)?.value?.let { ip ->
                    RaceApi.saveBaseUrl(activity, "http://$ip")
                    status.text = "$text\n设备地址已记录：http://$ip"
                }
            }
            "SCAN_START" -> {
                status.text = "ESP 正在扫描附近 WiFi…"
            }
            "SCAN_BEGIN" -> {
                clearList()
                status.text = "发现 ${parts.getOrNull(1) ?: "0"} 个 WiFi"
            }
            "SCAN_END" -> {
                status.text = "扫描完成，共 ${parts.getOrNull(1) ?: rowCount} 个；绿色为已保存，点击自动连接"
            }
            "AP" -> {
                val name = Uri.decode(parts.getOrNull(2) ?: "")
                val rssi = parts.getOrNull(3) ?: ""
                val auth = parts.getOrNull(4) ?: "0"
                val saved = savedSet.contains(name)
                val lock = if (auth != "0") "加密" else "开放"
                val suffix = if (saved) "  SAVED · 自动连接" else ""
                addRow("$name   $rssi dBm  $lock$suffix", saved) {
                    if (saved) connectSaved(name) else {
                        ssid.setText(name)
                        pass.requestFocus()
                    }
                }
            }
            "SAVED" -> {
                val name = Uri.decode(parts.getOrNull(2) ?: "")
                savedSet.add(name)
                addRow("已保存：$name   点击自动连接", saved = true, onForget = { forget(name) }) {
                    connectSaved(name)
                }
            }
            "SAVED_END" -> {
                status.text = "已保存 ${parts.getOrNull(1) ?: rowCount} 个网络"
                if (scanAfterSavedList) {
                    scanAfterSavedList = false
                    scan()
                }
            }
            "FORGOTTEN" -> {
                val name = Uri.decode(parts.getOrNull(1) ?: "")
                savedSet.remove(name)
                status.text = "已忘记 $name"
                saved()
            }
            "ERR" -> {
                status.text = Uri.decode(parts.getOrNull(1) ?: "操作失败")
            }
        }
    }
}
