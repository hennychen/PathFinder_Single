# Debug Session: qmi8658-bus-reset
- **Status**: [CLOSED - SUPERSEDED by debug-qmi8658-min-bringup.md]
- **Supersede Note (2026-09-17)**: 本会话的结论方向正确（失败在 QMI8658 事务路径内、非物理总线粘死——Hypothesis B Confirmed），但根因最终由 min-bringup 会话定位：
  1. **RC1**: `trans_queue_depth = 4`（IDF6 实验性异步 I2C 路径）导致完成事件丢失、事务永久挂起且 `xfer_timeout_ms` 不被尊重。这正解释了本会话"bus reset 返回 ESP_OK 但下一读仍挂"、"持久句柄/降速/拆分事务均无效"的现象——挂点在驱动异步队列层，任何设备侧手段都无法恢复。
  2. **RC2**: attitude 任务忙等饿死 app_main 引发的时序敏感性，解释了插桩使启动停点前移的现象。
  - 修复（`trans_queue_depth = 0` + attitude 阻塞等待）后真机全绿（`qmi-run3/4/5.log`、`normal-run4.log`、`spd-run1..3.log`）。本会话"外部观察优先于固件内插桩"的教训已吸收进 min-bringup 的线级观察方法（SCL/SDA 空闲判定排除物理层）。
- **Issue**: Deferred QMI8658 diagnostics still hang on the first `WHO_AM_I` read at `0x6B` even after switching to a persistent device handle and lowering the I2C speed to `100kHz`.
- **Debug Server**: N/A for ESP-IDF UART-based runtime evidence
- **Log File**: UART monitor / `idf.py monitor`

## Reproduction Steps
1. Build firmware.
2. Flash device to the current target board.
3. Observe boot logs until the deferred IMU diagnostic starts.
4. Confirm whether the first `addr=0x6B pass=1` read now returns or still hangs.

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | The shared I2C bus is left in a bad state before deferred IMU access, and `i2c_master_bus_reset()` will turn the hang into a real read result or explicit error. | High | Low | Pending |
| B | The hang is inside the QMI8658 transaction itself, so a bus reset will not change the observed stop point. | High | Low | Pending |
| C | The bus reset succeeds but the following read still returns a bad chip ID (`0x00` or `0xFF`), indicating a board-level or device-mode problem instead of a generic bus-stuck condition. | Medium | Low | Pending |
| D | `i2c_master_bus_reset()` itself fails or hangs on this shared bus, which would shift the stop point earlier and implicate bus ownership or controller state. | Medium | Low | Pending |
| E | A low-intrusion asynchronous quick probe using the same `transmit_receive` semantics as the boot-safe path will still hang even before BLE/OBD startup fully settles, which would implicate task context or post-boot bus state rather than late-service interference. | High | Low | Pending |
| F | The quick probe will succeed early but the 3s deferred diagnostic will still hang later, which would implicate later system timing/load rather than “any async context after boot.” | Medium | Low | Pending |

## Log Evidence
- Build/flash/monitor completed on real board after adding `i2c_master_bus_reset()` immediately before the first deferred `0x6B` read.
- Key runtime lines from the bus-reset experiment:
  - `W qmi8658: deferred diag step: addr=0x6B pass=1 timeout=20ms mode=persistent_handle speed=100000`
  - `W qmi8658: deferred diag bus state: bus=0x3c0c0998 addr=0x6B pass=1 speed=100000`
  - `W qmi8658: deferred diag bus reset: addr=0x6B pass=1`
  - `W qmi8658: deferred diag bus reset result: addr=0x6B pass=1 err=ESP_OK`
  - no subsequent `deferred diag result` line was emitted
- Follow-up split-transaction experiment on the same deferred path:
  - `W qmi8658: deferred diag split tx: addr=0x6B pass=1 reg=0x00 timeout=20ms`
  - no subsequent `deferred diag split tx result` line was emitted
  - downstream BLE/OBD logs kept running, confirming the system stayed alive while the IMU diagnostic task stalled
- Additional transient-handle comparison attempt:
  - A new deferred diagnostic branch was inserted to run a fresh transient-handle split `tx/rx` before the persistent-handle path.
  - On two consecutive boots, the system no longer reached `board_support` completion or any `qmi8658` init logs; UART output stopped immediately after `tca9554: detected IO expander at 0x20 output=0xFF config=0xFF`.
  - The experiment was reverted, reflashed, and verified to restore the previous stable baseline where the system boots fully and the deferred diagnostic again stalls specifically at `deferred diag split tx: addr=0x6B pass=1 reg=0x00 timeout=20ms`.
- Additional early async quick-probe attempt:
  - A lower-intrusion asynchronous task was added to run a boot-safe-style transient `transmit_receive` `WHO_AM_I` probe shortly after `qmi8658_init()` failure, while keeping the 3s deferred diagnostic unchanged.
  - This variant also perturbed startup: UART output again stopped immediately after `tca9554: detected IO expander at 0x20 output=0xFF config=0xFF`, with no later `board_support` or `qmi8658` logs, and the monitor file length stayed fixed.
  - The quick-probe branch was reverted, reflashed, and the prior stable baseline was re-verified on hardware, restoring full boot plus the known deferred stall at `deferred diag split tx: addr=0x6B pass=1 reg=0x00 timeout=20ms`.

## Verification Conclusion
- Hypothesis A: **Rejected.** A pre-read `i2c_master_bus_reset()` returns `ESP_OK`, but it does not restore the first deferred `0x6B` access into a normal result.
- Hypothesis B: **Confirmed.** The failure remains inside the QMI8658 transaction path even after bus reset.
- Hypothesis C: **Not yet directly tested to completion.** Because the deferred access never returns, we still do not reach a post-reset bad chip-ID readout.
- Hypothesis D: **Rejected.** `i2c_master_bus_reset()` itself does not fail or hang on this shared bus.
- New sub-finding: the deferred split transaction stalls during the standalone `i2c_master_transmit(handle, &reg_addr, 1, timeout)` phase before any separate receive step begins. This strongly suggests the problematic edge is already present on the write/register-address phase to `0x6B`, not only on the combined `transmit_receive()` or read phase.
- New sub-finding: a more intrusive transient-handle comparison branch perturbed startup enough to move the observable stall point earlier into shared EXIO bring-up. That result is useful as a stability warning, but it is not clean evidence about QMI8658 transaction semantics and should not be used as the main comparison path.
- New sub-finding: even a seemingly lighter early async quick probe using the same combined `transmit_receive` semantics as the boot-safe path can still perturb startup enough to shift the visible stop point back to shared EXIO bring-up. This makes additional in-firmware timing experiments increasingly suspect.
- Next best narrowing path: preserve the current stable baseline and prefer evidence sources that do not materially change boot timing at all, such as board-level line capture, external bus observation, or isolated hardware bring-up outside the full application startup path.
