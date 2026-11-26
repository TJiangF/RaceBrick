package com.example.navbridge

import android.app.Activity
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import com.amap.api.maps.AMap
import com.amap.api.maps.CameraUpdateFactory
import com.amap.api.maps.TextureMapView
import com.amap.api.maps.model.CameraPosition
import com.amap.api.maps.model.LatLng
import com.amap.api.maps.model.LatLngBounds

/**
 * 离屏高德地图渲染器。
 *
 * 在 Activity 的 decor 里放一个 1200×1200 的 TextureMapView，缩到 1 像素显示，
 * 相机移到目标范围后 getMapScreenShot() 取原尺寸位图，再按投影裁出真正的范围。
 * 底图由高德 SDK 渲染，不需要天地图/OSM。
 */
class MapCapture(private val activity: Activity) {

    companion object {
        const val SIZE = 1200
    }

    private val handler = Handler(Looper.getMainLooper())
    private val container = FrameLayout(activity)
    private val mapView = TextureMapView(activity)
    private val map: AMap
    private var busy = false
    private var alive = true

    init {
        container.clipChildren = true
        mapView.layoutParams = FrameLayout.LayoutParams(SIZE, SIZE)
        container.addView(mapView)

        val lp = FrameLayout.LayoutParams(1, 1, Gravity.TOP or Gravity.END)
        (activity.window.decorView as ViewGroup).addView(container, lp)

        // 地图视图内部仍按 1200×1200 渲染，只是缩到 1 像素显示。
        mapView.pivotX = 0f
        mapView.pivotY = 0f
        mapView.scaleX = 1f / SIZE
        mapView.scaleY = 1f / SIZE

        mapView.onCreate(null)
        mapView.onResume()
        map = mapView.map
        map.uiSettings.setAllGesturesEnabled(false)
        map.uiSettings.isZoomControlsEnabled = false
        map.uiSettings.isCompassEnabled = false
        map.uiSettings.isScaleControlsEnabled = false
        map.isMyLocationEnabled = false
        map.isTrafficEnabled = false
        map.mapType = AMap.MAP_TYPE_NORMAL
        map.showBuildings(false)
        map.showIndoorMap(false)
        map.showMapText(false)
    }

    /**
     * @param nw 西北角（截图左上）
     * @param ne 东北角（截图右上）
     * @param sw 西南角（截图左下）
     */
    fun capture(nw: LatLng, ne: LatLng, sw: LatLng, done: (RasterCodec.Shot?) -> Unit) {
        if (!alive || busy) { done(null); return }
        busy = true
        val finished = booleanArrayOf(false)

        map.setOnCameraChangeListener(object : AMap.OnCameraChangeListener {
            override fun onCameraChange(p: CameraPosition?) {}
            override fun onCameraChangeFinish(p: CameraPosition?) {
                if (finished[0]) return
                finished[0] = true
                map.setOnCameraChangeListener(null)
                handler.postDelayed({ shot(nw, ne, sw, done, 3) }, 800)
            }
        })

        try {
            val bounds = LatLngBounds(sw, ne)
            map.moveCamera(CameraUpdateFactory.newLatLngBounds(bounds, SIZE, SIZE, 0))
        } catch (e: Exception) {
            busy = false
            done(null)
            return
        }

        handler.postDelayed({
            if (!finished[0] && alive) {
                finished[0] = true
                map.setOnCameraChangeListener(null)
                shot(nw, ne, sw, done, 3)
            }
        }, 4000)
    }

    private fun shot(
        nw: LatLng,
        ne: LatLng,
        sw: LatLng,
        done: (RasterCodec.Shot?) -> Unit,
        retries: Int,
    ) {
        if (!alive) { busy = false; done(null); return }
        try {
            map.getMapScreenShot(object : AMap.OnMapScreenShotListener {
                override fun onMapScreenShot(bitmap: Bitmap?) {}
                override fun onMapScreenShot(bitmap: Bitmap?, status: Int) {
                    try {
                        if (status != 1 || bitmap == null ||
                            bitmap.width < SIZE / 2 || bitmap.height < SIZE / 2
                        ) {
                            retry(nw, ne, sw, done, retries)
                            return
                        }
                        val w = bitmap.width
                        val h = bitmap.height
                        val pixels = IntArray(w * h)
                        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)

                        val proj = map.projection
                        val pnw = proj.toScreenLocation(nw)
                        val pne = proj.toScreenLocation(ne)
                        val psw = proj.toScreenLocation(sw)

                        // 投影坐标基于地图视图布局尺寸（SIZE）；截图尺寸可能被系统缩放，这里换算一下
                        val kx = w.toFloat() / SIZE
                        val ky = h.toFloat() / SIZE
                        val left = (pnw.x * kx).coerceIn(0f, w - 1f)
                        val top = (pnw.y * ky).coerceIn(0f, h - 1f)
                        val right = (pne.x * kx).coerceIn(left + 1f, w.toFloat())
                        val bottom = (psw.y * ky).coerceIn(top + 1f, h.toFloat())

                        if (right - left < w * 0.5f || bottom - top < h * 0.5f) {
                            retry(nw, ne, sw, done, retries)
                            return
                        }

                        var varied = false
                        for (i in pixels.indices step 97) if (pixels[i] != pixels[0]) { varied = true; break }
                        if (!varied) { retry(nw, ne, sw, done, retries); return }

                        busy = false
                        done(RasterCodec.Shot(pixels, w, h, left, top, right, bottom))
                    } catch (e: Throwable) {
                        retry(nw, ne, sw, done, retries)
                    } finally {
                        bitmap?.recycle()
                    }
                }
            })
        } catch (e: Throwable) {
            retry(nw, ne, sw, done, retries)
        }
    }

    private fun retry(
        nw: LatLng,
        ne: LatLng,
        sw: LatLng,
        done: (RasterCodec.Shot?) -> Unit,
        retries: Int,
    ) {
        if (retries <= 0 || !alive) { busy = false; done(null); return }
        handler.postDelayed({ shot(nw, ne, sw, done, retries - 1) }, 1200)
    }

    fun moveCamera(lat: Double, lng: Double, zoom: Float = 16.5f) {
        if (!alive) return
        runCatching { map.moveCamera(CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), zoom)) }
    }

    fun close() {
        if (!alive) return
        alive = false
        busy = false
        handler.removeCallbacksAndMessages(null)
        runCatching { mapView.onPause() }
        runCatching { mapView.onDestroy() }
        runCatching { (activity.window.decorView as ViewGroup).removeView(container) }
    }
}
