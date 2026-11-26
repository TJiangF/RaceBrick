package com.example.navbridge

/** App 侧出行工具；选择后持久化，下次打开继续使用。 */
enum class TravelMode(
    val id: String,
    val label: String,
    val simulateKmh: Int,
) {
    CAR("car", "汽车", 90),
    MOTORCYCLE("motorcycle", "摩托车", 50),
    BICYCLE("bicycle", "自行车", 20);

    companion object {
        val DEFAULT = CAR
        fun byId(id: String?): TravelMode = values().firstOrNull { it.id == id } ?: DEFAULT
        fun next(mode: TravelMode): TravelMode = values()[(mode.ordinal + 1) % values().size]
    }
}
