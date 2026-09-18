package com.pathfinder.navbridge.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.pathfinder.navbridge.protocol.NavProtocol
import java.util.UUID
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * PathFinder BLE 桥客户端:
 * - 扫描 "PathFinder Nav"
 * - 连接 + 请求 MTU 256 (与固件日志 ble mtu updated mtu=256 一致)
 * - 按 (协商后 payload 预算) 串行写 0xFF01, 单包或分片
 */
@SuppressLint("MissingPermission")
class PathFinderBleClient(
    private val context: Context,
    private val listener: Listener,
) {
    interface Listener {
        fun onLog(line: String)
        fun onConnectionStateChanged(connected: Boolean)
    }

    companion object {
        private const val TAG = "PathFinderBle"
        const val DEVICE_NAME = "PathFinder Nav"
        // 已知开发板 MAC (ESP32-S3, 从设备日志 Bluetooth MAC 读取); 扫描不可见时直连兜底
        const val KNOWN_DEVICE_ADDRESS = "94:A9:90:0F:D2:A2"
        val SERVICE_UUID: UUID = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
        val NAV_CHAR_UUID: UUID = UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb")
        private const val REQUESTED_MTU = 256
        private const val CCC_DESCRIPTOR_UUID = "00002902-0000-1000-8000-00805f9b34fb"
        private const val SCAN_TIMEOUT_MS = 15000L
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var gatt: BluetoothGatt? = null
    private var navChar: BluetoothGattCharacteristic? = null
    private val connected = AtomicBoolean(false)
    private val writing = AtomicBoolean(false)
    private val writeQueue = ConcurrentLinkedQueue<ByteArray>()
    private val messageId = AtomicInteger(0)
    @Volatile private var payloadBudget = NavProtocol.BLE_ATT_DEFAULT_PAYLOAD_MAX

    // region 生命周期

    @Volatile private var scanning = false
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            // 华为芯片的硬件名字过滤器不可靠, 全量扫描后在软件层匹配
            val name = result.device.name ?: scanRecordName(result.scanRecord?.bytes)
            Log.d(TAG, "adv: addr=${result.device.address} name=$name")
            if (name != null && name.contains(DEVICE_NAME, ignoreCase = true)) {
                stopScan()
                log("device found: $name ${result.device.address}")
                connect(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            log("scan failed: errorCode=$errorCode")
            retryScan()
        }
    }

    /** 从原始广播记录解析完整设备名 (部分栈 device.name 恒为 null) */
    private fun scanRecordName(bytes: ByteArray?): String? {
        if (bytes == null) return null
        var i = 0
        while (i + 1 < bytes.size) {
            val len = bytes[i].toInt() and 0xFF
            if (len == 0) break
            val type = bytes[i + 1].toInt() and 0xFF
            if (i + 1 + len > bytes.size) break
            // 0x09 Complete Local Name / 0x08 Shortened Local Name
            if (type == 0x09 || type == 0x08) {
                return String(bytes, i + 2, len - 1, Charsets.UTF_8)
            }
            i += 1 + len
        }
        return null
    }

    fun start() {
        val adapter = adapter ?: run {
            log("no bluetooth adapter")
            return
        }
        if (!adapter.isEnabled) {
            log("bluetooth disabled, please enable it")
            return
        }
        if (connected.get()) return
        log("scanning for $DEVICE_NAME ...")
        scanning = true
        // 不带硬件过滤器: 华为控制器对 setDeviceName 过滤匹配失败时静默丢结果
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        adapter.bluetoothLeScanner?.startScan(null, settings, scanCallback) ?: run {
            scanning = false
            log("bluetooth le scanner unavailable")
            return
        }
        mainHandler.postDelayed({
            if (scanning) {
                stopScan()
                log("scan timeout, trying direct connect to $KNOWN_DEVICE_ADDRESS")
                // 华为机扫描不可见时直连已知 MAC: BLE 控制器会自己发 CONNECT_REQ
                try {
                    val device = adapter.getRemoteDevice(KNOWN_DEVICE_ADDRESS)
                    connect(device)
                } catch (e: IllegalArgumentException) {
                    log("direct connect failed: bad address")
                    retryScan()
                }
            }
        }, SCAN_TIMEOUT_MS)
    }

    /** 扫描失败/超时后自动重试, 15s 间隔, 直到连上为止 */
    private fun retryScan() {
        if (connected.get() || scanning) return
        mainHandler.postDelayed({
            if (!connected.get() && !scanning) {
                log("retry scan")
                start()
            }
        }, 15000L)
    }

    fun stop() {
        stopScan()
        writeQueue.clear()
        gatt?.let { g ->
            g.disconnect()
            g.close()
        }
        gatt = null
        navChar = null
        if (connected.getAndSet(false)) {
            listener.onConnectionStateChanged(false)
        }
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    @Suppress("DEPRECATION")
    private fun connect(device: BluetoothDevice) {
        log("connecting ${device.address}")
        gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            device.connectGatt(context, false, gattCallback)
        }
    }

    // endregion

    // region GATT 回调

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    log("connected, discovering services")
                    g.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    log("disconnected status=$status")
                    connected.set(false)
                    payloadBudget = NavProtocol.BLE_ATT_DEFAULT_PAYLOAD_MAX
                    writeQueue.clear()
                    g.close()
                    if (gatt === g) gatt = null
                    navChar = null
                    listener.onConnectionStateChanged(false)
                    // 自动重连
                    mainHandler.postDelayed({ start() }, 3000L)
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                log("service discovery failed status=$status")
                g.disconnect()
                return
            }
            val service = g.getService(SERVICE_UUID)
            if (service == null) {
                log("service 0xFFF0 not found")
                g.disconnect()
                return
            }
            val characteristic = service.getCharacteristic(NAV_CHAR_UUID)
            if (characteristic == null) {
                log("characteristic 0xFF01 not found")
                g.disconnect()
                return
            }
            navChar = characteristic
            log("requesting mtu=$REQUESTED_MTU")
            g.requestMtu(REQUESTED_MTU)
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            payloadBudget = if (status == BluetoothGatt.GATT_SUCCESS && mtu > NavProtocol.BLE_ATT_WRITE_OVERHEAD) {
                (mtu - NavProtocol.BLE_ATT_WRITE_OVERHEAD).coerceAtMost(
                    NavProtocol.NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX
                )
            } else {
                NavProtocol.BLE_ATT_DEFAULT_PAYLOAD_MAX
            }
            log("mtu=$mtu status=$status payload_budget=$payloadBudget")
            connected.set(true)
            listener.onConnectionStateChanged(true)
        }

        @Deprecated("Deprecated in Java")
        @Suppress("DEPRECATION")
        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (characteristic.uuid == NAV_CHAR_UUID) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    log("write failed status=$status")
                }
                writing.set(false)
                pumpQueue()
            }
        }
    }

    // endregion

    // region 写入

    /** 提交一条导航事件: 自动选择单包/分片并串行写入。 */
    fun sendNavigation(
        turnType: Int,
        stepDistanceM: Int,
        totalDistanceM: Int,
        remainTimeS: Int,
        roadName: String,
    ) {
        if (!connected.get()) {
            log("not connected, drop nav packet")
            return
        }
        val frames = NavProtocol.encodeFrames(
            turnType = turnType,
            stepDistanceM = stepDistanceM,
            totalDistanceM = totalDistanceM,
            remainTimeS = remainTimeS,
            roadName = roadName,
            attPayloadBudget = payloadBudget,
            messageId = messageId.incrementAndGet(),
        )
        frames.forEach { writeQueue.add(it) }
        log("nav packet queued: frames=${frames.size} budget=$payloadBudget turn=$turnType road=$roadName")
        pumpQueue()
    }

    @Suppress("DEPRECATION")
    private fun pumpQueue() {
        if (writing.getAndSet(true)) return
        try {
            while (true) {
                val frame = writeQueue.poll() ?: break
                val characteristic = navChar ?: break
                val g = gatt ?: break
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    val result = g.writeCharacteristic(
                        characteristic,
                        frame,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
                    )
                    if (result != android.bluetooth.BluetoothStatusCodes.SUCCESS) {
                        log("writeCharacteristic failed: $result")
                        break
                    }
                } else {
                    characteristic.value = frame
                    // 有响应写: 所有栈都会可靠回调 onCharacteristicWrite 释放队列
                    // (华为等旧栈对 NO_RESPONSE 不回调, 会导致队列卡死)
                    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    if (!g.writeCharacteristic(characteristic)) {
                        log("writeCharacteristic returned false")
                        break
                    }
                }
                // 等待 onCharacteristicWrite 继续泵队列
                return
            }
            // 队列空或写失败: 释放锁
            writing.set(false)
        } catch (e: Exception) {
            writing.set(false)
        }
    }

    // endregion

    private fun log(line: String) {
        Log.i(TAG, line)
        listener.onLog(line)
    }
}
