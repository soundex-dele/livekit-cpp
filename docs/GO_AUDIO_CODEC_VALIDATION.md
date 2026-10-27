# PCMU/PCMA 音频兼容验证

2026-10-27，Windows x64 自动化。Go 1.13.10 固定基线 `a390d03d…`，协议
`f09f87aa…`。主服务完整替代仍未完成，本记录不代表真机或生产验收。

## 实现与音频证据

- SFU 接受单声道 PCMU/PCMA，RTP 时钟为 8000 Hz；Opus 为 48000 Hz。
  原生接收器使用每条轨道的协商时钟，不再把全部音频按 48 kHz 处理。
- 原生 RTP 测试检查 PT 0/8/111、音频分类、载荷字节、时间戳及回绕保真，
  拒绝非 8 kHz 或双声道 G.711。原生 CTest **8/8**，日志
  `out/audio-codec-ctest.log`。
- 官方 Web SDK **2.23.0**、无头 Chrome **156.0.8078.48**，Go/C++ **18/18**：
  单/双 PC 各 9 项，包含 PCMU、PCMA、Opus 混合、反向发布、TrackInfo MIME、
  静音恢复、重订阅、迟加入及信令恢复。Web Audio 检查实际解码的 440、880、
  1320、1760 Hz 音调、RMS、能量、样本推进和 RTC 编码/时钟，不只检查包计数。
  报告：`tests/results/go-audio-codec-verified/2026-10-27T08-16-39-347Z-d43237/report.json`。

SDK 无公开 audioCodec 选项；测试在 LocalSenderCreated 事件中对对应浏览器
transceiver 设置 codec preferences，两端采用相同方法。G.711 必须显式设置
64 kbps 的 audioPreset；默认 speech preset 使 Chrome 在应用 Go 应答时失败，
之后 SDK 重连回退 Opus，不能把这种回退算为 G.711 通过。

固定产物：`out/parity-reference/livekit-server-cpp-g711-v1.exe`。
SHA-256：`3e5596cb2b4a32591944c0b6a6eceac002e3090fac8f75e571a2a8b17ae49490`。
Go SHA-256：`c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98`。
源码快照与依赖哈希：`out/parity-reference/g711-v1-provenance/`。
该产物包含共享工作区的 Studio 授权改动，不代表本轮已完整验收 Studio 功能。

## 回归及未关闭问题

同产物房间 API **36/36**：
`tests/results/go-room-g711-regression/20261027T082314Z-a8b75e/report.json`。
原有单 PC 媒体/恢复 **13/13**：
`tests/results/go-single-pc-g711-regression/2026-10-27T08-25-29-418Z-c1bf34/report.json`。

视频回归尚未关闭：首轮 H.264/VP8 混合 **13/14**，单 PC 迟加入者 VP8 未持续
解码，`go-codec-g711-regression/2026-10-27T08-23-18-394Z-f1aaf7`；原有媒体集成
完成前 7 项后 720p 超时，`go-g711-media-regression-20261027`。串行 H.264 重跑
为 **7/14**，首条单 PC H.264 不解码，日志同时出现 BUNDLE PT102 参数冲突警告：
`go-codec-g711-serial/2026-10-27T08-27-22-157Z-7a5465`。
旧 H.264 固定产物同机对照 **14/14**：
`go-codec-g711-prior-control/2026-10-27T08-30-12-470Z-00ae36`。
这些失败不能简单归为机器负载，也不能被 18/18 音频结果掩盖。

开启原生 info 日志后的同一 G.711 产物 H.264 对照 **14/14**：
`go-codec-g711-diagnostic/2026-10-27T08-39-19-824Z-086a9f`。核查原生源码与独立
SDP 探针后确认 M137 对上述 PT 参数警告仅记录直方图，并不拒绝 SDP；探针中的
“必须拒绝冲突”断言不符合当前库行为，已移除，日志保留在
`out/g711-bundle-before-ctest.log`。未据此改写 SDP，也未把该警告作为已证明根因。
诊断轮通过不能单独关闭间歇视频失败。

## 后续静音策略产物的回归

静音修复后的固定产物 `livekit-server-cpp-g711-mute-v1.exe`，SHA-256
`ceb77e887bedfe9727bfce18dc2f7f41a0dfbb4d64e69cff485d5d473845a307`：

- 音频首轮 Go 完成双 PC 9 项后 ICE 建连失败，C++ 完成 18 项，整体失败：
  `go-audio-g711-mute/2026-10-27T08-55-18-338Z-568544`。
- 保持超时和断言重跑，Go/C++ 完整 **18/18**：
  `go-audio-g711-mute-recheck/2026-10-27T08-57-54-457Z-a4959c`。
- H.264/VP8 回归 Go 在初始建连失败，C++ 完成 9 项后在单 PC 混合发布时 H.264
  解码停滞，不能算差分通过：`go-codec-g711-mute/2026-10-27T08-59-29-072Z-e82d08`。
- 原有媒体集成完成前 7 项，720p 切换超时：`go-g711-mute-media-20261027`。
  全部是自动化结果，视频稳定性和完整替代仍未验收。
- 同机用旧 H.264 固定产物及其原始集成脚本对照 **9/9**，包括三层 VP8：
  `go-g711-prior-media-20261027`。这进一步要求定位新产物的视频失败；并非已证明
  是 G.711 时钟修改导致，也不能据此排除时序/网络波动。
- 同一静音产物的原有单 PC 收发/恢复 Go/C++ **13/13**：
  `go-single-pc-g711-mute/2026-10-27T09-07-06-540Z-c791e0`。

## 保留的前置失败

后续持续解码、多轮重订阅及传输诊断见 [视频稳定性排查](GO_VIDEO_STABILITY.md)。
新通过记录不覆盖本页历史失败。

- `go-audio-codec-before/2026-10-27T07-44-39-279Z-8b5ccc`、
  `go-audio-codec-fixture/2026-10-27T07-50-01-956Z-19c470`、
  `go-audio-codec-mono/2026-10-27T07-59-49-675Z-bb17d4`：默认码率导致
  Go 应答被浏览器拒绝，均不能作为有效 G.711 基线。
- 修正码率后的 Go 诊断 **18/18**：
  `go-audio-codec-bitrate/2026-10-27T08-09-47-348Z-fe1e45`。
- 有效修复前对照：Go **18/18**，旧 H.264 C++ 产物首项 PCMU 即失败，后续项未
  执行：`go-audio-codec-valid-baseline/2026-10-27T08-12-04-370Z-ffc0cc`。

上述目录除明确注明外均在 `tests/results/`。完整弱网、容量、设备/浏览器组合、
外部 SIP worker、RED 等其他音频路径仍需独立验证。
