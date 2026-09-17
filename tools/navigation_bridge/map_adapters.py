from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Dict, Iterable, Mapping


TURN_CHOICES: Dict[str, int] = {
    "straight": 0,
    "left": 1,
    "right": 2,
    "left_front": 3,
    "right_front": 4,
    "u_turn": 5,
    "arrive": 6,
}


TURN_ALIASES: Dict[str, str] = {
    "0": "straight",
    "1": "left",
    "2": "right",
    "3": "left_front",
    "4": "right_front",
    "5": "u_turn",
    "6": "arrive",
    "straight": "straight",
    "go_straight": "straight",
    "continue": "straight",
    "left": "left",
    "turn_left": "left",
    "right": "right",
    "turn_right": "right",
    "left_front": "left_front",
    "slight_left": "left_front",
    "bear_left": "left_front",
    "right_front": "right_front",
    "slight_right": "right_front",
    "bear_right": "right_front",
    "u_turn": "u_turn",
    "uturn": "u_turn",
    "turn_back": "u_turn",
    "arrive": "arrive",
    "destination": "arrive",
    "arrived": "arrive",
}


AMAP_ICON_TYPE_TO_TURN: Dict[int, str] = {
    2: "left",
    3: "right",
    4: "left_front",
    5: "right_front",
    6: "left",
    7: "right",
    8: "u_turn",
    9: "straight",
    15: "arrive",
    17: "left",
    18: "straight",
    19: "u_turn",
    21: "left",
    22: "right",
    25: "left",
    26: "right",
    27: "straight",
    28: "u_turn",
    65: "left_front",
    66: "right_front",
}


class ProviderAdapterError(ValueError):
    pass


@dataclass(frozen=True)
class NormalizedNavigationEvent:
    provider: str
    turn_key: str
    step_distance_m: int
    total_distance_m: int
    remain_time_s: int
    road_name: str

    @property
    def turn_type(self) -> int:
        return TURN_CHOICES[self.turn_key]


def _require_mapping(payload: Any) -> Mapping[str, Any]:
    if not isinstance(payload, Mapping):
        raise ProviderAdapterError("event payload must be a JSON object")
    return payload


def _first_present(payload: Mapping[str, Any], keys: Iterable[str]) -> Any:
    for key in keys:
        if key in payload and payload[key] is not None:
            return payload[key]
    raise ProviderAdapterError(f"missing required field, expected one of: {', '.join(keys)}")


def _first_present_from_sources(sources: Iterable[Mapping[str, Any]], keys: Iterable[str]) -> Any:
    for source in sources:
        for key in keys:
            if key in source and source[key] is not None:
                return source[key]
    raise ProviderAdapterError(f"missing required field, expected one of: {', '.join(keys)}")


def _int_value(value: Any, field_name: str) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ProviderAdapterError(f"{field_name} must be an integer-compatible value") from exc
    if parsed < 0:
        raise ProviderAdapterError(f"{field_name} must be non-negative")
    return parsed


def _str_value(value: Any) -> str:
    if value is None:
        return ""
    return str(value)


def _normalize_turn(value: Any) -> str:
    token = str(value).strip().lower()
    normalized = TURN_ALIASES.get(token)
    if normalized is None:
        raise ProviderAdapterError(f"unsupported turn token: {value}")
    return normalized


def _normalize_amap_turn(value: Any) -> str:
    if isinstance(value, bool):
        raise ProviderAdapterError(f"unsupported amap turn token: {value}")

    if isinstance(value, int):
        normalized = AMAP_ICON_TYPE_TO_TURN.get(value)
        if normalized is None:
            raise ProviderAdapterError(f"unsupported amap iconType: {value}")
        return normalized

    try:
        int_value = int(str(value).strip())
    except ValueError:
        return _normalize_turn(value)

    normalized = AMAP_ICON_TYPE_TO_TURN.get(int_value)
    if normalized is None:
        raise ProviderAdapterError(f"unsupported amap iconType: {value}")
    return normalized


def _mapping_value(value: Any) -> Mapping[str, Any] | None:
    return value if isinstance(value, Mapping) else None


def _normalize_generic(payload: Mapping[str, Any]) -> NormalizedNavigationEvent:
    return NormalizedNavigationEvent(
        provider="generic",
        turn_key=_normalize_turn(_first_present(payload, ("turn", "turn_key", "turn_type"))),
        step_distance_m=_int_value(_first_present(payload, ("step_distance_m", "stepDistance", "distance")), "step_distance_m"),
        total_distance_m=_int_value(_first_present(payload, ("total_distance_m", "totalDistance", "remain_distance_m")), "total_distance_m"),
        remain_time_s=_int_value(_first_present(payload, ("remain_time_s", "remainTime", "remain_duration_s")), "remain_time_s"),
        road_name=_str_value(payload.get("road_name", payload.get("roadName", ""))),
    )


def _normalize_amap(payload: Mapping[str, Any]) -> NormalizedNavigationEvent:
    navi_info = _mapping_value(payload.get("naviInfo"))
    route_info = _mapping_value(payload.get("routeInfo"))
    sources = tuple(source for source in (payload, navi_info, route_info) if source is not None)

    return NormalizedNavigationEvent(
        provider="amap",
        turn_key=_normalize_amap_turn(
            _first_present_from_sources(sources, ("iconType", "icon_type", "turn", "maneuver", "turnType", "turn_type"))
        ),
        step_distance_m=_int_value(
            _first_present_from_sources(
                sources,
                ("segmentRemainDistance", "stepDistance", "step_distance", "distance", "segmentRetainDistance"),
            ),
            "step_distance_m",
        ),
        total_distance_m=_int_value(
            _first_present_from_sources(
                sources,
                ("routeRemainDistance", "pathRetainDistance", "totalDistance", "total_distance", "pathRemainDistance"),
            ),
            "total_distance_m",
        ),
        remain_time_s=_int_value(
            _first_present_from_sources(
                sources,
                ("routeRemainTime", "pathRetainTime", "remainTime", "remain_time", "pathRemainTime"),
            ),
            "remain_time_s",
        ),
        road_name=_str_value(
            _first_present_from_sources(
                sources,
                ("nextRoadName", "currentRoadName", "roadName", "road_name"),
            )
        ),
    )


def _normalize_baidu(payload: Mapping[str, Any]) -> NormalizedNavigationEvent:
    return NormalizedNavigationEvent(
        provider="baidu",
        turn_key=_normalize_turn(_first_present(payload, ("turn", "maneuver", "action", "turnType"))),
        step_distance_m=_int_value(_first_present(payload, ("stepDistance", "distance", "nextDistance")), "step_distance_m"),
        total_distance_m=_int_value(_first_present(payload, ("totalDistance", "remainDistance", "distanceRemaining")), "total_distance_m"),
        remain_time_s=_int_value(_first_present(payload, ("remainTime", "timeRemaining", "totalRemainTime")), "remain_time_s"),
        road_name=_str_value(payload.get("nextRoadName", payload.get("currentRoadName", payload.get("roadName", "")))),
    )


def _normalize_tencent(payload: Mapping[str, Any]) -> NormalizedNavigationEvent:
    return NormalizedNavigationEvent(
        provider="tencent",
        turn_key=_normalize_turn(_first_present(payload, ("turn", "maneuver", "action", "turnType"))),
        step_distance_m=_int_value(_first_present(payload, ("stepDistance", "distance", "nextDistance")), "step_distance_m"),
        total_distance_m=_int_value(_first_present(payload, ("totalDistance", "remainingDistance", "remainDistance")), "total_distance_m"),
        remain_time_s=_int_value(_first_present(payload, ("remainTime", "remainingTime", "remainDuration")), "remain_time_s"),
        road_name=_str_value(payload.get("nextRoadName", payload.get("roadName", ""))),
    )


PROVIDER_NORMALIZERS: Dict[str, Callable[[Mapping[str, Any]], NormalizedNavigationEvent]] = {
    "generic": _normalize_generic,
    "amap": _normalize_amap,
    "baidu": _normalize_baidu,
    "tencent": _normalize_tencent,
}


def provider_names() -> list[str]:
    return sorted(PROVIDER_NORMALIZERS.keys())


def normalize_navigation_event(provider: str, payload: Any) -> NormalizedNavigationEvent:
    payload_mapping = _require_mapping(payload)
    normalizer = PROVIDER_NORMALIZERS.get(provider)
    if normalizer is None:
        raise ProviderAdapterError(f"unsupported provider: {provider}")
    return normalizer(payload_mapping)
