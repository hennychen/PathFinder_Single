# Navigation Bridge Prototype

This folder contains a minimal desktop-side BLE navigation sender prototype for PathFinder.

## Files

- `bridge_demo.py`
  - Encodes the repository navigation packet format
  - Normalizes upstream map-event JSON through provider adapters
  - Chooses single-packet or fragment-frame output based on the ATT payload budget
  - Can print frames only, or write them to the BLE navigation characteristic with `bleak`
- `map_adapters.py`
  - Provider-neutral normalization entry
  - Current skeleton adapters: `generic`, `amap`, `baidu`, `tencent`
- `examples/*.json`
  - Sample event payloads for each provider skeleton
- `verify_adapters.py`
  - Runs normalization and frame-selection checks against repository-owned sample events
  - Includes official-like `amap` `onNaviInfoUpdate(NaviInfo)` samples

## Install

Optional BLE runtime dependency:

```bash
pip install bleak
```

## Print Frames Only

```bash
python tools/navigation_bridge/bridge_demo.py --print-only --turn left --road-name "Zhongshan Road"
```

## Load Provider Event JSON

```bash
python tools/navigation_bridge/bridge_demo.py --print-only --provider amap --event-file tools/navigation_bridge/examples/amap_event.json --show-normalized
```

```bash
python tools/navigation_bridge/bridge_demo.py --print-only --provider amap --event-file tools/navigation_bridge/examples/amap_navi_info_event.json --show-normalized
```

```bash
python tools/navigation_bridge/bridge_demo.py --print-only --provider baidu --event-file tools/navigation_bridge/examples/baidu_event.json --show-normalized
```

## Verify Provider Adapters

```bash
python tools/navigation_bridge/verify_adapters.py
```

This verifies:

- provider event normalization
- packet encoding
- default `20B` ATT budget behavior
- fragment marker selection for longer packets
- long UTF-8 road-name cases that may expand to `3` fragments under the default budget

## Simulate Default Budget

The firmware currently assumes a default ATT payload budget of `20B` until MTU exchange completes.

```bash
python tools/navigation_bridge/bridge_demo.py --print-only --att-payload-budget 20 --road-name "Long Chinese Road Name"
```

If the encoded navigation packet does not fit, the script automatically emits `0xFF` fragment frames.

## Write To Device

```bash
python tools/navigation_bridge/bridge_demo.py --device-name "PathFinder Nav"
```

## Notes

- The script uses the same packet layout documented in `docs/materials/navigation_bridge_protocol.md`
- `amap` adapter now supports official-like nested `naviInfo` payloads in addition to flat bridge JSON
- Provider adapters are a repository-owned skeleton and may need field-name alignment with the final mobile SDK callback objects
- UTF-8 road names are trimmed on character boundaries before encoding
- Fragment frames use:
  - `0xFF + message_id + chunk_index + chunk_count + chunk_len + chunk_data`
