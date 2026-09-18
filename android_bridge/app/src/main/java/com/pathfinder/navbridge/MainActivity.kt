package com.pathfinder.navbridge

import android.Manifest
import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.amap.api.maps.MapsInitializer
import com.amap.api.navi.AMapNavi
import com.amap.api.navi.SimpleNaviListener
import com.amap.api.navi.enums.NaviType
import com.amap.api.navi.model.AMapCalcRouteResult
import com.amap.api.navi.model.NaviInfo
import com.amap.api.navi.model.NaviLatLng
import com.pathfinder.navbridge.ble.PathFinderBleClient
import com.pathfinder.navbridge.provider.AmapAdapter
import com.pathfinder.navbridge.provider.AmapNotificationListener

/**
 * PathFinder 导航桥:
 * 1. 高德导航 SDK 模拟/GPS 导航
 * 2. onNaviInfoUpdate -> AmapAdapter 归一化 -> NavProtocol 编码 -> BLE 写 0xFF01
 * 设备侧收到后 NAV IDLE -> PHONE LIVE。
 *
 * 导航回调走官方 SimpleNaviListener 空实现类的内部对象,
 * 只 override 需要的方法, 避免与 navi-3dmap 10.x 的 AMapNaviListener 接口签名不匹配。
 */
class MainActivity : AppCompatActivity() {

    private lateinit var ble: PathFinderBleClient
    private lateinit var logView: TextView
    private lateinit var statusView: TextView
    private lateinit var handler: Handler
    private var navi: AMapNavi? = null
    private var amapReceiver: AmapNotificationListener.EventReceiver? = null

    private val naviListener = object : SimpleNaviListener() {
        override fun onInitNaviSuccess() {
            appendLog("[navi] init success")
        }

        override fun onInitNaviFailure() {
            appendLog("[navi] init failure")
        }

        override fun onStartNavi(type: Int) {
            appendLog("[navi] start navi type=$type")
        }

        override fun onEndEmulatorNavi() {
            appendLog("[navi] emulator navi end")
        }

        override fun onArriveDestination() {
            appendLog("[navi] arrived")
        }

        override fun onCalculateRouteFailure(routeResult: AMapCalcRouteResult?) {
            appendLog("[navi] route failure code=${routeResult?.errorCode} detail=${routeResult?.errorDetail}")
        }

        override fun onReCalculateRouteForYaw() {
            appendLog("[navi] recalculate for yaw")
        }

        override fun onReCalculateRouteForTrafficJam() {
            appendLog("[navi] recalculate for traffic jam")
        }

        override fun onCalculateRouteSuccess(routeResult: AMapCalcRouteResult?) {
            appendLog("[navi] route success, starting emulator navi")
            navi?.startNavi(NaviType.EMULATOR)
        }

        override fun onGetNavigationText(type: Int, text: String?) {
            // 播报文本, 忽略
        }

        override fun onNaviInfoUpdate(naviInfo: NaviInfo?) {
            val info = naviInfo ?: return
            val event = AmapAdapter.fromNaviInfo(
                iconType = info.iconType,
                curStepRetainDistance = info.curStepRetainDistance,
                pathRetainDistance = info.pathRetainDistance,
                pathRetainTime = info.pathRetainTime,
                nextRoadName = info.nextRoadName,
                currentRoadName = info.currentRoadName,
            )
            appendLog("[navi] turn=${info.iconType} step=${info.curStepRetainDistance}m " +
                    "total=${info.pathRetainDistance}m remain=${info.pathRetainTime}s " +
                    "road=${event.roadName}")
            ble.sendNavigation(
                turnType = event.turnType,
                stepDistanceM = event.stepDistanceM,
                totalDistanceM = event.totalDistanceM,
                remainTimeS = event.remainTimeS,
                roadName = event.roadName,
            )
        }
    }

    private val requiredPermissions: Array<String>
        get() = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        handler = Handler(Looper.getMainLooper())
        buildUi()
        ble = PathFinderBleClient(this, object : PathFinderBleClient.Listener {
            override fun onLog(line: String) {
                handler.post { appendLog("[ble] $line") }
            }

            override fun onConnectionStateChanged(connected: Boolean) {
                handler.post {
                    statusView.text = if (connected) "BLE: CONNECTED" else "BLE: DISCONNECTED"
                }
            }
        })
        requestPermissionsThenInit()
    }

    private fun requestPermissionsThenInit() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            onPermissionsReady()
            return
        }
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 1) {
            onPermissionsReady()
        }
    }

    @SuppressLint("MissingPermission")
    private fun onPermissionsReady() {
        initNavi()
        // 自动扫描并连接 PathFinder 设备, 无需手动点按钮
        ble.start()
        registerAmapNotificationSource()
    }

    /** 真实高德 App 模式: 通知事件 -> 归一化 -> BLE (与 SDK 模式共用发送路径) */
    private fun registerAmapNotificationSource() {
        if (amapReceiver != null) return
        amapReceiver = AmapNotificationListener.EventReceiver { event ->
            appendLog("[amap-notify] turn=${event.turnType} step=${event.stepDistanceM}m " +
                    "road=${event.roadName}")
            ble.sendNavigation(
                turnType = event.turnType,
                stepDistanceM = event.stepDistanceM,
                totalDistanceM = event.totalDistanceM,
                remainTimeS = event.remainTimeS,
                roadName = event.roadName,
            )
        }.also { it.register(this) }
        appendLog("[amap-notify] receiver registered")
    }

    @SuppressLint("MissingPermission")
    private fun initNavi() {
        try {
            // 高德 SDK 8.1.0+ 隐私合规: 必须在调用任何 SDK 接口前声明
            MapsInitializer.updatePrivacyShow(this, true, true)
            MapsInitializer.updatePrivacyAgree(this, true)
            navi = AMapNavi.getInstance(applicationContext)
            navi?.addAMapNaviListener(naviListener)
            appendLog("[navi] listener registered")
        } catch (e: Exception) {
            appendLog("[navi] init failed: ${e.message}")
        }
    }

    /** 深链拉起真实高德地图 App 开始步行导航演示 (用户也可在高德内自行操作) */
    private fun launchRealAmap() {
        if (!AmapNotificationListener.isListenerEnabled(this)) {
            appendLog("[amap-notify] notification access NOT granted, HUD will not update")
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
            return
        }
        val uri = Uri.parse("androidamap://navi?sourceApplication=PathFinder&lat=39.917337&lon=116.397056&dev=0&style=2")
        val intent = Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (intent.resolveActivity(packageManager) != null) {
            startActivity(intent)
            appendLog("[amap] launched real amap navigation")
        } else {
            appendLog("[amap] amap app not installed")
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        amapReceiver?.let { unregisterReceiver(it) }
        amapReceiver = null
        navi?.stopNavi()
        ble.stop()
    }

    // region UI

    @SuppressLint("SetTextI18n")
    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 48, 32, 32)
        }
        statusView = TextView(this).apply { text = "BLE: DISCONNECTED" }
        logView = TextView(this).apply {
            text = "logs:\n"
            setTextIsSelectable(true)
        }
        val scroll = ScrollView(this).apply { addView(logView) }

        fun button(label: String, onClick: () -> Unit): Button =
            Button(this).apply {
                text = label
                setOnClickListener { onClick() }
            }

        root.addView(statusView)
        root.addView(button("Start BLE") { ble.start() })
        root.addView(button("Emulator Navi (demo route)") { startEmulatorNavi() })
        root.addView(button("Stop Navi") {
            navi?.stopNavi()
            appendLog("[navi] stopped")
        })
        root.addView(button("Grant Notification Access") {
            val enabled = AmapNotificationListener.isListenerEnabled(this)
            appendLog("[amap-notify] listener enabled=$enabled")
            if (!enabled) {
                startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
            }
        })
        root.addView(button("Launch Real Amap Navi") { launchRealAmap() })
        root.addView(scroll, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))
        setContentView(root)
    }

    @SuppressLint("MissingPermission")
    private fun startEmulatorNavi() {
        val navi = navi ?: run {
            appendLog("[navi] not initialized")
            return
        }
        // 北京站 -> 故宫博物院, 模拟导航
        val from = NaviLatLng(39.904556, 116.427231)
        val to = NaviLatLng(39.917337, 116.397056)
        val strategy = navi.strategyConvert(true, false, false, false, false)
        val ok = navi.calculateDriveRoute(
            arrayListOf(from),
            arrayListOf(to),
            ArrayList(),
            strategy,
        )
        appendLog("[navi] calculateDriveRoute sent ok=$ok")
    }

    private fun appendLog(line: String) {
        logView.append(line + "\n")
    }

    // endregion
}
