# Go 主服务替代：RPC 与跨房 API 失败契约

日期：2026-10-27（Asia/Shanghai）。这是 R01/R07 的增量验证，主服务完整替代
仍未完成。范围和余项见 [替代计划](GO_REPLACEMENT.md)。

后续 JWT 回归观察到 Go 在“RPC 接收者退出”时存在 404/503 两种结果，取决于
参与者删除、PSRPC 重试和 endpoint 注销顺序。该场景的原始失败、收紧的清理
检查及允许结果集合见 [JWT 验证记录](GO_TOKEN_VALIDATION.md)；下文保留本轮
历史记录，不再将一次 503 响应视为所有退出竞态的唯一结果。

## 实现范围

- 新增 `RoomService.PerformRpc`，JSON/protobuf 均可调用官方浏览器 SDK 的 RPC
  handler；调用通过实际 WebRTC reliable data channel，不代理到 Go 服务。
- 请求 UUID、caller identity、版本、15,360 字节 UTF-8 负载限制、默认 15 秒响应
  超时、7 秒 ACK 超时及错误映射对齐固定 Go 基线。回复绑定参与者 SID，其他
  参与者伪造的 ACK/response 不能完成请求。
- HTTP 断开、请求截止时间、参与者断开、服务关闭均回收 pending 请求和计时器。
  HTTP 等待期间不占用房间线程；响应写入时重新设置写超时。
- 单机参与者 API 调度对齐默认 PSRPC 的三次可恢复错误尝试及每次一秒的无目标
  查找窗口；期间入房可在后续尝试接收请求。不存在目标最终返回
  `503/unavailable/no response from servers`。
- `ForwardParticipant`/`MoveParticipant` 对齐源房间 admin 与 destinationRoom
  grant、同房间错误（含 Twirp `meta.argument`）、目标查找、超时和失败响应。
  固定 Go 开源后端本身返回 `500/unknown/not implemented`，这里没有实现或声称
  跨房迁移成功；拒绝后源房间参与者保持不变。

上述调度仅为本地行为实现，**没有实现 Redis/PSRPC 线协议**，也没有完成独立
Egress、Ingress、SIP 或 Agent worker 兼容。`rpc_pending` 统计这三类异步 API
的待完成调度，用于验证取消后的回收；不能代表完整生产可观测性。

Go 的 wire timeout 使用 uint32 的 `response_timeout_ms - RTT`，RTT 较大时会
回绕。C++ 保留该基线字段行为，但服务端实际计时器仍使用原始 timeout；原生
测试确认回绕不会延长服务端截止时间。

## 基线与产物

- Go 1.13.10：`a390d03d28c895742d9b13c0894880ee67733801`，源码、构建及
  protocol 固定信息见 [首批验证](GO_REPLACEMENT_VALIDATION.md)。
- C++：平台 `livekit-cpp` 子模块 `a21f3c0` 加未提交工作区修改。
  Windows x64 Release，专用 `out/windows-parity`；未覆盖其他任务的正式产物。
- Chrome 156.0.8078.47，无头模式；Node 24.13.0，Web SDK 2.15.6、Server SDK
  2.13.3。复用旧仓库已安装测试依赖，未修改旧仓库源码。

| 产物 | SHA-256 |
| --- | --- |
| Go 参考服务 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| RPC 修复后、跨房失败契约修复前 | `6f7f9bd7ae6b2bee8ee9be04074054045a49817e7bcb7a6bbf81528c111cf663` |
| 本轮最终 C++ 服务 | `56707dca60b95b586aa9f870e1516672dce9ffb329543cf43d63d830501b91e5` |

## 检查结果

`tests/go_rpc_parity.mjs` 自行启动隔离的 Go/C++ 参考进程，比较 HTTP 状态、完整
Twirp 错误体及成功 payload。UUID、时间和动态 participant SID 单独做语义检查。
测试包含真实浏览器数据通道，不使用 RPC mock；测试密钥与进程运行结束后清理。
旧测试依赖不包含 PerformRpc schema，因此通过其 protobuf runtime 按固定协议
字段构造消息，二进制编码/解码同时与真实 Go 服务验证。

| 检查 | 结果 | 证据（相对仓库） |
| --- | --- | --- |
| RPC 26 项，最终跨房改动之前 | 26/26 | `tests/results/go-rpc-parity/2026-10-27T02-38-02-911Z-c33b28/report.json` |
| 扩展 50 项，跨房改动之前 | 28/50，22 项差异 | `tests/results/go-rpc-parity/2026-10-27T02-40-19-745Z-99468e/report.json` |
| 扩展 50 项，最终产物 | **50/50** | `tests/results/go-rpc-parity/2026-10-27T02-44-32-122Z-01f0c5/report.json` |
| JSON 房间 API 回归，最终产物 | **36/36** | `tests/results/go-room-api-relocation-regression/20261027T024536Z-36e396/report.json` |
| 原生 CTest，最终产物 | **5/5** | `out/rpc-relocation-ctest.log` |
| 官方 SDK 无头媒体回归，最终产物 | **9/9** | `tests/results/go-rpc-relocation-media-20261027/result.json` |

构建日志：`out/rpc-relocation-build.log`。依赖既有编译警告仍保留；没有修改
WebRTC 源码、放宽媒体断言、增加生产超时或更换 Go 基线。
最终差分目录的 `provenance.json` 保存基线提交、C++ 产物和源文件哈希。

媒体回归沿用 `tests/integration.mjs`；运行前确认其内容及 lockfile 与旧仓库
安装依赖的版本一致，仅换行不同。检查 API/JWT、ICE/DTLS、合成音视频、数据、
metadata、静音/重订、后加入、反向发布、三层 VP8 选层/恢复和权限清理。
没有修改媒体断言，也没有将该结果表述为真实设备或跨系统验收。

RPC 用例覆盖 JSON/protobuf 与空值、UTF-8 边界、方法不存在、应用错误和自定义
错误、可恢复错误重试、请求/响应过大、缺失目标、延迟入房、数据权限、管理权限、
ACK/响应/HTTP 超时、16 路并发关联、伪造回复、客户端间 RPC、HTTP 中止和目标
断开。另有两组各 12 项 Forward/Move 权限与失败契约检查。

原生 RPC 测试覆盖回复先于 ACK、伪造 ACK/response、重复回复、超时、发送失败、
参与者 SID 隔离、取消和对象销毁后的 handle 安全，以及 uint32 timeout 边界。

## 保留的失败与测试环境调整

- 最初 RPC 20 项差分有 3 项不一致：不存在目标、HTTP deadline、参与者断开。
  调整的是 C++ 错误及调度行为，扩展套件保留这些用例。
- `tests/results/go-rpc-parity/2026-10-27T02-33-23-316Z-e6662f/report.json`
  是参考服务连接失败：Go 浏览器在首条 RPC 前无法建立完整 PeerConnection，
  结果为基础设施错误、0/0，C++ 未启动。不是 C++ 通过或 RPC 行为失败的证据。
- 该失败的 Go 日志出现外部 STUN 候选。套件本来只测试同机 RPC，现为两端浏览器
  都显式配置 `rtcConfig.iceServers=[]`，去除 WAN/STUN 依赖，并保存浏览器 console
  与 pageerror。随后 26 项、修复前 50 项、修复后 50 项均完成两端连接。
  这不证明公网 ICE/NAT 已通过，也不把单次日志归因为已确定的网络根因。
- 原生测试曾因 Windows 调度在 25 ms 等待内未观察到 20 ms 定时器而失败；测试
  观察窗口改为 80 ms，生产 deadline 与预期错误没有改变。

## 复现与边界

```powershell
cmake --build out/windows-parity --config Release --parallel 6
ctest --test-dir out/windows-parity -C Release --output-on-failure
node tests/go_rpc_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/windows-parity/Release/livekit-server.exe `
  --node-modules D:/workspace/livekit-cpp/tests/node_modules
```

产物目录被 Git 忽略，未随源码提交。测试监听 loopback，Go/C++ 分别使用固定
59200–59280 / 59300–59380 UDP 范围，勿并行启动多份该套件；HTTP 端口自动分配。

完整 RoomService 字段与错误、token/ACL、断线恢复、stream/data track/blob、
Redis/集群及外部 worker 仍未完成。本轮未重跑 Linux、CLI 四轨、Windows→WSL、
公网、弱网、容量或长期稳定性；历史失败继续有效。所有结果均为自动化检查，
未完成实际设备人工验收，未部署、提交或推送。
