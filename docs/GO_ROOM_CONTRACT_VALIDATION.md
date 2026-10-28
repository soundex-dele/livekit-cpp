# RoomService 参数与线协议兼容增量

2026-10-27，Windows x64 Release，自动化验证。固定 Go 1.13.10 基线不变。
本轮推进 R00/R01；**完整主服务替代、R01 整体和实际设备验收均未完成**。

## 实现

- 补齐 UpdateParticipant 名称的 256 UTF-8 字节限制，保持 metadata 512 KiB、
  attributes 键和值合计 64 KiB 的独立限制。UpdateParticipant 和
  UpdateRoomMetadata 在权限与状态查找前校验边界；CreateRoom 保持权限优先。
  错误的状态、code、msg、meta.argument 与固定 Go 对齐。
- Get/Remove/UpdateParticipant 在房间不存在时返回 participant not found；
  UpdateSubscriptions 和 SendData 的不存在端点返回 503/unavailable。
  SendData 在查询房间前检查 nonce，并保留 Go 的具体长度错误。
- 未配置的 room preset 返回 400/invalid_argument，且不创建房间。
- Twirp 缺少 token 时以空 grants 进入路由和解码，随后仍须通过方法权限检查。
  非 POST、无效 Content-Type 返回 404/bad_route；拒绝以合法 MIME 字符串开头
  的错误类型，继续接受 MIME 参数。
- JSON 只解码首个值；未知字段可忽略，已知字段、snake_case/lowerCamelCase
  别名、嵌套字段和 map 键的重复会拒绝。Go `EmitUnpopulated` 的缺省消息
  `null` 输出已补齐；未设置的 oneof/proto3 optional 仍不输出。
- SendData 接口不再额外拒绝 60,001 字节的数据；API 成功不代表 SCTP 已交付。
  超过当前 64 KiB 编码 DataPacket 接收边界的消息在入队前丢弃，避免可靠队列
  堵塞或把超大 API 消息误判为订阅者背压并踢人。此边界不代表完成 R07 大包兼容。

未新增音视频流水线；未修改 libwebrtc；生产 C++ 进程没有启动或转发到 Go。

## 同输入差分

`tests/go_room_contract_parity.mjs` 启动独立 Go/C++ 进程，使用专用回环端口、
随机临时密钥及房间。检查全部 14 个方法的无权限、仅入房权限、无 token 拒绝，
房间权限隔离，以及 roomCreate/roomList/roomAdmin 的独立允许和拒绝路径。
两种格式还覆盖边界、错误优先级、无效请求与拒绝后的状态。

每项除两端比较外还断言已核验的参考 HTTP 状态；参考服务异常不能计作 C++
通过。报告保留两端响应、时间、二进制和测试哈希、脱敏配置；进程退出时删除
含临时密钥的配置。基础设施错误返回 2，行为/契约失败返回 1。

| 同一份最终测试 | 结果 | 报告 |
| --- | --- | --- |
| 旧 config-yaml-v1 产物 | 123/234 | `tests/results/go-room-contract-before-verified/2026-10-27T11-40-59-325Z-93132b/report.json` |
| room-contract-v2 产物 | **234/234** | `tests/results/go-room-contract-verified/2026-10-27T11-42-47-724Z-9b8e5c/report.json` |

首轮 203 项、修订用例名称后的 208 项报告和中间产物记录也保留在
`tests/results/go-room-contract-before*`、`go-room-contract-after` 中，未覆盖历史失败。

检查中仅对解析器/路由错误比较 HTTP 状态与 Twirp code；业务错误比较完整
错误对象。房间随机 SID 检查格式，时间不跨服务比较。`enabled_codecs` 和
`turn_password` 属于 R04/R08 剩余范围，明确不纳入本轮字段一致性断言。
这不是所有请求字段、错误文本、认证失败或所有合法 JSON 输入的完整矩阵；
当前 JSON 深度保护仍为 100，未声称与 Go 的全部深层输入行为相同。

## 回归与产物

- CTest **10/10**：`out/room-contract-ctest-final.log`。新增 Twirp 测试覆盖
  JSON framing、重复别名、嵌套 map/数组及 null 输出；core 覆盖超限名称
  不产生部分更新，以及超大 API 数据不踢人。
- 原有 JSON 房间 API **36/36**：
  `tests/results/go-room-contract-regression/20261027T114204Z-3c577a/report.json`。
- 官方 SDK 参与者套件新增 JSON/protobuf 两项：向实际发布视频的参与者提交
  超限 name/metadata/attributes，并同时尝试改元数据、撤销权限；检查拒绝后
  完整 ParticipantInfo/version 不变、连接仍在且订阅视频继续解码。
  首轮两端新增检查均通过，但 Go 后续 kind 用例发生 PC 建连失败，整体失败：
  `tests/results/go-participant-room-contract/2026-10-27T11-42-05-148Z-706287/report.json`。
  C++ 首轮完成全部 21 项；该参考失败不能记成整轮通过。
- 同条件参与者复查仍因 Go 的后续 dynamic-recorder 建连失败而整体失败，
  C++ 再次 **21/21**；两端新增状态保持/解码检查再次通过：
  `tests/results/go-participant-room-contract-recheck/2026-10-27T11-44-40-242Z-b5f582/report.json`。
- 独立 C++ 官方 SDK 无头媒体集成 **9/9**，含实际音视频解码、定向数据、
  metadata、静音、重订阅、三层 VP8、动态权限和删除清理：
  `tests/results/room-contract-media-20261027/result.json`。
- RPC 回归首轮在 Go 初始 callee PC 建连时失败，未进入用例，返回基础设施
  错误 2，不能记为 0/0 通过：
  `tests/results/go-rpc-room-contract/2026-10-27T11-46-58-794Z-482f52/report.json`。
- 同条件 RPC 复查 **50/50** 契约通过，本轮 50 项响应均相同；保留原有退出
  竞态的两个已核验 Go 错误分支，没有缩减门槛：
  `tests/results/go-rpc-room-contract-recheck/2026-10-27T11-49-09-049Z-68ff3f/report.json`。

固定二进制：`out/parity-reference/livekit-server-cpp-room-contract-v2.exe`。
SHA-256：`97475e0cfffac0766d9dd48c04e82fce4dc4703abe0e2df24dec55e43e95b0bc`。
构建日志：`out/room-contract-build-final.log`。
源码快照、工作树差异、依赖提交/状态、WebRTC 库及原生测试哈希：
`out/parity-reference/room-contract-v2-provenance/`。
其中 `regressions.json` 关联通过、失败和基础设施错误报告，并检查源码/产物漂移。
产物与报告被 Git 忽略，保存在当前工作区，不等于已经发布或打包交付。

```powershell
node tests/go_room_contract_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-room-contract-v2.exe
ctest --test-dir out/windows-parity -C Release --output-on-failure
```

Redis/PSRPC、外部 Egress/Ingress/SIP/Agent worker、剩余编码/网络路径、Linux
本轮重建、弱网容量及 24 小时稳定性、实际设备与灰度回滚验收仍未由本轮完成。
既有间歇视频、四轨帧数及跨系统失败不因本轮控制面通过而关闭。
本轮缺失端点差分验证响应契约，尚未完成 Go RPC 端点发现重试及超时等待的
完整时序对照；也没有实现 Redis/PSRPC。
