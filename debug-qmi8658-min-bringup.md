# Debug Session: qmi8658-min-bringup
- **Status**: [CLOSED - RESOLVED]
- **Issue**: QMI8658 在 `0x6B` 的访问非确定性挂死（停在寄存器地址写阶段，WHO_AM_I 长期只读到 `0xFF/0x00`），以及后续连带暴露的正常固件 boot loop。
- **Debug Server**: N/A for ESP-IDF UART-based runtime evidence
- **Log File**: `logs/qmi-run1..5.log`, `logs/normal-run1..4.log`

## Resolution Summary (two stacked root causes)

### Root Cause 1: `trans_queue_depth = 4` routed the bus onto the experimental async driver path
- `board_support.c` 初始化 I2C bus 时配置了 `trans_queue_depth = 4`，boot 日志有明确警告：
  `i2c.master: Please note i2c asynchronous is only used for specific scenario ... experimental ... not compatible with i2c_master_probe`
- 该异步队列路径在并发/时序变化下丢失完成事件，导致事务永久挂起、`xfer_timeout_ms` 从未被尊重、读数静默错乱（`0xFF/0x00` 假值）。
- **Fix**: 单变量改为 `trans_queue_depth = 0`（完全同步路径，官方示例所用）。
- **Evidence**: 改动后 `qmi-run3/4/5.log` 三轮全绿：
  - `detect result: err=ESP_OK addr=0x6B chip=0x05`（WHO_AM_I 首次正确）
  - `revision=0x7C`
  - `write ctrl7/ctrl6 result: err=ESP_OK`
  - `readback: whoami=ESP_OK/0x05 revision=ESP_OK/0x7C ctrl7=ESP_OK/0x43 ctrl6=ESP_OK/0x00`
  - `official sequence completed: addr=0x6B detect=0x05 revision_before=0x7C`
- 改动前 `qmi-run1/2.log` 的线级观察任务给出决定性排除证据：挂死在 `detect-retry` 期间 **SCL=1 SDA=1（总线完全空闲）**，排除物理层（SDA 被从机楔死/时钟拉伸/总线粘死），锁定驱动软件层。

### Root Cause 2: attitude 任务忙等饿死 app_main → I2C bus lock 超时 → boot loop
- Root Cause 1 修复后，正常固件出现新问题（`normal-run1..3.log`）：100% 复现的 boot loop，挂在 `board_support_reset_lcd()` → `tca9554_write_pin(EXIO2, low)` → `ESP_ERR_TIMEOUT (0x107)` → `ESP_ERROR_CHECK` abort。
- 分析（IDF 源码 + 日志交叉验证）：
  - `i2c_master.c` L1003 `s_i2c_synchronous_transaction`：`xSemaphoreTake(bus_lock_mux, timeout)` 拿不到锁直接返回 `ESP_ERR_TIMEOUT` 且**无任何驱动日志**——与失败前日志无 "I2C software timeout" 吻合，说明不是事务挂死而是拿不到锁。
  - `task_attitude`（核 0、优先级 4、250Hz）等待用 `esp_rom_delay_us` + `taskYIELD()` **忙等**。`taskYIELD()` 只让给同优先级及以上任务，优先级 1 的 `app_main`（同核 0）永远抢不到 CPU；唯一窗口 attitude 又持锁做 I2C 读——`app_main` 被饿死满 100ms 后超时。
  - 之前从未暴露：QMI 坏 → attitude 任务从未创建；QMI 修好后该隐患显形。
- **Fix** (`service_attitude.c`): 采样间隙等待改为 `vTaskDelay(coarse_ticks)` 阻塞 + 仅最后 ~500µs 自旋收尾（不持 I2C 锁），250Hz 节奏保持。
- **Evidence** (`normal-run4.log`, 40s 采集): `board_support_reset_lcd` 不再超时，`LCD panel create success` + `Touch panel create success` + `UI service ready`，心跳每 5s 稳定持续到 40s，零复位。QMI 全链路 + attitude ready（gyro_bias 校准通过）。

## Reproduction Steps (final verification path)
1. `idf.py -B build build && idf.py -B build -p COM3 flash`
2. `powershell -File tools\qmi_capture.ps1 -Seconds 40 -OutFile logs\normal-runN.log`
3. 检查：无 `lcd reset low failed`、无 abort、心跳连续、attitude ready。

## Hypotheses & Verification (final)
| ID | Hypothesis | Result | Evidence |
|----|------------|--------|----------|
| A | 最小 bring-up 下 0x6B 仍卡寄存器写 → 器件/板级前置问题 | **Refuted** | queue_depth=0 后最小路径官方序列三轮全绿（qmi-run3/4/5） |
| B | 完整 app 的共享 I2C 时序/任务交互污染 QMI 访问 | **Confirmed (via RC2)** | attitude 忙等饿死 app_main 的 bus lock 竞争被证实并修复 |
| C | 板级缺少复位/片选/电源门控前置 | **Refuted** | board_support_init + probe 即足够，全链路可通 |
| D | IDF6 新驱动事务语义不兼容该器件 | **Confirmed (via RC1)** | `trans_queue_depth=4` 实验性异步路径挂死+假数据；同步路径全通 |
| E | TCA9554/PCF85063 初始化顺序影响 0x6B | **Refuted** | 正常固件全链路（tca9554/pcf85063/qmi/BLE/OBD）一次起 |

## Key Changes
- `board_support.c`: `.trans_queue_depth = 4 → 0`（含决策注释 debug-point qmi-min-bringup:queue-depth-0）
- `service_attitude.c`: 忙等 → 阻塞等待（debug-point attitude:blocking-wait）
- `qmi8658.c`: 恢复 `qmi8658_init()` 完整官方序列 CTRL1(0x40)/CTRL2(0x40)/CTRL3(0x30)/CTRL5(0x61)/CTRL7(0x43)/CTRL6(0x00)；补 phase tracker + `qmi8658_run_minimal_bringup()`
- `app_main.c`: `PATHFINDER_QMI_MIN_BRINGUP` 分支 + 核 1 线级观察任务 `task_qmi_line_observer`
- `tools/qmi_capture.ps1`: RTS 硬复位 + 定时 UART 采集脚本

## Remaining / Follow-up
- SPD2010 触摸读参失败（面板创建成功，但 `tp_read_data`/`write_tp_cpu_start_cmd` I2C 事务失败）——下一个调试会话的主题（用户第二优先级）。
- `i2c.master: i2c_master_bus_rm_device: Wrong I2C status, cannot delete device` 偶发报错，疑似与触摸失败相关，随 SPD2010 一起查。
- PCF85063/低功耗细节复核（用户第三优先级）。
- SD 卡 `sdmmc_card_init failed (0x107)`（未插卡时出现，待确认是否正常降级路径）。
