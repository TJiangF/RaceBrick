package com.example.navbridge

import android.os.Handler
import android.os.Looper
import androidx.appcompat.app.AppCompatActivity

open class LocalizedActivity : AppCompatActivity() {
    private val main = Handler(Looper.getMainLooper())
    private val localize = object : Runnable {
        override fun run() {
            Lang.localizeTree(window?.decorView)
            if (Lang.isEnglish) main.postDelayed(this, 350)
        }
    }

    override fun onResume() {
        super.onResume()
        Lang.localizeTree(window?.decorView)
        main.removeCallbacks(localize)
        if (Lang.isEnglish) main.post(localize)
    }

    override fun onPause() {
        main.removeCallbacks(localize)
        super.onPause()
    }
}
