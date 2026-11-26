package com.example.navbridge

import android.content.Intent
import android.os.Bundle
import android.widget.LinearLayout
import android.widget.TextView

class HomeActivity : LocalizedActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_home)
        findViewById<LinearLayout>(R.id.cardRace).setOnClickListener {
            startActivity(Intent(this, RaceActivity::class.java))
        }
        findViewById<LinearLayout>(R.id.cardNav).setOnClickListener {
            startActivity(Intent(this, MainActivity::class.java))
        }
        findViewById<LinearLayout>(R.id.cardWifi).setOnClickListener {
            startActivity(Intent(this, MainActivity::class.java)
                .putExtra(MainActivity.EXTRA_OPEN_WIFI, true))
        }
        findViewById<LinearLayout>(R.id.cardSettings).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<TextView>(R.id.homeStatus).text = "Local-first · 设备连接可选"
    }
}
