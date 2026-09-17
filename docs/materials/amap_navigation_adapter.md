# 高德导航 Adapter 对齐说明

本文档记录仓库内 `tools/navigation_bridge/map_adapters.py` 的 `amap` 适配入口当前对齐到的字段形态。

## 当前回调入口

仓库当前把高德导航桥接主要对齐到：

- `AMapNaviListener.onNaviInfoUpdate(NaviInfo)`

样本文件：

- `tools/navigation_bridge/examples/amap_event.json`
- `tools/navigation_bridge/examples/amap_navi_info_event.json`
- `tools/navigation_bridge/examples/amap_arrive_event.json`
- `tools/navigation_bridge/examples/amap_long_chinese_event.json`

## 当前支持的字段名

`amap` adapter 会优先从 `payload.naviInfo` 读取；若没有嵌套对象，则回退到扁平字段。

当前已接入字段：

- 转向：
  - `iconType`
  - `turn`
  - `maneuver`
  - `turnType`
- 下一步距离：
  - `segmentRemainDistance`
  - `stepDistance`
- 总剩余距离：
  - `routeRemainDistance`
  - `pathRetainDistance`
- 总剩余时间：
  - `routeRemainTime`
  - `pathRetainTime`
- 路名：
  - `nextRoadName`
  - `currentRoadName`

## 当前 `iconType -> turn_key` 映射

当前仓库先把高德常见转向图标收敛到固件协议已有的 `turn_type 0..6`：

- `2 -> left`
- `3 -> right`
- `4 -> left_front`
- `5 -> right_front`
- `6 -> left`
- `7 -> right`
- `8 -> u_turn`
- `9 -> straight`
- `15 -> arrive`
- `17 -> left`
- `18 -> straight`
- `19 -> u_turn`
- `21 -> left`
- `22 -> right`
- `25 -> left`
- `26 -> right`
- `27 -> straight`
- `28 -> u_turn`
- `65 -> left_front`
- `66 -> right_front`

说明：

- 这是一层“收敛映射”，不是 1:1 保真映射
- 环岛、后方转向、靠左靠右等 richer icon，当前会被压缩到现有固件能显示的有限转向集合
- 若后续固件 UI 扩展更多转向类型，这里应同步细化
