# 车载仪表 + 语音交互 Spec

## 1. 文档目的

本文档将“微雪 ESP32-S3-Touch-LCD-1.46 单板车载仪表 + 语音交互”方案收敛为可执行的软件规格，适用于当前空仓库从 0 到 1 建设。目标是先明确边界、模块、阶段验收与代码组织，再进入逐阶段实施。

## 2. 项目基线

- 硬件平台：Waveshare ESP32-S3-Touch-LCD-1.46
- 软件栈：ESP-IDF v5.4+、FreeRTOS、LVGL、NimBLE、ESP-SR
- 运行约束：单 ESP32-S3R8、16MB Flash、8MB PSRAM、圆形 412x412 屏、板载麦克风和喇叭
- 仓库现状：当前仓库仅有 `LICENSE`，默认按绿地工程建设，不假设已有可复用工程骨架

## 3. 双视角规格

### 3.1 业务子域视角

系统拆分为 6 个业务子域：

| 子域 | 目标 | 输入 | 输出 |
| :--- | :--- | :--- | :--- |
| 仪表显示 | 输出车载主视觉信息 | 姿态、导航、OBD、RTC、电量、语音状态 | Page 0-5 UI |
| 姿态感知 | 稳定输出横滚/俯仰 | QMI8658 原始加速度/陀螺仪 | roll/pitch |
| 导航协同 | 接收手机导航并投射到圆屏 | BLE 导航帧 | 箭头、距离、路名、自动跳转 |
| 车辆数据 | 读取 OBD-II 实时状态 | ELM327 BLE 数据 | 车速、RPM、水温、告警 |
| 语音交互 | 唤醒、对话、控制设备 | 麦克风、Wi-Fi、云端响应 | TTS 播放、表情页、设备控制 |
| 电源与系统 | 维持基础运行与待机策略 | RTC、电池 ADC、用户操作 | 时钟、电量、背光、轻睡眠 |

### 3.2 工程实施视角

系统按 4 层组织：

1. `drivers`
   - 屏幕/触摸
   - QMI8658
   - PCF85063
   - audio codec / i2s
   - battery adc
   - tf card spi
2. `services`
   - attitude_service
   - ui_service
   - nav_service
   - obd_service
   - voice_service
   - power_service
   - log_service
3. `app`
   - page manager
   - event bus
   - state store
   - system bootstrap
4. `main`
   - 启动入口
   - 组件初始化
   - FreeRTOS 任务创建

## 4. 建议目录结构

```text
PathFinder_Single/
├─ docs/
│  ├─ vehicle_dashboard_voice_spec.md
│  ├─ vehicle_dashboard_voice_tasks.md
│  └─ vehicle_dashboard_voice_checklist.md
├─ main/
│  └─ app_main.c
├─ components/
│  ├─ board_support/
│  ├─ drivers_display/
│  ├─ drivers_touch/
│  ├─ drivers_qmi8658/
│  ├─ drivers_pcf85063/
│  ├─ drivers_audio/
│  ├─ voice_backend/
│  ├─ drivers_power/
│  ├─ service_log/
│  ├─ service_ui/
│  ├─ service_attitude/
│  ├─ service_navigation/
│  ├─ service_obd/
│  ├─ service_voice/
│  ├─ service_power/
│  ├─ app_pages/
│  ├─ app_state/
│  └─ app_events/
├─ managed_components/
├─ sdkconfig.defaults
├─ idf_component.yml
└─ CMakeLists.txt
```

## 5. 关键状态模型

建议建立统一的 `WorkflowState` 作为跨任务共享真源，避免页面、BLE、语音各自维护一份状态。

```c
typedef struct {
    uint8_t state_code;
    uint32_t session_count;
    uint32_t session_duration_ms;
    uint32_t state_duration_ms;
    uint32_t input_bytes;
    uint32_t output_bytes;
    uint32_t input_frame_count;
    uint32_t output_frame_count;
    uint32_t state_change_count;
    bool output_silence;
    char input_text[96];
    char output_text[96];
} voice_runtime_state_t;

typedef struct {
    bool ui_ready;
    bool voice_active;
    bool nav_active;
    bool obd_connected;
    bool ble_phone_connected;
    bool ble_obd_connected;
    bool wifi_connected;
    uint8_t current_page;
    uint8_t previous_page;
    float roll_deg;
    float pitch_deg;
    uint16_t heading_deg;
    uint16_t speed_kmh;
    uint16_t rpm;
    int8_t coolant_temp_c;
    uint8_t battery_percent;
    voice_runtime_state_t voice_runtime;
    uint32_t last_nav_update_ms;
    uint32_t last_user_action_ms;
} WorkflowState;
```

## 6. 页面规格

### 6.1 页面定义

| 页面 | 代号 | 主数据源 | 刷新频率 |
| :--- | :--- | :--- | :--- |
| 姿态仪表 | `PAGE_ATTITUDE` | IMU | 20 FPS |
| 指南针 | `PAGE_COMPASS` | IMU/磁向推导占位 | 10 FPS |
| 导航 | `PAGE_NAV` | BLE 手机导航 | 5 FPS |
| OBD 仪表 | `PAGE_OBD` | OBD BLE | 5 FPS |
| 系统 | `PAGE_SYSTEM` | RTC/电池/连接状态 | 2 FPS |
| 语音表情 | `PAGE_VOICE` | voice state | 10 FPS |

### 6.2 页面切换优先级

1. `VOICE_ACTIVE` 时强制显示 `PAGE_VOICE`
2. 导航新数据到达且未在语音中时，切到 `PAGE_NAV`
3. OBD 告警时可短暂抢占到 `PAGE_OBD`
4. 普通手势/按键在 `PAGE_ATTITUDE` 到 `PAGE_SYSTEM` 间循环
5. 强制页结束后返回 `previous_page`

当前实现补充：

- 页面抢占已统一收口到 `app_state`，按 `VOICE > OBD 告警 > NAV > 用户页` 做中心仲裁
- `service_voice / service_navigation / service_obd` 当前只发布状态，不再各自维护恢复页；`service_ui` 只负责写入用户页选择和按仲裁结果渲染

## 7. 任务与核心绑定

| 任务 | Core | 优先级 | 说明 |
| :--- | :--- | :--- | :--- |
| `task_lvgl` | 1 | 5 | UI tick、页面刷新、动画 |
| `task_voice` | 1 | 6 | 唤醒、录音流、播放控制 |
| `task_imu` | 0 | 4 | 250Hz 采样 + 姿态解算 |
| `task_ble` | 0 | 3 | 手机导航和 OBD 连接管理 |
| `task_obd` | 0 | 2 | PID 轮询 |
| `task_power` | 0 | 1 | 电量与休眠状态 |
| `task_log` | 0 | 1 | SD 日志 |

要求：

- UI 和语音固定在 Core 1
- IMU 和 BLE 固定在 Core 0
- 页面层禁止直接访问驱动，统一走 service 或 state
- 所有跨任务共享数据通过 event queue + state snapshot 访问

## 8. 模块边界

### 8.1 `drivers_qmi8658`

- 负责 I2C 初始化、寄存器配置、原始数据读取
- 不负责滤波与姿态融合
- 输出：`qmi8658_sample_t`

### 8.2 `service_attitude`

- 负责互补滤波、低通滤波、角度归一化
- 输出：`roll_deg`、`pitch_deg`
- 更新频率：输入 250Hz，输出 25Hz

### 8.3 `service_navigation`

- 负责导航帧解析、超时处理、页面跳转事件
- 不负责手机侧协议生成
- 对外提供 `nav_state_t`
- 当前实现已补 `task_navigation + packet queue`，先以 `service_navigation_submit_packet()` 作为导航包入口
- 当前已固定 `Service=0xFFF0`、`Navigation Char=0xFF01`、`OBD Char=0xFF02`
- 当前会把有效包、非法包、最近路名、剩余距离/时间写入 `WorkflowState.navigation`
- 当前若语音页未抢占，则新导航包会切到 `PAGE_NAV`；若语音抢占中，则先暂存并在语音结束后跳转；`10s` 无新包后自动回退上一页
- 当前手机对 `0xFF01` 的 write / write-no-rsp 会通过共享 `service_ble` bridge 进入现有导航包解析链路
- 当前 `PAGE_NAV` 已从文本占位页升级为首版专用导航 UI：状态 chip、转向标题、中心箭头、下一步距离、路名、提示文案和总距离/ETA/包统计

### 8.3.1 `service_ble`

- 负责共享 NimBLE Host 生命周期、GATT table 注册、advertising 与手机连接状态回写
- 当前 bridge 固定暴露 `0xFFF0 / 0xFF01 / 0xFF02`，设备名保持 `PathFinder Nav`
- 当前 `service_navigation` 与 `service_obd` 通过回调注册把导航/OBD 写入桥接到各自 service，而不再各自持有 NimBLE Host
- 当前 bridge 会按已协商 ATT MTU 计算单次可写 payload 预算；默认连接预算为 `20B`，MTU 更新后才会放宽导航包可接受长度，避免手机侧在未协商 MTU 时直接写入超长路名包
- 当前已补 `service_ble_client` 能力：可在共享 Host 上执行扫描、按广告名匹配、发现 service/characteristic、订阅 notify，并提供流式读写接口，作为后续自定义 `hal_obd_transport` 的最小接缝

### 8.4 `service_obd`

- 负责 ELM327 会话、PID 轮询调度、阈值告警
- 优先实现 RPM/Speed/Coolant 三项
- 燃油与节气门列为扩展项
- 当前实现已补 `obd_provider_esp_obd_ii` 适配层，采用轮询式 provider -> service 两层结构：provider 负责 `obd_begin` / `obd_read_pid` 和 round-robin PID 调度，service 负责状态汇聚、超时降级、页面抢占与 bridge/mock 优先级控制
- 当前已补 `obd_adapter_profile` 真源，先收敛 `active_sdkconfig / generic_elm327_ble / vgate_icar_pro_ble` 三类 profile；当前运行仍以 `sdkconfig.defaults` 的活动配置为准，待真机扫描确认后再决定是否切到 `Vgate` profile
- 当前已新增 `service_ble_elm327` 最小 provider：复用共享 `service_ble_client` 完成 BLE 扫描、AT 初始化 (`ATZ/ATE0/ATL0/ATS0/ATH0/ATAT0/ATSP0`) 与 `010C/010D/0105/0111/012F` 五项 PID 轮询解析
- 当前 provider 选择逻辑已改为：共享 `service_ble` Host 活跃时优先走 `service_ble_elm327`，否则保留 `esp_obd_ii` 原始 provider；但该真链路尚无 fresh verification，初始化序列与适配器兼容性仍需真机确认
- 当前已补 OBD provider 诊断态：`WorkflowState.obd` 会额外携带 backend 名称、稳定诊断码、运行阶段、连接尝试次数、失败次数、更新次数、扫描/连接/发现三段 BLE 耗时、最近一次失败阶段、最近一次失败 rc 与最后一条诊断文案；其中共享 `service_ble_client` 现已可区分 `scan_timeout / service_not_found / characteristic_not_found / descriptor_not_found` 等失败来源，并尽量以短摘要格式透出命中的设备名、缺失 UUID 以及命中的 service/characteristic/descriptor handle，`PAGE_OBD` 可直接显示“没扫到设备”与“GATT 不匹配”等卡点，同时补充 `Scan / Conn / Disc` 分段耗时以及最近一次失败定位

### 8.5 `service_voice`

- 负责语音生命周期状态：
  - idle
  - wake_detected
  - streaming
  - speaking
  - error
- 对页面管理器只暴露状态机，不直接操作 UI 控件
- 通过 `voice_backend` 适配层对接 `xiaozhi-esp32`，当前选型为“组件化引入”
- 当前实现补充 `task_voice + command queue`，后续唤醒词、录音流、播放控制统一通过任务上下文串行处理
- 当前 `drivers_audio` 已提供最小帧级接口，`task_voice` 周期性执行 MIC capture tick、speaker silence tick 与会话日志
- 当前音频通道策略已统一收口到 `service_audio`：`service_voice` 只声明当前阶段需要 `MIC capture`、`Speaker playback` 或 `think hold`，不再直接作为系统级音频仲裁者
- 当前 `voice_backend` 已开始承接最小 session/runtime hook，`task_voice` 通过 backend 决定帧消耗与播放输出
- 当前 `voice_backend` 占位实现已具备脚本化 mock 会话流转：wake 后自动推进到 streaming，再进入 silence think 和 mock reply speaking，最后回落 idle
- 当前语音运行时信息已写入 `WorkflowState`，`PAGE_VOICE` 直接展示 state/session/duration/input/output，并补充当前阶段停留时长、输入/输出帧计数、状态切换计数，以及输入/输出气泡文本
- 当前 `PAGE_VOICE` 已有增强版专用 UI：状态 chip、相位文案、中心 orb、表情词、小波条、输入/输出活动条、双气泡提示、阶段时间线，以及基于运行时字节量/会话时长/帧计数推导的动态摘要与统计区；双气泡文案已优先直接消费 `voice_runtime.input_text/output_text`

### 8.6 `service_ui`

- 管理页面创建、切换、圆屏安全区域布局
- 只消费 state，不持有业务真源

### 8.7 `service_power`

- 负责用户空闲时长评估、背光策略和轻睡眠准入判断
- 当前首版已接入 `30s` 降背光与 `120s` 轻睡眠执行链；首版唤醒源为 BOOT 键与触摸中断
- 当前首版已接入电池 ADC 周期采样：`service_power` 每 `2s` 读取 `drivers_power` 输出，并把 `battery_percent / battery_voltage_mv` 发布到状态总线
- 用户操作真源当前来自 BOOT 键和触摸事件，统一写入 `WorkflowState.last_user_action_ms`

### 8.8 `service_log`

- 负责 TF 卡挂载与运行时日志文件落盘
- 当前首版按板卡 SPI 走线 `SCK=GPIO14 / MISO=GPIO16 / MOSI=GPIO17 / CS=EXIO3` 接入
- 当前通过 `esp_log_set_vprintf()` 把串口日志同步追加到 `/sdcard/pathfinder.log`
- 挂卡失败时只降级为串口日志，不阻塞主系统启动

## 9. 数据契约

### 9.1 导航帧

建议不要继续沿用“固定 12 字节”描述，而改为“最小头 + 可变路名”的协议文档化，便于实现：

```c
typedef struct __attribute__((packed)) {
    uint8_t turn_type;
    uint16_t step_distance_m;
    uint16_t total_distance_m;
    uint16_t remain_time_s;
    uint8_t road_name_len;
    char road_name[32];
} nav_packet_t;
```

说明：

- 当前工程已把这组字段实现为 `navigation_protocol.{h,c}`，并提供 parse / encode 双向函数，方便 BLE 收包和 mock 注入共用同一协议入口
- 手机桥接侧对接规范已单独整理到 `docs/materials/navigation_bridge_protocol.md`
- 当前仓库也已补桌面侧发送原型 `tools/navigation_bridge/bridge_demo.py`，可作为手机桥 / 桌面脚本的起步真源
- 当前桌面桥接已开始按“provider adapter -> 统一导航事件 -> NavigationPacket”分层，`tools/navigation_bridge/map_adapters.py` 负责把不同地图厂商事件先归一化，再进入统一编码链路
- 当前桌面桥接已补 `tools/navigation_bridge/verify_adapters.py`，用仓库内事件样本对 provider 归一化和默认 ATT 预算下的分片策略做轻量回归
- 当前验证已确认：若仍停留在默认 `20B` ATT payload，长中文路名对应的最大导航包可能会拆成 `3` 片；只有在更大 MTU 下，分片数才会进一步下降
- 当前 `amap` provider 已开始向官方 `NaviInfo` 对齐：bridge 侧优先读取 `payload.naviInfo` 中的 `iconType / segmentRemainDistance / routeRemainDistance / routeRemainTime / nextRoadName / currentRoadName`，再回退到扁平桥接字段
- `road_name_len` 必须限制最大长度，例如 32 字节
- 当前协议层已补 UTF-8 安全裁剪：若中文路名在 32 字节边界处截断，会自动回退到完整字符边界，避免 UI 展示半个汉字或非法尾字节
- BLE 单包长度需要结合 MTU 校验，必要时使用裁剪路名
- 接收端必须做长度合法性检查

若 BLE 默认 MTU 不足以承载完整导航包，当前仓库额外支持兼容型分片帧：

```c
typedef struct __attribute__((packed)) {
    uint8_t marker;      // 固定 0xFF，避免与 turn_type 0..6 冲突
    uint8_t message_id;  // 同一条导航消息的分片编号
    uint8_t chunk_index; // 从 0 开始
    uint8_t chunk_count; // 总分片数
    uint8_t chunk_len;   // 当前分片数据长度
    uint8_t chunk_data[];
} nav_fragment_frame_t;
```

说明：

- 设备侧当前要求分片按顺序到达，并在 `1s` 窗口内完成重组
- 重组完成后，仍进入现有的单包 `navigation_protocol_parse_packet()` 解析链路
- 若手机侧已协商到足够 MTU，仍优先使用原始单包协议，避免不必要的桥接复杂度
- 协议层当前也已提供 `navigation_protocol_encode_fragment_frames()`，方便手机桥、桌面脚本或 mock 直接复用仓库真源生成分片帧

### 9.2 OBD 状态

```c
typedef struct {
    bool connected;
    uint16_t speed_kmh;
    uint16_t rpm;
    int8_t coolant_temp_c;
    uint8_t throttle_percent;
    uint8_t fuel_percent;
    bool alert_active;
    uint32_t sample_count;
    uint32_t last_update_ms;
    char alert_text[48];
} obd_state_t;
```

说明：

- 当前工程已把 OBD 接入收敛为 `service_obd` + `obd_protocol` 两层：协议层负责固定长度载荷 parse / encode，service 层负责队列化收包、超时降级、告警生成与 `WorkflowState` 发布
- 当前 `PAGE_OBD` 首版 UI 已消费上述状态，展示状态 chip、速度、RPM、水温、油门/油量条和告警摘要
- 当前 OBD 告警已具备最小页面行为：当 `alert_active=true` 且未被语音强制页占用时，系统会短时抢占到 `PAGE_OBD`；告警清除后自动回退到此前页面
- 当前 BLE `0xFF02` Characteristic 已作为 OBD 载荷入口转发到 `service_obd_submit_packet()`；真实 `esp_obd_ii` 与 ELM327 会话仍待后续替换内部数据源

## 10. 非功能要求

### 10.1 性能

- 姿态页主观刷新平滑，不低于 20 FPS
- 导航页切换延迟小于 300ms
- 导航新数据到页面更新小于 200ms
- 车速/RPM 视觉更新时间不高于 1s

### 10.2 稳定性

- 任一 BLE 数据异常不得导致 UI 任务崩溃
- 语音会话中断后 2s 内必须恢复到可交互状态
- I2C 设备读取失败需具备重试与降级日志

### 10.3 可维护性

- 页面和业务状态解耦
- 每个硬件模块单独组件化
- 每个 service 至少有 1 个最小自测入口或 mock 接口

## 11. 阶段目标重构

### P0 基础 bring-up

目标：

- 工程可编译
- 屏幕可显示
- 触摸可上报
- RTC 可读写
- 系统页可显示时间和状态占位

### P1 姿态仪表

目标：

- QMI8658 驱动稳定
- 输出 roll/pitch
- 姿态页与指南针页可用

### P2 语音基础集成

目标：

- 跑通板卡音频输入输出
- 语音状态可驱动 Page 5
- 与 UI 页切换不冲突

### P3 导航接入

目标：

- 手机侧可发 BLE 导航包
- 设备端解析并展示导航页
- 导航超时返回上一页

### P4 OBD 接入

目标：

- 连接 ELM327
- 至少完成 3 个 PID 的稳定读取与 UI 展示

当前阶段进展：

- 已完成 `service_obd` 骨架、状态模型、mock 包注入、告警文案生成与 `PAGE_OBD` 首版 UI
- 尚未完成 `esp_obd_ii` 真接入、PID 调度和新鲜构建验证

### P5 整合优化

目标：

- 任务优先级与切页策略稳定
- 电量、背光、轻睡眠可用

## 12. 当前阶段建议

由于仓库为空，建议严格按下面顺序推进，而不是同时并行所有模块：

1. 建 ESP-IDF 工程骨架
2. 跑通屏幕、触摸、RTC、日志
3. 建统一 `WorkflowState` 和页面管理器
4. 接 IMU，再接姿态页面
5. 接语音页面状态机
6. 最后接导航和 OBD

原因：屏幕/UI 和状态总线是所有后续模块的公共底座，先打稳可以显著减少返工。
