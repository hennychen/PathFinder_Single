# 手机导航桥接协议

本文档面向手机侧 BLE 导航桥、桌面调试脚本和后续高德事件适配层，说明如何向设备的 `0xFF01` characteristic 发送导航数据。

当前桥接层设计为“地图厂商无关”：

- 地图 SDK 事件先在桥接侧归一化成统一导航事件
- 再由统一导航事件编码为单包或分片帧
- 因此高德、百度、腾讯或其它地图厂商的差异，应该收敛在桥接 adapter，而不是固件协议层

其中高德导航当前已进一步对齐到更接近官方的 `onNaviInfoUpdate(NaviInfo)` 数据结构，详见 `docs/materials/amap_navigation_adapter.md`。

## 1. GATT 固定值

- 设备名：`PathFinder Nav`
- Service UUID：`0xFFF0`
- Navigation Characteristic：`0xFF01`
- OBD Characteristic：`0xFF02`

## 2. 发送规则

手机侧写入 `0xFF01` 时，按下面顺序选择发送方式：

1. 若当前已知 ATT payload 预算足够承载完整导航包，优先发送单包
2. 若未做 MTU exchange，或当前预算不足，改发分片帧
3. 分片必须按顺序发送，且同一条消息需要在 `1s` 内发完

当前设备侧默认预算：

- 默认 ATT MTU：`23`
- 默认 ATT payload：`20B`
- 导航单包最大：`40B`

这意味着：

- 短路名包可以直接单包发送
- 较长中文路名在未协商大 MTU 时，通常要走分片

## 3. 单包格式

```c
typedef struct __attribute__((packed)) {
    uint8_t turn_type;
    uint16_t step_distance_m;
    uint16_t total_distance_m;
    uint16_t remain_time_s;
    uint8_t road_name_len;
    uint8_t road_name[road_name_len];
} nav_packet_t;
```

字段说明：

- `turn_type`：
  - `0` 直行
  - `1` 左转
  - `2` 右转
  - `3` 左前方
  - `4` 右前方
  - `5` 掉头
  - `6` 到达
- `step_distance_m`：下一步距离，小端
- `total_distance_m`：总剩余距离，小端
- `remain_time_s`：总剩余时间，小端
- `road_name_len`：路名字节数，最大 `32`
- `road_name`：UTF-8 路名

注意：

- 设备侧会按 UTF-8 完整字符边界裁剪路名，避免半个汉字
- 单包总长度必须等于 `8 + road_name_len`

## 4. 分片格式

当单包长度超过当前 ATT payload 预算时，改发下面的分片帧：

```c
typedef struct __attribute__((packed)) {
    uint8_t marker;      // 固定 0xFF
    uint8_t message_id;  // 同一条导航消息的分片编号
    uint8_t chunk_index; // 从 0 开始递增
    uint8_t chunk_count; // 总分片数
    uint8_t chunk_len;   // 当前分片数据长度
    uint8_t chunk_data[chunk_len];
} nav_fragment_frame_t;
```

约束：

- `marker` 必须为 `0xFF`
- `chunk_index` 必须从 `0` 开始，严格递增
- `chunk_count` 在同一条消息内必须一致
- 协议层支持的单片最大数据段上限是 `35B`
- 但桌面/手机桥接应按“当前 ATT payload 预算 - 5B 头部”来计算实际 `chunk_len`
- 因此在默认 `20B` ATT payload 下，单片数据段只有 `15B`，最大 `40B` 导航包最多会拆成 `3` 片
- 若手机侧已协商到更大 MTU，实际分片数会下降；在 `35B` 数据段预算下，最大导航包才会降到最多 `2` 片

设备侧行为：

- 只接受顺序分片
- 若消息号变化、分片总数变化、顺序错乱或长度超界，会直接丢弃当前重组状态
- 若 `1s` 内未收完整条消息，也会丢弃
- 重组完成后，再进入现有单包导航协议解析链路

## 5. 手机侧伪代码

```c
nav_packet_t packet = build_nav_packet(...);
uint8_t encoded[40];
size_t encoded_len = encode_single_packet(packet, encoded);

if (encoded_len <= current_att_payload_budget) {
    ble_write(ff01, encoded, encoded_len);
    return;
}

uint8_t message_id = next_message_id();
size_t offset = 0;
size_t chunk_index = 0;
size_t chunk_count = (encoded_len + 35 - 1) / 35;

while (offset < encoded_len) {
    size_t chunk_len = min(35, encoded_len - offset);
    uint8_t frame[40];
    frame[0] = 0xFF;
    frame[1] = message_id;
    frame[2] = (uint8_t)chunk_index;
    frame[3] = (uint8_t)chunk_count;
    frame[4] = (uint8_t)chunk_len;
    memcpy(&frame[5], &encoded[offset], chunk_len);
    ble_write(ff01, frame, chunk_len + 5);
    offset += chunk_len;
    chunk_index++;
}
```

## 6. 仓库内真源

协议与编码 helper 以仓库代码为准：

- `components/service_navigation/include/navigation_protocol.h`
- `components/service_navigation/navigation_protocol.c`
- `components/service_navigation/service_navigation.c`

当前仓库已提供：

- 单包 encode / parse
- UTF-8 安全路名裁剪
- 分片帧重组
- 分片帧编码 helper：`navigation_protocol_encode_fragment_frames()`
- 桌面侧发送原型：`tools/navigation_bridge/bridge_demo.py`
- 厂商无关 adapter 骨架：`tools/navigation_bridge/map_adapters.py`
- 示例事件 JSON：`tools/navigation_bridge/examples/*.json`
- 桥接层回归校验脚本：`tools/navigation_bridge/verify_adapters.py`
- 高德字段对齐说明：`docs/materials/amap_navigation_adapter.md`
