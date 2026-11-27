package com.example.navbridge

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

object BlePermissions {
    const val REQUEST = 41

    fun missing(activity: Activity): Array<String> {
        val need = mutableListOf<String>()
        fun ask(permission: String) {
            if (ContextCompat.checkSelfPermission(activity, permission) != PackageManager.PERMISSION_GRANTED) {
                need += permission
            }
        }
        ask(Manifest.permission.ACCESS_FINE_LOCATION)
        ask(Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= 31) {
            ask(Manifest.permission.BLUETOOTH_SCAN)
            ask(Manifest.permission.BLUETOOTH_CONNECT)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            ask(Manifest.permission.POST_NOTIFICATIONS)
            ask(Manifest.permission.NEARBY_WIFI_DEVICES)
        }
        return need.toTypedArray()
    }

    fun canConnect(activity: Activity): Boolean {
        val fine = ContextCompat.checkSelfPermission(activity, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED
        if (!fine) return false
        if (Build.VERSION.SDK_INT >= 31) {
            val scan = ContextCompat.checkSelfPermission(activity, Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED
            val connect = ContextCompat.checkSelfPermission(activity, Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED
            if (!scan || !connect) return false
        }
        return true
    }

    /** Returns true when all permissions are already available. */
    fun ensure(activity: Activity): Boolean {
        val need = missing(activity)
        if (need.isEmpty()) return true
        ActivityCompat.requestPermissions(activity, need, REQUEST)
        return false
    }
}
