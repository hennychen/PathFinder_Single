# 语音集成决策

## 当前结论

- `xiaozhi-esp32` 选用“组件化引入”策略，不走子仓库整体嵌入
- 工程内通过 `components/voice_backend/` 作为唯一适配层承接第三方语音栈
- `service_voice` 只依赖统一状态回调与后端元信息，不直接依赖 `xiaozhi-esp32` 的目录结构

## 选择理由

- 当前仓库仍处于单板 bring-up 阶段，组件化引入更容易保持主仓整洁
- `service_voice`、`drivers_audio`、UI 抢占逻辑已经先落在本仓，后续只需要把 `voice_backend` 的占位实现替换为真实接线
- 后续若 `xiaozhi-esp32` 升级或需要切换到板卡裁剪版，影响范围可收敛在 `voice_backend`

## 当前代码边界

- `drivers_audio`：负责板载 MIC / PCM5101 的 I2S 初始化、enable，以及最小读写/静音帧接口
- `voice_backend`：负责描述语音后端供应商、接入方式、状态回调、session start/stop 和最小帧处理 hook；当前占位实现已能自动跑出 `wake -> streaming -> think(silence) -> speak(mock payload) -> idle` 闭环
- `service_voice`：负责把后端状态映射到 `WorkflowState` 和 `PAGE_VOICE` 抢占逻辑，并通过 `task_voice` 命令队列统一串行化状态流；当前已补 capture/playback tick、会话生命周期日志，以及运行时统计发布，运行态已细化到阶段停留时长、输入/输出帧计数、状态切换次数和输入/输出文本
- `service_ui`：当前已消费 `voice_runtime` 字段，为 `PAGE_VOICE` 渲染增强版首屏语音页（状态 chip、相位文案、中心 orb、表情词、小波条、输入/输出双气泡提示、阶段时间线、动态摘要/速率统计、阶段停留/帧计数说明）；双气泡文案已优先消费 `voice_runtime.input_text/output_text`

## 下一步

- 用真实 `xiaozhi-esp32` 组件替换 `voice_backend` 当前占位实现
- 对齐小智板卡配置中的采样率、位宽、AFE 管线和播放链路
- 在 `voice_backend` 中把当前最小 runtime hook 替换为真实 AFE 输入、编码/解码输出和 TTS 播放链路
- 把当前 `PAGE_VOICE` 增强版首屏语音页继续推进为更完整的表情 / 波形 / 对话气泡 UI；当前已先落输入/输出双气泡提示、阶段时间线、动态摘要/速率统计、阶段停留/帧计数说明，以及基于状态总线发布的会话文本，后续仍可继续补更强的波形动画与真实文本真链路
