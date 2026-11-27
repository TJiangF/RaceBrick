package com.example.navbridge

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.TextView

class ConnectActivity : LocalizedActivity() {
    private lateinit var status: TextView
    private lateinit var connect: Button
    private lateinit var skip: Button
    private var pendingConnect = false
    private var openingHome = false
    private val main = Handler(Looper.getMainLooper())

    private val bleListener = object : BleSession.Listener {
        override fun onStatus(message: String) {
            status.text = message
        }

        override fun onSessionChanged(active: Boolean) {
            if (active) status.text = "已连接 RaceBrick"
        }

        override fun onReady() {
            status.text = "已连接 RaceBrick"
            main.postDelayed({ openHome() }, 250L)
        }

        override fun onLine(line: String) = Unit
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_connect)
        status = findViewById(R.id.tvConnectStatus)
        connect = findViewById(R.id.btnConnectDevice)
        skip = findViewById(R.id.btnSkipConnect)
        BleSession.ensure(this)
        connect.setOnClickListener { connectNow() }
        skip.setOnClickListener {
            BleSession.disconnect()
            openHome()
        }
        if (BleSession.verified) {
            main.post { openHome() }
        }
    }

    override fun onStart() {
        super.onStart()
        BleSession.bind(bleListener)
    }

    override fun onStop() {
        BleSession.unbind(bleListener)
        super.onStop()
    }

    override fun onDestroy() {
        main.removeCallbacksAndMessages(null)
        super.onDestroy()
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
            status.text = "正在搜索 RaceBrick…"
        } else {
            status.text = "需要蓝牙和定位权限才能连接设备"
        }
    }

    private fun connectNow() {
        if (BleSession.verified) {
            openHome()
            return
        }
        if (!BlePermissions.ensure(this)) {
            pendingConnect = true
            return
        }
        BleSession.connect()
        status.text = "正在搜索 RaceBrick…"
    }

    private fun openHome() {
        if (openingHome) return
        openingHome = true
        startActivity(Intent(this, HomeActivity::class.java))
        finish()
    }
}
