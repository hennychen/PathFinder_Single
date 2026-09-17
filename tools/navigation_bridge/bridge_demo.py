#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # pragma: no cover - optional runtime dependency
    BleakClient = None
    BleakScanner = None

try:
    from map_adapters import ProviderAdapterError, TURN_CHOICES, normalize_navigation_event, provider_names
except ImportError:  # pragma: no cover - fallback when imported as package module
    from tools.navigation_bridge.map_adapters import (
        ProviderAdapterError,
        TURN_CHOICES,
        normalize_navigation_event,
        provider_names,
    )


NAV_PACKET_MIN_SIZE = 8
NAV_ROAD_NAME_MAX = 32
NAV_PACKET_MAX_SIZE = NAV_PACKET_MIN_SIZE + NAV_ROAD_NAME_MAX

NAV_FRAGMENT_MARKER = 0xFF
NAV_FRAGMENT_HEADER_SIZE = 5

BLE_ATT_DEFAULT_MTU = 23
BLE_ATT_WRITE_OVERHEAD = 3
BLE_ATT_DEFAULT_PAYLOAD_MAX = BLE_ATT_DEFAULT_MTU - BLE_ATT_WRITE_OVERHEAD

DEVICE_NAME = "PathFinder Nav"
SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
NAV_CHAR_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"


def utf8_safe_prefix(data: bytes, max_len: int) -> bytes:
    bounded = data[:max_len]
    while bounded:
        try:
            bounded.decode("utf-8")
            return bounded
        except UnicodeDecodeError:
            bounded = bounded[:-1]
    return b""


@dataclass(frozen=True)
class NavigationPacket:
    turn_type: int
    step_distance_m: int
    total_distance_m: int
    remain_time_s: int
    road_name: str = ""

    def encode_single_packet(self) -> bytes:
        if self.turn_type < 0 or self.turn_type > 6:
            raise ValueError("turn_type must be in range 0..6")

        road_name_bytes = utf8_safe_prefix(self.road_name.encode("utf-8"), NAV_ROAD_NAME_MAX)
        payload = bytearray()
        payload.append(self.turn_type & 0xFF)
        payload.extend(int(self.step_distance_m).to_bytes(2, "little", signed=False))
        payload.extend(int(self.total_distance_m).to_bytes(2, "little", signed=False))
        payload.extend(int(self.remain_time_s).to_bytes(2, "little", signed=False))
        payload.append(len(road_name_bytes))
        payload.extend(road_name_bytes)

        if len(payload) < NAV_PACKET_MIN_SIZE or len(payload) > NAV_PACKET_MAX_SIZE:
            raise ValueError("single navigation packet length is out of range")
        return bytes(payload)

    def encode_frames(self, att_payload_budget: int, message_id: int) -> List[bytes]:
        single_packet = self.encode_single_packet()
        if att_payload_budget <= 0:
            raise ValueError("att_payload_budget must be positive")

        if len(single_packet) <= att_payload_budget:
            return [single_packet]

        fragment_payload_budget = att_payload_budget - NAV_FRAGMENT_HEADER_SIZE
        if fragment_payload_budget <= 0:
            raise ValueError("att_payload_budget is too small for fragment frames")

        frames: List[bytes] = []
        chunk_count = (len(single_packet) + fragment_payload_budget - 1) // fragment_payload_budget
        for chunk_index in range(chunk_count):
            start = chunk_index * fragment_payload_budget
            end = min(start + fragment_payload_budget, len(single_packet))
            chunk = single_packet[start:end]
            frame = bytearray()
            frame.append(NAV_FRAGMENT_MARKER)
            frame.append(message_id & 0xFF)
            frame.append(chunk_index & 0xFF)
            frame.append(chunk_count & 0xFF)
            frame.append(len(chunk) & 0xFF)
            frame.extend(chunk)
            frames.append(bytes(frame))
        return frames


def format_frames(frames: Sequence[bytes]) -> str:
    lines = []
    for index, frame in enumerate(frames):
        lines.append(f"frame[{index}] len={len(frame)} hex={frame.hex(' ')}")
    return "\n".join(lines)


def load_event_payload(path: str) -> dict:
    event_path = Path(path)
    return json.loads(event_path.read_text(encoding="utf-8"))


def packet_from_args(args: argparse.Namespace) -> NavigationPacket:
    if args.event_file:
        normalized = normalize_navigation_event(args.provider, load_event_payload(args.event_file))
        if args.show_normalized:
            print(
                "normalized_event="
                f"provider={normalized.provider} "
                f"turn={normalized.turn_key} "
                f"step={normalized.step_distance_m} "
                f"total={normalized.total_distance_m} "
                f"remain={normalized.remain_time_s} "
                f"road={normalized.road_name}"
            )
        return NavigationPacket(
            turn_type=normalized.turn_type,
            step_distance_m=normalized.step_distance_m,
            total_distance_m=normalized.total_distance_m,
            remain_time_s=normalized.remain_time_s,
            road_name=normalized.road_name,
        )

    return NavigationPacket(
        turn_type=TURN_CHOICES[args.turn],
        step_distance_m=args.step_distance,
        total_distance_m=args.total_distance,
        remain_time_s=args.remain_time,
        road_name=args.road_name,
    )


async def write_frames(device_name: str, char_uuid: str, frames: Sequence[bytes], response: bool) -> None:
    if BleakScanner is None or BleakClient is None:
        raise RuntimeError("bleak is not installed; use --print-only or install bleak")

    device = await BleakScanner.find_device_by_name(device_name)
    if device is None:
        raise RuntimeError(f"BLE device not found: {device_name}")

    async with BleakClient(device) as client:
        for frame in frames:
            await client.write_gatt_char(char_uuid, frame, response=response)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="PathFinder navigation BLE bridge prototype")
    parser.add_argument("--provider", choices=provider_names(), default="generic")
    parser.add_argument("--event-file", help="Path to provider event JSON file")
    parser.add_argument("--turn", choices=sorted(TURN_CHOICES.keys()), default="straight")
    parser.add_argument("--step-distance", type=int, default=150)
    parser.add_argument("--total-distance", type=int, default=1200)
    parser.add_argument("--remain-time", type=int, default=180)
    parser.add_argument("--road-name", default="Demo Road")
    parser.add_argument("--att-payload-budget", type=int, default=BLE_ATT_DEFAULT_PAYLOAD_MAX)
    parser.add_argument("--message-id", type=int, default=1)
    parser.add_argument("--device-name", default=DEVICE_NAME)
    parser.add_argument("--char-uuid", default=NAV_CHAR_UUID)
    parser.add_argument("--write-with-response", action="store_true")
    parser.add_argument("--show-normalized", action="store_true")
    parser.add_argument("--print-only", action="store_true")
    return parser


async def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    try:
        packet = packet_from_args(args)
    except (ProviderAdapterError, OSError, json.JSONDecodeError, ValueError) as exc:
        parser.error(str(exc))

    single_packet = packet.encode_single_packet()
    frames = packet.encode_frames(args.att_payload_budget, args.message_id)

    print(f"single_packet_len={len(single_packet)} att_payload_budget={args.att_payload_budget}")
    print(format_frames(frames))

    if args.print_only:
        return 0

    await write_frames(args.device_name, args.char_uuid, frames, args.write_with_response)
    print("write complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
