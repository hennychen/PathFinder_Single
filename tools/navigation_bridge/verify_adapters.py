#!/usr/bin/env python3
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

try:
    from bridge_demo import BLE_ATT_DEFAULT_PAYLOAD_MAX, NavigationPacket, load_event_payload
    from map_adapters import normalize_navigation_event
except ImportError:  # pragma: no cover - fallback when imported as package module
    from tools.navigation_bridge.bridge_demo import BLE_ATT_DEFAULT_PAYLOAD_MAX, NavigationPacket, load_event_payload
    from tools.navigation_bridge.map_adapters import normalize_navigation_event


ROOT = Path(__file__).resolve().parent


@dataclass(frozen=True)
class VerificationCase:
    provider: str
    file_name: str
    expected_turn_key: str
    expected_step_distance_m: int
    expected_total_distance_m: int
    expected_remain_time_s: int
    expected_road_name: str
    expected_frame_count: int


CASES = [
    VerificationCase(
        provider="generic",
        file_name="generic_event.json",
        expected_turn_key="left",
        expected_step_distance_m=180,
        expected_total_distance_m=2450,
        expected_remain_time_s=360,
        expected_road_name="Zhongshan Road",
        expected_frame_count=2,
    ),
    VerificationCase(
        provider="amap",
        file_name="amap_event.json",
        expected_turn_key="right",
        expected_step_distance_m=120,
        expected_total_distance_m=1860,
        expected_remain_time_s=300,
        expected_road_name="Renmin Road",
        expected_frame_count=1,
    ),
    VerificationCase(
        provider="amap",
        file_name="amap_navi_info_event.json",
        expected_turn_key="right",
        expected_step_distance_m=120,
        expected_total_distance_m=1860,
        expected_remain_time_s=300,
        expected_road_name="Renmin Road",
        expected_frame_count=1,
    ),
    VerificationCase(
        provider="amap",
        file_name="amap_arrive_event.json",
        expected_turn_key="arrive",
        expected_step_distance_m=30,
        expected_total_distance_m=30,
        expected_remain_time_s=12,
        expected_road_name="Destination Ramp",
        expected_frame_count=2,
    ),
    VerificationCase(
        provider="baidu",
        file_name="baidu_event.json",
        expected_turn_key="left_front",
        expected_step_distance_m=90,
        expected_total_distance_m=1320,
        expected_remain_time_s=240,
        expected_road_name="Minsheng Avenue",
        expected_frame_count=2,
    ),
    VerificationCase(
        provider="tencent",
        file_name="tencent_event.json",
        expected_turn_key="arrive",
        expected_step_distance_m=20,
        expected_total_distance_m=20,
        expected_remain_time_s=15,
        expected_road_name="Destination",
        expected_frame_count=1,
    ),
    VerificationCase(
        provider="amap",
        file_name="amap_long_chinese_event.json",
        expected_turn_key="left",
        expected_step_distance_m=350,
        expected_total_distance_m=4680,
        expected_remain_time_s=540,
        expected_road_name="世纪大道高架辅路人民中路延长线",
        expected_frame_count=3,
    ),
]


def assert_equal(actual, expected, label: str) -> None:
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def verify_case(case: VerificationCase) -> None:
    payload = load_event_payload(str(ROOT / "examples" / case.file_name))
    normalized = normalize_navigation_event(case.provider, payload)

    assert_equal(normalized.provider, case.provider, "provider")
    assert_equal(normalized.turn_key, case.expected_turn_key, "turn_key")
    assert_equal(normalized.step_distance_m, case.expected_step_distance_m, "step_distance_m")
    assert_equal(normalized.total_distance_m, case.expected_total_distance_m, "total_distance_m")
    assert_equal(normalized.remain_time_s, case.expected_remain_time_s, "remain_time_s")
    assert_equal(normalized.road_name, case.expected_road_name, "road_name")

    packet = NavigationPacket(
        turn_type=normalized.turn_type,
        step_distance_m=normalized.step_distance_m,
        total_distance_m=normalized.total_distance_m,
        remain_time_s=normalized.remain_time_s,
        road_name=normalized.road_name,
    )
    single_packet = packet.encode_single_packet()
    frames = packet.encode_frames(BLE_ATT_DEFAULT_PAYLOAD_MAX, message_id=1)

    if len(single_packet) > BLE_ATT_DEFAULT_PAYLOAD_MAX:
        if frames[0][0] != 0xFF:
            raise AssertionError("fragmented frame must start with 0xFF marker")

    assert_equal(len(frames), case.expected_frame_count, "frame_count")
    print(
        f"PASS provider={case.provider} file={case.file_name} "
        f"single_len={len(single_packet)} frame_count={len(frames)} road={normalized.road_name}"
    )


def main() -> int:
    print(json.dumps({"cases": len(CASES), "att_payload_budget": BLE_ATT_DEFAULT_PAYLOAD_MAX}, ensure_ascii=False))
    for case in CASES:
        verify_case(case)
    print("ALL_CASES_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
