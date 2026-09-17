# 当前阶段审计

更新时间：2026-09-17（第二版，纳入真机验证进展；第一版结论"缺 idf.py、无上板证据"已过时）

## 0. 2026-09-17 审计更新（增量）

第一版审计（09-16）后，项目完成了 ESP-IDF 环境搭建、fresh build、以及 12 轮真机串口采集（`logs/qmi-run1..5.log`、`logs/normal-run1..4.log`、`logs/spd-run1..3.log`）：

- **QMI8658 两大根因已修复并真机验证**（`debug-qmi8658-min-bringup.md`，CLOSED）：
  - RC1 `trans_queue_depth=4→0`（IDF6 实验性异步 I2C 路径挂死+假数据）
  - RC2 attitude 忙等饿死 app_main（`vTaskDelay` 阻塞等待修复）
- **SPD2010 触摸三根因已修复**（`debug-spd2010-touch.md`，RESOLVED-PENDING-FINGER-TEST）：INT 上拉恢复、rm_device 重试、LVGL 容错读
- **P0 最小准入事实通过**：启动零复位、LCD 创建、RTC 时间戳、UI ready（`logs/normal-run4.log` 40s）
- 两份早期 OPEN 调试文档已标记 CLOSED-SUPERSEDED（被上述根因解释）
- 验证证据索引见 `docs/materials/verification/README.md`

当前阻塞项已从"缺构建环境"变为：
1. 触摸真人手指测试（P1 收口最后一项，需一次采集）
2. ELM327 实物（OBD 真链路唯一缺口，需硬件）
3. 手机导航 BLE 真包（需手机或桌面 bleak 桥联调）
4. 语音真组件接入（xiaozhi-esp32，纯软件工作）
5. PCF85063 低功耗复核 + 轻睡眠恢复验证

以下为第一版审计正文（里程碑代码状态部分仍然有效，构建/真机状态列以第 0 节更新为准）。

## 1. 审计结论

按仓库代码真源看，当前项目已经进入 **M7 系统整合阶段**，并且大部分系统级能力已完成静态接入：

- 中心页面仲裁已收口
- 中心音频优先级已收口
- 电池 ADC、30 秒降背光、120 秒轻睡眠已接入
- 共享 BLE bridge、导航分片、OBD provider 骨架已接入
- SD 卡日志已接入

但按构建与手测准入口径看，项目 **尚未通过 P0 手测准入**：

- 当前开发机缺少 `idf.py`
- 没有 fresh build 证据
- 没有新的上板启动、屏幕、触摸、RTC、BLE、OBD、轻睡眠实测证据

因此当前最准确的阶段判断是：

1. **代码接入阶段**：`M7` 基本完成
2. **验收阶段**：仍处于 `P0 未通过`，整体属于“代码闭环已形成，但缺构建/上板验证”

## 2. 审计口径

本审计把每个里程碑拆成四个视角：

- `代码状态`：仓库中是否已存在对应实现
- `构建状态`：是否有 fresh build 证据
- `真机状态`：是否有 fresh 上板/联调证据
- `明确缺口`：阻止里程碑真正关闭的问题

说明：

- 当前仓库 `docs/vehicle_dashboard_voice_tasks.md` 的勾选，更接近“代码已接入仓库”
- 当前 `docs/vehicle_dashboard_voice_checklist.md` 和 Definition of Done，更接近“是否达到可验收状态”

## 3. 里程碑审计

| 里程碑 | 代码状态 | 构建状态 | 真机状态 | 判断 |
| :--- | :--- | :--- | :--- | :--- |
| `M0` 仓库初始化 | 已完成 | 未验证 | 不适用 | 代码完成 |
| `M1` 板级 bring-up | 已接入 | 未验证 | 无 fresh 证据 | 代码完成，验收未完成 |
| `M2` UI 框架 | 已接入 | 未验证 | 无 fresh 证据 | 代码完成，验收未完成 |
| `M3` 姿态模块 | 已接入 | 未验证 | 无 fresh 证据 | 代码完成，验收未完成 |
| `M4` 语音框架 | 已接入 | 未验证 | 无 fresh 证据 | 代码完成，验收未完成 |
| `M5` BLE 导航 | 已深度接入 | 未验证 | 无手机真包 fresh 证据 | 代码完成，联调未完成 |
| `M6` OBD 接入 | 已有真链路骨架 | 未验证 | 无 ELM327 fresh 证据 | 架构完成，真接入未收口 |
| `M7` 系统整合 | 已接入 | 未验证 | 无 fresh 证据 | 代码阶段已到位 |

## 4. 关键代码证据

### 4.1 已达到 M7 的直接证据

主启动链路已经把系统级模块全部串起：

- `main/app_main.c`
  - `service_log_init()`
  - `service_attitude_init()`
  - `service_audio_init()`
  - `service_voice_init()`
  - `service_navigation_init()`
  - `service_ble_init()`
  - `service_obd_init()`
  - `service_ui_init()`
  - `service_power_init()`

这说明当前代码已经不是单模块 bring-up，而是整机系统收口。

### 4.2 页面仲裁已中心化

`components/app_state/app_state.c` 当前已根据 override 统一解析页面：

- `NAVIGATION`
- `OBD_ALERT`
- `VOICE`

当前页面优先级已经是：

- `VOICE > OBD alert > NAV > user page`

这属于典型的 `M7` 系统级整合工作，不属于早期模块接入。

### 4.3 音频优先级已中心化

`components/service_audio/service_audio.c` 已存在统一 owner priority：

- `VOICE`
- `OBD`
- `NAVIGATION`
- `SYSTEM`

`service_voice` 已改为向 `service_audio` 申报请求，而不是自己直接控制 MIC / Speaker。

### 4.4 电源管理已是真实现

`components/service_power/service_power.c` 当前已落地：

- `30s` 无操作降背光
- `120s` 无操作轻睡眠
- `esp_light_sleep_start()`

`components/drivers_power/drivers_power.c` 也已具备电池电压采样与百分比换算。

### 4.5 SD 日志已接入

`components/service_log/service_log.c` 当前已实现：

- `/sdcard` 挂载
- `/sdcard/pathfinder.log` 文件
- `esp_log_set_vprintf()` tee 到 TF 卡

这说明 `M7` 最后一项系统日志能力已进入仓库。

### 4.6 导航链路已超过首版接入

当前导航侧不只是“能解析包并切页”，还已经具备：

- UTF-8 安全路名裁剪
- BLE ATT payload 预算保护
- 分片帧重组
- 分片编码 helper
- 桌面桥接发送原型
- 地图厂商无关 adapter
- 高德 `NaviInfo` 对齐样本和本地回归脚本

因此当前导航代码成熟度明显高于 `M5 初接入`。

### 4.7 OBD 已有真链路骨架，但仍未完成验收收口

当前 OBD 侧已经具备：

- `obd_provider_esp_obd_ii`
- `obd_provider_service_ble_elm327`
- `service_ble_client`
- OBD 诊断态、告警态、页面透出

但还缺：

- `ELM327` 实物连接 fresh 证据
- RPM / 车速 / 水温真值读取 fresh 证据
- 真机兼容性结论

所以 `M6` 的准确描述应是：

- **架构与静态接入已完成**
- **真实接入与验收未完成**

## 5. 验收阶段判断

当前项目虽然代码上已进入 `M7`，但按验收清单仍不能说已经通过 `P0`。

原因：

1. 当前环境缺少 `idf.py`
2. 还没有 fresh build 证据
3. 还没有新的上板证据去关闭以下最小准入项：
   - 设备正常启动
   - 屏幕显示
   - 触摸输入
   - RTC 有效时间
   - 页面切换成立

因此当前阶段不应表述为“项目已完成到 P5”，而应表述为：

- **代码阶段接近 P5 / M7**
- **验证阶段仍停留在 P0 未关闭**

## 6. 当前最准确的项目状态

一句话版本：

> 当前项目已经完成到 `M7 系统整合` 的代码接入阶段，但仍缺 `idf.py build` 和 fresh 上板联调证据，所以还不能说通过了 `P0` 手测准入。

更工程化的说法：

1. `M0-M5`：代码接入已完成
2. `M6`：真链路骨架已完成，但 ELM327 真接入未验收收口
3. `M7`：代码级系统整合已完成
4. `Checklist / DoD`：整体仍未关闭，首要阻塞是缺构建与上板验证

## 7. 下一步建议

当前不建议继续扩功能，建议先把工作重心切到“验证收口”：

1. 先补 ESP-IDF 环境，拿到 `idf.py build`
2. 跑 `P0 + P1` 最小手测闭环
3. 再做手机导航 BLE 真包联调
4. 最后做 ELM327 实物联调

若验证资源仍未到位，则下一步最有价值的工作是继续补“验证脚本 / 样本 / 诊断可观测性”，而不是继续扩大功能面。
