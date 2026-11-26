package com.example.navbridge

import android.content.Context
import com.amap.api.services.core.AMapException
import com.amap.api.services.geocoder.GeocodeResult
import com.amap.api.services.geocoder.GeocodeSearch
import com.amap.api.services.geocoder.GeocodeQuery
import com.amap.api.services.geocoder.RegeocodeResult
import com.amap.api.services.help.Inputtips
import com.amap.api.services.help.InputtipsQuery
import com.amap.api.services.help.Tip

/**
 * 高德 POI 输入联想（Inputtips）+ 无坐标建议的地理编码兜底。
 *
 * 高德返回的 POI 坐标就是 GCJ-02，和底图/算路一致，不需要再做坐标转换。
 */
class SearchEngine(
    private val app: Context,
    private val onTips: (List<Tip>) -> Unit,
    private val onStatus: (String) -> Unit,
) {

    // 异步回调期间保持引用，避免对象被回收
    private var inputtips: Inputtips? = null
    private var geocodeSearch: GeocodeSearch? = null

    fun suggest(keyword: String) {
        val query = InputtipsQuery(keyword, "").apply { cityLimit = false }
        val tipsApi = runCatching { Inputtips(app, query) }.getOrNull()
        if (tipsApi == null) {
            onStatus("搜索初始化失败，检查 Key")
            return
        }
        inputtips = tipsApi
        tipsApi.setInputtipsListener { list, code ->
            if (code == AMapException.CODE_AMAP_SUCCESS) {
                onTips(list ?: emptyList())
            } else {
                onTips(emptyList())
                onStatus("联想失败：$code")
            }
        }
        runCatching { tipsApi.requestInputtipsAsyn() }
            .onFailure { onStatus("联想请求失败：${it.message}") }
    }

    /** 建议里没有坐标时，用地名 + 区县做地理编码。 */
    fun geocode(name: String, city: String, done: (Double, Double, String) -> Unit) {
        val geocode = runCatching { GeocodeSearch(app) }.getOrNull()
        if (geocode == null) {
            onStatus("地理编码初始化失败")
            return
        }
        geocodeSearch = geocode
        geocode.setOnGeocodeSearchListener(object : GeocodeSearch.OnGeocodeSearchListener {
            override fun onRegeocodeSearched(result: RegeocodeResult?, code: Int) {}
            override fun onGeocodeSearched(result: GeocodeResult?, code: Int) {
                val point = result?.geocodeAddressList?.firstOrNull()?.latLonPoint
                if (code == AMapException.CODE_AMAP_SUCCESS && point != null) {
                    done(point.latitude, point.longitude, name)
                } else {
                    onStatus("没有找到“$name”的坐标，换个写法试试")
                }
            }
        })
        runCatching { geocode.getFromLocationNameAsyn(GeocodeQuery(name, city)) }
            .onFailure { onStatus("地理编码请求失败：${it.message}") }
    }
}
