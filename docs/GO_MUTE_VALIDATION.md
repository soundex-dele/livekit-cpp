# 管理员静音策略对照

2026-10-27，Go 1.13.10 固定基线 `a390d03d…`。本次推进 R01/R09 的一个子集，
完整主服务替代仍未完成。

## 行为与实现

- 默认 `room.enable_remote_unmute=false`。管理员请求取消静音时返回 HTTP 412、
  Twirp `failed_precondition`、`remote unmute not enabled`；拒绝时不得发取消静音
  信号或修改轨道状态。发布者主动取消静音不受该管理 API 配置限制。
- JSON 配置新增 `room.enable_remote_unmute`，显式 true 时允许管理员取消静音。
  此处只新增这一配置项；完整 Go YAML/CLI/env 配置兼容尚未实现。
- 对未知/空轨道 SID 的静音返回成功且无 track；允许远程取消静音时，未知轨道
  同样返回成功且无 track；默认拒绝取消静音的检查先于轨道查找。
- Go 此方法直接调用 participant RPC。没有参与者端点时返回 503 / `unavailable`
  / `no response from servers`；权限检查仍在资源及策略检查之前。C++ 本地分派
  对齐最终错误，不表示已实现 Go 的 PSRPC 等待/重试或多节点路由。
- 重复静音不再重复广播参与者状态；管理请求仍向发布者发送对应 mute 信号。
  已登记但尚未建立媒体的轨道也更新静音状态。

## 测试入口与修复前证据

`tests/go_mute_parity.mjs` 使用官方 Web SDK 2.23.0 和无头 Chrome，以同一套
JSON/protobuf 请求检查方法响应、发布者/订阅者静音状态、发布者主动恢复后的
实际音频能量推进、重复请求、未知轨道、缺失参与者和非管理员拒绝。
成功响应比较 SID 对应关系、type、muted、MIME 以及空 track 语义；原始响应保留，
不宣称全部 TrackInfo 字段或 JSON 缺省字段呈现完全一致。

修复前默认配置 Go 完成全部 21 项，C++ 只有 **9/21** 与其匹配：
`tests/results/go-mute-before/2026-10-27T08-34-14-763Z-2bd37c/report.json`。
C++ 错误允许远程取消静音；未知轨道返回 404，缺失参与者也返回 404；两侧 JSON
成功响应采用 snake_case，后续测试补正 MIME 的字段读取，不放宽错误断言。

原有 `tests/integration.mjs` 改为确认默认远程取消静音被拒绝，再由发布者主动
恢复；显式允许管理员恢复由此差分套件覆盖。

## 当前验收结果

默认、显式 true、显式 false 各 **21/21**，合计 **63/63** Go/C++ 对照通过：

- `tests/results/go-mute-default/2026-10-27T08-52-14-552Z-ed5160/report.json`
- `tests/results/go-mute-true/2026-10-27T08-52-40-249Z-1a0907/report.json`
- `tests/results/go-mute-false/2026-10-27T08-54-22-268Z-eef87a/report.json`

同产物原生 CTest **8/8**，日志 `out/g711-mute-ctest.log`；房间 API **36/36**：
`tests/results/go-room-g711-mute/20261027T085346Z-76e276/report.json`。
原生测试另检查待发布轨道、重复通知、拒绝时零信号副作用及缺失房间错误优先级。
配置负向检查 **5/5**：拒绝 null/数组 room、未知字段和非布尔开关，结果为源码
快照目录的 `config-validation.json`。

固定产物：`out/parity-reference/livekit-server-cpp-g711-mute-v1.exe`。
SHA-256：`ceb77e887bedfe9727bfce18dc2f7f41a0dfbb4d64e69cff485d5d473845a307`。
构建日志：`out/g711-mute-build.log`；源码快照与依赖哈希：
`out/parity-reference/g711-mute-v1-provenance/`。
共享工作区 HEAD 已由并行任务提交为 `2018d35…`，快照仍包含本任务未提交实现；
该提交不等于整个工作区已提交或全部验收。

同产物音频 Go/C++ **18/18**，首轮参考 ICE 失败保留；混合视频及 720p 回归仍
失败，详见 [音频编码验证的后续回归](GO_AUDIO_CODEC_VALIDATION.md)。静音 63/63
不等于整体媒体门禁通过。
同产物原有单 PC 收发/恢复回归 **13/13**：
`tests/results/go-single-pc-g711-mute/2026-10-27T09-07-06-540Z-c791e0/report.json`。
源码快照目录的 `regressions.json` 同时关联通过及失败报告；`workspace-drift.json`
检查快照之后实现及测试源码是否变化。

所有结果均为 Windows 自动化。动态权限、完整 TrackInfo 字段、PSRPC 超时/重试
和跨节点生命周期仍需独立验收。
