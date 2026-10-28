# 自助元数据更新与旧协议响应兼容

2026-10-28，Windows x64 自动化。Go 参考仍为固定的 1.13.10 提交
`a390d03d28c895742d9b13c0894880ee67733801`。本增量推进 R02/R03，
**不代表 R02/R03 或主服务替代整体验收完成**。

## 已实现

- 自助 `UpdateParticipantMetadata` 的名称超过 256 UTF-8 字节、metadata
  超过 512 KiB、attributes 本次请求的键和值超过 64 KiB 时，返回
  `RequestResponse/LIMIT_EXCEEDED`，保留原 `request_id` 和请求回显。
  不再因抛出异常而关闭参与者连接。
- 校验顺序为自助更新权限、名称、metadata、attributes；全部检查完成后
  才更新状态。失败不改 ParticipantInfo/version、不刷新 token，不产生部分更新。
  错误正文分别为 Go 的 `exceeds name length limit`、
  `exceeds metadata size limit`、`exceeds attributes size limit`。
- 动态撤销 `canUpdateOwnMetadata` 后，返回 `NOT_ALLOWED` 及
  `does not have permission to update own metadata`，即使请求同时超限。
- `/rtc` 从查询参数读取 SDK 和协议，`/rtc/v1` 使用 JoinRequest 中的
  ClientInfo；保存到参与者，用于响应兼容。数字解析按 Go 的 int32 范围处理，
  无效数字使用 0，不把错误格式部分解析成有效协议版本。
- 协议 ≤14 不发送成功 RequestResponse，但继续发送支持的错误响应；
  Rust 协议 <10 不接收 RequestResponse 和 TrackSubscribed，避免旧解码器
  收到不支持的消息。过滤只作用于这些响应，不跳过状态更新或媒体处理。

参考：Go `pkg/rtc/participant.go` 的 `UpdateMetadata/checkMetadataLimits`、
`participant_signal.go` 的 `sendRequestResponse`、`clientinfo.go` 的
`SupportsRequestResponse`、`types/protocol_version.go` 及
`pkg/service/utils.go` 的 `ParseClientInfo`。

此历史产物只实现默认值；后续五项非默认 limit 配置及真实媒体验证见
[内容上限配置兼容验证](GO_LIMITS_VALIDATION.md)，完整 R09 仍未完成。
attributes 校验跟随 Go 对本次请求映射的检查，不宣称对合并后的属性集合
实施了新的总量限制。没有改动 libwebrtc 或新增媒体流水线。

## 差分证据

新增 `tests/go_signal_metadata_parity.mjs`。官方 Web SDK 2.23.0 在独立回环
服务上发布合成 VP8 视频，订阅者通过实际入站 RTP 统计检查持续解码。
测试同时检查服务器响应、错误请求回显、完整参与者状态不变、信令连接
未重新创建、原 PC 对象保持，以及失败后合法 SDK 更新仍能被对端观察到。

| 检查 | 结果 | 报告目录，均含 report.json |
| --- | --- | --- |
| 旧 room-contract-v2，双 PC | Go 15/15，C++ 1/15 | `tests/results/go-signal-metadata-before-corrected/2026-10-28T00-48-50-832Z-858c6a/` |
| signal-metadata-v1，双 PC | **15/15 对照通过** | `tests/results/go-signal-metadata-dual/2026-10-28T00-52-11-427Z-6a5312/` |
| signal-metadata-v1，单 PC | **15/15 对照通过** | `tests/results/go-signal-metadata-single/2026-10-28T00-53-17-079Z-012b78/` |

两种模式都确认实际 SDK PC 模式，未用双 PC 回退代替单 PC。每轮 15 项中，
10 项使用真实 SDK/视频，5 项使用无媒体原始 WebSocket 验证 js/14、js/15、
rust/9、rust/10、rust/15 的消息兼容。后 5 项在两轮中重复；它们不代表真实
旧 Rust SDK 或其他历史 SDK 的完整接入验收。

首轮测试把 LIMIT_EXCEEDED/NOT_ALLOWED 错写成数字 2/1，产生了参考端
断言失败；已改用协议生成的枚举常量，没有修改 Go。错误测试报告保留在
`tests/results/go-signal-metadata-before/2026-10-28T00-46-24-374Z-ee1bc3/`，
不作为服务器行为差异证据。修正后的旧产物失败包含会话已被关闭导致的
后续连带失败，不把 14 项失败当成 14 个独立根因。

## 回归和产物

- 原生 CTest **10/10**：`out/signal-metadata-ctest.log`。包括协议解析边界、
  请求原子性、失败后恢复及旧 Rust 响应过滤。
- RoomService JSON/protobuf **234/234**：
  `tests/results/go-room-contract-signal-metadata/2026-10-28T00-52-53-336Z-75bf25/report.json`。
- 无窗口媒体集成 **9/9**：
  `tests/results/signal-metadata-media-20261028/result.json`。
- JWT 回归：C++ **20/20**，Go **17/18**；Go 在
  `explicit data permission overrides publish default` 遇到 PC 连接失败，
  测试进程退出码为 1，不能视为整轮通过。保留报告：
  `tests/results/go-token-signal-metadata/2026-10-28T00-53-58-284Z-6b49af/report.json`。
  此回归使用旧版 Web SDK 2.15.6，失败根因尚未定位。
- 固定产物：`out/parity-reference/livekit-server-cpp-signal-metadata-v1.exe`。
  SHA-256：`2d33413734380491e817c78eedb14ef3d0f90b4d25f102e50a86d76b4cebe736`。
- 当前源码基于 `ffa48bd` 加本轮变更，包含既有 Studio admission/macOS 合并。
  源码快照、依赖状态、构建/原生测试及二进制哈希：
  `out/parity-reference/signal-metadata-v1-provenance/`。
  构建日志：`out/signal-metadata-build.log`。

```powershell
node tests/go_signal_metadata_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-signal-metadata-v1.exe
# 再加 --single-pc true 验证单 PC
ctest --test-dir out/windows-parity -C Release --output-on-failure
```

未做实际设备人工验收。本轮不关闭历史间歇视频、四轨/跨系统失败，不替代
配置全覆盖、one-shot、全部信令竞态和协议矩阵、集群、外部 worker、压测、
24 小时稳定性及灰度回滚验收。
