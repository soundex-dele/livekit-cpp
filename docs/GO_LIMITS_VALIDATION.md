# 内容上限配置兼容验证

2026-10-28，Asia/Shanghai；Windows x64 自动化。本增量推进 R01/R02/R09，
**不表示这些工作包或主服务替代已完成**。

## 实现范围

- YAML/JSON `limit.max_room_name_length`、`max_participant_identity_length`、
  `max_participant_name_length`、`max_metadata_size`、`max_attributes_size`。
  默认值分别为 256、256、256、524288、65536 字节；按 UTF-8 字节计数。
- `0` 表示取消对应内容上限。保留 Go 有符号长度上限为负时拒绝所有值的语义；
  metadata/attributes 为 uint32，拒绝负数、溢出、错误类型。null 保留默认值。
  尚未实现的 `limit` 字段和拼写错误明确拒绝，不静默忽略。
- 非零的旧 `room.max_room_name_length`、`max_participant_identity_length`、
  `max_metadata_size` 覆盖 `limit` 中的对应值；旧配置的零值不覆盖。
- CreateRoom、UpdateParticipant、UpdateRoomMetadata、RTC 入房及自助元数据
  更新共享配置。保留 API 与信令各自的权限／参数校验顺序、错误正文和原子性。
- `/rtc` 与 `/rtc/v1` 的 validate/升级前校验检查 identity 和房间名；空值仍拒绝，
  超长错误采用 Go 的状态码与文本。后续更新的名称上限不额外施加到初始 JWT 名称。
- JWT 原有 32 KiB 上限会先于合法 metadata 上限拒绝入房，现调整到 HTTP 的
  1 MiB header envelope；大 metadata/attributes JWT 已通过真实入房对照。
  取消内容上限不取消 HTTP／WebSocket 的独立传输上限。

源码依据为固定提交 `a390d03d28c895742d9b13c0894880ee67733801` 的
`pkg/config/config.go`、`pkg/service/utils.go`、`roomservice.go`、
`roommanager.go` 及 `pkg/rtc/participant.go`。当前同级 Go checkout 已变化，
本轮通过 `git show <固定提交>:<文件>` 核对，继续使用原固定参考二进制，未更改基线。

## 自动化结果

`tests/go_limits_parity.mjs` 自建回环 Go/C++ 进程，以相同请求检查两端；
报告保存配置（密钥脱敏）、二进制及测试源码哈希、逐项错误和原始进程日志。
正向路径有显式断言，参考端失败不会被判为 C++ 通过。

| 检查 | 结果 | 报告目录（含 report.json） |
| --- | --- | --- |
| 修复前 track-update-v1 | Go 118/118；C++ 五种配置均拒绝启动，退出码 2 | `tests/results/go-limits-baseline-corrected/2026-10-28T01-43-07-983Z-f54aef/` |
| 修复后 limits-v2 | **118/118** | `tests/results/go-limits-final/2026-10-28T01-45-10-453Z-cd2a5d/` |
| 非默认上限 SDK 双 PC | **15/15** | `tests/results/go-limits-media-dual/2026-10-28T01-45-57-448Z-a1faa7/` |
| 非默认上限 SDK 单 PC | **15/15** | `tests/results/go-limits-media-single/2026-10-28T01-46-49-438Z-e7fd89/` |
| RoomService JSON/protobuf 回归 | **234/234** | `tests/results/go-room-contract-limits/2026-10-28T01-45-22-197Z-d49325/` |
| 配置入口回归 | **16/16** | `tests/results/go-config-limits/20261028T014605Z-f61efc/` |
| 原生 CTest | **10/10** | `out/limits-ctest-final.log` |
| 常规无窗口媒体集成 | **9/9** | `tests/results/limits-media-integration-20261028/result.json` |

118 项由小上限 31 项、零上限 23 项、非零旧配置覆盖 31 项、旧配置零值不覆盖
31 项、有符号负上限 2 项构成。覆盖 JSON/protobuf 边界、UTF-8 字节、校验顺序、
失败后状态不变、两种 validate 路由、实际 JWT 入房、自助更新及失败后恢复。
这套无媒体线协议测试不替代媒体验收。

媒体检查使用官方 Web SDK 2.23.0 和无窗口 Chrome，发布合成 VP8 视频；
名称／metadata／attributes 上限分别设为 64/128/64 字节。每种模式 15 项中
10 项检查真实解码、错误回显、状态/claims 不变及原 PC 保持，5 项为重复的旧协议
无媒体响应探针。单 PC 检查实际 PC 模式，无双 PC 回退。

## 失败记录与可复现产物

- 首轮 `go-limits-before/2026-10-28T01-35-46-058Z-649eb3/` 使用了旧协议包，
  JSON 解码遇到新权限字段失败；已改用现有现代 SDK 配套协议包。
- `go-limits-baseline/2026-10-28T01-39-30-916Z-9d48b4/` 的空 identity 探针
  被 SDK 的签名校验提前拒绝；修正为自签 HS256 测试令牌后，Go 118/118。
- 首次新产物 `go-limits-verified/2026-10-28T01-41-01-941Z-6c4594/` 为
  95/118，包含上述测试夹具错误、32 KiB JWT 拒绝及构建期间编辑造成的旧错误
  文本残留。limits-v2 在源码稳定后重建，重新通过全部差分及 CTest。
- 最终二进制：`out/parity-reference/livekit-server-cpp-limits-v2.exe`，SHA-256
  `aaed01162b256491e9b4a6e2831122a67805061e37f35191f24f60a0b8e08352`。
- Go SHA-256：`c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98`。
- C++ 源码基于 `36c85467fe753a5b03b2f9968c376424fbccb00b` 加工作区变更，
  包含接续的轨道更新。源码快照、文件哈希、依赖状态、构建/原生日志和回归摘要：
  `out/parity-reference/limits-v2-provenance/`。

```powershell
node tests/go_limits_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-limits-v2.exe
node tests/go_signal_metadata_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-limits-v2.exe `
  --configured-limits true --single-pc true
ctest --test-dir out/windows-parity -C Release --output-on-failure
```

本轮未做 Linux 重建或实际设备人工验收。完整配置、消息体/订阅/轨道数量/数据速率
配额、集群、外部 worker、历史媒体稳定性问题、容量/弱网/24 小时及灰度回滚仍待完成。
