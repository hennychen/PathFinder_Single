# Debug Session: qmi8658-reg-window
- **Status**: [CLOSED - SUPERSEDED by debug-qmi8658-min-bringup.md]
- **Supersede Note (2026-09-17)**: 本会话观察到的所有挂死与假读数（`0xFF/0x00`、首读挂起、拆分事务写阶段停住、额外 EXIO 读扰动启动等）已被 min-bringup 会话的两个根因完整解释并修复：
  1. **RC1**: `board_support.c` 的 `trans_queue_depth = 4` 启用了 IDF6 实验性异步 I2C 路径，事务完成事件丢失导致挂死与假数据（改为 `0` 后 `qmi-run3/4/5.log` 三轮全绿，`chip=0x05` 首读正确）。这解释了本会话的 Hypothesis B/D 现象：不是寄存器窗口或器件模式问题，而是异步驱动路径问题。
  2. **RC2**: attitude 任务忙等饿死 `app_main` 导致 bus lock 超时 boot loop（`normal-run1..3.log`），这解释了本会话"侵入式插桩使启动停更早"的时序敏感性：任何增加核 0 负载的插桩都会加剧锁竞争。
  - 修复后真机验证：`normal-run4.log` 40s 零复位、attitude ready、全服务链路正常；`spd-run1..3.log` 延续稳定。
- **Issue**: QMI8658 on the ESP32-S3 board responds on I2C address `0x6B`, but firmware bring-up stalls once register access moves beyond the minimal `WHO_AM_I (0x00)` probe path.
- **Debug Server**: N/A for ESP-IDF UART-based runtime evidence
- **Log File**: UART monitor / `idf.py monitor`

## Reproduction Steps
1. Build firmware with `idf.py build`.
2. Flash device on `COM3` with `idf.py -p COM3 flash`.
3. Observe boot logs with `idf.py -p COM3 monitor`.
4. Watch `qmi8658` init logs during `service_attitude_init()`.

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | The IMU acknowledges `0x6B`, but is not in a usable register-bank state, so `0x00` may read while `0x01+` breaks the transaction path. | High | Low | Pending |
| B | ESP-IDF 6 `i2c_master_transmit_receive()` blocks on this device when reading `0x01`, independent of returned register value. | High | Medium | Pending |
| C | The device is still in the wrong bus or power mode, and Arduino `Wire` timing differs enough from IDF new I2C driver to expose it. | Medium | Medium | Pending |
| D | A missing board-level prerequisite such as CS state or settle timing leaves only partial IMU visibility on the bus. | Medium | Medium | Pending |

## Log Evidence
- Pre-change logs:
  - `diag probe reg=0x00 timeout=20ms`
  - `diag probe reg=0x00 value=0x00`
  - `diag probe reg=0x01 timeout=20ms`
  - no further progress after `0x01`
- Post-change logs:
  - `W qmi8658: detected IMU bus response addr=0x6B raw chip id=0xFF`
  - `I qmi8658: init step: safe whoami probe timeout=20ms`
  - `I qmi8658: safe WHO_AM_I addr=0x6B detect=0xFF probe=0x00`
  - `W qmi8658: IMU WHO_AM_I mismatch on safe path addr=0x6B detect=0xFF probe=0xFF`
  - `E service_attitude: QMI8658 init failed: ESP_ERR_INVALID_RESPONSE`
  - `W app_main: attitude service skipped: ESP_ERR_INVALID_RESPONSE`
  - downstream services continue: `drivers_audio ready`, `service_voice ready`, `service_ble ready`, `service_obd ready`
- IMU interrupt-line experiment:
  - One direct EXIO read immediately before QMI detect reported `int1=0 int2=1`
  - When that extra TCA9554 read was inserted on the shared I2C path, boot stalled at `qmi8658: init step: detect`
  - When the same TCA9554 input-read was moved into `board_support_init()`, boot stalled even earlier, right after `tca9554: detected IO expander at 0x20`
- Deferred post-boot diagnostic experiment:
  - Added a delayed task that runs 3 seconds after `service_attitude_init()` fails, so diagnostics do not execute on the normal boot path.
  - First deferred version (`probe_chip_id_at_address` + safe probe on `0x6A/0x6B`) logged only `deferred diag start`, then emitted no further `qmi8658` result logs.
  - Refined deferred version reduced the flow to minimal single-register reads and logged `deferred diag step: addr=0x6B pass=1 timeout=20ms`, then emitted no `deferred diag result` line after that.
  - In one intervening boot, the normal boot-path `detect` probe briefly returned the expected `0x05`, while the immediately following safe probe still returned `0x00`.
  - Persistent-handle deferred version still stalled on the first read after logging `deferred diag open: addr=0x6B` and `deferred diag step: addr=0x6B pass=1 ... mode=persistent_handle`.
  - Lowering the deferred diagnostic handle speed from `400kHz` to `100kHz` did not change the behavior; the log still stopped at the first `addr=0x6B pass=1` read.
  - Adding `i2c_master_bus_reset(board_support_get_i2c_bus())` immediately before the first deferred `0x6B` read produced `deferred diag bus reset result: ... err=ESP_OK`, but still emitted no later `deferred diag result` line.
  - Splitting the deferred access into `i2c_master_transmit(handle, &reg_addr, 1, timeout)` followed by `i2c_master_receive()` moved the stall point even earlier: the log stopped immediately after `deferred diag split tx: addr=0x6B pass=1 reg=0x00 timeout=20ms`, with no `split tx result` line.
  - A subsequent attempt to compare against a fresh transient-handle split path proved too intrusive: after flashing that variant, two consecutive boots stalled immediately after `tca9554: detected IO expander at 0x20`, before `board_support` completion and before any `qmi8658` logs.
  - Reverting that transient-handle branch restored the prior stable baseline, and the deferred diagnostic once again progressed to `deferred diag split tx: addr=0x6B pass=1 reg=0x00 timeout=20ms` before stalling.
  - A later attempt to add an earlier asynchronous quick probe using the same transient `transmit_receive` semantics as the boot-safe path also proved too intrusive: boot again stopped immediately after `tca9554: detected IO expander at 0x20`, with no later `board_support` or `qmi8658` logs.
  - Reverting that quick-probe branch again restored the stable baseline, confirming the startup shift was caused by the instrumentation variant rather than a persistent board state change.

## Verification Conclusion
- Hypothesis A: **Confirmed enough to act on**. Access beyond the safe `0x00` path is not needed to reproduce failure, and the bus still does not return the expected chip ID.
- Hypothesis B: **Still plausible**. The earlier hard stall at `0x01` is avoided once normal boot stops touching `0x01+`.
- Hypothesis C: **Still open**. The mismatch between official Arduino behavior and IDF new-driver behavior remains unexplained.
- Hypothesis D: **Still open**. A board-level prerequisite may still be missing because the device responds on `0x6B` but does not identify cleanly.
- New sub-finding: shared-I2C sequencing is fragile. Additional `tca9554_read_pin()` transactions around IMU bring-up can move the stall point earlier, so further board-state sampling must avoid injecting fresh EXIO reads into the normal boot path.
- New sub-finding: even after the rest of the system is up, the first fresh single-register `WHO_AM_I` transaction in the deferred diagnostic task can hang on `0x6B`. This points away from a pure boot-order issue and toward a deeper bus/device transaction instability on QMI8658.
- New sub-finding: persistent handle reuse and reduced I2C speed (`100kHz`) do not resolve the first-read hang in deferred diagnostics, so the issue is unlikely to be caused only by device-handle churn or bus frequency.
- New sub-finding: explicit bus recovery also does not resolve the deferred hang. `i2c_master_bus_reset()` succeeds, but the subsequent IMU access still stalls.
- New sub-finding: the deferred split-transaction experiment indicates the stall is already on the write/register-address phase to `0x6B`, before any separate receive phase. This reduces the likelihood that repeated-start or readback semantics are the primary trigger.
- New sub-finding: some broader deferred-diagnostic instrumentation can perturb boot timing enough to shift the visible stall point earlier into shared EXIO/I2C bring-up. Those variants need to be treated as intrusive and rolled back quickly once they stop providing clean QMI evidence.
- New sub-finding: even an early async quick probe that reuses the boot-safe `transmit_receive` semantics can still perturb startup enough to mask the original QMI failure mode. The practical debugging boundary has moved from “add more firmware probes” toward “observe the existing stable probe from outside the firmware path.”
- The current patch succeeded at its immediate goal: preserve full system boot by failing fast on IMU mismatch and skipping `service_attitude`.
