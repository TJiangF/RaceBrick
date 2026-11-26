package com.example.navbridge

/**
 * 地图配色/风格。默认是 MotoNav 的“掌机像素”主题。
 *
 * base 四色用于天地图 4 色分类；palette 是整帧 16 色量化调色板。
 */
class MapStyle(
    val id: String,
    val label: String,
    val block: Int,
    val road: Int,
    val park: Int,
    val water: Int,
    val route: Int,
    val routeCase: Int,
    val hud: Int,
    val light: Boolean,
    paletteOverride: IntArray? = null,
) {
    val baseColors: IntArray get() = intArrayOf(block, road, park, water)

    val palette: IntArray = paletteOverride ?: intArrayOf(
        0xFF000000.toInt(), // 黑（箭头/HUD 阴影/面板描边）
        road,
        block,
        park,
        water,
        route,
        routeCase,
        0xFFFFFFFF.toInt(), // 白（信息面板/HUD/箭头）
        0xFFFFB600.toInt(), // 缓行
        0xFFFF9500.toInt(), // 电子眼
        0xFFEE1C20.toInt(), // 拥堵/电子眼
        0xFF940000.toInt(), // 严重拥堵
        0xFF34C759.toInt(), // 畅通/推荐车道
        0xFF1E88E5.toInt(), // 监控电子眼
        0xFFD81B60.toInt(), // 违章电子眼
        0xFFF5F0E8.toInt(), // 米白辅助色
    )

    val palette565: IntArray = IntArray(16) { to565(palette[it]) }

    private fun to565(c: Int): Int =
        (((c shr 19) and 0x1F) shl 11) or
            (((c shr 10) and 0x3F) shl 5) or
            ((c shr 3) and 0x1F)
}

object MapStyles {
    /** MotoNav 掌机像素：绿色地面、浅色道路、深色路线。 */
    val PIXEL = MapStyle(
        "pixel", "掌机像素",
        0xFF8CC273.toInt(), 0xFFE6FBD6.toInt(), 0xFF316952.toInt(), 0xFF081821.toInt(),
        0xFF081821.toInt(), 0xFFE6FBD6.toInt(), 0xFFE6FBD6.toInt(), light = false,
    )

    /** MotoNav 罪恶都市：米色街区、深色道路、品红路线。 */
    val VICE = MapStyle(
        "vice", "罪恶都市",
        0xFFE6DBCE.toInt(), 0xFF31313A.toInt(), 0xFF73C26B.toInt(), 0xFF4AC6DE.toInt(),
        0xFFF749C5.toInt(), 0xFF7B1C63.toInt(), 0xFFFFFFFF.toInt(), light = true,
    )

    /** MotoNav 原版：浅灰街区、深色道路、蓝色路线。 */
    val ORIGINAL = MapStyle(
        "original", "原版",
        0xFFDEDFDE.toInt(), 0xFF313131.toInt(), 0xFF5ABE5A.toInt(), 0xFF4A92DE.toInt(),
        0xFF2192FF.toInt(), 0xFFFFFFFF.toInt(), 0xFFFFFFFF.toInt(), light = true,
    )

    /** MotoNav 极简黑白：黑色地面、灰色道路、白色路线。 */
    val MINIMAL = MapStyle(
        "minimal", "极简黑白",
        0xFF000000.toInt(), 0xFF848284.toInt(), 0xFF000000.toInt(), 0xFF000000.toInt(),
        0xFFFFFFFF.toInt(), 0xFF3A3D3A.toInt(), 0xFFFFFFFF.toInt(), light = false,
    )

    /** SDK 原图继续用原有多色 16 色板，避免被地图风格染色。 */
    val SDK_FRAME = MapStyle(
        "sdk", "SDK 原图",
        0xFFF5F0E8.toInt(), 0xFFF0F0F0.toInt(), 0xFFA8D8A0.toInt(), 0xFF8FC8E8.toInt(),
        0xFFB45CFF.toInt(), 0xFF2B2B2B.toInt(), 0xFFFFFFFF.toInt(), light = true,
        paletteOverride = intArrayOf(
            0xFF000000.toInt(), 0xFF2B2B2B.toInt(), 0xFF6E6E6E.toInt(), 0xFFB0B0B0.toInt(),
            0xFFF0F0F0.toInt(), 0xFFF5F0E8.toInt(), 0xFF8FC8E8.toInt(), 0xFF1E88E5.toInt(),
            0xFFA8D8A0.toInt(), 0xFF2E6B3A.toInt(), 0xFFF0C850.toInt(), 0xFFFF9500.toInt(),
            0xFFFF3B30.toInt(), 0xFFB00020.toInt(), 0xFFB45CFF.toInt(), 0xFF00E5FF.toInt(),
        ),
    )

    val all = listOf(PIXEL, VICE, ORIGINAL, MINIMAL)
    val DEFAULT = PIXEL

    fun byId(id: String?): MapStyle = all.firstOrNull { it.id == id } ?: DEFAULT
    fun next(style: MapStyle): MapStyle = all[(all.indexOf(style).coerceAtLeast(0) + 1) % all.size]
}
