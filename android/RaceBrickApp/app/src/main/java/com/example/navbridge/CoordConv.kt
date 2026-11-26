package com.example.navbridge

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * WGS-84（NEO/GPS 原生） → GCJ-02（高德）。
 *
 * NEO 模块输出 WGS84；高德的地图、算路、路线坐标都是 GCJ-02。
 * setExtraGPSData(1, ...) 可以喂 WGS84 给导航 SDK，但算路起点仍要先转成 GCJ-02。
 */
object CoordConv {
    private const val A = 6378245.0
    private const val EE = 0.00669342162296594323

    private fun outOfChina(lat: Double, lng: Double): Boolean =
        lng < 72.004 || lng > 137.8347 || lat < 0.8293 || lat > 55.8271

    private fun transformLat(x: Double, y: Double): Double {
        var r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * sqrt(abs(x))
        r += (20.0 * sin(6.0 * x * Math.PI) + 20.0 * sin(2.0 * x * Math.PI)) * 2.0 / 3.0
        r += (20.0 * sin(y * Math.PI) + 40.0 * sin(y / 3.0 * Math.PI)) * 2.0 / 3.0
        r += (160.0 * sin(y / 12.0 * Math.PI) + 320.0 * sin(y * Math.PI / 30.0)) * 2.0 / 3.0
        return r
    }

    private fun transformLng(x: Double, y: Double): Double {
        var r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(abs(x))
        r += (20.0 * sin(6.0 * x * Math.PI) + 20.0 * sin(2.0 * x * Math.PI)) * 2.0 / 3.0
        r += (20.0 * sin(x * Math.PI) + 40.0 * sin(x / 3.0 * Math.PI)) * 2.0 / 3.0
        r += (150.0 * sin(x / 12.0 * Math.PI) + 300.0 * sin(x / 30.0 * Math.PI)) * 2.0 / 3.0
        return r
    }

    fun wgsToGcj(lat: Double, lng: Double): DoubleArray {
        if (outOfChina(lat, lng)) return doubleArrayOf(lat, lng)
        var dLat = transformLat(lng - 105.0, lat - 35.0)
        var dLng = transformLng(lng - 105.0, lat - 35.0)
        val radLat = lat / 180.0 * Math.PI
        var magic = sin(radLat)
        magic = 1 - EE * magic * magic
        val sqrtMagic = sqrt(magic)
        dLat = (dLat * 180.0) / ((A * (1 - EE)) / (magic * sqrtMagic) * Math.PI)
        dLng = (dLng * 180.0) / (A / sqrtMagic * cos(radLat) * Math.PI)
        return doubleArrayOf(lat + dLat, lng + dLng)
    }

    /** GCJ-02 → WGS-84（高德路线/定位转到天地图 WGS84 时使用）。 */
    fun gcjToWgs(lat: Double, lng: Double): DoubleArray {
        var wLat = lat
        var wLng = lng
        repeat(4) {
            val gcj = wgsToGcj(wLat, wLng)
            wLat -= gcj[0] - lat
            wLng -= gcj[1] - lng
        }
        return doubleArrayOf(wLat, wLng)
    }
}
