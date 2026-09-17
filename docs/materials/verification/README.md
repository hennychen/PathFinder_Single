# 验证证据目录

用于存放各阶段的：

- 串口日志
- 屏幕实拍
- 录屏
- 手测备注

## 已有真机证据（2026-09-16 ~ 09-17 更新）

串口日志统一存放在仓库根目录 `logs/`，采集方式见 `tools/qmi_capture.ps1`（RTS 硬复位 + 定时 UART 采集，COM3）。

### P0 最小准入 —— 已通过（事实层面）

| 证据 | 日志 | 结论 |
| :--- | :--- | :--- |
| 设备正常启动（零复位 boot loop） | `logs/normal-run4.log`（40s 连续心跳） | QMI 修复后 RC2（attitude 忙等饿死）已消除 |
| 屏幕显示 | `logs/normal-run4.log`：`LCD panel create success` + `UI service ready` | 通过 |
| RTC 有效时间 | `logs/normal-run4.log` / `spd-run3.log`：pcf85063 ready、心跳时间戳连续 | 通过 |
| 页面切换 | 待补录屏/实拍（代码路径已验证：UI service ready 后页面仲裁运行） | 事实通过，正式收口待实拍 |

### P1 基础联调 —— 大部分通过

| 证据 | 日志 | 结论 |
| :--- | :--- | :--- |
| QMI8658 全链路 | `logs/qmi-run3/4/5.log`：`chip=0x05`、`revision=0x7C`、官方序列完成、attitude ready（gyro_bias 校准通过） | 三轮全绿，根因见 `debug-qmi8658-min-bringup.md` |
| SPD2010 触摸（电气/时序层） | `logs/spd-run1/2/3.log`：面板创建成功、触摸健康计数正常 | 根因修复已验证；**待真人手指触压采集**关闭 `debug-spd2010-touch.md`（RESOLVED-PENDING-FINGER-TEST） |
| OBD 扫描 | `logs/spd-run*.log`：BLE 扫描运行中，`open elm327 ble client failed: ESP_ERR_TIMEOUT`（无 ELM327 实物，预期失败） | 代码路径验证通过；真链路待实物 |
| 导航 BLE | 心跳 `nav=0`（无手机连接，预期） | 桌面桥 `tools/bridge_demo.py` 回归通过；真机手机包待联调 |
| 电池 ADC | 心跳 `bat=100%/4210mV`（首帧 `0mV` 为采样首读占位） | 基本通过，百分比曲线待实测校准 |

### 待补证据（按优先级）

1. 触摸真人手指触压采集（关闭 P1 最后一项）
2. ELM327（Vgate iCar Pro BLE 4.0）实物广播名/UUID + AT 初始化 + 5 项 PID 真值
3. 手机导航 BLE 真包联调（或先用桌面 bleak 桥代替）
4. 轻睡眠进入/唤醒恢复（BLE/语音/SD flush）
5. 页面切换与 OBD 告警抢占实拍/录屏
