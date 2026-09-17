# SPD2010 Touch Read Failure — Debug Record

- Status: **RESOLVED-PENDING-FINGER-TEST** (3 root causes fixed & verified by evidence; final coordinate-report test needs a human finger)
- Priority: 2 (user-defined, after QMI8658 CLOSED-RESOLVED)
- Log basis: `logs/normal-run4.log`, `logs/spd-run1.log`, `logs/spd-run2.log`, `logs/spd-run3.log`

## 1. Symptom (normal-run4)

```
I (2678) SPD2010: Touch panel create success, version: 2.0.1     <- read_fw_version OK
...
E (2812) lcd_panel.io.i2c: panel_io_i2c_tx_buffer(193): i2c transaction failed
E (2821) SPD2010: write_tp_cpu_start_cmd(275): Tx failed
E (2826) SPD2010: tp_read_data(448): Write cpu start cmd failed
E (2832) SPD2010: read_data(160): read data failed
E (2856/2887) ... read_tp_status_length Tx failed               <- 2nd/3rd poll
< then 35 seconds completely silent — no further touch errors >
E (10769/27924/28409) i2c.master: i2c_master_bus_rm_device(1205): Wrong I2C status, cannot delete device
```

## 2. Root causes (all verified)

### RC1: TP BIOS→CPU boot window NACKs (benign, explained the 3 errors)
- First runtime poll hits `tic_in_bios` → issues `cpu_start` → TP loads firmware and NACKs for ~100ms
- Evidence: spd-run1 health counter — `fail` frozen at 3 forever; ok=1533/1536 by t=37.5s
- Failures #2/#3 are polls landing inside that same window (~30ms apart)

### RC2: Touch INT pull-up cleared by component init → LVGL EVENT-mode indev dead + poll degeneration
- `configure_touch_interrupt()` sets pull-up on GPIO4; the spd2010 component's `gpio_config()` (no `pull_up_en`) silently clears it during create
- spd-run2 evidence: `int=0` on every health line with no finger; read cadence ~25ms (every cycle, `should_poll` always true) instead of 80ms fallback
- Consequence: INT stuck low → no falling edges → LVGL EVENT-mode indev never wakes → widget-level touch dead even though polling works
- Fix: `gpio_set_pull_mode(GPIO4, GPIO_PULLUP_ONLY)` after component create
- spd-run3 evidence: `int=1`, cadence back to ~74ms, boot-window note `int level after pull-up restore=0` (TP was still in boot window at that instant)

### RC3: i2c_master_bus_rm_device INVALID_STATE race + permanent handle leak (QMI 250Hz path)
- rm_device requires bus status > I2C_STATUS_START(3); READ=0/READ_ALL=1/WRITE=2 all fail the check; any concurrent transaction on the shared bus triggers it
- QMI read path does add→transmit→rm per sample at 250Hz; on failure IDF logs E and returns **without freeing** → permanent handle + device_list leak
- Fix: bounded retry (5×200µs) in `close_i2c_device()`; spd-run1..3: zero "handle leaked" warnings (E lines from IDF's first attempt remain but retry recovers)

## 3. Hardening fixes (same session)

- esp_lvgl_port_touch.c: replaced `ESP_ERROR_CHECK(esp_lcd_touch_read_data())` with tolerant warn+RELEASED — INT-driven read during boot window would otherwise abort the firmware (latent panic bomb, never triggered in captures)

## 4. Hypothesis table (final)

| ID | Hypothesis | Verdict |
|---|---|---|
| H1 | TP CPU boot window NACKs | CONFIRMED (fail counter freezes) |
| H2 | Dual-reader interleaving corrupts reads | NOT OBSERVED (post-window 100% ok; latent risk remains, no serialization added) |
| H3 | rm_device race + leak | CONFIRMED & FIXED (retry, no leak warnings) |
| H4 | Reset timing insufficient | REJECTED (failures 134ms post-reset, create succeeded) |
| H5 | LVGL ESP_ERROR_CHECK panic bomb | CONFIRMED code fact, defused |
| H6 | INT pull-up cleared by component | CONFIRMED & FIXED (int=0→1, cadence 25→74ms) |

## 5. Evidence log

| Run | Change | Result |
|---|---|---|
| spd-run1 | +touch health counter, +rm retry, +LVGL tolerant read | fail frozen at 3 (boot window), rm errors gone as leaks, 40s stable |
| spd-run2 | +int level & pressed counters in health line | `int=0` stuck → RC2 discovered; cadence ~25ms |
| spd-run3 | +pull-up restore after touch create | `int=1`, cadence ~74ms, boot-window fail=1, no abort, no leak warnings |

## 6. Remaining

- **Finger test**: press/swipe screen during a capture — expect `pressed>0`, `service_ui: touch down x=.. y=..` log, and LVGL widget response (page swipe). This is the only unverified acceptance item.
- Optional cleanup (not blocking): the IDF-side E log on first rm attempt is cosmetic noise.

