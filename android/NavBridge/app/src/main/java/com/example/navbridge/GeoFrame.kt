package com.example.navbridge

import kotlin.math.cos

/**
 * 本地平面坐标（米），原点取所在 0.1° 网格的固定点，App 重启后不变。
 * x 向东、y 向北。几十公里内误差很小。
 *
 * 圆屏只认这个平面坐标 + frameId，不认经纬度。
 */
class GeoFrame private constructor(
    val refLat: Double,
    val refLng: Double,
    val id: Int,
) {
    private val kx = cos(Math.toRadians(refLat)) * 111320.0
    private val ky = 110574.0

    fun x(lng: Double): Double = (lng - refLng) * kx
    fun y(lat: Double): Double = (lat - refLat) * ky
    fun lng(x: Double): Double = refLng + x / kx
    fun lat(y: Double): Double = refLat + y / ky

    companion object {
        fun at(lat: Double, lng: Double): GeoFrame {
            val la = Math.round(lat * 10) / 10.0
            val lo = Math.round(lng * 10) / 10.0
            val a = Math.round(la * 10)
            val b = Math.round(lo * 10)
            val id = ((a * 7919L + b * 104729L) and 0x7FFFL).toInt() or 1
            return GeoFrame(la, lo, id)
        }
    }
}
