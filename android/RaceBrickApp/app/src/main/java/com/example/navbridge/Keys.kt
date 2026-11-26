package com.example.navbridge

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import java.security.MessageDigest

/**
 * 高德 Key 管理。
 *
 * 高德 Android Key 绑定“包名 + 签名 SHA1”，换包名或换签名都会失效。
 * Key 可以在高德控制台申请后填到 local.properties 的 AMAP_KEY，或在 App 里填写。
 */
object Keys {
    private const val PREFS = "navbridge"
    private const val K_AMAP = "amap"

    fun userKey(c: Context): String =
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(K_AMAP, "").orEmpty().trim()

    /** App 里没填时回退到打包时写入的 BuildConfig.AMAP_KEY */
    fun amap(c: Context): String = userKey(c).ifEmpty { BuildConfig.AMAP_KEY.trim() }

    /**
     * 同步写盘。必须在重启进程前完成，否则 Runtime.exit 会把还没落盘的 apply() 丢掉。
     * @return 是否写入成功
     */
    fun save(c: Context, key: String): Boolean =
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(K_AMAP, key.trim()).commit()

    /** 把 Key 交给高德各 SDK。用反射，SDK 版本差异不会导致编译失败。 */
    fun apply(c: Context) {
        val ctx = c.applicationContext
        val k = amap(ctx)
        if (k.isEmpty()) return
        runCatching {
            Class.forName("com.amap.api.maps.MapsInitializer")
                .getMethod("setApiKey", String::class.java).invoke(null, k)
        }
        runCatching {
            Class.forName("com.amap.api.location.AMapLocationClient")
                .getMethod("setApiKey", String::class.java).invoke(null, k)
        }
        runCatching {
            val cls = Class.forName("com.amap.api.services.core.ServiceSettings")
            val inst = cls.getMethod("getInstance").invoke(null)
            cls.getMethod("setApiKey", String::class.java).invoke(inst, k)
        }
        runCatching {
            Class.forName("com.amap.api.navi.AMapNavi")
                .getMethod("setApiKey", Context::class.java, String::class.java).invoke(null, ctx, k)
        }
    }

    /** 当前安装包的签名 SHA1，高德控制台要填这个。 */
    fun signSha1(c: Context): String = try {
        val pm = c.packageManager
        val bytes: ByteArray? = if (Build.VERSION.SDK_INT >= 28) {
            pm.getPackageInfo(c.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
                .signingInfo?.apkContentsSigners?.firstOrNull()?.toByteArray()
        } else {
            @Suppress("DEPRECATION")
            pm.getPackageInfo(c.packageName, PackageManager.GET_SIGNATURES)
                .signatures?.firstOrNull()?.toByteArray()
        }
        bytes?.let { MessageDigest.getInstance("SHA1").digest(it).joinToString(":") { b -> "%02X".format(b) } }
            ?: "读不到"
    } catch (e: Exception) {
        "读不到（${e.message}）"
    }
}
