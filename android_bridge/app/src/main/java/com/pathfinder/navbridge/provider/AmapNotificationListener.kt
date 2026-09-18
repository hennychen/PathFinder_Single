package com.pathfinder.navbridge.provider

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import com.pathfinder.navbridge.protocol.NavProtocol

/**
 * 真实高德地图 App 导航数据源。
 *
 * 高德导航时前台服务发常驻通知:
 * - title   = "沿当前道路行驶300米" / "300米后右转" 等
 * - text    = "进入 望京东路" / "到达目的地附近" 等
 * 监听并解析 -> NormalizedNavEvent -> BLE。
 *
 * 协议/固件零改动; 固件 10s 无包自动回 NAV IDLE, 通知消失无需显式停止包。
 */
class AmapNotificationListener : NotificationListenerService() {

    /** 最近一次解析到的路名, 胶囊兜底帧复用 */
    private var lastRoad = ""

    companion object {
        const val ACTION_EVENT = "com.pathfinder.navbridge.AMAP_NAV_EVENT"
        private const val EXTRA_TURN = "turn"
        private const val EXTRA_STEP = "step"
        private const val EXTRA_TOTAL = "total"
        private const val EXTRA_REMAIN = "remain"
        private const val EXTRA_ROAD = "road"
        private const val AMAP_PACKAGE = "com.autonavi.minimap"

        private val TURN_KEYWORDS = listOf(
            "到达" to NavProtocol.TURN_ARRIVE,
            "目的地" to NavProtocol.TURN_ARRIVE,
            "掉头" to NavProtocol.TURN_U_TURN,
            "左转" to NavProtocol.TURN_LEFT,
            "右转" to NavProtocol.TURN_RIGHT,
            "左前方" to NavProtocol.TURN_LEFT_FRONT,
            "右前方" to NavProtocol.TURN_RIGHT_FRONT,
            "靠左" to NavProtocol.TURN_LEFT_FRONT,
            "靠右" to NavProtocol.TURN_RIGHT_FRONT,
            "直行" to NavProtocol.TURN_STRAIGHT,
            "环岛" to NavProtocol.TURN_LEFT,
        )

        /** "300米后右转" -> 300; "沿xx路行驶1.2公里" -> 1200 */
        private val DISTANCE_REGEX =
            Regex("(\\d+(?:\\.\\d+)?)\\s*(公里|千米|km|米|m)")

        fun isListenerEnabled(context: Context): Boolean {
            val raw = android.provider.Settings.Secure.getString(
                context.contentResolver, "enabled_notification_listeners"
            ) ?: return false
            return raw.split(":").any {
                it.equals(context.packageName, ignoreCase = true)
            }
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn?.packageName != AMAP_PACKAGE) return
        val extras = sbn.notification?.extras ?: return
        val title = extras.getCharSequence(android.app.Notification.EXTRA_TITLE)?.toString().orEmpty()
        val text = extras.getCharSequence(android.app.Notification.EXTRA_TEXT)?.toString().orEmpty()
        val bigText = extras.getCharSequence(android.app.Notification.EXTRA_BIG_TEXT)?.toString().orEmpty()
        if (title.isBlank() && text.isBlank() && bigText.isBlank()) return

        val event = parse(title, if (text.isNotBlank()) text else bigText)
            // 华为胶囊通知兜底: 导航活跃但文案被胶囊吞掉 -> 发直行心跳, 让 HUD 脱离 NAV IDLE
            ?: if (extras.getBoolean("com.autonavi.minimap.navigating")) {
                NormalizedNavEvent(NavProtocol.TURN_STRAIGHT, 0, 0, 0, lastRoad)
            } else null
        if (event != null && event.roadName.isNotBlank()) lastRoad = event.roadName
        val intent = Intent(ACTION_EVENT).setPackage(packageName)
            .putExtra(EXTRA_TURN, event?.turnType ?: return)
            .putExtra(EXTRA_STEP, event?.stepDistanceM ?: 0)
            .putExtra(EXTRA_TOTAL, event?.totalDistanceM ?: 0)
            .putExtra(EXTRA_REMAIN, event?.remainTimeS ?: 0)
            .putExtra(EXTRA_ROAD, event?.roadName ?: "")
        sendBroadcast(intent)
    }

    /** title/text -> 协议事件; 解析不出转向则返回 null (避免噪声覆盖上一帧) */
    fun parse(title: String, text: String): NormalizedNavEvent? {
        val combined = "$title $text"

        val turn = TURN_KEYWORDS.firstOrNull { combined.contains(it.first) }?.second

        val dist = DISTANCE_REGEX.find(combined)?.let { m ->
            val value = m.groupValues[1].toDouble()
            when (m.groupValues[2]) {
                "公里", "千米", "km" -> (value * 1000).toInt()
                else -> value.toInt()
            }
        }

        // 无转向且无距离: 大概率是非导航通知, 忽略
        if (turn == null && dist == null) return null

        val road = extractRoadName(title, text)

        return NormalizedNavEvent(
            turnType = turn ?: NavProtocol.TURN_STRAIGHT,
            stepDistanceM = dist ?: 0,
            totalDistanceM = dist ?: 0,
            remainTimeS = 0,
            roadName = road,
        )
    }

    /** "进入 望京东路" -> "望京东路"; 无路名时返回上一帧路名 (空串占位由 UI 兜底) */
    private fun extractRoadName(title: String, text: String): String {
        val enter = Regex("进入\\s*(.{2,20}?)(?:路|街|道|巷|桥|隧道|高速|大道|环路)?(?:后|，|。|$)")
        text.let { enter.find(it)?.groupValues?.get(1)?.trim()?.let { r -> if (r.isNotBlank()) return r } }
        title.let { enter.find(it)?.groupValues?.get(1)?.trim()?.let { r -> if (r.isNotBlank()) return r } }
        // "沿望京东路行驶" 形态
        val along = Regex("沿\\s*(.{2,20}?)(?:行驶|直行|进入)")
        (text + title).let { along.find(it)?.groupValues?.get(1)?.trim()?.let { r -> if (r.isNotBlank()) return r } }
        return ""
    }

    /** UI 侧便捷注册 */
    class EventReceiver(private val onEvent: (NormalizedNavEvent) -> Unit) : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val event = NormalizedNavEvent(
                turnType = intent.getIntExtra(EXTRA_TURN, NavProtocol.TURN_STRAIGHT),
                stepDistanceM = intent.getIntExtra(EXTRA_STEP, 0),
                totalDistanceM = intent.getIntExtra(EXTRA_TOTAL, 0),
                remainTimeS = intent.getIntExtra(EXTRA_REMAIN, 0),
                roadName = intent.getStringExtra(EXTRA_ROAD).orEmpty(),
            )
            onEvent(event)
        }

        fun register(context: Context) {
            val filter = IntentFilter(ACTION_EVENT)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(this, filter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                context.registerReceiver(this, filter)
            }
        }
    }
}
