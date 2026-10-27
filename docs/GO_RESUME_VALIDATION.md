# Go 主服务替代：双 PeerConnection 信令恢复

日期：2026-10-27（Asia/Shanghai）。本轮推进 R03；主服务完整替代仍未完成。

后续 `/rtc/v1` 与单 PC 的实施和独立验证见
[单 PC 验证](GO_SINGLE_PC_VALIDATION.md)；本报告仍限定双 PC 产物和场景。

## 实现

- 将信令连接关闭与参与者退出分开。已有 ICE 连接时，TCP/WebSocket 中断只移除
  信令 sink，保留 SID、双 PeerConnection、发布轨道和订阅状态。未建立 ICE 的
  会话仍在信令关闭时清理；显式 Leave、管理员移除、媒体失败与关闭服务仍退出。
- `/rtc?reconnect=1` 按已认证的房间和 identity 查找已有参与者并替换信令连接。
  旧 socket 的迟到关闭和读取回调通过 sink 归属检查隔离，不再影响新的连接。
  现有服务端 claims 保持权威，旧 JWT 不能恢复管理员已撤销的权限。
- 恢复首先发送 ReconnectResponse，再补发近期离开记录、当前可见参与者、房间
  状态及刷新 token。离开缓存按固定 Go 的 128 条界限保存最小身份/版本/状态，
  不复制每个远端参与者的大 metadata 或轨道清单。
- subscriber 调用原生 `RestartIce` 并产生新的 offer，允许替换在信令中断期间
  丢失的未答复 offer；publisher 接受 SDK 发起的 ICE restart offer。去除此前
  JoinResponse 中禁用 resume 的配置。
- SyncState 校验客户端申报的媒体发布 SID，接受现有/待完成的已登记轨道；
  应用停用轨道和订阅/退订状态。未知发布要求 STATE_MISMATCH 完整重连。
  尚未实现的 data track 发布不会被静默视为恢复成功。
- 心跳超时只断开信令；已建立 ICE 的参与者不再因旧 30 秒信令计时而被销毁。

## 对照范围

使用固定 Go 1.13.10（`a390d03d28c895742d9b13c0894880ee67733801`）与 protocol
`f09f87aab29b865850a967924726bf76b7db4755`。C++ 为平台子模块 `a21f3c0` 加未
提交修改，Windows x64 专用 Release 构建，没有启动或代理 Go 来实现生产功能。

新增 `tests/go_resume_parity.mjs`：官方 Web SDK 2.15.6、Server SDK 2.13.3、
Chrome 156.0.8078.47 无头、Node 24.13.0。每个客户端经独立字节透明 TCP 代理
连接信令；ICE/DTLS 媒体直接走 UDP。代理断开已有 TCP 并暂缓新握手，模拟纯信令
中断，不使用桌面或物理设备。

检查实际视频 framesDecoded 和音频 totalAudioEnergy 的增量、参与者/轨道 SID、
浏览器 PeerConnection 对象不变以及新的 SDP ICE ufrag。检查 SDK 连接状态本身
不作为媒体恢复的充分证据。31 秒中断期间按多个时间点检查解码推进，不将该测试
表述为逐帧无丢包、延迟分位数或全网断开验收。

客户端握手超时显式设置为 45 秒，允许代理保持超过 30 秒的中断；不修改生产
服务端超时。其他场景保持短中断。原始缺失会话和未建立 ICE 的关闭通过 Chrome
WebSocket 收发验证。

## 当前产物和验证

| 产物 | SHA-256 |
| --- | --- |
| Go 参考 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| C++ 修复前 | `41f284f5b9d3e0204f384be5b330bf1673280fdc1c7738e659859df899f02c8f` |
| 当前 C++ | `335f0d7310f6dff829d7062c4f00ae47f585a1a1eb8c0217668a0b2fae2b2bae` |

旧二进制：`out/parity-reference/livekit-server-cpp-pre-resume.exe`。
构建日志 `out/resume-build.log`，原生测试 `out/resume-ctest.log`。

| 检查 | 当前结果 | 证据 |
| --- | --- | --- |
| 恢复差分 | 15/15 | `tests/results/go-resume-parity/2026-10-27T04-23-30-075Z-8623ab/report.json` |
| CTest | 6/6 | `out/resume-ctest.log` |
| RoomService | 36/36 | `tests/results/go-room-api-resume-regression/20261027T042331Z-d1bebd/report.json` |
| JWT 非周期场景 | 20/20 | `tests/results/go-token-resume-regression/2026-10-27T04-24-25-146Z-3c6133/report.json` |
| 媒体 ACL | 23/23 | `tests/results/go-acl-resume-regression/2026-10-27T04-25-25-544Z-921e88/report.json` |
| RPC 契约 | 50/50 | `tests/results/go-rpc-resume-regression/2026-10-27T04-26-52-864Z-afd557/report.json` |
| 参与者及附属生命周期 | 19/19 | `tests/results/go-participant-resume-regression/2026-10-27T04-27-58-231Z-e88d8f/report.json` |
| 原有无头媒体 | 9/9 | `tests/results/go-resume-media-regression-20261027/result.json` |

以上使用同一最终 C++ 产物。RPC 本次 50 项恰好全部响应严格匹配，但退出竞态
仍按已验证的两种 Go 错误契约判断，不据此声称完整 PSRPC 等价。JWT 五分钟周期
仅在前一增量实际等待验证，本次未重跑。无头媒体使用原有单轮重订设置。
长中断实测 Go 31,222 ms、C++ 31,230 ms，各有 8 次解码推进采样。
最终恢复结果目录的 `provenance.json` 保存本轮源码、测试及产物哈希。

原生 core 额外检查：空 SyncState 不退出、恢复不创建未知参与者、旧 token 不改
当前权限、恢复消息顺序、旧 sink 关闭不影响新连接、在线 sink 交接、128 条缓存
上限及缓存不保留 metadata。真实 PeerConnection 建立与解码由差分套件验证。

## 保留的失败与测试修正

- `tests/results/go-resume-parity/2026-10-27T04-10-23-358Z-58dd15`：旧 C++ 在
  信令中断后停止解码；Go 通过前 8 项，随后测试清理调用了 LocalParticipant
  不存在的 getTrackPublicationBySid。改用 trackPublications map 查找后重跑。
- `2026-10-27T04-12-41-058Z-70938c`：两端通过前 10 项；Node 原始 WebSocket
  观察 Go 缺失会话响应超时，C++ 仍完成随后场景。改由 Chrome 捕获原始帧验证，
  不修改 Go 服务或放宽所需 Leave reason/action。
- `2026-10-27T04-15-50-474Z-a9381d`：两端通过前 11 项；测试把 SDK 首次重试
  推迟 40 秒，SDK 在进入 resuming 前被自己的状态一致性检查关闭媒体，双方
  长中断断言失败。保留日志中的 detected connection state mismatch。修正为
  SDK 正常进入恢复，代理保持 TCP 握手 31 秒，并使用公开握手超时选项；不关闭
  SDK 的一致性检查，不放宽媒体推进断言。

## 未完成

- R03 的 `/rtc/v1`、单 PeerConnection/one-shot join、完整老协议与 SDK 矩阵、
  SDP/ICE 候选与恢复交错竞态、轨道更新和全部订阅响应仍未完成。
- 本轮是单节点、同机、纯信令中断的恢复。未验证 UDP 同时中断、跨 NAT/TURN、
  换网、多节点迁移、节点死亡或长期无人接收信令时的全部回收路径。
- 数据包在本次短中断后可以收发；可靠消息重放、last_message_seq、去重、
  data track/blob/stream 与相应 SyncState 字段仍属 R07，不能视为全部已实现。
- 原四轨、跨系统 LOW/401 失败继续保留。没有 Linux 重跑、设备人工验收、容量/
  弱网门槛、部署、提交或推送。完整替代与发布门禁继续按 GO_REPLACEMENT.md。

```powershell
node tests/go_resume_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/windows-parity/Release/livekit-server.exe `
  --node-modules D:/workspace/livekit-cpp/tests/node_modules
```

HTTP/TCP 代理端口自动分配；Go/C++ UDP 分别使用 60200–60380 / 60400–60580。
不并行运行多份此套件。临时密钥配置、浏览器上下文和自建进程在退出时清理；
报告保留二进制/源码哈希、失败记录和场景证据。
