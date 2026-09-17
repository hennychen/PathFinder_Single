# 车载仪表 + 语音交互任务拆解

> 注：当前开发机缺少 `idf.py`，下面的勾选表示“代码已接入到仓库”，不等同于“已完成构建/上板验收”。真正验收仍以第 5 节 Definition of Done 为准。
>
> 当前阶段的代码审计结论已单独整理到 `docs/materials/current_stage_audit.md`，用于区分“代码阶段已到哪一步”与“验收阶段实际关闭到哪一步”。

## 1. 执行原则

- 按单线程顺序推进，避免多模块同时起量
- 每阶段遵循：定边界 -> 读真源 -> 写失败用例/验证点 -> 最小修复 -> 定向验证 -> 判断是否达到手测准入
- 仅在当前阶段验收通过后进入下一阶段

## 2. 里程碑与任务

### M0 仓库初始化

- [x] 创建 ESP-IDF 工程基础目录与顶层 `CMakeLists.txt`
- [x] 添加 `idf_component.yml`
- [x] 增加 `sdkconfig.defaults`
- [x] 建立 `components/` 目录骨架
- [x] 建立 `docs/` 与阶段文档入口

交付物：

- `idf.py set-target esp32s3` 待具备 ESP-IDF 环境后验证
- [x] 项目目录结构固定

依赖：

- 无

### M1 板级 bring-up

- [x] 建 `board_support` 组件，集中放板级引脚和总线配置
- [x] 接入 LCD 初始化，能显示纯色/测试图
- [x] 接入触摸中断和坐标读取
- [x] 接入 PCF85063，完成读写时间
- [x] 建系统日志输出与错误码约定

交付物：

- Page System 占位页
- 触摸事件日志
- RTC 时间显示

依赖：

- M0

### M2 UI 框架

- [x] 接入 LVGL
- [x] 建 `service_ui`
- [x] 建页面管理器和页面枚举
- [x] 建 `WorkflowState`
- [x] 完成手势/按键切页基础逻辑

交付物：

- Page 0-4 空页面可切换
- 页面切换动画和返回逻辑

依赖：

- M1

### M3 姿态模块

- [x] 建 `drivers_qmi8658`
- [x] 建 `service_attitude`
- [x] 完成 250Hz 采样
- [x] 完成互补滤波
- [x] 完成低通滤波
- [x] 将姿态数据投递到状态总线
- [x] 完成姿态页和指南针页首版

交付物：

- 稳定 roll/pitch 数值
- 姿态页 20 FPS
- 当前 `heading_deg` 先由陀螺仪 Z 轴积分提供占位值，尚不代表真北航向

依赖：

- M2

### M4 语音框架接入

- [x] 选定 `xiaozhi-esp32` 集成方式：组件化引入或子仓库导入
- [x] 打通 I2S 输入输出
- [x] 建 `service_voice` 状态机
- [x] 语音激活时切换到 Page 5
- [x] 语音结束恢复上一页

交付物：

- Page 5 表情页占位
- 语音状态驱动的切页链路
- 已确定 `xiaozhi-esp32` 采用“组件化引入 + `voice_backend` 适配层隔离”的接入方式
- `drivers_audio` 已完成板载 MIC / PCM5101 的最小 I2S 通道初始化接入，并由 `service_voice_init()` 串起输入输出 enable 链路
- `service_voice` 当前已创建 `task_voice`（Core 1 / Priority 6）与命令队列，状态切换统一收敛为异步命令入口
- `drivers_audio` 当前已补最小音频帧接口：MIC 读取、speaker 写入、静音填充
- 当前 `task_voice` 已在任务上下文中执行 capture/playback tick，并输出基础会话生命周期日志
- `voice_backend` 当前已补最小 runtime hook：session start/stop 与帧处理接口，`task_voice` 已改为通过 backend 决定输入消耗与输出生成
- `voice_backend` 当前占位实现已升级为脚本化 mock backend：收到 wake 后可自动经历 `streaming -> thinking(silence) -> speaking(mock payload) -> idle` 最小闭环
- 语音运行时信息当前已进入 `WorkflowState`，包含 state/session/duration/input/output/output mode
- `voice_runtime` 当前已补输入/输出文本字段，`service_voice` 会按 mock 会话阶段发布气泡文案
- `PAGE_VOICE` 当前已升级为增强版首屏语音页：状态 chip、相位文案、中心 orb、表情词、小波条、输入/输出活动条、双气泡提示、阶段时间线、动态摘要/速率统计，以及基于阶段停留时长/帧计数/切换次数的运行态说明；双气泡文案优先直接消费状态总线文本
- 当前 `service_voice` 已具备“状态机 + 页面抢占 + 最小音频初始化 + 后端适配层 + 语音任务骨架 + backend runtime hook”底座，`xiaozhi-esp32` 真组件仍待后续落地

依赖：

- M2

### M5 BLE 导航

- [x] 设计 GATT Service/Characteristic UUID 常量
- [x] 建手机导航帧解析器
- [x] 建 `service_navigation`
- [x] 导航到达时切页
- [x] 10 秒超时回退上一页
- [x] 导航路名长度与非法包校验

交付物：

- 导航页首版显示箭头/转向/距离/路名
- 模拟导航包可驱动 UI
- 当前已新增 `navigation_protocol` 协议定义，固定 `Service=0xFFF0`、`Navigation Char=0xFF01`、`OBD Char=0xFF02`
- 当前导航帧采用“最小头 + 可变路名”协议：`turn + step_distance + total_distance + remain_time + road_name_len + road_name`
- `service_navigation` 当前已创建 `task_navigation`（Core 0 / Priority 3）与包队列，负责协议解析、非法包计数、页面自动跳转与 `10s` 超时回退
- 当前 `PAGE_NAV` 已升级为首版专用导航页：状态 chip、转向标题、中心箭头、下一步距离、路名、说明文案，以及总距离/ETA/包统计
- 当前可通过 `service_navigation_submit_packet()` 或 `service_navigation_submit_mock_packet()` 注入模拟导航包，作为后续 BLE Characteristic 写入入口的替身
- 当前 `navigation_protocol` 已补 UTF-8 安全路名裁剪：收到或构造中文路名时，会在 `32B` 上限内按完整字符边界截断，避免圆屏 UI 出现半个汉字或非法尾字节
- 当前已新增共享 `service_ble` 组件承接 NimBLE Host、GATT table 和 advertising；导航写特征值 `0xFF01` 会通过 bridge 回调直接把手机侧 payload 投递到现有包队列
- 当前 `service_ble` 已补 ATT payload 预算保护：默认按 `MTU=23 -> payload=20B` 接收导航包，只有在手机侧完成 MTU 协商后才放宽到更大的导航载荷；超预算写入会在 BLE bridge 层直接拒绝并输出 `allowed / mtu / attr_max` 诊断日志
- 当前 `service_navigation` 已补兼容型分片重组：短导航包仍走原单包协议；若手机侧未协商到足够 MTU，可改为发送 `0xFF + message_id + chunk_index + chunk_count + chunk_len + chunk_data` 分片帧，由设备侧在 `1s` 窗口内按顺序重组后再进入现有解析链路
- 当前已补 `navigation_bridge_protocol.md` 与 `navigation_protocol_encode_fragment_frames()`：手机桥 / 桌面脚本现在可以直接按仓库真源选择“单包 or 分片”发送，不需要再从设备端代码反推协议
- 当前已新增 `tools/navigation_bridge/bridge_demo.py`：提供桌面侧最小 BLE 导航发送原型，可先打印单包/分片十六进制帧，再在安装 `bleak` 后直接向 `PathFinder Nav / 0xFF01` 写入
- 当前已补 `tools/navigation_bridge/map_adapters.py` 与 `examples/*.json`：桥接层开始按“地图厂商无关 adapter -> 统一 NavigationPacket”分层，高德 / 百度 / 腾讯的差异先收在发送端前面
- 当前已补 `tools/navigation_bridge/verify_adapters.py` 与长中文路名样本：可本地回归验证 provider 归一化、默认 `20B` ATT 预算下的单包/分片选择，以及中文路名场景的分片退路
- 当前已用长中文路名样本校正默认预算口径：在 `20B` ATT payload 下，最大导航包可能拆成 `3` 片，而不是之前口头假设的 `2` 片
- 当前 `amap` adapter 已开始对齐官方 `onNaviInfoUpdate(NaviInfo)` 形态：支持 `naviInfo.iconType / segmentRemainDistance / routeRemainDistance / routeRemainTime / nextRoadName / currentRoadName`，并补了到达态与嵌套回调样本
- 当前 BLE 连接状态会回写 `ble_phone_connected`，System 页可看到手机连接是否建立

依赖：

- M2

### M6 OBD 接入

- [x] 引入 `esp_obd_ii`
- [ ] 连接 ELM327
- [x] 建 PID 调度器
- [ ] 完成 RPM/车速/水温读取
- [x] 完成 Page 3 仪表页首版
- [x] 建超阈值告警事件

- 当前已新增 `service_obd` 与 `obd_protocol`，补齐了 OBD 运行态、包解析、mock 注入口和超时降级逻辑
- 当前已新增 `obd_provider_esp_obd_ii` 适配层，并补了 `service_ble_elm327` 最小 provider；当前 provider 会按 Host 场景自动选择 backend：共享 `service_ble` Host 活跃时优先走 `service_ble_elm327`，否则保留 `esp_obd_ii`
- 当前已新增 `obd_adapter_profile` 真源，先收敛 `active_sdkconfig / generic_elm327_ble / vgate_icar_pro_ble` 三类 profile；当前默认仍保持 generic 风格配置，待 `Vgate iCar Pro BLE 4.0` 实物到手并完成 BLE 扫描后再最终落锤
- 当前已新增 `service_ble_client` 基础能力，可在共享 Host 上执行扫描、广告名匹配、service/characteristic 发现、notify 订阅与流式读写，为后续自定义 `hal_obd_transport` 复用现有 NimBLE Host 铺路
- 当前已在 `sdkconfig.defaults` 补齐 NimBLE 多角色默认项（central + peripheral + observer + broadcaster + GATT client/server）以及 `esp_obd_ii` 的 ELM327 BLE 缺省参数，避免后续 BLE 真连时被基础配置卡住
- 当前 `0xFF02` BLE Characteristic 已从 `service_navigation` 转发到 `service_obd_submit_packet()`，为后续 ELM327 / 手机桥接复用入口
- 当前 `PAGE_OBD` 已升级为首版专用仪表页：状态 chip、速度/RPM、水温、油门条、油量条、告警文案和统计摘要
- 当前 OBD 超阈值告警已落成最小事件链：`alert_active + alert_text` 状态发布、日志告警，以及在非语音强制页场景下短时抢占 `PAGE_OBD` 后自动回退
- 当前已补 OBD provider 诊断态与页面透出：`WorkflowState.obd` 现可记录 backend、稳定诊断码、阶段、连接尝试/失败/更新计数、扫描/连接/发现三段 BLE 耗时、最近一次失败阶段、最近一次失败 rc 以及最后一条诊断文案；共享 `service_ble_client` 已能区分 `scan_timeout / service_not_found / characteristic_not_found / descriptor_not_found` 等失败来源，并尽量以短摘要格式附带命中的设备名、缺失 UUID 与命中的 handle，`PAGE_OBD` 会进一步标识 `ELM327 MISS / ELM327 GATT / ELM327 FAIL`，并对过长 detail 做展示侧裁剪，避免圆屏布局被诊断字符串挤爆
- 当前虽已把 NimBLE Host 生命周期抽到共享 `service_ble`，并补了 `service_ble_client + service_ble_elm327` 真链路骨架，但仍缺 fresh verification、真实适配器兼容性确认以及 `esp_obd_ii` 原生 transport 是否继续保留的最终收口

交付物：

- OBD 页显示核心指标
- 基础告警可触发

依赖：

- M2
- M5 的 BLE 管理可复用部分

### M7 系统整合

- [x] 统一页面抢占优先级
- [x] 统一音频优先级策略
- [x] 完成电池 ADC 与百分比换算
- [x] 实现 30 秒降背光
- [x] 实现 2 分钟轻睡眠
- [x] 接入 SD 卡日志

- 当前已把页面抢占规则统一收口到 `app_state`：以 `WorkflowState` 为真源，根据 `VOICE > OBD 告警 > NAV > 用户页` 的优先级解析 `current_page`
- 当前 `service_voice / service_navigation / service_obd` 已移除各自维护的恢复页逻辑，改为只发布业务状态；页面恢复统一由中心仲裁处理，避免语音、导航、告警叠加时出现回退错乱
- 当前 `service_ui_show_page()` 已只负责写入用户选中页；当高优先级页面抢占时，手势/按键切页会被锁定，抢占结束后自动回到用户页
- 当前已新增中心 `service_audio`：统一管理 MIC / Speaker 的请求、优先级解析和 I2S 通道开关；首版优先级固定为 `VOICE > OBD > NAV > SYSTEM`
- 当前 `service_voice` 不再直接决定音频通道 enable，而是向 `service_audio` 申报 `wake capture / stream capture / think hold / reply playback` 等策略；System 页现可直接显示当前音频 owner 与策略文案
- 当前已新增 `drivers_power`：按官方 demo 的 `GPIO8 / ADC1_CH7 / ADC_ATTEN_DB_12` 接入电池采样，并复用官方 `*3.0 / 0.990476` 分压修正；`service_power` 每 `2s` 会刷新一次 `battery_percent / battery_voltage_mv`
- 当前已新增 `service_power` 首版骨架：`app_state.last_user_action_ms` 现在会由 BOOT 键和触摸交互更新，服务侧每 `500ms` 评估空闲时长，并在 `30s` 无操作后关闭背光；System 页可直接看到 `Idle / Backlight / Sleep gate`
- 当前降背光策略已附带 `keep awake` 保护：语音会话、导航活跃或 OBD 告警期间不会触发背光关闭，避免关键业务页被电源策略误伤
- 当前 `2 分钟轻睡眠` 已接成真实执行链：`service_power` 会在空闲时间达到 `120s` 后调用 `esp_light_sleep_start()`，并通过 `BOOT 键(GPIO0)` 与 `触摸中断(GPIO4)` 作为首版唤醒源；唤醒后会恢复背光并回写用户活跃时间
- 当前已新增 `service_log`：按板卡 TF 口 `GPIO14/16/17 + EXIO3` 的 SPI 走线挂载 `/sdcard`，并通过 `esp_log_set_vprintf()` 将运行时日志同步追加到 `/sdcard/pathfinder.log`；若挂卡失败则只降级为串口日志，不阻塞主系统启动

交付物：

- 系统页完整
- 电源管理与日志可用

依赖：

- M3
- M4
- M5
- M6

## 3. 第一批代码任务建议

如果下一步直接开始实现，建议只开下面 6 个任务：

1. 初始化 ESP-IDF 工程骨架
2. 建 `board_support` 和引脚配置头文件
3. 接入 LCD 并显示启动页
4. 接入 LVGL 并显示 System 页面
5. 建 `WorkflowState` 和页面枚举
6. 加入 RTC 读取并显示时间

这 6 个任务完成后，项目才具备继续接 IMU/语音/导航/OBD 的稳定底座。

## 4. 风险前置任务

以下事项建议尽早验证，否则后面会被动返工：

- [ ] 微雪板卡官方示例能否直接在 ESP-IDF v5.4+ 下编译
- [x] LCD 与触摸是否已有现成组件或 BSP
- [ ] 麦克风与 PCM5101 的 I2S 配置是否与小智适配板卡一致（当前仅完成工程接线与静态接入，待与小智板卡配置逐项比对）
- [ ] `esp_obd_ii` 与当前 NimBLE 版本是否兼容
- [ ] 导航包最大长度是否会超过当前 BLE MTU

## 5. Definition of Done

每个里程碑完成必须同时满足：

- [ ] 代码编译通过
- [ ] 关键功能有串口日志或屏幕证据
- [ ] 新增模块已接入统一状态模型
- [ ] 无明显阻塞下一里程碑的问题
- [ ] 文档更新到对应阶段结果
