# ESP32-S3-Touch-LCD-1.46 板卡真源摘录

## 1. 官方真源

- 官方文档主页：
  - `https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.46`

## 2. 与当前实现直接相关的硬件事实

### 2.1 主板资源

- 主控：ESP32-S3R8
- 屏幕分辨率：412 x 412
- 显示控制器：SPD2010
- 触摸控制器：SPD2010
- IMU：QMI8658
- RTC：PCF85063
- 音频 DAC：PCM5101
- GPIO 扩展：TCA9554PWR

### 2.2 共享 I2C 总线

- I2C SCL：GPIO10
- I2C SDA：GPIO11
- 触摸、QMI8658、PCF85063 共用这组引脚

### 2.3 LCD / Touch 连接

- LCD_SDA0：GPIO46
- LCD_SDA1：GPIO45
- LCD_SDA2：GPIO42
- LCD_SDA3：GPIO41
- LCD_SCK：GPIO40
- LCD_CS：GPIO21
- LCD_TE：GPIO18
- LCD_BL：GPIO5
- LCD_RST：EXIO2
- TP_SDA：GPIO11
- TP_SCL：GPIO10
- TP_INT：GPIO4
- TP_RST：EXIO1

### 2.4 RTC / IMU / 音频连接

- RTC_SCL：GPIO10
- RTC_SDA：GPIO11
- RTC_INT：GPIO9
- IMU_INT1：EXIO5
- IMU_INT2：EXIO4
- SD_SCK：GPIO14
- SD_MISO / D0：GPIO16
- SD_MOSI / CMD：GPIO17
- SD_CS / D3：EXIO3
- BAT_ADC：GPIO8
- MIC_WS：GPIO2
- MIC_SCK：GPIO15
- MIC_SD：GPIO39
- Speak_DIN：GPIO47
- Speak_LRCK：GPIO38
- Speak_BCK：GPIO48

## 3. 当前工程决策

- `board_support` 负责沉淀官方板级引脚与共享总线初始化
- `drivers_tca9554` 负责 EXIO 控制，当前实现按 `0x20` 地址接入，待真实上板再次确认
- `drivers_display` 使用 `esp_lcd_spd2010 + esp_lcd_touch_spd2010 + esp_lvgl_port` 串起 LCD/Touch/LVGL
- `drivers_display` 当前已切换到 `PSRAM canvas + SRAM trans buffer` 策略：`buffer_size = 412 * 412`，`trans_size = 412 * 32`，`buff_spiram = true`
- `drivers_qmi8658` 当前已按板级原理图真源收口到 `0x6B` 地址接入（QMI8658C `SDO/SAO` 上拉到 `VCC3V3`）；上板最新证据显示 `WHO_AM_I` 与 `0x00..0x08` 寄存器窗口仍读成全 `0x00`，初始化尚未成功，配置目标仍为 `CTRL1=0x40`、`CTRL2=0x15`（`+-4g @ 250Hz`）、`CTRL3=0x55`（`+-512dps @ 250Hz`）、`CTRL7=0x03`
- `service_attitude` 以 250Hz 采样 QMI8658，做互补滤波和一阶低通滤波，并把 `roll_deg / pitch_deg / heading_deg` 投递到 `WorkflowState`
- `service_ui` 当前按页面类型使用差异化刷新周期：姿态页 `50ms`、指南针 `100ms`、导航/OBD `200ms`、System `500ms`
- `service_voice` 当前已提供 `idle / wake_detected / streaming / speaking / error` 状态机；页面抢占已统一收口到 `app_state`，由状态仲裁层按 `VOICE > OBD 告警 > NAV > 用户页` 规则解析当前页
- `drivers_audio` 当前已按 ESP-IDF v5.4 `driver/i2s_std.h` 新接口接入板载 MIC / PCM5101：MIC 先按 `16kHz / 32bit / mono` 初始化，PCM5101 先按 `24kHz / 16bit / stereo` 初始化
- `drivers_power` 当前已按官方 demo 的 `GPIO8 / ADC1_CH7 / ADC_ATTEN_DB_12` 接入电池采样，并沿用官方 `*3.0 / 0.990476` 分压修正口径；当前百分比采用首版 Li-ion 分段近似曲线
- `service_log` 当前已按板载 TF 的 SPI 走线 `GPIO14/16/17 + EXIO3` 接入 `/sdcard` 挂载，并把 ESP-IDF 运行时日志同步追加到 `/sdcard/pathfinder.log`
- `service_audio` 当前已作为系统级音频仲裁层落地：统一管理 MIC / Speaker 请求、优先级与 I2S 通道开关，首版优先级固定为 `voice > obd > navigation > system`
- `service_power` 当前已作为系统级电源策略骨架落地：基于 `last_user_action_ms` 评估空闲时长，`30min` 无操作后关闭背光，`60min` 后允许进入 `light sleep`；当前为 bring-up 放宽版阈值（原设计目标 `30s / 120s`，待整机联调后再收紧）；首版唤醒源固定为 `BOOT(GPIO0)` 与 `Touch INT(GPIO4)`
- `voice_backend` 当前作为 `xiaozhi-esp32` 的独立适配层落在仓库内，接入策略固定为“组件化引入”，当前占位实现先提供后端信息与状态回调骨架
- `service_voice_init()` 当前会串起 `drivers_audio_init()` 以及输入/输出通道 enable，作为后续集成 `xiaozhi-esp32` 的最小音频底座
- `service_voice` 当前已创建 `task_voice`（Core 1 / Priority 6）与命令队列，状态变更统一走异步命令入口，再由后端回调映射回 `WorkflowState`
- `drivers_audio` 当前已补 `read_input / write_output / write_silence` 最小帧接口，`task_voice` 会按状态执行 capture/playback tick 并记录会话日志
- `voice_backend` 当前已补 `start_session / stop_session / process_audio` 最小 runtime hook，`task_voice` 已改为通过 backend 决定输入消耗与播放输出
- `app_state` 当前已补 `voice_runtime` 字段，`service_voice` 会把 state/session/duration/input/output、阶段停留时长、输入/输出帧计数、状态切换次数，以及输入/输出文本发布到状态总线，`PAGE_VOICE` 直接展示这些字段
- `service_ui` 当前已把 `PAGE_VOICE` 升级为增强版首屏语音页：状态 chip、相位文案、中心活动 orb、表情词、小波条、输入/输出活动条、双气泡提示、阶段时间线、动态摘要/速率统计区，以及阶段停留/帧计数说明；双气泡已优先直连状态总线文本
- 当前指南针页的 `heading_deg` 先使用陀螺仪 Z 轴积分做占位，尚不具备真北参考
- P0/P1 已从“只拉起共享 I2C 与 RTC 读时钟能力”推进到“RTC 读写、LCD 测试图、LVGL System 页面、手势/按键切页”的代码接入阶段，当前已进入 M3 姿态链路代码接入阶段

## 4. 未决问题

- SPD2010 刷新区域要求 `x_start/x_end` 满足 4 像素对齐，当前 LVGL 接入尚未做 rounder 回调，上板后需重点验证
- TCA9554 的 I2C 地址当前按 `0x20` 实现，仍需结合官方示例或实机 I2C scan 复核
- 触摸 INT 的有效电平当前按低电平中断处理，需在真实硬件上确认
- RTC 首次 seed 时间当前写入固定 bring-up 默认值，后续仍需由 NTP / 手机同步策略接管
- `sdkconfig.defaults` 当前已补 `CONFIG_FREERTOS_HZ=1000`、`CONFIG_SPIRAM_USE_MALLOC=y` 和 LVGL `clib` / refresh 相关默认项，待具备 `idf.py` 后需要做一次实际配置生效验证
- `service_attitude` 当前用 `esp_timer + esp_rom_delay_us` 维持 250Hz 采样节拍；后续若补 `CONFIG_FREERTOS_HZ=1000`，应再复核任务调度与 CPU 占用
- `drivers_audio` 当前已完成最小音频帧级读写与静音填充，但尚未补缓冲策略、格式转换和 AFE/TTS 真数据链路
- `voice_backend` 当前仍是占位实现，尚未替换为真实 `xiaozhi-esp32` 组件接线；但已补脚本化 mock 会话流转，可自动经历 streaming / think silence / mock speak / idle
- `task_voice` 当前已通过 backend hook 串起最小帧级处理，但 backend 仍未接 ESP-SR AFE、唤醒词、编码/解码和对话音频流
- `PAGE_VOICE` 当前虽已具备增强版首屏语音页，并新增输入/输出双气泡提示、阶段时间线、动态摘要、阶段停留/帧计数说明和状态总线文本消费，但仍未切到更完整的语音表情 / 波形 / 对话气泡 UI
- `service_navigation` 当前已落地 `task_navigation`（Core 0 / Priority 3）与包队列，协议入口固定为 `service_navigation_submit_packet()`，后续 BLE Characteristic 写入时直接复用
- 导航协议当前固定为 `Service=0xFFF0`、`Navigation Char=0xFF01`、`OBD Char=0xFF02`，载荷为 `turn + step_distance + total_distance + remain_time + road_name_len + road_name`
- `WorkflowState` 当前已补 `navigation` 运行态，`PAGE_NAV` 已升级为首版专用导航页：状态 chip、转向标题、中心箭头、下一步距离、路名、提示文案，以及总距离/ETA/包统计
- 导航新包当前会在非语音抢占场景下自动切到 `PAGE_NAV`，若语音页正在占用则先暂存，待语音结束后补跳转；`10s` 无新包会自动回退前一页
- 当前已新增共享 `service_ble` 组件承接 NimBLE Host、GATT table 和 advertising：设备名 `PathFinder Nav`，导航特征值 `0xFF01` 收到写入后会通过回调投递到现有导航包队列；`0xFF02` 会转发到 `service_obd_submit_packet()` 作为 OBD 载荷入口
- `WorkflowState` 当前已补 `obd` 运行态，`PAGE_OBD` 已升级为首版专用仪表页：状态 chip、速度/RPM、水温、油门条、油量条、告警文案和统计摘要
- `service_obd` 当前已补最小告警页抢占逻辑：当水温 / RPM / 车速超过阈值时，会发布 `alert_active` 与文案，并在非语音强制页场景下短时切到 `PAGE_OBD`，告警清除后自动回退
- `service_obd` 当前已接入 `esp_obd_ii` provider 骨架，并补了最小 round-robin PID 调度；当 `0xFF02` bridge / mock 输入近期活跃时，provider 会让出数据源优先级，避免和现有手机桥接链路打架
- `service_obd` 当前已补 `obd_adapter_profile` 真源：内置 `generic_elm327_ble` 与 `vgate_icar_pro_ble` 两套已知 BLE 参数模板，并保留 `active_sdkconfig` 作为运行时实际生效配置
- `service_ble` 当前已补独立 `service_ble_client` 基础能力：可在共享 NimBLE Host 上执行扫描、广告名匹配、service/characteristic 发现、notify 订阅与流式读写，供后续自定义 ELM327 BLE transport 复用
- `service_obd` 当前已补 `service_ble_elm327` 最小 provider：复用共享 `service_ble_client` 发送 `ATZ/ATE0/ATL0/ATS0/ATH0/ATAT0/ATSP0` 初始化序列，并轮询 `010C/010D/0105/0111/012F` 解析 RPM / 车速 / 水温 / 油门 / 油量
- `service_obd` 当前已补 provider 诊断态透出：`WorkflowState.obd` 会同步 backend 名称、稳定诊断码、阶段、尝试/失败/更新计数、扫描/连接/发现三段 BLE 耗时、最近一次失败阶段、最近一次失败 rc 和最后一条诊断文案；共享 `service_ble_client` 还能进一步区分 `scan_timeout` 与 `service/characteristic/descriptor not found`，并尽量以短摘要格式附带命中的设备名、缺失 UUID 与命中的 handle，`PAGE_OBD` 可直接显示当前是“没扫到设备”还是“GATT 不匹配”，同时会对过长 detail 做展示侧裁剪
- `sdkconfig.defaults` 当前已切到 NimBLE 多角色配置：保留导航页所需的 Peripheral/GATT Server，同时为 ELM327 BLE 链路打开 Central/Observer/GATT Client，并写入 `esp_obd_ii` 缺省 UUID 与设备名过滤项
- `esp_obd_ii` 的 BLE transport 当前仍会自行 `nimble_port_init()` / `nimble_port_freertos_init()`；为避免双 Host 初始化，当前 `service_obd` 在共享 Host 活跃时优先走本地 `service_ble_elm327` provider，而不是直接打开 `esp_obd_ii` 的原生 BLE transport
- `WorkflowState.ble_phone_connected` 当前已开始由 BLE GAP connect / disconnect 事件驱动，System 页可直接显示手机连接状态
- `heading_deg` 仅用于 M3 UI 首版占位，后续需结合导航方向或其他参考源重新定义“航向”语义
- 当前导航已进入“协议解析 + 页面链路 + 最小 NimBLE GATT Server”阶段，但仍未完成手机侧高德事件桥接
- 当前虽已补最小 NimBLE GATT Server，但还未完成手机侧高德事件桥接和多包策略联调；中文路名的 UTF-8 安全裁剪已在协议层落地，真包联调时仍需复核展示效果
- 导航协议虽已限制 `road_name_len <= 32`，并在仓库内补了 UTF-8 安全边界裁剪；共享 `service_ble` 也已按当前 ATT MTU 做 payload 预算保护（默认仅 `20B`），同时 `service_navigation` 已补 `0xFF` 分片帧重组退路，但真实手机端是否会主动协商到足够 MTU、以及分片是否需要真正启用，仍需联调确认
