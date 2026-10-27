# H.264 编码转发兼容验证

2026-10-27，Windows x64 自动化。Go 固定基线 1.13.10 / `a390d03d…`，
协议 `f09f87aa…`。本次推进 R03/R04；完整主服务替代仍未完成。

后续状态：本文记录的默认 remote-unmute 差异已由 [静音增量](GO_MUTE_VALIDATION.md)
修复。后续 G.711 产物回归出现间歇视频失败，见 [音频编码验证](GO_AUDIO_CODEC_VALIDATION.md)；
本文的 14/14 属于下述固定历史产物，不能覆盖之后的失败。

## 实现与范围

- SFU 协商能力与软件编码器解耦，声明 VP8 和 H.264 packetization-mode=1。
  H.264 的 native 编码/解码端点只供 PeerConnection 协商；若意外收到帧处理调用，
  会计入审计并返回错误。转发路径不使用 Go，也不进行视频编解码。
- 根据 AddTrack 请求的编码关联发布 CID，按源轨道约束订阅编码、profile 和
  packetization mode。不能依赖浏览器 SDP 的第一个编码，它仍可能是 VP8。
- OnTrack 早于完整应答，待本地描述生效后再读取轨道参数。BUNDLE 下不能把原生
  channel 的编码合集作为每条轨道的编码，使用对应 MID 的实际协商结果。
- H.264 复用原生 depacketizer、H26xPacketBuffer、reference finder 和 packetizer，
  处理 STAP-A、FU-A、完整帧、依赖与重新分包；NACK/RTX 继续走原生模块。
  保留发布者 SPS/PPS 原始内容，不沿用原生接收解码器为低延迟改写 SPS VUI 的结果。
- 原生合成测试用真实 SPS/PPS 语法和大 IDR 测试 STAP-A/FU-A，丢失一个片段时不得
  转发半帧；RTX 恢复后，上下行重新组帧的全部 Annex-B 字节应相同，PT/SSRC 应映射。
  合成 slice 本身不声称可解码，真实解码由浏览器用例覆盖。

`tests/go_codec_parity.mjs` 使用官方 Web SDK 2.23.0、无头 Chrome，单/双 PC 各
7 项：实际 H.264 解码（禁止 VP8 回退）、TrackInfo 编码、同房间 VP8/H.264 混合、
双向 H.264、发布者静音恢复、撤订重订、迟加入的多编码解码。统计检查 640×360
画面与持续推进的解码帧；服务端检查零视频编解码调用和实际转发帧。

## 当前验证

官方 Web SDK **2.23.0**、无头 Chrome **156.0.8078.48**，正式 Go/C++ 对照
**14/14**，单 PC 和双 PC 各 7 项：
`tests/results/go-codec-verified/2026-10-27T07-28-07-653Z-b54511/report.json`。
该报告 C++ 累计组装 561 帧、转发 608 帧（包含混合 VP8），视频编码/解码调用均为 0。
H.264 实测为 mode 1；C++ 本次选择 baseline `42001f`，Go 选择 constrained baseline
`42e01f`，两侧浏览器均实际解码。本轮不声称 SDP/profile 选择逐字相同。

同产物 CTest **8/8**，日志 `out/h264-v3-ctest.log`；房间 API **36/36**，报告
`tests/results/go-room-api-h264-v3/20261027T072843Z-2b9d38/report.json`。
原有单 PC 媒体/恢复/扩容 **13/13**：
`tests/results/go-single-pc-h264-regression/2026-10-27T07-30-18-150Z-d139d5/report.json`。
原有媒体集成 **9/9**，包含三层 VP8 选层与恢复：
`tests/results/go-h264-media-regression-20261027/result.json`。
数据回归单 PC **22/22**、双 PC **22/22**：
`tests/results/go-data-h264-single-recheck/2026-10-27T07-34-50-712Z-441eb7/report.json`；
`tests/results/go-data-h264-dual/2026-10-27T07-35-45-515Z-95db80/report.json`。
这些回归使用同一个固定 C++ 产物；正式编码报告目录的 `regressions.json` 关联
报告与哈希。更早的 RPC/ACL/参与者全矩阵未在此产物重跑，仍引用各自历史产物。

固定 C++ 副本：`out/parity-reference/livekit-server-cpp-h264-v3.exe`。
SHA-256：`a91b3b73c6e230467c91090b3b5e612f793acf1b25883b68a45c56f2a424d282`。
Go SHA-256：`c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98`。
正式差分目录附 `provenance.json` 和 `sources.zip`，含未提交源码、原生依赖哈希与
构建参数。构建日志为 `out/h264-bundle-build.log`。生产实现不依赖参考 Go 进程。

快照之后共享工作区于 15:29 增加了并行任务的 Studio grant/入房授权改动，涉及
`auth.hpp`、`auth.cpp`、`http.cpp` 等；这些改动保留，未包含于本轮固定产物。
报告目录的 `workspace-drift.json` 记录差异；不能把本轮通过描述为之后整个工作树
已经通过编译或集成验证。

## 保留的失败与诊断

- `go-codec-before/2026-10-27T06-41-19-694Z-016653`：最初用管理员远程取消
  静音，Go 默认拒绝。测试改成发布者主动恢复。管理 API 的 remote-unmute 默认
  策略差异仍需修复；没有通过放宽 Go 错误断言来接受 C++ 的行为。
- `go-codec-baseline/2026-10-27T06-55-54-804Z-698239`：修正用例后 Go 14/14，
  旧 data 产物 C++ 首项即失败；后续项未执行。不能写成 C++ 执行了 14 项。
- `go-codec-parity/2026-10-27T06-44-01-841Z-321e66`：Go 14/14，初版 C++
  在 OnTrack 读取空参数后退出，未完成解码。
- `out/h264-ctest.log`：通用原生 H.264 assembler 缺少 Annex-B 恢复步骤，
  字节保真断言失败。改用原生 H26xPacketBuffer 后该断言通过。
- `go-codec-fixed/2026-10-27T06-59-15-818Z-9ba405`：仍回退 VP8；发现编辑与
  上一轮编译重叠导致旧对象文件被沿用，强制重新编译相关文件。
- `go-codec-h264/2026-10-27T07-10-19-300Z-ed58c3`：Go ICE 建连失败，不能作
  差分通过依据；C++ 仍回退 VP8，进一步修复 AddTrack 编码与媒体段的关联。
- `go-codec-requested-diagnostic/2026-10-27T07-20-37-340Z-5f1eda`：仅 C++ 诊断，
  双 PC 7/7，单 PC 首项失败；发布端已编码 H.264，订阅端却按 VP8 接收。
  修复 BUNDLE 编码合集与 MID 编码之间的混淆。该记录不是 Go/C++ 完整差分。
- `out/h264-v2-ctest.log`：RPC 单测在繁忙机器上 run_for 返回前未派发已到期
  回调，读到上一条成功结果；补充非阻塞 poll，保持 ACK/response 超时值不变。
- `go-data-h264-single/2026-10-27T07-33-16-890Z-115b61`：Go 完成前 20 项后，
  legacy-agent 的 ICE 建连失败；C++ 完成 22 项。虽然报告有 20/20 已执行项匹配，
  整体 exit=1，不能算完整数据差分通过。未放宽超时或删除失败用例。

## 尚未覆盖

H.264 mode 0、所有 profile/level/浏览器与设备组合、H.264 simulcast、带外 SPS/PPS、
全部丢包重排/参数变化边界仍需独立验收。其他编码 PCMU/PCMA/RED/VP9/AV1/H.265、
SVC、E2EE、完整弱网/容量、跨系统旧失败、集群和外部 worker 不由本次通过替代。
所有结果均为自动化，不代表实际设备人工验收或生产灰度。

测试使用独立进程和随机 HTTP 端口，UDP Go 61400–61580、C++ 61600–61780。
`--only cpp`/`--only go` 仅供诊断；正式对照必须不传 `--only`，两端完成全部 14 项。

```powershell
node tests/go_codec_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-h264-v3.exe
```
