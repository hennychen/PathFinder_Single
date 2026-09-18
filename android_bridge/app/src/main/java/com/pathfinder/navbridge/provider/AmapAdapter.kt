package com.pathfinder.navbridge.provider

import com.pathfinder.navbridge.protocol.NavProtocol

/**
 * 高德导航事件 -> PathFinder 统一导航事件。
 *
 * 对齐仓库真源 docs/materials/amap_navigation_adapter.md 与
 * tools/navigation_bridge/map_adapters.py:
 * - iconType -> turn_type 收敛映射
 * - step: curStepRetainDistance / segmentRemainDistance
 * - total: pathRetainDistance / routeRemainDistance
 * - remain: pathRetainTime / routeRemainTime
 * - road: nextRoadName / currentRoadName
 */
data class NormalizedNavEvent(
    val turnType: Int,
    val stepDistanceM: Int,
    val totalDistanceM: Int,
    val remainTimeS: Int,
    val roadName: String,
)

object AmapAdapter {
    // 与 map_adapters.py AMAP_ICON_TYPE_TO_TURN 一致
    private val ICON_TYPE_TO_TURN = mapOf(
        2 to NavProtocol.TURN_LEFT,
        3 to NavProtocol.TURN_RIGHT,
        4 to NavProtocol.TURN_LEFT_FRONT,
        5 to NavProtocol.TURN_RIGHT_FRONT,
        6 to NavProtocol.TURN_LEFT,
        7 to NavProtocol.TURN_RIGHT,
        8 to NavProtocol.TURN_U_TURN,
        9 to NavProtocol.TURN_STRAIGHT,
        15 to NavProtocol.TURN_ARRIVE,
        17 to NavProtocol.TURN_LEFT,
        18 to NavProtocol.TURN_STRAIGHT,
        19 to NavProtocol.TURN_U_TURN,
        21 to NavProtocol.TURN_LEFT,
        22 to NavProtocol.TURN_RIGHT,
        25 to NavProtocol.TURN_LEFT,
        26 to NavProtocol.TURN_RIGHT,
        27 to NavProtocol.TURN_STRAIGHT,
        28 to NavProtocol.TURN_U_TURN,
        65 to NavProtocol.TURN_LEFT_FRONT,
        66 to NavProtocol.TURN_RIGHT_FRONT,
    )

    private val TURN_NAME_TO_TYPE = mapOf(
        "straight" to NavProtocol.TURN_STRAIGHT,
        "go_straight" to NavProtocol.TURN_STRAIGHT,
        "continue" to NavProtocol.TURN_STRAIGHT,
        "left" to NavProtocol.TURN_LEFT,
        "turn_left" to NavProtocol.TURN_LEFT,
        "right" to NavProtocol.TURN_RIGHT,
        "turn_right" to NavProtocol.TURN_RIGHT,
        "left_front" to NavProtocol.TURN_LEFT_FRONT,
        "slight_left" to NavProtocol.TURN_LEFT_FRONT,
        "bear_left" to NavProtocol.TURN_LEFT_FRONT,
        "right_front" to NavProtocol.TURN_RIGHT_FRONT,
        "slight_right" to NavProtocol.TURN_RIGHT_FRONT,
        "bear_right" to NavProtocol.TURN_RIGHT_FRONT,
        "u_turn" to NavProtocol.TURN_U_TURN,
        "uturn" to NavProtocol.TURN_U_TURN,
        "turn_back" to NavProtocol.TURN_U_TURN,
        "arrive" to NavProtocol.TURN_ARRIVE,
        "destination" to NavProtocol.TURN_ARRIVE,
    )

    fun turnFromIconType(iconType: Int): Int =
        ICON_TYPE_TO_TURN[iconType] ?: NavProtocol.TURN_STRAIGHT

    fun turnFromName(name: String?): Int =
        TURN_NAME_TO_TYPE[name?.lowercase()] ?: NavProtocol.TURN_STRAIGHT

    /**
     * 从高德 NaviInfo 回调字段归一化。
     * NaviInfo 字段名以导航 SDK 为准 (getPathRetainDistance 单位米, getPathRetainTime 单位秒)。
     */
    fun fromNaviInfo(
        iconType: Int,
        curStepRetainDistance: Int,
        pathRetainDistance: Int,
        pathRetainTime: Int,
        nextRoadName: String?,
        currentRoadName: String? = null,
    ): NormalizedNavEvent = NormalizedNavEvent(
        turnType = turnFromIconType(iconType),
        stepDistanceM = curStepRetainDistance.coerceIn(0, 0xFFFF),
        totalDistanceM = pathRetainDistance.coerceIn(0, 0xFFFF),
        remainTimeS = pathRetainTime.coerceIn(0, 0xFFFF),
        roadName = (nextRoadName?.takeIf { it.isNotBlank() } ?: currentRoadName ?: ""),
    )
}
