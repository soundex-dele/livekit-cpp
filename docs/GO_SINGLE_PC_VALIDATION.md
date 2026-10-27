# Go 主服务替代：`/rtc/v1` 与单 PeerConnection

日期：2026-10-27（Asia/Shanghai）。R03 增量；完整替代仍未完成。

## 实现

- `/rtc/v1`、`/rtc/v1/validate` 及旧 `/rtc` 的 `join_request` 参数解析
  WrappedJoinRequest/JoinRequest，支持带 padding 的 base64url、原始 protobuf
  和 GZIP。解压上限 1 MiB，校验 CRC，支持拼接 GZIP 成员，拒绝截断和超限。
  新增 ZLIB 构建依赖，Windows bootstrap 和 Linux 安装说明同步更新。
- JoinRequest 的订阅设置、资料覆盖、属性合并、初始发布请求、内嵌 publisher
  offer 和恢复设置接入既有房间/权限逻辑。空属性值不删除 token 中已有属性。
  这里尚不声称所有请求字段、错误文本及配置选项均与 Go 等价。
- 单 PC 会话的 publisher/subscriber 引用同一原生 PeerConnection。客户端发起
  offer，服务端回传对应编号的 answer；服务端需要订阅新轨道时发送
  MediaSectionsRequirement，请客户端补接收媒体段并协商。
- 订阅仅复用远端 SDP 允许接收的媒体段，避免原生 AddTrack 将下行放进客户端
  sendonly 段。不足时创建新的原生 transceiver，并在固定 M137 适配层设置 AddTrack 的
  原生关联标记，使新的远端媒体段可以按 JSEP 5.10 匹配，不手写分配 MID。
  收发共用 ICE/DTLS/SRTP，SFU
  按协商方向分别绑定原生接收和发送模块，双向 RTCP 均进入对应模块。
- 恢复继续保留 SID、PC、发布和订阅。客户端发起单 PC 的 ICE restart；服务端
  在新 offer 后重发中断期间丢失的媒体段请求。双 PC 保留服务端 subscriber
  ICE restart 路径。可靠数据重放和 data track 尚未实现。

## 验证方法与当前证据

参考 Go 1.13.10：`a390d03d28c895742d9b13c0894880ee67733801`，protocol：
`f09f87aab29b865850a967924726bf76b7db4755`。C++ 为平台子模块 `a21f3c0`
加未提交修改。生产 C++ 没有嵌入、代理或启动 Go。

`tests/go_single_pc_parity.mjs` 使用本机官方 Web SDK 2.23.0、Server SDK
2.13.3、无头 Chrome，通过透明 TCP 代理单独中断信令。检查 SDK 实际模式为
publisher-only、没有 subscriber PC、没有回退连接，并检查实际解码帧/音频能量
推进；不以成功入房或连接状态代替媒体证据。SDK 当前协议为 17，普通可靠/不可靠
数据走协商内通道；代码中协议 18 的固定 ID 通道路径尚无端到端验收。

新增原生 `join-request` 测试覆盖解压上限、CRC、截断、拼接成员和准确边界。
最终单 PC 对照 **13/13**：
`tests/results/go-single-pc-parity/2026-10-27T05-14-15-393Z-945bcd/report.json`。
请求解析只比较 HTTP 成功/拒绝状态，不宣称错误文本全部等价。媒体场景使用
SDK 协议 17，已确认压缩入房内嵌 offer、未压缩入房、双向多轨媒体、数据、
媒体段耗尽、取消/恢复订阅、常规恢复、恢复时新发布，以及单/双 PC 混合互通。
最终 C++ RTP 转发计数 3,244，视频编码/解码调用均为 0。

| 产物 | SHA-256 |
| --- | --- |
| Go | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| 本轮 C++ | `2081f3a7852222e71720be4d9774c32320b996605a11756ab9b2a811d387c976` |

已另存 `out/parity-reference/livekit-server-cpp-single-pc.exe`。源码与测试哈希见
最终结果目录 `provenance.json`；构建日志 `out/single-pc-final-build.log`。

上述产物验证时 CTest **6/7**，不能记为全绿。本轮过程中工作树的
`tests/unit/native_send_transport_test.cpp` 被另行扩展了纯视频冷启动/容量恢复
场景（本轮未修改或撤销该文件）；其源码哈希由
`c13ff1de3b9d5ae2e9355a222d95f9dcfcb49c5dc6ef7f50aa13c2201dcc929e`
变为 `1c8c962c075d9e110b9df14d2abeaf26e26b760ef03021c540912a82ccc71ccd`。
该次扩展测试在链路容量恢复后仍停留约 138,598 bps，视频没有恢复；复跑再次失败。
生产 `native_send_transport.cpp` 的哈希在前后相同。这是未关闭的恢复缺陷，不能
将早先旧测试的 7/7 结论覆盖到扩展后的测试。证据：
`out/single-pc-ctest-native-send-failure.log`、`out/single-pc-native-send-repeat.log`。

同产物 RoomService **36/36**：
`tests/results/go-room-api-single-pc-regression/20261027T051607Z-db4062/report.json`。
同产物双 PC 恢复 **15/15**、ACL **23/23**、RPC **50/50 契约**（本次 50 项
响应恰好严格匹配；仍保留此前已验证的退出竞态双错误契约）：
- `tests/results/go-resume-single-pc-regression/2026-10-27T05-15-58-299Z-f1f002/report.json`
- `tests/results/go-acl-single-pc-regression/2026-10-27T05-18-43-305Z-f26a21/report.json`
- `tests/results/go-rpc-single-pc-regression/2026-10-27T05-19-51-697Z-cccf65/report.json`

额外回归失败保留：
- `tests/results/go-single-pc-media-regression-20261027/`：原有无头媒体前 7 项
  通过，720p simulcast 切换超时；当时另一恢复套件也在执行。并发只是环境
  记录，尚不能据此断言根因。
- `tests/results/go-participant-single-pc-regression/2026-10-27T05-21-01-418Z-caf72c/`：
  Go 首场 ICE 连接失败，C++ 19 项完成；该轮差分不算通过。

- `tests/results/go-single-pc-media-isolated-20261027/`：本任务其他媒体套件结束后
  单独复测，仍在 720p 切换失败。上行 f 层已编码并发送 417 帧 1280×720，
  qualityLimitationReason=none，下行没有达到所需分辨率；不能以并发解释或关闭。
- `tests/results/go-single-pc-media-before-control-20261027/`：修复前的 `335f0d…`
  产物在相同无头 Chrome 156.0.8078.48 环境也失败；但该次上行 f 层为 0 帧、
  qualityLimitationReason=bandwidth。两次观测位置不同，不能据此直接断言相同
  根因，亦不能把当前失败简单归为新单 PC 回归。原有 9 项套件目前未全通过。

参与者顺序复测 **19/19**：
`tests/results/go-participant-single-pc-regression/2026-10-27T05-22-54-005Z-10e733/report.json`。
JWT 非周期场景 **20/20**：
`tests/results/go-token-single-pc-regression/2026-10-27T05-25-30-545Z-1154df/report.json`。
五分钟周期本次未重新等待验证。上述源码快照之后，工作树中另有
`video_allocation.hpp` 与原生测试修改；本报告保留原产物和源码哈希，不将后续
源码改动未重新构建的结果归到该产物。

## 保留失败

- `tests/results/go-single-pc-before/2026-10-27T04-50-53-168Z-fbe973`：
  Chrome 启动阶段退出，没有启动服务或执行用例，记为基础设施失败。
- `tests/results/go-single-pc-before/2026-10-27T04-52-07-933Z-daa16a`：
  Go 9/9；旧 C++ 自动回退双 PC，被模式断言拒绝。旧产物为
  `out/parity-reference/livekit-server-cpp-pre-single-pc.exe`，SHA-256：
  `335f0d7310f6dff829d7062c4f00ae47f585a1a1eb8c0217668a0b2fae2b2bae`。
- `tests/results/go-single-pc-parity/2026-10-27T04-53-02-009Z-8041ba`：
  C++ 单 PC 入房通过，但发布协商超时。修复 SDP answer 丢失 offer 编号。
- `2026-10-27T04-58-08-924Z-95985c`、`2026-10-27T04-59-40-536Z-cb93f3`：
  C++ 前 6 项通过，多视频失败。后一报告保留双方 SDP、轨道列表和 RTP 统计，
  确认一条下行被放入客户端 sendonly 的 MID 7；其他下行仍有解码推进。
  修复选择可接收媒体段，不放宽解码断言。

- `2026-10-27T05-05-13-187Z-9bedcf`：限制复用方向后，新增 transceiver
  未被 M137 关联到后续客户端 offer。原生实现只匹配 created_by_addtrack；
  补齐这个关联标记，保留失败 SDP 与统计。

- `2026-10-27T05-09-12-854Z-c16c59`：前 9 项通过；恢复期间的新发布仍失败。
  对照原生 AddTrack 发现显式复用漏掉 has_ever_been_used_to_send 条件，旧 sender
  的 MSID/SSRC 被复用于新发布。恢复原生“不复用已经发送过的 sender”规则，
  新发布需要新的关联；保留旧 SDP、RTP 统计和解码失败。

## 未完成与复现

R03 仍需完整旧协议/客户端矩阵、协议 18 数据通道、SDP/ICE 候选交错及反复恢复
竞态、所有轨道更新/订阅响应。REST/WHIP one-shot 不等于本轮内嵌 offer 的 WS
入房，仍未实现。完整 HTTP 错误文本/字段/上限以及全部 JoinRequest 参数仍待补。
R07 的 data track、stream/blob、可靠恢复和去重不因单 PC 数据收发通过而完成。

本轮仅单机 Windows 合成媒体自动化；无 Linux 重跑、真机人工验收、NAT/TURN、
弱网/容量、外部 worker 联调或部署。旧四轨和跨系统 LOW/401 失败继续保留。

```powershell
node tests/go_single_pc_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/windows-parity/Release/livekit-server.exe `
  --node-modules D:/workspace/livekit-cpp/tests/node_modules `
  --modern-node-modules D:/workspace/livekit-cli-cpp/web/node_modules
```

HTTP 端口动态分配，UDP Go 60600–60780、C++ 60800–60980；不同时运行两份。
测试清理自建进程、临时密钥配置和浏览器上下文；失败与哈希保留在报告目录。
