package com.example.navbridge

import android.app.Application
import com.amap.api.location.AMapLocationClient
import com.amap.api.maps.MapsInitializer
import com.amap.api.navi.NaviSetting
import com.amap.api.services.core.ServiceSettings

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        // 高德合规声明，必须在任何高德功能之前。
        runCatching {
            NaviSetting.updatePrivacyShow(this, true, true)
            NaviSetting.updatePrivacyAgree(this, true)
            MapsInitializer.updatePrivacyShow(this, true, true)
            MapsInitializer.updatePrivacyAgree(this, true)
            AMapLocationClient.updatePrivacyShow(this, true, true)
            AMapLocationClient.updatePrivacyAgree(this, true)
            ServiceSettings.updatePrivacyShow(this, true, true)
            ServiceSettings.updatePrivacyAgree(this, true)
        }
        Keys.apply(this)
    }
}
