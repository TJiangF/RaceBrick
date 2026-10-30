package com.example.navbridge

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity

class SettingsActivity : AppCompatActivity() {
    private lateinit var etKey: EditText
    private lateinit var tvSign: TextView
    private lateinit var tvVersion: TextView
    private lateinit var tvMediaStatus: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        etKey = findViewById(R.id.etKey)
        tvSign = findViewById(R.id.tvSign)
        tvVersion = findViewById(R.id.tvVersion)
        tvMediaStatus = findViewById(R.id.tvMediaStatus)

        etKey.setText(Keys.amap(this))
        tvSign.text = "包名：$packageName\nSHA1：${Keys.signSha1(this)}"
        tvVersion.text = "NavBridge ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})"

        findViewById<Button>(R.id.btnBack).setOnClickListener { finish() }
        findViewById<Button>(R.id.btnBackground).setOnClickListener {
            startActivity(Intent(
                Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.parse("package:$packageName"),
            ))
        }
        findViewById<Button>(R.id.btnSaveKey).setOnClickListener {
            val key = etKey.text.toString().trim()
            if (key.isEmpty()) {
                Toast.makeText(this, "Key 不能为空", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            if (!Keys.save(this, key)) {
                Toast.makeText(this, "保存失败", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            Toast.makeText(this, "已保存，正在重启", Toast.LENGTH_SHORT).show()
            val i = Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK)
            startActivity(i)
            Runtime.getRuntime().exit(0)
        }
    }

    override fun onResume() {
        super.onResume()
        tvMediaStatus.text = if (MediaControl.hasAccess(this)) {
            "通知使用权：已授权（ESP 可控制音乐）"
        } else {
            "通知使用权：未授权（请在系统设置中开启）"
        }
        tvMediaStatus.setTextColor(
            if (MediaControl.hasAccess(this)) 0xFF5ED6A3.toInt() else 0xFFE0A040.toInt()
        )
    }
}
