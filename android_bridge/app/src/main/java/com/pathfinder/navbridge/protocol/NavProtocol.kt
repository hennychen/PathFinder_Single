package com.pathfinder.navbridge.protocol

/**
 * PathFinder 导航 BLE 协议编码。
 *
 * 必须与固件真源字节级一致：
 * - components/service_navigation/include/navigation_protocol.h
 * - components/service_navigation/navigation_protocol.c
 * - docs/materials/navigation_bridge_protocol.md
 */
object NavProtocol {
    const val NAV_PACKET_MIN_SIZE = 8
    const val NAV_ROAD_NAME_MAX = 32
    const val NAV_PACKET_MAX_SIZE = NAV_PACKET_MIN_SIZE + NAV_ROAD_NAME_MAX // 40

    const val NAV_FRAGMENT_MARKER = 0xFF
    const val NAV_FRAGMENT_HEADER_SIZE = 5

    const val BLE_ATT_DEFAULT_MTU = 23
    const val BLE_ATT_WRITE_OVERHEAD = 3
    const val BLE_ATT_DEFAULT_PAYLOAD_MAX = BLE_ATT_DEFAULT_MTU - BLE_ATT_WRITE_OVERHEAD // 20

    const val NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX = 40

    // 转向类型 0..6, 与 firmware navigation_turn_type_t 对齐
    const val TURN_STRAIGHT = 0
    const val TURN_LEFT = 1
    const val TURN_RIGHT = 2
    const val TURN_LEFT_FRONT = 3
    const val TURN_RIGHT_FRONT = 4
    const val TURN_U_TURN = 5
    const val TURN_ARRIVE = 6

    /**
     * UTF-8 安全前缀裁剪: 返回不超过 maxBytes 的完整 UTF-8 字符前缀。
     * 与固件 utf8_safe_prefix_len / Python utf8_safe_prefix 行为一致。
     */
    fun utf8SafePrefix(text: String, maxBytes: Int): ByteArray {
        val bytes = text.toByteArray(Charsets.UTF_8)
        // 前向遍历, 与固件 utf8_safe_prefix_len 一致: 逐字符累计, 截断到完整字符边界
        var offset = 0
        while (offset < maxBytes && offset < bytes.size) {
            val lead = bytes[offset].toInt() and 0xFF
            val charLen = when {
                lead in 0x00..0x7F -> 1
                lead in 0xC2..0xDF -> 2
                lead in 0xE0..0xEF -> 3
                lead in 0xF0..0xF4 -> 4
                else -> break // 孤立续字节或非法首字节
            }
            if (offset + charLen > maxBytes || offset + charLen > bytes.size) break
            for (i in 1 until charLen) {
                if ((bytes[offset + i].toInt() and 0xC0) != 0x80) return bytes.copyOf(offset)
            }
            offset += charLen
        }
        return bytes.copyOf(offset)
    }

    /** 编码单包: turn(1) step(2LE) total(2LE) remain(2LE) name_len(1) name(n) */
    fun encodeSinglePacket(
        turnType: Int,
        stepDistanceM: Int,
        totalDistanceM: Int,
        remainTimeS: Int,
        roadName: String,
    ): ByteArray {
        require(turnType in 0..6) { "turn_type must be 0..6" }
        val road = utf8SafePrefix(roadName, NAV_ROAD_NAME_MAX)
        val payload = ByteArray(NAV_PACKET_MIN_SIZE + road.size)
        payload[0] = turnType.toByte()
        payload[1] = (stepDistanceM and 0xFF).toByte()
        payload[2] = ((stepDistanceM shr 8) and 0xFF).toByte()
        payload[3] = (totalDistanceM and 0xFF).toByte()
        payload[4] = ((totalDistanceM shr 8) and 0xFF).toByte()
        payload[5] = (remainTimeS and 0xFF).toByte()
        payload[6] = ((remainTimeS shr 8) and 0xFF).toByte()
        payload[7] = road.size.toByte()
        road.copyInto(payload, 8)
        require(payload.size in NAV_PACKET_MIN_SIZE..NAV_PACKET_MAX_SIZE)
        return payload
    }

    /**
     * 按 ATT payload 预算选择单包或分片帧。
     * 与 bridge_demo.py encode_frames 完全一致。
     * 分片帧: 0xFF + message_id + chunk_index + chunk_count + chunk_len + data
     */
    fun encodeFrames(
        turnType: Int,
        stepDistanceM: Int,
        totalDistanceM: Int,
        remainTimeS: Int,
        roadName: String,
        attPayloadBudget: Int,
        messageId: Int,
    ): List<ByteArray> {
        require(attPayloadBudget > 0)
        val single = encodeSinglePacket(turnType, stepDistanceM, totalDistanceM, remainTimeS, roadName)
        if (single.size <= attPayloadBudget) return listOf(single)

        val fragmentBudget = attPayloadBudget - NAV_FRAGMENT_HEADER_SIZE
        require(fragmentBudget > 0) { "att payload budget too small for fragments" }

        val chunkCount = (single.size + fragmentBudget - 1) / fragmentBudget
        val frames = ArrayList<ByteArray>(chunkCount)
        for (index in 0 until chunkCount) {
            val start = index * fragmentBudget
            val end = minOf(start + fragmentBudget, single.size)
            val chunk = single.copyOfRange(start, end)
            val frame = ByteArray(NAV_FRAGMENT_HEADER_SIZE + chunk.size)
            frame[0] = NAV_FRAGMENT_MARKER.toByte()
            frame[1] = (messageId and 0xFF).toByte()
            frame[2] = (index and 0xFF).toByte()
            frame[3] = (chunkCount and 0xFF).toByte()
            frame[4] = chunk.size.toByte()
            chunk.copyInto(frame, NAV_FRAGMENT_HEADER_SIZE)
            frames.add(frame)
        }
        return frames
    }

    fun defaultBudget(): Int = BLE_ATT_DEFAULT_PAYLOAD_MAX
}
