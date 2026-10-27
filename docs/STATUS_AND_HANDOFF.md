# RTC 系统功能现状与暂停交接

> 本文保留 2026-10-26 的历史交接快照。2026-10-27 已按用户要求恢复主服务
> 替代工作，当前实现与验收状态见 [Go 主服务替代](GO_REPLACEMENT.md) 和
> [视频稳定性排查](GO_VIDEO_STABILITY.md)；下文的“未支持”是当时状态。

更新日期：2026-10-26（Asia/Shanghai）。范围：`D:\workspace\livekit-cpp` 服务端、
`D:\workspace\livekit-sdk-cpp` SDK，以及 `D:\workspace\webrtc` 原生依赖。

用户已要求本次整理后暂停目标，后续再开发。目标“帮我实现这份规划”尚未完成，
本次只整理文档，不继续修复、扩展功能或启动媒体测试。完整需求仍保留在
[IMPLEMENTATION.md](IMPLEMENTATION.md)，本文件作为恢复开发时的首读入口。

## 1. 当前结论

目前已有可运行的 **C++ 单机 RTC 服务端 + Windows 原生 SDK**，核心是复用
libwebrtc 的 Opus/VP8 SFU，具备发布订阅、三层 simulcast 选层、原生拥塞控制、
音频优先分配和本地 TURN 联调。Windows 和 Linux x64 服务端均已构建运行。

尚未形成火山引擎/声网同类产品的完整交付：移动端、项目隔离与管理后台、云录制、
混流转推、多节点和生产部署体系仍缺失；最新文件回归及跨系统互通仍有失败。
不能宣称生产就绪，也不能据现有测试宣称达到容量、弱网或端到端时延指标。

下文“已实现”表示存在实现与相应验证，**不表示完整产品验收完成**；“部分实现”
表示已有基础但仍缺关键能力或验收；“未交付”不因存在类型、客户端方法或 preset
而变成已支持。历史通过记录不能覆盖最新失败。

## 2. 已实现的服务端能力

| 模块 | 当前实现 | 边界 |
| --- | --- | --- |
| 入房与信令 | `/rtc` 双 PeerConnection、protobuf 二进制消息、JSON 请求、ICE trickle、ping/pong | 未支持 `/rtc/v1` 单连接；断线使用完整重连 |
| 身份与权限 | JWT HS256、issuer、exp/nbf、房间授权、发布/订阅/数据权限及权限变更 | 不是多项目隔离或完整企业权限体系 |
| 房间管理 | 创建/查询/删除房间、参与者查询与移除、轨道静音、参与者更新、订阅更新、房间 metadata、SendData | 已验证 RoomService 子集，不等于完整 LiveKit 商业 API |
| API 协议 | Twirp JSON/protobuf、Bearer Token、标准错误结构 | 仅已实现服务端方法可用 |
| 生命周期 | 重复 identity 替换、房间容量、空房与心跳超时、离开和关闭清理 | 单机内存状态；无持久化集群恢复 |
| SFU 转发 | Opus/VP8/RTX；发布、后加入订阅、静音、退订、重订及重新协商 | 视频完整组帧后用原生模块重新打包，编码内容不变；组帧额外时延待测 |
| 原生协议复用 | 原生 RTP/RTCP、NACK/RTX、PLI、重传缓存、收包统计、TWCC/REMB；订阅者独立 RTP 状态 | 不维护自研协议栈；未开放的 FEC/编码能力不因 libwebrtc 存在实现而自动支持 |
| 下行拥塞控制 | 每个订阅传输共享原生 GoogCC、pacing、padding/probing 和反馈；发送时记录实际时间 | 初始/最小/最大码率仍为内部默认值，部署配置待补 |
| 分层订阅 | 最多三层 VP8 simulcast；独立质量/目标尺寸、关键帧切层、失活层回退、重订保留设置 | 浏览器真实三层发布已测；不等于原生 SDK 已能多层发布 |
| 带宽策略 | 原生目标的 85% 用于媒体；音频预留、视频优先级、先基础层后升级、降层/暂停/恢复和升层等待 | 应用只做预算与选层策略；完整弱网质量尚未验收 |
| 队列与资源 | 每传输 4 MiB 应用媒体准入、视频整帧准入、原生 TTL、静音/移除取消队列、销毁清理 | 重传历史/原生 padding 有各自生命周期，不代表全进程固定内存上限 |
| 数据通道 | reliable/lossy、指定接收者、认证发送者、有界可靠发送队列 | 未交付新式 data track/blob、业务 RPC/数据流 |
| TURN 凭证 | 静态 ICE；coturn REST 会话临时凭证、环境变量密钥、TTL/URL 校验 | 初次/完整入房签发；未交付在线刷新、即时吊销、无损轮换 |
| 运维基础 | JSON 配置、healthz、受 roomList 权限保护的 metrics、退出信号与清理 | 无完整监控告警平台、管理 UI 或生产安装包 |
| 构建与测试 | Windows x64、WSL Ubuntu 22.04 x64 ELF；依赖固定、Linux 引导脚本、原生/浏览器/CLI/TURN 测试 | Linux 跨系统回归未通过；离线源码引导不等于离线产品交付 |

保留的 `media_mode=relay` 解码重编码路径是旧兼容模式，不能计作 MCU 混流服务。
SFU 集成测试对服务端视频 Encode/Decode 调用数检查为零。

## 3. 已实现的 SDK 与工具能力

| 模块 | 当前实现 | 验证范围与限制 |
| --- | --- | --- |
| C++ SDK | C++20 `Room/Participant/TrackPublication`，动态发布/取消发布/订阅、状态快照、事件、数据、停止/复用及异步请求 | Windows x64 已实测；源码级 CMake 集成，尚无完整跨平台分发包 |
| 媒体输入 | 合成图案/音调、默认麦克风接口、文件及 FFmpeg 网络输入 | 文件/TCP H.264 输入解码后发布 VP8；不是 H.264 SFU 转发；物理设备质量未完整验收 |
| 媒体输出 | 远端自动解码、音视频播放、每轨 WAV/Y4M、带自有内存的 I420 视频帧回调 | 不是同步 MP4 或服务器云录制；Y4M 名义 30 fps |
| 统计 | 可选原生 RTCStats 事件，200–60000 ms，默认关闭、每连接最多一个请求在途 | 保留编码/解码/传输和质量限制字段；完整产品质量面板待做 |
| CLI | Token/API 操作、入房、文件/网络流发布、定向订阅、数据与录制、统计输出 | 最新 Windows 文件四轨回归失败，流回归通过，见下一节 |
| 会议 Demo | ImGui/SDL 多成员观看、I420 渲染、轨道/静音/离开状态、入房取消和错误状态 | 历史 Windows 渲染及集成通过；实际鼠标键盘、多屏 DPI 等桌面验收未完成 |
| 服务端 API 客户端 | Room/Egress/Ingress/SIP/AgentDispatch/Connector 六类、49 个 RPC 的客户端与协议测试 | 后五类客户端存在不代表本 C++ 服务端实现了对应后端；既有真实 API 联调主要是 Room |

SDK 自身与官方 LiveKit Server 1.13.10 的历史验证见
[SDK VALIDATION](../../livekit-sdk-cpp/VALIDATION.md)。这部分通过不能替代与本项目
C++ 服务端的最新集成验证。SDK Linux、Android、iOS 不在当前已验收平台内。

## 4. 最新验证快照及未关闭问题

完整时间线见 [VALIDATION.md](../VALIDATION.md)。下列路径均相对服务端仓库。

| 检查 | 最新状态 | 证据与结论 |
| --- | --- | --- |
| Windows/Linux 服务端原生测试 | 4/4 通过 | `out/pacer-pause-after-windows.log`、`out/build-linux-ext4.log`；含原生暂停/TTL 回归 |
| SDK 单测 | 最近记录 7/7 通过 | 2026-10-23 原生计时队列/统计增量；本次整理未重跑 |
| Windows Chrome | 9 组通过 | `tests/results/pacer-ttl-fixed-windows/`；含五轮重订及已有音视频继续推进 |
| Linux 本机 REST TURN | UDP 12 组、TCP 11 组通过 | `tests/results/pacer-ttl-fixed/20261026-083512/`；各五轮重订、错误/过期凭证拒绝、实际 relay 与清理 |
| 五秒 UDP 中断 | 单次恢复通过 | 同上；恢复 977 ms RAW，丢弃上行 599/下行 531 包；不能推出 P95 |
| Windows CLI 网络流 | 通过 | `tests/results/pacer-ttl-fixed-stream/20261026-084120/`；全量/定向视频 300/210 帧，静音恢复与踢人通过 |
| Windows CLI 文件四轨 | **失败，待定位** | `tests/results/pacer-ttl-fixed-cli/20261026-084007/`；视频 108/78 帧，第二路未达每路至少 80 帧门槛 |
| Windows 客户端 → Linux/WSL | **整体失败** | `tests/results/pacer-ttl-fixed-cross/20261026-083730/`；LOW 超时、文件视频 108/40 帧、流入房 HTTP 401 |
| 高层画质恢复 | 功能断言通过，时延待验收 | TURN 成功运行中一次高层重开耗时 14,699 ms；30 s 功能门槛不能替代快速恢复指标 |

优先保留以下问题上下文：

1. **文件启动/分配时序尚未定位。** 失败运行音频 406/408 回调帧，采样队列峰值
   6 ms/17,401 bytes，无 socket/admission 丢包，记录一次带宽暂停。下一步先分析
   `file-pub.stdout.log` 每 500 ms 的原生 outbound stats，对齐编码、分配、转发及
   接收时间线。实际发布视频 metadata 为 640×360，输入 fixture 为 320×180；
   尚不能据此确定根因。不得降低 80 帧门槛、增加 TTL 或绕过拥塞控制。
2. **跨系统失败须分开诊断。** HTTP 401 发生在媒体前，先查 Token 的 exp/nbf、
   凭证和对应服务端日志；不能称为解码失败。已有约 9% 的 WSL 计时差证据是线索，
   不能替代媒体缺陷定位。此前时钟对照未解决问题，设置已恢复；本次未修改时钟。
3. **失败证据留存可以改善。** 文件测试当前在帧数断言后写 `media-evidence.json`，
   因而失败运行缺该摘要，但原始日志和 metrics 仍在。可后续改为失败也保留摘要。
4. **稳定性尚无结论。** 先前固定/REST TURN 发布端受带宽限制等失败仍保留；
   本次原生补丁修复的是确定性暂停/TTL 缺陷，不代表解决了全部间歇性媒体失败。

## 5. 尚未完成的产品能力

| 阶段/领域 | 当前基础 | 剩余交付 |
| --- | --- | --- |
| P0 平台与兼容 | 固定协议/依赖、Windows/Linux x64 构建及兼容表 | 稳定 Linux 跨系统互通；Android/iOS arm64 原生构建、绑定及真机互通 |
| P0/P1 媒体可靠性 | SFU、原生重传/拥塞控制、simulcast 和预算分配已实现 | 关闭当前回归；真实弱网/AV 同步/端到端时延/首帧验收；部署码率配置 |
| P1 SDK 设备与媒体 API | 默认设备、文件输入、I420 输出和统计基础 | 相机/屏幕采集与预览；设备枚举、选择、热插拔；自定义 PCM/I420/NV12 输入及完整音频回调；统一质量 API、队列验收 |
| P1 移动端与分发 | C++ 核心 | Android JNI/Kotlin、iOS Objective-C++/Swift、移动端音频路由/生命周期、SDK 包与示例；其他桌面平台后续扩展 |
| P1 项目与权限 | 房间 Token、部分权限、容量限制 | 项目隔离、密钥轮换、项目配额、版本化管理 API、后台 UI、细粒度发布者订阅 ACL、隐藏参与者 |
| P1 网络与部署 | coturn 静态/临时凭证、本机 UDP/TCP relay、中断测试 | 公网 NAT/跨网络、TURN/TLS、Wi-Fi/蜂窝换网、在线凭证刷新与撤销/轮换、长通话；HTTPS/WSS 部署验收 |
| P1 运维交付 | healthz/metrics/脚本与日志 | Linux 安装包/容器/完整离线交付、监控告警、诊断流程、备份恢复与升级演练 |
| P1 容量与稳定性 | 小规模真实媒体与模拟链路测试 | 2–16 人房间、100 并发真实媒体会话、硬件/CPU/内存/出口报告、24 h 浸泡与资源回收 |
| P2 RU 录制 | 客户端每轨 WAV/Y4M | 独立录制 worker、分轨时间线 manifest、存储/上传/回放、失败恢复；客户端录制不计为 RU |
| P2 MCU 混流 | 旧 decoded relay | 音频混音及 mix-minus、视频合成、三种布局、时钟同步、H.264/AAC 输出与合成 MP4 |
| P2 LU 直播网关 | SDK 文件/网络输入能力 | RTMP/WHIP ingress、RTMP/RTMPS egress、重连；独立网关进程及端到端验收 |
| P2 任务控制 | API 客户端方法 | 持久化幂等任务、租约、资源配额、worker 故障恢复、Webhook 与防重放；PostgreSQL/S3 等实际集成 |
| P3 分布式 | 单房单 SFU 的设计约束 | 多节点调度、drain/滚动升级、控制面 HA、任务迁移及媒体 worker 独立扩缩容 |
| P3 扩展 | 当前 Opus/VP8/RTX | dynacast、SVC、更多编码及原生 FEC/RED 适配、E2EE、AI 转写/对话、SIP、SRT、多地域互联及其他语言封装 |

此外，订阅 fps 限制、完整轨道 metadata 更新、同 RID 无重新协商的 SSRC 重启、
说话人检测、无缝会话恢复/自动故障转移等尚未交付。

原规划的验收门槛保持不变：正常网络入房成功率 ≥99.5%、远端首帧 P95 ≤2 s、
采集到播放 P95 ≤300 ms；随机/突发丢包、150 ms RTT、抖动和带宽塌陷；10% 随机
丢包下无超过 2 s 连续音频停顿；连通恢复后媒体恢复 P95 ≤10 s；100 会话与 24 h
稳定性；worker 两小时 AV 偏差 ≤100 ms。**这些整体验收均未完成。**

## 6. 必须保留的架构与依赖约束

- RTP/RTCP、NACK/RTX、PLI/FIR、FEC、拥塞控制、pacing、分包/组帧继续复用
  libwebrtc；应用层负责路由、权限、订阅策略和协商映射。未接入能力也按此原则扩展。
- 原生基线是 M137 `aad183504be13d5a9c529581a27c53767570b5d6`，附带明确的
  [pacer 暂停/TTL 补丁](../patches/README.md)。当前原生工作树仅修改
  `modules/pacing/prioritized_packet_queue.cc`。历史文档“WebRTC 未修改”只适用于历史增量。
- 补丁修复累计暂停造成新包被误判过期的问题；真实 TTL 与平均排队统计语义保留。
  升级需复核补丁；不得通过延长 TTL 或移除回归绕开问题。
- `cmake/CheckWebRTCPatches.cmake` 检查源码，不能证明预编译库已更新；
  必须运行链接真实库的 `native-send` 回归。当前 Windows/Linux 原生库已重建验证。
- 默认 PeerConnection 媒体通道的发送接口已撤下，避免与 SFU 重复发送 RTCP/TWCC，
  BaseChannel 的 ICE/DTLS/SRTP 仍保留。临时跟踪插桩已从正式产物移除。
- 服务端持有控制面/SFU 及未来 worker；SDK 持有公共接口与平台适配。
  SDK 服务客户端、CLI 输入或本地录制不能代替服务端 worker 实现。

## 7. 工作区、产物与恢复入口

暂停整理前的 Git HEAD（历史快照，**不是包含全部开发成果的交付提交**）：

| 仓库 | HEAD | 工作区状态 |
| --- | --- | --- |
| `D:\workspace\livekit-cpp` | `bc0abafdf58a40a953d962802340570b32af36d1` | 大量已修改及未跟踪源码、测试、文档；需整体保留 |
| `D:\workspace\livekit-sdk-cpp` | `8abc7e919fc3b3e298e0bd6da7aa541ef084186d` | CMake、CLI、SDK、头文件、测试和文档有未提交修改，包含既有工作 |
| `D:\workspace\webrtc` | `aad183504be13d5a9c529581a27c53767570b5d6` | 上述单文件原生补丁 |

暂停整理时没有创建提交、PR 或部署；随后用户要求本地提交，记录见下一节。
`out/` 与 `tests/results/` 被 Git 忽略，
普通 Git 提交不会保存二进制、日志、录制和诊断脚本；后续整理交付时需另行归档。
不要 reset/clean 这些工作区，也不要用历史 HEAD 覆盖 SDK 既有修改。

最新验证产物 SHA-256（本次整理引用验证记录，未重新构建）：

- Windows 服务端：`6d52866b6bbd853d1d68683c62490d2ba6bda92c301f1f0e530569dc925577b7`
- Linux 服务端：`aaa13c270ffaa7749f4b4a44cf00ad0ef52be40857671fc6af603b5fc12e0339`
- Windows SDK CLI：`922963b8a267dd169ed2ac88ad8c35bb85c08042cda0d4f95a3261165a27ae0d`

Windows 产物分别位于两仓库的 `out/windows-x64/Release/`。
WSL 当前验证快照为 `/root/livekit-validation-20261023-2038/`，服务端位于
`livekit-cpp/out/linux-clang15/bin/livekit-server`，旁边有已打补丁的 `webrtc`。
本机辅助同步构建脚本为 `out/continue-linux-build.sh`；它不是正式分发入口。
`out/trace-native-channels.py`、`out/trace-downstream.py` 是临时插桩工具，
恢复开发时不要将插桩产物当作正式回归产物。

恢复开发建议按以下顺序执行，用户恢复目标前不自动执行：

1. 阅读本文件、[兼容矩阵](COMPATIBILITY.md)、[验证记录](../VALIDATION.md)，
   检查三个仓库的现有改动和实际产物，避免覆盖未提交成果。
2. 优先分析已保存的 Windows 四轨失败日志，解决启动/分配问题；分别定位跨系统
   HTTP 401 与媒体故障。保持原有断言，补全失败摘要。
3. 修复后运行受影响的原生、浏览器及 CLI 回归，再验证 Linux 本机/TURN 和跨系统；
   记录真实版本/哈希、成功与失败，随后开展弱网、时延、容量和稳定性验收。
4. 基础媒体稳定后，依完整规划推进 SDK 设备/移动端、项目控制与部署，再做 RU/MCU/LU，
   最后进入分布式与扩展能力；不要将局部通过勾选为整个 P0/P1 完成。

常用入口：Windows `ctest --preset windows-release --output-on-failure`；
`tests/sdk_cli_integration.py`、`tests/sdk_cli_stream_integration.py`；
`tests/linux_wsl_integration.py`。浏览器、TURN 和网络故障命令见
[NETWORK_VALIDATION.md](NETWORK_VALIDATION.md)，依赖及构建见
[README](../README.md)、[补丁说明](../patches/README.md)。

## 8. 暂停记录

2026-10-26 用户明确要求整理现状后停止目标、后续再开发。交接文档已完成，
目标按要求暂停为 `paused`，未标记 `complete`；所有剩余交付和失败记录继续保留。
本次未启动新的构建、媒体测试、后台服务或后续自动开发任务。

## 9. 暂停后的本地提交（2026-10-26）

用户随后要求提交代码，本次将现有开发成果分别提交到三个仓库，不恢复开发目标。

- WebRTC：`40200b3d886ed5e2a81bd993c8f337e2b740d864`，原生 pacer 暂停/TTL 修复。
- SDK：`2cfd118f59cca8589c21792728dd65af775239f9`，原生 RTCStats 与 Windows task queue。
- 服务端：本节所在提交包含当前 SFU、分层与带宽分配、TURN、Linux 支持、测试和交接文档；
  可用 `git log -1 -- docs/STATUS_AND_HANDOFF.md` 定位本次交接提交。

提交前检查了工作区及 diff 格式；没有重新构建或重跑媒体测试，上一节的失败与
验收边界保持不变。忽略的测试结果和构建产物仍保留在本地，不包含在提交内。
本次只做本地 Git 提交，没有推送远端或部署。
