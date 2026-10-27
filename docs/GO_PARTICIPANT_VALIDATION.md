# Go 主服务替代：隐藏参与者与附属参与者生命周期

日期：2026-10-27（Asia/Shanghai）。本轮继续 R02，补齐隐藏可见性、参与者类型、
普通容量与附属参与者的房间生命周期。主服务完整替代尚未完成。

## 实现与基线行为

- JWT 接受 `hidden`、`agent`，保留已有 `recorder`。`kind` 大小写不敏感，映射
  固定协议的 STANDARD/INGRESS/EGRESS/SIP/AGENT/CONNECTOR/BRIDGE；未知值
  回退 STANDARD。`kindDetails` 保留有效值及顺序/重复，忽略未知枚举名称。
- 隐藏参与者可以入房；JoinResponse 的其他参与者列表不包含隐藏成员，隐藏
  成员自己的 join/update 正常发送，其他成员看不到其后续成员更新和离开事件。
  管理 API 仍可列出/查询隐藏成员；隐藏本身不减少普通人数、不绕过容量限制。
- hidden 的 reliable/lossy UserPacket 清除内层发送者 identity/SID，外层
  participant_identity 清空；外层 participant_sid 仍为认证后的 SID，与 Go
  一致。SDK DataReceived 的 participant 为空；这不是传输层完全匿名化。
- 隐藏参与者可接收其他人的媒体和定向数据。本轮以真实合成 VP8 视频解码帧持续
  增长验证接收路径，不只检查房间列表或轨道声明。
- EGRESS/AGENT 类型及 recorder/agent 旧 grant 是附属参与者：不计入普通
  `num_participants`、不占 maxParticipants，但管理 API 保留这些实际会话。
- recorder/EGRESS 入房开始 activeRecording，最后一个录制参与者离开后停止，
  发送 RoomUpdate 供 SDK 更新 isRecording。仅通过 API 动态开启 recorder
  标记不直接开始 activeRecording，按固定 Go 的 join/leave 行为处理。
- 房间没有普通参与者时按 emptyTimeout/departureTimeout 关闭，附属参与者也
  随之断开，不再因 map 里还有 Agent/recorder 而永久保留房间。

动态 hidden 的两个重要契约已通过实际 Go 对照：

1. 将已公开的成员改为 hidden，不会追溯删除其他客户端已经知道的成员信息；
   新入房者看不到它，后续隐藏状态更新不再广播。取消隐藏会广播当前状态。
2. 同一次 UpdateParticipant 中，Go 先更新 metadata/name/attributes，再更新
   permission。因此“设为 hidden”请求中的 metadata 仍按旧可见性发送；后续
   metadata 才只发给自己。C++ 已调整这一顺序，未把基线行为误判为测试不稳定。

`Rooms::refresh_room` 统一刷新人数/发布者数和房间通知；普通房间使用时间与最近
离开时间单独保存，使附属参与者不改变首次普通成员入房/空房计时的含义。
`num_publishers` 仍沿用原实现按已发布轨道计算，与 Go 完整 publisher transport
状态的等价性不在本轮证明范围内。

## 基线和产物

- Go 1.13.10：`a390d03d28c895742d9b13c0894880ee67733801`，protocol
  `f09f87aab29b865850a967924726bf76b7db4755`；未修改参考源码和协议版本。
- C++：平台子模块 `a21f3c0` 加未提交工作区修改，Windows x64 Release，专用
  `out/windows-parity`。保留其他任务使用的 `out/windows-x64` 产物。
- Chrome 156.0.8078.47 无头模式；Web SDK 2.15.6、Server SDK 2.13.3、Node
  24.13.0。双服务使用 loopback、客户端 `iceServers=[]`。

| 产物 | SHA-256 |
| --- | --- |
| Go 参考服务 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| C++ 修复前快照 | `64760f8d86195104981b47c77560603c75530a9b73bb82e14412b5f43490d215` |
| 本轮 C++ 产物 | `7de3e48401a24449321cee063d546378e9d5b23393e731ed82d4d932a6aca7df` |

修复前快照位于 `out/parity-reference/livekit-server-cpp-pre-participants.exe`。
编译日志 `out/participant-build.log`；原生测试 `out/participant-ctest.log`。
最终对照目录的 `provenance.json` 保存源文件哈希，避免只用脏工作树基线提交描述
实际验证版本。WebRTC 依赖未改，已有编译警告保留。

## 自动化验证

新增 `tests/go_participant_parity.mjs`。所有连接均来自官方 SDK、临时密钥、自建
测试房间，测试完成后清理进程、浏览器上下文及密钥配置。

| 检查 | 结果 | 证据（相对仓库） |
| --- | --- | --- |
| 参与者及生命周期对照 | **19/19** | `tests/results/go-participant-parity/2026-10-27T03-17-30-582Z-290fe7/report.json` |
| 原生 CTest | **6/6** | `out/participant-ctest.log` |
| 房间 API 回归 | **36/36** | `tests/results/go-room-api-participant-regression/20261027T031405Z-47cf2e/report.json` |
| 媒体 ACL 回归 | **23/23** | `tests/results/go-acl-participant-regression/2026-10-27T03-21-55-626Z-e2eacf/report.json` |
| RPC / Forward / Move 回归 | **50/50** | `tests/results/go-rpc-participant-regression/2026-10-27T03-23-07-957Z-2c9a3f/report.json` |
| 原有无头媒体集成 | **9/9** | `tests/results/go-participant-media-regression-20261027/result.json` |

19 组场景包括隐藏入房/快照、管理员查询、人数、自身 metadata、可靠/不可靠
匿名 identity 数据、定向数据、真实视频接收、离开通知、动态 hidden/unhidden、
录制与 Agent 容量、普通隐藏成员容量拒绝、录制状态、动态 Agent 人数、七种
kind/未知 kind/重复与未知 kindDetails、动态 recorder 标记、两种空房关闭路径。

旧 Server SDK 不认识 CONNECTOR/BRIDGE 的 JSON 枚举名称，开启 ignoreUnknownFields
后读成默认 STANDARD。本套件既验证该 SDK 实际行为，也独立检查原始 Twirp JSON
中的 kind/kind_details，未删除新枚举覆盖。原生 core 测试还检查隐藏 join 快照、
不向观察者泄漏更新/离开、附属录制入满房、管理列表、容量拒绝和录制离开清理。

以上均验证同一份 C++ 产物。原有集成脚本及 lockfile 与依赖安装位置已逐行核对，
仅换行不同；保留音视频推进、三层 VP8 选层/恢复、重订和 SFU 编解码零调用断言。

## 保留的失败和限制

- `2026-10-27T03-09-49-797Z-ebb3e0`：旧 C++ 拒绝 hidden token；Go 在“同次
  更新 metadata 应保密”的错误预期上失败。源码和实测确认 metadata 先更新。
- `2026-10-27T03-11-47-127Z-6bbda8`：Go 已通过前 15 项，类型循环首次标准
  参与者 PeerConnection 连接失败；旧 C++ 仍拒绝 hidden token。不是完整对照
  通过。测试现及时关闭已离开/已删房的浏览器上下文，避免保留合成媒体定时器；
  不能据此断言这一次 ICE 失败的唯一根因已经确定。
- `2026-10-27T03-13-57-178Z-93e2f6`：两端均过前 15 项，均因旧 SDK 的新枚举
  默认值行为而未通过类型断言。修正读取层并增加原始 JSON 检查后完成 19 项。
  没有修改 Go 基线、延长生产超时或放宽音视频断言。
- 本轮结束时 R02 仍缺完整 grant、token 刷新及完整动态权限矩阵；token 刷新的
  后续实现和独立证据见 [JWT 验证记录](GO_TOKEN_VALIDATION.md)。R03 会话恢复等
  后续工作未完成。类型/附属生命周期支持不等于外部 worker 实际联调。
- 未验证隐藏发布者的完整媒体发布矩阵、新式 data track/blob、发布者计数的
  完整 Go 语义、全部通知顺序/时间窗口及 Linux。旧四轨和跨系统失败继续保留。
- 全部为自动化检查，没有桌面自动化或实际设备人工验收，没有部署、提交或推送。

```powershell
node tests/go_participant_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/windows-parity/Release/livekit-server.exe `
  --node-modules D:/workspace/livekit-cpp/tests/node_modules
```

HTTP 端口自动分配；Go/C++ UDP 范围分别为 59800–59980 / 60000–60180，不并行
运行多份本套件。测试产物目录被 Git 忽略，未打包进源码提交。
