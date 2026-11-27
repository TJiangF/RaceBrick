package com.example.navbridge

import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

class HomeActivity : LocalizedActivity() {
    private lateinit var homeStatus: TextView
    private lateinit var btnHomeConnect: Button
    private var pendingConnect = false

    private val bleListener = object : BleSession.Listener {
        override fun onStatus(message: String) {
            homeStatus.text = message
            refreshConnectButton()
        }

        override fun onSessionChanged(active: Boolean) {
            refreshStatus()
        }

        override fun onReady() {
            refreshStatus()
        }

        override fun onLine(line: String) = Unit
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_home)
        homeStatus = findViewById(R.id.homeStatus)
        btnHomeConnect = findViewById(R.id.btnHomeConnect)
        BleSession.ensure(this)

        findViewById<LinearLayout>(R.id.cardRace).setOnClickListener {
            startActivity(Intent(this, RaceActivity::class.java))
        }
        findViewById<LinearLayout>(R.id.cardNav).setOnClickListener {
            startActivity(Intent(this, MainActivity::class.java))
        }
        findViewById<LinearLayout>(R.id.cardWifi).setOnClickListener {
            when {
                BleSession.verified -> startActivity(
                    Intent(this, MainActivity::class.java)
                        .putExtra(MainActivity.EXTRA_OPEN_WIFI, true)
                )
                BleSession.connected -> toast("设备正在连接，请稍候")
                else -> toast("请先在主页连接设备")
            }
        }
        findViewById<LinearLayout>(R.id.cardSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        btnHomeConnect.setOnClickListener {
            if (BleSession.connected || BleSession.verified) {
                BleSession.disconnect()
                refreshStatus()
            } else {
                connectNow()
            }
        }

        refreshStatus()
    }

    override fun onStart() {
        super.onStart()
        BleSession.bind(bleListener)
        refreshStatus()
    }

    override fun onStop() {
        BleSession.unbind(bleListener)
        super.onStop()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != BlePermissions.REQUEST || !pendingConnect) return
        pendingConnect = false
        if (BlePermissions.canConnect(this)) {
            BleSession.connect()
            refreshStatus()
        } else {
            homeStatus.text = "需要蓝牙和定位权限才能连接设备"
        }
    }

    private fun connectNow() {
        if (!BlePermissions.ensure(this)) {
            pendingConnect = true
            return
        }
        BleSession.connect()
        homeStatus.text = "正在连接 RaceBrick…"
        refreshConnectButton()
    }

    private fun refreshStatus() {
        homeStatus.text = when {
            BleSession.verified -> "已连接 RaceBrick"
            BleSession.connected -> "已连接，等待设备确认…"
            BleSession.status != "未连接 RaceBrick" -> BleSession.status
            else -> "设备未连接 · 本地预览"
        }
        refreshConnectButton()
    }

    private fun refreshConnectButton() {
        btnHomeConnect.text = if (BleSession.connected || BleSession.verified) {
            "断开设备"
        } else {
            "连接设备"
        }
    }

    private fun toast(message: String) {
        Toast.makeText(this, Lang.tr(message), Toast.LENGTH_SHORT).show()
    }
}
