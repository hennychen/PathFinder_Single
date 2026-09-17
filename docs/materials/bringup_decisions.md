# Bring-up 决策记录

## 2026-09-15

### 决策 1：创建项目资料目录

- 结论：创建 `docs/materials/`
- 原因：当前仓库从 0 起步，板卡资料、引脚映射、未决问题如果不集中沉淀，后续接 LCD / Touch / RTC / 音频时会反复查证并增加返工

### 决策 2：P0 优先打通 I2C + RTC

- 结论：优先实现 `board_support` 的 I2C 初始化和 `drivers_pcf85063`
- 原因：RTC 走共享 I2C，总线一旦稳定，后续 QMI8658 与触摸也能复用同一基础设施

### 决策 3：LCD/Touch 先不硬上

- 结论：本轮不直接写 LCD/Touch 真初始化
- 原因：官方文档显示 LCD/Touch reset 依赖 TCA9554 扩展 IO（EXIO1/EXIO2），如果跳过这层直接写屏，后面大概率返工

### 决策 4：LCD/Touch/LVGL 改走官方组件链

- 结论：显示链路采用 `espressif/esp_lcd_spd2010` + `espressif/esp_lcd_touch_spd2010` + `espressif/esp_lvgl_port`
- 原因：比手写 SPD2010 面板寄存器序列更稳，能更快完成 M1/M2 的真实 bring-up，并降低后续 UI 演进时的维护成本

### 决策 5：先补 TCA9554，再落 LVGL System 页面

- 结论：新增 `drivers_tca9554` 与 `drivers_display`，由 `board_support` 统一管理 EXIO reset / backlight / BOOT key
- 原因：这块板的 `LCD_RST=EXIO2`、`TP_RST=EXIO1`、`IMU_INT1=EXIO5`、`IMU_INT2=EXIO4` 都依赖扩展 IO，先把 EXIO 管通，后续触摸、IMU、SD 才不会重复返工

### 决策 6：M3 先落姿态真链路，指南针先用占位航向

- 结论：先新增 `drivers_qmi8658` 与 `service_attitude`，让 `WorkflowState` 开始产出真实 `roll_deg / pitch_deg`；`heading_deg` 第一版先由陀螺仪 Z 轴积分提供占位值
- 原因：当前板上没有磁力计，直接承诺“真北航向”不诚实；先把姿态仪表页做成真实数据链路，能更稳地支撑后续导航页、姿态 UI 和上板验证

### 决策 7：显示缓冲先切到 PSRAM canvas，再等待 rounder 实机验证

- 结论：`drivers_display` 的 LVGL 接入先改为 `PSRAM canvas + SRAM trans buffer`，并在 `sdkconfig.defaults` 中补 `FreeRTOS/LVGL/PSRAM` 推荐默认项；`SPD2010` 的 4 像素对齐 `rounder_cb` 继续保留为待验证项
- 原因：`esp_lvgl_port 2.6.1` 已明确支持 `trans_size` 与 `buff_spiram`，这部分收益确定、兼容面也清晰；但 `rounder_cb` 在当前仓库还没有真实头文件和编译证据，先不盲写字段，避免把显示链路从“性能风险”改成“编译风险”

### 决策 8：先兑现页面刷新频率，再继续追 rounder

- 结论：`service_ui` 先改成按页面类型分配刷新周期，优先保证姿态页达到 `50ms` 刷新节拍（20 FPS）
- 原因：相比 `rounder_cb` 的版本不确定性，页面刷新节拍是当前仓库里明确可控、且直接影响 M3 交付质量的项；先把任务表里已经承诺的“姿态页 20 FPS”兑现掉，更符合当前实施顺序

### 决策 9：M4 先落语音状态机和页面抢占，不先硬接音频链路

- 结论：先新增 `service_voice`，把 `idle / wake_detected / streaming / speaking / error` 状态和 `PAGE_VOICE` 抢占/恢复链路接到 `WorkflowState`
- 原因：语音页切换逻辑属于 UI 和状态总线的公共底座，先落这层可以把后续 `xiaozhi-esp32`、I2S 麦克风、PCM5101 输出的接入范围收窄到驱动和 service 内部，避免再次改页面管理器

### 决策 10：M5 先把导航协议、解析器和页面链路做成 mock 可驱动形态

- 结论：先新增 `service_navigation` 与 `navigation_protocol`，固定 `Service=0xFFF0`、`Navigation Char=0xFF01`、`OBD Char=0xFF02`，并通过 `service_navigation_submit_packet()` / `service_navigation_submit_mock_packet()` 模拟 BLE 收包
- 原因：当前环境缺少 NimBLE / 手机侧联调条件，但导航协议解析、非法包校验、页面自动跳转和 `10s` 超时回退都能先独立兑现；这样后续接真 BLE 时，只需把 Characteristic 写入入口接到现有 service，不需要再返工 UI 和状态总线

### 决策 11：在 `service_navigation` 内直接补最小 NimBLE GATT Server，不额外拆独立 BLE 管理层

- 结论：当前先让 `service_navigation` 直接承接 `PathFinder Nav` advertising、`0xFFF0` service 和 `0xFF01` write characteristic，手机写入后立即投递到导航包队列
- 原因：M5 当前目标是把“手机导航包 -> 协议解析 -> 状态总线 -> 导航页”的主链闭合，而不是先做通用 BLE 抽象层；等进入 M6 OBD 时，再基于现有 `0xFF02` 占位 characteristic 评估是否抽出独立 `service_ble`

### 决策 12：先把 `PAGE_NAV` 升级成专用首版导航页，再考虑更复杂的地图风格 UI

- 结论：当前 `PAGE_NAV` 先采用“状态 chip + 转向标题 + 中心箭头 + 距离/路名/ETA”布局，直接消费 `WorkflowState.navigation`
- 原因：现阶段真实数据链路和页面跳转已经具备，继续停留在文本占位页会拖累 M5 交付质量；而地图/HUD 风格 UI 需要更多资源和真机视觉验证，适合放到后续路测阶段再细化

### 决策 13：M6 先把 NimBLE 切到多角色默认配置，再做 ELM327 真连接

- 结论：在 `sdkconfig.defaults` 中同时开启 `Central + Peripheral + Observer + Broadcaster + GATT Client/Server`，并显式写入 `esp_obd_ii` 的 ELM327 BLE 缺省参数
- 原因：当前项目既要保留 `service_navigation` 的 `PathFinder Nav` GATT Server，又要让 `esp_obd_ii` 作为 BLE Central 去找 ELM327；如果不先把多角色配置固化，后续真机联调会先被基础蓝牙角色开关卡住

### 决策 14：在共享 NimBLE Host 未收口前，先禁用 `esp_obd_ii` provider 的真 BLE 打开路径

- 结论：当前不再直接尝试打开 `esp_obd_ii` 自带的 BLE transport；共享 `service_ble` Host 活跃时，先走本仓库内的 `service_ble_elm327` provider，并继续保留 `0xFF02` bridge / mock 数据入口
- 原因：`esp_obd_ii` 的 BLE transport 当前会自行执行 `nimble_port_init()` 与 `nimble_port_freertos_init()`；若与现有导航 bridge 直接并存，属于双 Host 初始化冲突。先用本地 provider 绕开这层冲突，把真链路最小闭环跑通，再决定是否要继续收口外部组件源码

### 决策 15：先抽 `service_ble`，把 NimBLE Host 生命周期从 `service_navigation` 中移出去

- 结论：新增共享 `service_ble` 组件，统一承接 NimBLE Host 初始化、GATT table 注册、advertising 与手机连接状态回写；`service_navigation` 与 `service_obd` 仅通过 bridge 回调接收 `0xFF01 / 0xFF02` 写入
- 原因：当前最大的结构性问题不是导航/OBD 哪个 service 先连真链路，而是“谁拥有 BLE Host”。先把 Host 生命周期抽成独立组件，后续不论是让 `esp_obd_ii` 复用现有 Host，还是再抽更底层 `hal_obd_transport` 适配，改动面都会显著收窄

### 决策 16：先把 OBD BLE 参数抽成 adapter profile，再等 `Vgate iCar Pro BLE 4.0` 实物确认最终参数

- 结论：在 `service_obd` 内新增 `obd_adapter_profile` 真源，内置 `generic_elm327_ble` 与 `vgate_icar_pro_ble` 两套已知模板，并保留 `active_sdkconfig` 作为当前实际生效配置
- 原因：当前项目可以继续推进 UI、状态机和共享 BLE Host 收口，但 `Vgate iCar Pro BLE 4.0` 的广告名/UUID 仍需以真机 BLE 扫描结果为准。先把 profile 边界固定住，后续切设备只需替换配置，不必再回头拆业务代码

### 决策 17：先补共享 Host 上的 `service_ble_client`，再决定如何接管 `esp_obd_ii` transport

- 结论：在 `service_ble` 中新增独立 `service_ble_client` 能力，统一提供扫描、广告名匹配、service/characteristic 发现、notify 订阅与流式读写接口，但本轮先不直接篡改 `esp_obd_ii` 的内部 port 源文件
- 原因：`esp_obd_ii` 当前会把自带的 BLE transport 源直接编入组件，贸然硬接容易把问题从“共享 Host 未复用”扩大成“外部组件源码分叉”。先把共享 Host 上的 client 接缝收口，后续不论是自定义 `hal_obd_transport` 还是替换 factory，都会更可控

### 决策 18：先在本仓库内补 `service_ble_elm327` 最小 provider，绕开 `esp_obd_ii` 原生 BLE transport 的双 Host 冲突

- 结论：当前 `service_obd` 在共享 `service_ble` Host 活跃时，优先选择本地 `service_ble_elm327` provider；它直接复用 `service_ble_client` 完成 BLE 扫描、AT 初始化和基础 PID 轮询解析，而不是直接打开 `esp_obd_ii` 自带的 BLE transport
- 原因：这样能先把 RPM / 车速 / 水温等真链路关键路径跑通，同时避免现在就分叉外部组件源码。后续若需要保留 `esp_obd_ii` 的解析层或 factory 机制，再在已有真链路证据基础上做更小范围的收口

### 决策 19：在没有 `Vgate iCar` 实物时，优先补 OBD provider 观测性而不是继续猜参数

- 结论：先把 OBD provider 的 backend、稳定诊断码、阶段、尝试次数、失败次数、更新次数和最后一条诊断文案统一发布到 `WorkflowState.obd`，并在 `PAGE_OBD` 直接显示
- 原因：当前缺的是 fresh verification 和设备侧证据，不是更多拍脑袋的 UUID/AT 猜测。先把“卡在扫描、连接、初始化还是 PID 轮询”这件事做成可见事实，后续真机插上后联调效率会高很多

### 决策 20：进一步细分共享 BLE client 的失败来源，先把“没扫到设备”和“GATT 不匹配”区分开

- 结论：在 `service_ble_client` 内新增稳定失败码与诊断文案，至少区分 `scan_timeout / connect_fail / service_not_found / characteristic_not_found / descriptor_not_found / subscribe_fail`
- 原因：对当前阶段来说，最影响联调效率的不是更多功能，而是看到失败后能立刻判断该去查广播名、UUID 还是 notify/descriptor。先把失败来源拆细，并尽量保留命中的设备名与缺失 UUID，后续插上真实适配器时能更快命中正确方向

### 决策 21：OBD 诊断 detail 统一收成短摘要格式，页面展示再额外裁剪

- 结论：共享 `service_ble_client` 与 `service_obd` 继续保留“设备名 / 缺失 UUID / handle”等关键诊断信息，但 detail 统一收成长度受控的短摘要；`PAGE_OBD` 在展示层再做一层裁剪，避免圆屏上的多行拥挤
- 原因：当前阶段最重要的是无设备联调时的可观测性，但 412x412 圆屏的可读性也必须顾及。把完整信息留在状态总线和日志里，把页面文案收成更紧的摘要，能同时兼顾联调效率与 UI 稳定性

### 决策 22：把 BLE 连接过程拆成 Scan / Connect / Discovery 三段耗时

- 结论：在共享 `service_ble_client` 诊断中新增扫描、连接、发现三段耗时字段，并由 `service_obd -> WorkflowState.obd -> PAGE_OBD` 原样透传
- 原因：当前无设备阶段最缺的是“失败前到底卡了多久”的证据。只看状态码能定位方向，但还看不出是扫描太久、连接建立慢，还是 GATT 发现卡住；把三段耗时补齐后，后续接入 `Vgate iCar Pro BLE 4.0` 时能更快判断问题落点

### 决策 23：把最近一次 BLE 失败阶段与 rc 作为独立字段透出

- 结论：在共享 `service_ble_client` 中新增 `last_failure_stage / last_failure_rc` 语义，并由 `service_obd -> WorkflowState.obd -> PAGE_OBD` 独立展示，不再只混在 `detail` 文案中
- 原因：当前 detail 更适合放“人读的摘要”，但联调时还需要快速看到结构化的失败定位信息。把最近一次失败阶段和 rc 拆成独立字段后，页面、日志与后续串口证据可以直接按同一语义对齐

### 决策 24：M7 先统一页面仲裁，不再让语音/导航/OBD 各自维护恢复页

- 结论：把页面抢占优先级统一收口到 `app_state`，由中心状态层按 `VOICE > OBD 告警 > NAV > 用户页` 解析 `current_page`；`service_voice`、`service_navigation`、`service_obd` 仅发布业务状态，不再各自维护 `resume_page` / `forced_page`
- 原因：当前页面恢复逻辑分散在多个 service 中，语音抢占、导航新包和 OBD 告警叠加时容易出现恢复顺序不一致。先把仲裁收成单一真源，可以在不依赖新硬件的前提下完成 M7 的第一项系统整合，并为后续音频优先级、电源管理和日志接入保留更稳定的状态边界

### 决策 25：M7 再统一音频仲裁，不再让业务 service 直接决定 I2S 通道开关

- 结论：新增中心 `service_audio`，统一管理各业务模块的 MIC / Speaker 请求、优先级和 `drivers_audio` 通道开关；首版优先级固定为 `VOICE > OBD > NAV > SYSTEM`
- 原因：当前只有 `service_voice` 直接驱动音频，但后续导航播报、OBD 提示音和系统提示一旦接入，就会出现多个模块各自开关 I2S 的结构性冲突。先把音频占用真源收口到系统层，可以让后续新增播报能力时只需要“声明请求”，而不必回头改底层驱动控制路径

### 决策 26：M7 先把空闲策略接成“降背光 + 休眠准入”，暂不立刻执行真实 light sleep

- 状态：已被决策 28 的实现阶段覆盖
- 结论：新增 `service_power` 首版骨架，统一根据 `WorkflowState.last_user_action_ms` 评估空闲时长；达到 `30s` 先关闭背光，达到 `120s` 先只标记 `light_sleep_ready`
- 原因：当前仓库还没有完整的唤醒源、SD 日志 flush 和语音/BLE 恢复策略，直接硬接 `esp_light_sleep_start()` 风险太高。先把空闲时间真源、背光控制和“是否具备入睡资格”这三件事做成稳定状态，再进入真正的轻睡眠接入，返工面会小很多

### 决策 27：电池采样先贴官方 `BAT_Driver` 口径接入，再把百分比曲线留在仓库内可调

- 结论：新增 `drivers_power`，按官方 demo 的 `GPIO8 / ADC1_CH7 / ADC_ATTEN_DB_12`、`*3.0 / 0.990476` 分压修正接入电池采样；电量百分比先采用仓库内的 Li-ion 分段近似曲线
- 原因：当前最重要的是把 `battery_percent` 从占位值变成可观测的真实链路，而不是过早追求某一块电芯的绝对精度。先贴官方真源把 ADC 和分压系数接准，后续若实测发现百分比曲线偏差，再只调整仓库内映射函数即可，不需要回头改底层采样驱动

### 决策 28：轻睡眠先以 GPIO 唤醒闭环为主，不额外引入更重的恢复编排

- 结论：`service_power` 当前在空闲达到 `120s` 后直接执行 `esp_light_sleep_start()`，首版仅使用 `BOOT(GPIO0)` 与 `Touch INT(GPIO4)` 作为唤醒源；唤醒后立即恢复背光并刷新用户活跃时间
- 原因：当前阶段最重要的是把“能睡、能醒、醒后继续交互”的主链闭合，而不是一次性把所有潜在唤醒源和复杂恢复策略都拉进来。先用已有输入源形成最小闭环，可以在不扩大改动面的前提下完成 M7 的轻睡眠能力，后续再视 SD 日志 flush、BLE/语音恢复实测情况补更细的睡眠前后编排

### 决策 29：SD 日志先采用板载 TF 的最小 SPI 挂载，不为首版引入额外日志队列

- 结论：新增 `service_log`，按板卡真源的 `SCK=GPIO14 / MISO=GPIO16 / MOSI=GPIO17 / CS=EXIO3` 挂载 `/sdcard`，并通过 `esp_log_set_vprintf()` 将运行时日志同步追加到 `/sdcard/pathfinder.log`；挂卡失败时只降级为串口日志，不阻塞系统启动
- 原因：M7 当前缺的是“日志可落卡”的最小闭环，而不是高吞吐日志基础设施。先复用现有 `APP_LOG* / ESP_LOG*` 入口，把 TF 卡挂载与文件追加链路做通，就能在后续真实上板和 BLE/OBD 联调时直接获得持久化证据；是否要继续演进为异步缓冲、按 RTC 切片或睡眠前显式 flush，适合等具备硬件验证后再做

### 决策 30：中文路名先在协议层做 UTF-8 安全裁剪，不把字符边界处理散落到 UI 和 mock 入口

- 结论：`navigation_protocol` 统一负责路名的 UTF-8 边界处理；无论是手机侧 BLE 收包还是 `service_navigation_submit_mock_packet()` 构造导航包，都会在 `32B` 限额内回退到完整字符边界
- 原因：当前最容易在真机联调时出现的问题，不是“长度超了”本身，而是中文 UTF-8 在边界处被硬截断后把 UI 和日志都弄成半个字。先把字符边界规则收口到协议层，可以避免 service/UI 各自再写一套裁剪逻辑，后续即使调整路名预算，也只需要改一处真源

### 决策 31：导航 BLE 写入按当前 ATT MTU 做预算保护，不默认假设手机侧已协商大 MTU

- 结论：共享 `service_ble` 在 bridge 层按“当前 ATT MTU - 3”计算单次可写 payload 预算；默认新连接先按 `20B` 接收导航载荷，只有收到 `BLE_GAP_EVENT_MTU` 后才放宽到更大的导航包
- 原因：当前导航协议全包最大 `40B`，而 BLE 默认 ATT MTU 只有 `23`。如果先默认手机会协商大 MTU，真机联调时最先出现的就会是“长路名偶发写不进去”这种不稳定现象。先把预算保护收在 bridge 层，可以把问题稳定地暴露成结构化日志，再决定手机侧是否必须主动做 MTU exchange

### 决策 32：多包退路先做成兼容型分片重组，不直接改掉现有单包导航协议

- 结论：`service_navigation` 当前支持一种保留标记分片帧：`0xFF + message_id + chunk_index + chunk_count + chunk_len + chunk_data`。短包继续走原有单包协议；只有当手机侧未协商到足够 MTU 时，才需要发送该分片帧，由设备侧在 `1s` 窗口内按顺序重组
- 原因：现有单包协议已经被 mock、UI 和解析链路消费，直接整体替换成新协议会扩大回归面。先把分片做成桥接层兼容扩展，可以在不破坏已有单包链路的前提下，为默认 `20B` payload 预算提供明确退路，后续手机侧也更容易按能力决定“直接单包”还是“保守分片”

### 决策 33：手机侧桥接协议单独成文，并提供仓库内分片编码 helper 作为发送真源

- 结论：新增 `docs/materials/navigation_bridge_protocol.md` 作为手机侧 BLE 导航桥对接文档，同时在 `navigation_protocol` 中补 `navigation_protocol_encode_fragment_frames()`，让外部桥接脚本和后续高德适配层可以直接复用仓库内的单包/分片编码规则
- 原因：到这一步为止，设备侧已经有单包解析、UTF-8 裁剪、MTU 预算保护和分片重组能力，如果手机侧仍靠口口相传或手抄注释来实现发送逻辑，后续联调还是容易漂。把对接规范和编码 helper 一起补齐，能让“手机该怎么发”也拥有同级别真源，减少桥接侧二次发明协议的空间

### 决策 34：桌面侧先补最小发送原型，而不是等手机桥整体方案确定后再动手

- 结论：新增 `tools/navigation_bridge/bridge_demo.py` 与配套 `README.md`，先提供一个能打印单包/分片十六进制帧、并在安装 `bleak` 后可直接向 `0xFF01` 写入的最小发送端原型
- 原因：当前导航桥接最缺的已经不是协议字段定义，而是“有没有一个现成发送端可以马上拿来试”。先把桌面原型放进仓库，后续无论手机桥最终选 Android、iOS 还是桌面中转，团队都已经有一份可执行的发送真源，不必每次从零再写一遍编码逻辑

### 决策 35：桥接层先按“地图厂商无关 adapter”分层，而不是直接把高德事件结构写死到发送脚本里

- 结论：新增 `tools/navigation_bridge/map_adapters.py`，把上游地图事件先归一化成统一导航事件，再由 `bridge_demo.py` 编码成 NavigationPacket；同时补 `examples/generic|amap|baidu|tencent_event.json` 作为各 provider 的最小样例
- 原因：设备端协议本身只关心 `turn_type / step_distance / total_distance / remain_time / road_name`，真正存在厂商差异的是手机侧事件字段名和转向枚举。如果一开始就把高德事件对象写死进发送脚本，后面接其它地图时桥接层很快会分叉。先把 provider adapter 层独立出来，可以把“是否兼容其它地图”的代价收敛成新增或修正一个 adapter，而不是重写整条 BLE 发送链路

### 决策 36：桥接层先补仓库内回归脚本，把 provider 归一化和分片选择固定成可重复验证的基线

- 结论：新增 `tools/navigation_bridge/verify_adapters.py`，用仓库内 `examples/*.json` 样本逐个校验 provider 归一化结果、默认 `20B` ATT 预算下的单包/分片选择，并额外覆盖长中文路名样本
- 原因：到这一步为止，桥接层已经开始承接不同地图厂商的字段差异，如果没有一组最小回归检查，后续只要改动某家的字段别名或转向映射，就很容易无意中影响另一家。先把验证脚本补进仓库，可以在没有真机、没有手机 SDK 的环境里，仍然对桥接逻辑保有一份可重复执行的“新鲜验证证据”。这轮验证还额外纠正了一个关键口径：默认 `20B` ATT payload 下，最大 `40B` 导航包最多会拆成 `3` 片，而不是之前口头估计的 `2` 片

### 决策 37：高德先按官方 `NaviInfo` 回调字段对齐，而不是长期停留在仓库自定义扁平 JSON

- 结论：`tools/navigation_bridge/map_adapters.py` 的 `amap` 入口当前优先读取 `payload.naviInfo`，并接入 `iconType / segmentRemainDistance / routeRemainDistance / routeRemainTime / nextRoadName / currentRoadName` 这些更接近官方 `onNaviInfoUpdate(NaviInfo)` 的字段；同时补了嵌套回调样本和到达态样本
- 原因：前一阶段的扁平 JSON 更像桥接层内部测试数据，虽然方便，但它会把“字段是不是来自真实 SDK”这件事遮住。既然下一步目标是把高德从原型推进到可落地对接，就应尽早让 adapter 面对更真实的回调结构，把不确定性尽量前移并收敛在桥接层
