# 验证记录

## 当前增量：原生 pacer 暂停后的 TTL 修复（2026-10-26）

本轮找到一个与系统时钟校准无关、可在 Windows/Linux 确定性复现的缺陷：
M137 原生队列返回了扣除累计暂停时间的入队时刻，但 TTL 与调度使用实际时钟。
空队列暂停一秒后，新包仅入队 1 ms 就被当作超过 500 ms TTL。真实重订测试
中的累计暂停触发同类现象，原生日志显示新视频包不断过期，NACK 对应缓存为空。

- 修复前 Windows 失败证据：`out/pacer-pause-before.log`；Linux 同一断言也
  失败。修复在原生 `PrioritizedPacketQueue` 内使用它已保存的原始入队时间，
  没有增加 TTL、强制带宽、放宽媒体断言或重写 pacing。补丁及应用说明见
  [patches/README.md](patches/README.md)。相邻 WebRTC 工作树现在包含这份明确
  的本地补丁；此前“未改 WebRTC”的描述仅适用于那些历史增量。
- 原生回归覆盖空队列暂停、新包不误删、音频优先级、真实 TTL 过期、平均排队
  时间扣除暂停，以及八轮暂停。Windows/Linux 构建后 CTest 均 **4/4 通过**。
  Windows 日志 `out/pacer-pause-after-windows.log`；Linux 为
  `out/build-linux-ext4.log`。CMake 对未打补丁源码的拒绝也已实测。
- 临时透传跟踪 `tests/results/channel-trace/20261023-225827/` 还确认默认
  PeerConnection 媒体通道在 disabled 后仍发送 RTCP/TWCC。适配层撤下默认通道
  的发送接口，保留其 ICE/DTLS/SRTP 连接。仅此改动不能修复停滞：
  `channel-ownership/20261023-230046/`、`nack-trace/20261023-230503/`、
  `pacer-trace/20261023-230759/` 仍失败；所有临时日志插桩均已从正式产物移除。
- 最终 Linux 产物 `tests/results/pacer-ttl-fixed/20261026-083512/` 整体
  **ok=true**：REST TURN/UDP **12 组**、TCP **11 组**。每种传输都实测完成
  **五轮**取消/重新订阅，每轮要求 360p 恢复且已有音频与其他视频持续推进。
  原有各层切换、权限、错误/过期凭证拒绝、密钥脱敏与资源清理也通过。
- UDP 五秒中断丢弃上行 **599**、下行 **531** 包，放行后恢复
  **977 ms（RAW）/832 ms（Node）**。两个 SFU 均 exit=0，coturn 正常终止。
  仍是单次样本；一次高层重新开启耗时 **14,699 ms**，虽然符合现有 30 秒
  分层功能断言，但不能据此宣称快速画质恢复或平台时延门槛已经达标。
- 同一 ELF 的跨系统复测 `pacer-ttl-fixed-cross/20261026-083730/` 仍整体
  **ok=false**：Windows 浏览器 LOW 超时；文件四轨视频 **108/40** 帧，第二路
  未达 80 帧门槛；流测试在入房阶段得到 **HTTP 401**，未进入媒体验证。不能
  把这次流测试归为解码失败或仅凭 401 断言时钟根因。专用 Linux 进程 exit=0。
- Windows Chrome **9 组通过**，含五轮重订和原有音视频推进断言：
  `tests/results/pacer-ttl-fixed-windows/`。因此本次已证实原生暂停/TTL 缺陷的
  修复，但不证明跨系统故障均已解决。
- Windows CLI 文件回归 `tests/results/pacer-ttl-fixed-cli/20261026-084007/`
  **未通过**：合成媒体/数据/录制部分通过，文件视频 **108/78** 帧，第二路未达
  原有 80 帧门槛；音频 **406/408** 回调帧。采样排队峰值 **6 ms / 17,401 bytes**，
  发送失败和应用队列准入丢包计数均为零；记录到一次带宽暂停。该失败保留，
  不能以“仅差两帧”或 pacer 单测通过而忽略，也不据此直接断言其他根因。
- Windows 流测试 `tests/results/pacer-ttl-fixed-stream/20261026-084120/`
  通过 TCP H.264 输入转 VP8、定向订阅、静音和移除参与者。SDK 可执行文件未
  改动，SHA-256 仍为 `922963b8a267dd169ed2ac88ad8c35bb85c08042cda0d4f95a3261165a27ae0d`。

当前正式产物 SHA-256：Linux
`aaa13c270ffaa7749f4b4a44cf00ad0ef52be40857671fc6af603b5fc12e0339`；Windows
`6d52866b6bbd853d1d68683c62490d2ba6bda92c301f1f0e530569dc925577b7`。
本轮没有更改系统时钟设置。完整跨网络、P95、容量、移动端与长期稳定性仍待交付。

## 前一增量：coturn REST 临时凭证（2026-10-23）

服务端按参与者 SID 签发带到期时间的 coturn REST 凭证，支持从环境变量加载
共享密钥；签名使用 OpenSSL HMAC-SHA1/Base64，TURN URL 校验使用 libwebrtc。
入房响应与两条原生 PeerConnection 使用同一组临时凭证，共享密钥不进入响应。
RTC 传输、NACK/RTX、PLI、拥塞控制与 pacing 仍由原生 libwebrtc 实现。

- Windows/Linux 构建成功，CTest 均 **4/4 通过**。新增独立签名向量、TTL 类型与
  溢出拒绝、环境变量、密钥变更、URL 校验、静态 ICE 共存和入房凭证隔离检查。
  Linux 构建/CTest 日志为 `out/build-linux-ext4.log`；Windows 构建日志为
  `out/build-windows-turn-rest.log`。
- `tests/results/turn-rest/20261023-224659/result.json` 为 **ok=true**：真实
  coturn REST 模式 UDP **12 组**、TCP **11 组**通过。Node 独立核对实际浏览器
  ICE 配置的签名、到期时间和会话 SID；五条连接实际选中指定传输的 relay。
  错误密码和已过期但签名正确的凭证均完成 ICE gathering、返回 401、无 relay
  候选。两套 `secret_redaction_verified=true`，日志与浏览器 JSON 未包含共享密钥。
- UDP 五秒双向中断丢弃上行 **577**、下行 **546** 包；音视频恢复观测为
  **915 ms（CLOCK_MONOTONIC_RAW）**，Node 侧为 827 ms。该值是单次样本，
  不是 P95。两套服务进程均正常退出，SFU exit=0，无强制终止。
- 同一二进制的此前失败仍保留：`turn-rest/20261023-223525/` 的 UDP 通过，
  TCP 在关闭高层后未恢复 360p，发布端报告 bandwidth limitation；
  `turn-rest/20261023-224025/` 的 TCP 通过，UDP 在取消订阅后重新订阅未恢复
  360p。后者发布端仍编码三层，订阅端大量丢包/NACK，根因未定位。**本轮没有
  修复或消除这些间歇性媒体失败**，没有放宽断言，也不能将最终一次通过当作
  稳定性或跨系统验收。可用 `LK_CHROME_RTC_LOG=1` 保存浏览器诊断日志；
  浏览器构建决定实际可用的日志详细程度。
- 固定凭证回归 `tests/results/turn-static-regression/20261023-224846/`：TCP
  **10 组通过**，UDP 未通过 720p 检查。发布端高层 `framesEncoded=0` 且报告
  `qualityLimitationReason=bandwidth`；订阅端低/中层继续解码。这证明媒体失败
  也出现在固定凭证路径，不能仅归因于 REST 凭证或断网代理（此运行未启用代理）。
  根因与跨系统问题仍待定位；该运行顶层 `ok=false`，不计作完整回归通过。
- Windows 默认浏览器 **9 组**通过：`tests/results/windows-turn-rest-browser/`。
  原生四轨集成 `sdk-cli-integration/20261023-224526/` 通过，视频 **107/104**
  帧、音频 **405/404** 回调帧，采样排队峰值 **9 ms / 20,442 bytes**。
  `sdk-cli-stream/20261023-224622/` 通过，全量/定向视频 **300/209** 帧，
  音频 **1,103** 回调帧，采样排队峰值 **5 ms**。服务端视频编解码审计为零。

本轮二进制 SHA-256：

- Linux：`2b24e4f490c63786388af044aa5ddb4d47286dd74fb2a2e1a879cd436f8d5290`
- Windows：`4199f7130578d91dc4c7b54b447d8b3c313ff11f112580b5ab9155cdc446829a`
- Windows SDK：`922963b8a267dd169ed2ac88ad8c35bb85c08042cda0d4f95a3261165a27ae0d`

配置见 [TURN.md](docs/TURN.md) 与 `config.turn.example.json`。当前只在入房/完整
重连时签发；在线刷新、即时吊销、无损密钥轮换、跨到期长通话、TURN/TLS、公网
和完整弱网仍待验收。完整平台交付清单保持未完成。

## 前一增量：Linux 本机媒体、TURN-only 与中断恢复（2026-10-23）

同一 Linux ELF 已通过 Linux 本机 Chrome 的 **9 组**完整媒体测试。此前的
Windows/WSL 跨系统失败仍未解决，不能把本机通过扩大为跨系统或公网支持声明。
本轮没有修改生产媒体代码或 libwebrtc 源码，没有更改系统时钟配置。

- 可重复入口：`scripts/test-linux-browser.sh`，支持明确指定 Linux server 和
  Chrome 路径。证据为 `tests/results/linux-browser/20261023-222127/`，记录
  Linux x64、Node 22.16.0、Chrome 156.0.8078.47、实际二进制哈希，以及
  服务端退出码 **0**。另有首次本机通过记录 `linux-local-browser/20261023-220803/`。
  原九组断言保持不变，涵盖真实视频解码、非静音音频、三层 simulcast、独立
  订阅、静音/重订、权限和清理；最终队列、分配与活动传输归零，视频编解码为零。
- `tests/turn_integration.py` 启动并清理独立 coturn 和 SFU 进程。Ubuntu coturn
  包版本为 **4.5.2-3.1~ubuntu22.04.1**；本轮解包在测试工具目录中，未启动系统
  coturn 服务。随机测试凭证和 loopback 许可仅适用于本机验收。
- TURN/UDP 和 TURN/TCP 均通过媒体全流程；浏览器强制 `iceTransportPolicy=relay`，
  进一步检查五条活动发布/订阅连接实际选中 relay，且 `relayProtocol` 与套件
  一致。TCP 组关闭 coturn UDP 客户端监听；这不是 TURN/TLS 或公网防火墙测试。
  基础通过证据：`tests/results/turn/20261023-221336/`，两套各 **10 组**。
- `--outage` 在 TURN/UDP 前插入透明数据报代理，五秒内直接丢弃所有双向数据报，
  不缓存、不补发、不解析 RTP。断言视频确实停止推进，恢复后出现至少六个新
  解码帧和新增非静音音频。故障持续时间和十秒恢复门槛使用 `CLOCK_MONOTONIC_RAW`；
  此时钟仅用于测试计时，生产 WebRTC 保持原生时钟。
- 最终 UDP 故障证据：`tests/results/turn/20261023-222440/udp/browser/recovery.json`。
  上行丢弃 **594 包**、下行 **661 包**，无代理错误；放行到音视频恢复的未校准
  时钟观测为 **1,301 ms**，Node 侧观测为 1,135 ms。UDP 套件 **11 组通过**。
  同一最终运行的 TCP 套件 **10 组通过**，顶层 `result.json` 为 `ok=true`；
  两个 SFU 进程退出码均为零，coturn 均经 SIGTERM 停止，无强制终止。
  早期恢复样本另存 `20261023-221717`、`20261023-222255`，不以它们宣称 P95。
- 更新后的 Windows 浏览器默认路径仍通过 **9 组**，证据为
  `tests/results/windows-turn-harness-browser/result.json`。本轮未更改 C++ 产物，
  原生 CTest 和两套 CLI 的最近通过证据见前一增量，未重复无变化的编译。

Linux ELF SHA-256 仍为 `66dc71cc065d5eeb5afa742070093bd06e5de112d16c66d3e88422470e4912e5`。
运行命令及边界见 [NETWORK_VALIDATION.md](docs/NETWORK_VALIDATION.md)。跨系统媒体、
公网 NAT、TURN/TLS、凭证轮换、换网、弱网 P95、容量/长期稳定性、移动端和其他
完整 P0–P3 交付仍未完成。

## 前一增量：Linux 原生构建与跨系统验证（2026-10-23）

Linux ELF 构建及 CTest **3/3 通过**，但跨系统媒体整体验收**未通过**。
Windows 重新构建后，CTest **3/3**、浏览器 **9 组**与两套 CLI 集成全部通过。
原生 WebRTC 仓库保持干净；没有修改 RTP/RTCP、NACK/RTX、PLI、GoogCC 或 pacing。

- Linux 为 WSL Ubuntu 22.04.5 x64，Clang/libc++ 15、CMake 3.31.6、Boost 1.74、
  系统 libvpx 1.11.0 与校验归档构建的 dav1d 1.5.3。完整依赖见兼容性矩阵。
  应用适配层处理 PFFFT libc 特性宏、AOM NASM 的 `HAVE_UNISTD_H` 冲突、
  Boost 1.74 的 `invoke_result` 检测，以及一处结构化绑定的 lambda 捕获。
  依赖脚本已验证固定提交的离线引导和重复运行；在线 GitHub 获取曾超时。
- 构建与 CTest 日志：`out/build-linux-ext4.log`；构建目录为
  `/root/livekit-validation-20261023-2038/livekit-cpp/out/linux-clang15`，
  ELF 位于其 `bin/livekit-server`。不是完全静态可执行文件，动态依赖已记录。
- `tests/results/linux-wsl/20261023-215424/result.json` 整体 `ok=false`：
  浏览器 HIGH simulcast 超时，四轨录制未达到原有帧数门槛；流测试单次通过，
  全量/定向视频 **168 / 92 帧**、音频 **1,113 回调帧**，采样排队峰值 **260 ms**。
  三套测试使用同一专用进程，累计计数包含先前套件，不能把累计暂停数当作单套增量。
  `20261023-214815`、`20261023-215019` 另保留 LOW 失败和原生 info 日志。
- `tests/results/wsl-clocks/20261023-215750.json` 记录 Linux MONOTONIC
  **10.017359 s**，RAW **10.926791 s**，Windows **10.926949 s**；主客机单调
  时钟差约 **9.08%**。主机采样包含管道和调度延迟，但 RAW 与主机接近。
  内核曾记录 >11% TSC 调整，读取到的内核 tick 为 9167 微秒；原生日志报告
  时延增长限制探测。这是诊断线索，不能认定所有媒体失败均由环境造成。
- 临时 Hyper-V/ACPI 时钟源比较未解决差异；临时 tick=10000 对照也未解决：
  `tests/results/linux-wsl/20261023-220100/result.json` 三套测试均失败，包含
  此前单次通过的原生流测试。原 tsc 和 tick=9167 已恢复，恢复记录为
  `tests/results/linux-wsl/tick-comparison-20261023-220049.json`。未停用系统
  时间服务，未引入生产时钟替代实现。Linux 媒体通过结论不能成立。
- 所有 Linux 测试服务进程均由驱动持有并正常退出，退出码为零；失败没有降低
  媒体断言。驱动保存实际 ELF、哈希、运行库、逐套日志及整体失败状态。
- Windows 四轨视频 **103 / 104 帧**，音频 **409 / 408 回调帧**；采样排队峰值
  **14 ms**。证据：`tests/results/windows-linux-portability-files/20261023-215701/`。
  流测试全量/定向视频 **229 / 144 帧**，音频 **1,105 回调帧**，排队峰值
  **5 ms**、带宽暂停为零；证据：`tests/results/windows-linux-portability-stream/20261023-215810/`。
  浏览器结果：`tests/results/windows-linux-portability-browser/result.json`。

Linux 服务端 SHA-256：`66dc71cc065d5eeb5afa742070093bd06e5de112d16c66d3e88422470e4912e5`。
Windows 服务端 SHA-256：`88179c9e45f9e98c485632c138ec527e20dfd897e836233dac19c9320ab89910`。
SDK SHA-256：`922963b8a267dd169ed2ac88ad8c35bb85c08042cda0d4f95a3261165a27ae0d`。

Linux 持续媒体、完整弱网/TURN/恢复、移动端、容量/长期稳定性与其他 P0–P3
交付仍待完成。下一步需要在时钟稳定环境复验，同时继续定位实际媒体失败。

## 前一增量：原生计时队列、纯视频启动与 SDK 统计（2026-10-23）

Windows 服务端 CTest **3/3**、SDK CTest **7/7**、浏览器 **9 组**及两套
CLI 集成均通过。SDK 本轮增加可选 `rtc-stats` 事件，并显式接入 libwebrtc
的 Windows 原生计时队列。WebRTC 仓库源码保持未修改。

- 本地 WebRTC 清单默认 stdlib 计时队列，曾使探测发送呈约 15 ms 粒度。
  诊断日志显示探测簇只发送约一半所需数据就中断；应用构建现在直接编译
  `task_queue_win.cc`，通过原生 factory 注入服务端、SDK 与 SFU 环境。
  SFU 共享一条原生计时队列唤醒网络线程，用原生任务安全标记取消退出后的
  回调；RTP 状态及发送仍在网络线程。没有新增探测算法或操作系统计时实现。
- 纯视频订阅的零帧问题来自先启动探测、后挂接 RTP sender：空 padding
  请求使原生 pacer 在首包前进入发送失败状态。现在等待 sender 挂接后才
  将传输标为可用，并独立检查 SRTP 就绪。回归测试先建立连接、空等 600 ms，
  再挂接预算暂停的纯视频 sender，要求仅靠原生 padding 发出探测。
- 层被用户上限或失活移除时，立即回退到可用且可负担的目标，避免额外升层
  等待。恢复时若一半视频预算仍可满足目标，要求该余量持续 200 ms；普通
  恢复及升层仍要求一秒。测试覆盖余量等级变化重置等待及总预算守恒。
  新增冷启动模拟用例中首个 450 kbps 视频高于初始预算，经原生探测在
  200 ms 获得分配；这不是实际通话首帧时延。
- 四轨文件发布：视频 **101 / 104 帧**（每路输入 120 帧），音频
  **410 / 409 个回调帧**；采样排队峰值 **10 ms / 28,631 bytes**。
  证据：`tests/results/sdk-cli-integration/20261023-210935/media-evidence.json`。
- 流测试：全量视频 **300 帧**，指定视频 **204 帧**，音频 **1,105 个回调帧**；
  采样排队峰值 **14 ms**、带宽暂停为零。发布端原生统计约 30 fps，首批
  有效样本的可用上行带宽约 3.99 Mbps，质量限制为 `none`。
  证据：`tests/results/sdk-cli-stream/20261023-210840/result.json` 和
  `native-stats.json`。统计默认关闭，未启用的字段不会补造，候选地址和证书
  记录不输出。采样文件同时保存服务端与 SDK 二进制哈希。
- 浏览器三个真实 VP8 层、独立订阅、静音和重订均通过，记录 9 次切层。
  关闭房间后活动传输、队列和分配归零；视频 Encode/Decode 始终为零。
  最终日志为 `tests/results/integration.log`，停发高层后观察到 360p 用时
  2,174 ms，包含两秒失活窗口。这些单次样本不构成 P95 验收。
- 过程中保留了零视频、78 帧等失败记录，未降低原有帧数或排队断言。
  曾实验启用 SDK 额外探测配置，未保留；最终使用原生默认探测参数。

最终服务端 SHA-256：
`f6baaf048ef4870130ac4147ec6795e6fda010f92c8027779170d3180faf3155`。
SDK SHA-256：
`922963b8a267dd169ed2ac88ad8c35bb85c08042cda0d4f95a3261165a27ae0d`。

Linux 仍在首次构建验证中，尚无成功运行/媒体互通结论。已在 WSL Ubuntu
22.04 x64 安装工具链，适配 Boost headers、pkg-config libvpx、PFFFT 的
glibc 数学常量。系统 dav1d 0.9.2 缺少 M137 所需 API；已通过校验归档构建
dav1d 1.5.3，并增加配置阶段检查和可重复执行的依赖脚本。当前原生文件系统
构建目录为 `/root/livekit-validation-20261023-2038/livekit-cpp`，日志为
`out/build-linux-ext4.log`，dav1d 构建日志为 `out/build-linux-dav1d.log`。
完整弱网、TURN、100 会话、24 小时稳定性、移动端和其他 P0–P3 交付仍未完成。

## 前一增量：共享带宽分配、音频预留与优先级（2026-10-23）

Windows Release 构建、CTest **3/3**、浏览器 **9 组**与两套 SDK CLI 集成
均通过。SDK 仓库未改动。应用层每 200 ms 根据原生
GoogCC 的目标分配带宽，支持音频预留、订阅优先级、视频暂停/恢复、立即
降层及持续余量下的延迟升层。RTP/RTCP、NACK/RTX、PLI、码率估计、pacing、
padding 和探测均由 libwebrtc 完成；没有新增协议栈或应用层带宽估计器。

- 策略测试覆盖共享预算守恒、音频优先、基础层分配、显式优先级、初次启动、
  暂停/恢复、反复涨跌重置等待、用户上限及变化码率下的 2,000 次分配。
- 原生测试覆盖真实 REMB/TWCC 驱动目标变化，暂停清除媒体队列/重传历史，
  暂停期间仅保留 padding、恢复等待关键帧、恢复不取消已排队的 padding，
  以及设置 padding 需求后无反馈时仍继续原生初始探测。
- 60 秒模拟链路使用共享 FIFO、25 ms 传播延迟和最多 250 ms 的尾丢弃队列，
  容量从 1.5 Mbps 降为 100 kbps，再恢复至 1.5 Mbps。测试和实际 SFU 共用
  分配与 padding 策略，原生 TWCC 驱动高层、暂停和高层恢复。最低目标
  79,986 bps，恢复阶段最高目标 1,408,280 bps；链路丢弃 56 包，音频收到
  2,975 包，最大相邻接收间隔 97 ms。模拟器同步原生全局 TimeMillis 与
  Environment 时钟，因为 M137 ALR 检测和探测控制分别使用这两个时钟。
  这是合成 RTP 的传输测试，不等价于真实视频解码质量、音频听感或公网验收。
- 最终 SDK 四轨文件发布解码视频 **103 / 102 帧**（每路输入 120 帧），音频
  **405 / 404 个回调帧**，采样预计排队峰值 **5 ms / 6,363 bytes**，实际探测
  反馈包 2,532 个。服务端视频 Encode/Decode 为零。证据：
  `tests/results/sdk-cli-integration/20261023-200645/media-evidence.json`。
- 本轮过程中四轨测试曾低于原有 80 帧门槛，未降低断言。修复包括区分首次
  启动和拥塞恢复、保留原生重复初始探测、避免 padding 填满空闲带宽而掩盖
  ALR、首次关键帧即时分配、避免初始化/恢复时重复清队列打断探测。最终
  二进制 SHA-256 为
  `87692c3c0fe3a51a8a3e1cb582a7f174cc4611d2a1f089ffdde59c1f146f2482`。
- 最终浏览器验证三个实际 VP8 编码层、独立质量/尺寸设置、音频持续、退订/
  重订和停发层恢复；记录 13 次切层，媒体分配满足共享预算，关闭房间后
  队列、控制器和分配指标归零。手动 LOW→HIGH 观察 1,226 ms，停发最高层
  后首次观察到 360p 为 3,755 ms，包含失活窗口、选层及关键帧等待；不宣称
  P95 或快速切换验收通过。日志为 `tests/results/integration.log`。
- 最终流测试验证 TCP H.264 输入（发布仍为 VP8）、定向订阅、静音和踢人；
  全量视频 140 帧、指定视频 104 帧、音频 1,106 个回调帧。证据为
  `tests/results/sdk-cli-stream/20261023-200744/result.json`。全量帧数低于前一
  增量的 246 帧，仍需跟进冷启动/分配引入的帧数波动；功能测试通过不代表
  本增量已经完成性能验收。

采样预计排队时间不是 capture-to-playout 延迟；完整弱网、跨 NAT/TURN、
100 会话容量、24 小时稳定性、音画同步及移动端验收仍未完成。音频需求超过
链路预算时会停止视频分配，但这不能保证所有音频都能送达。完整实施目标
仍保持未完成，详见 `docs/IMPLEMENTATION.md`。

## 前一增量：VP8 simulcast 协商与订阅选层（2026-10-23）

Windows Release 构建、CTest **3/3**、浏览器 **9 组**及两套 C++ SDK CLI
集成全部通过。SDK 仓库未改动，本次新增的是服务端选层与互通验证。

- 使用真实 Chrome/官方 Web SDK 发布三个 VP8 编码层（320×180、640×360、
  1280×720），验证两个订阅者独立选择 LOW/HIGH、按目标尺寸选择 360p，
  接收端实际解码尺寸变化，且订阅 SSRC 始终不变。
- 验证音频持续接收、尺寸设置跨退订/重订保留，以及通过原生 sender 参数
  停用最高层后的 360p 回退、恢复最高层后的 720p 恢复。
- 本轮记录 9 次层切换；手动 LOW→HIGH 等待新增解码帧约 120 ms，
  HIGH→目标尺寸约 351 ms；首次高层订阅观察等待 2145 ms，停发回退约
  2184 ms（包含 2 秒失活窗口）。这些是一次本地运行的观察时间，不是
  capture-to-playout 延迟，也不足以证明首帧/切层 P95 达标。
- 原生 JSEP 适配解决 RID-only 默认接收参数缺少扩展映射，以及默认 Answer
  不协商 receive simulcast 的问题。各层独立复用 native NACK/RTX/组帧；
  只在目标关键帧处切换，沿用 native VP8 打包，不进行视频编解码。
- 浏览器结束审计为视频 Encode/Decode 调用 0；房间、参与者、轨道、队列、
  活动预算归零。`sfu_video_frames_gated` 包括原生组帧返回的重复帧、重传/探测
  负载及等待关键帧等情况，不可直接解释为视频丢帧率。
- 核心测试覆盖质量/尺寸策略、边界和单层回退、非法层元数据、保存订阅设置、
  未支持 fps 请求的原子拒绝、未知轨道不增长状态及发布者离开后的清理。
- 最终 CLI 四轨视频 98 / 102 帧、音频 406 / 405 个回调帧，采样预计排队
  峰值 16 ms；流测试全量视频 246 帧、指定轨道 160 帧、音频 1105 个回调帧。

最终二进制 SHA-256：
`d90ff8e9f9ed838fc080ffa8cc1ef03443d0b2c4a5e1decd57fd3084595d91ad`。

证据：`out/build-layers.log`、`out/windows-x64/Testing/Temporary/LastTest.log`、
`tests/results/integration.log`（含每次解码尺寸与等待时间）、
`tests/results/sdk-cli-integration/20261023-193451/media-evidence.json`、
`tests/results/sdk-cli-stream/20261023-193515/result.json`。

尚未完成带宽到订阅/层的分配、音频预算保留、优先级/帧率限制、dynacast/SVC、
真实弱网、端到端同步/延迟和容量验收。C++ SDK 的多层发布 API、移动端及其他
平台交付仍须单独实现和验证；完整平台目标保持进行中。

## 历史增量：原生探测初始化时序修复（2026-10-23）

已定位上一轮排队退化：传输先就绪时，M137 在首个 OnProcessInterval 内先
ResetConstraints 发起探测，随后才应用重复初始探测配置，导致首轮探测失败
后没有重试窗口。适配器现在先在网络不可用状态完成初始化，再允许 ICE 就绪。
仍使用原生探测/拥塞估计，未手动提升目标码率，也未修改 libwebrtc。

- 定向模拟时钟测试覆盖“网络在首个任务前可用，首轮无反馈，必须重试”；
  CTest **3/3**、浏览器 **8 组**、两套 SDK 集成全部通过。
- 关闭详细日志的最终四轨测试：视频 **99 / 108 帧**、音频 **411 / 410 个
  回调帧**；采样预计排队峰值 **30 ms**、队列峰值 **8484 字节**，目标带宽
  峰值 5448014 bps；探测簇 17 个、探测发送/反馈匹配各 2322 包。
- TCP 输入测试：全量接收视频 **243 帧**、指定轨道视频 **159 帧**，音频
  1103 个回调帧；静音控制和移除参与者检查通过。
- 浏览器审计确认真实探测反馈、服务端视频 Encode/Decode 调用为零，结束时
  房间、参与者、轨道、队列、活动预算归零。
- CLI 回归新增每条视频至少 80 帧（本地源发布 120 帧）、采样预计排队小于
  500 ms、探测反馈存在及零服务端视频编解码断言；未降低原有断言。

最终二进制 SHA-256：
`047eae1dcec6117d88f30f58ed04be09aa69dc4195e17769fc0e2a9fd7197b57`。

证据：`out/build-demand.log`、`out/windows-x64/Testing/Temporary/LastTest.log`、
`tests/results/integration.log`、
`tests/results/sdk-cli-integration/20261023-191049/media-evidence.json`、
`tests/results/sdk-cli-stream/20261023-191113/result.json`。
原生日志定位的失败运行保留于 `20261023-190656/`；修复后开启详细日志的
运行 `20261023-190853/` 也通过（排队峰值 10 ms、视频 102 / 104 帧）。

这修复了本地四轨场景中的已知退化，不等于真实弱网、端到端延迟、100 会话
和 24 小时验收。simulcast、订阅带宽分配和其余平台交付仍未完成。

## 历史增量：原生 VP8 组帧、打包与帧级准入（2026-10-23）

Windows Release 构建、CTest **3/3**、浏览器 **8 组**和两套 SDK CLI 集成
全部通过。协议与组帧处理复用 libwebrtc；原媒体帧数及音频断言未降低。

- `native-rtp` 新增编码字节逐字节验证、丢失一个分片后 NACK/RTX 恢复、
  乱序/重复分片、原生帧依赖发现、旋转/尺寸保留、切换源等待关键帧、拒绝
  缺少依赖的帧、RTP 时间戳回绕、SR 映射和静音后关键帧恢复。
- 队列容量按整帧批次检查，并计入原生传输开销；新增边界测试，验证拒绝后
  队列不包含半帧、不消耗输出序号、依赖帧被阻止、后续关键帧可恢复。
- 浏览器结束审计：组帧 87、各订阅成功入队 75、依赖/重复/时序等检查拒绝
  35；发送 309 包，其中 padding 48 包；收到下行反馈 87 次，关联 309 包。
  服务端视频 Encode/Decode 调用为零；房间、参与者、轨道、队列和活动预算
  在结束时归零。帧入队数不是送达/解码帧数。
- SDK 合成视频解码 177 帧、音频 605 个回调帧；四轨文件的两条视频分别
  解码 72 / 72 帧、音频 407 / 407 个回调帧。流测试全量视频 225 帧、
  指定订阅视频 142 帧、音频 1105 个回调帧，控制事件及踢人检查通过。

**性能问题仍未解决**：四轨测试采样的队列峰值 196856 字节，原生 pacer
预计排队时间峰值 1568 ms，目标带宽峰值 475852 bps；文件视频解码帧数低于
上一增量的 108 / 109 帧。该数据说明功能通过不能作为性能验收，需优先查明
原生探测/带宽预算与媒体需求的联动，结合选层和入队后过期恢复解决积压。
1568 ms 是预计排队时间，不是实测端到端延迟；目前未宣称符合延迟指标。

当前二进制 SHA-256：
`C1EF59D4EABEBF2B1B4097BED3DFF255F113EEC0E57E6FEBB83492108962B923`。

证据：`out/build-frames.log`、`out/windows-x64/Testing/Temporary/LastTest.log`、
`tests/results/integration.log`、
`tests/results/sdk-cli-integration/20261023-190201/`（含 `metrics.jsonl`）、
`tests/results/sdk-cli-stream/20261023-190225/result.json`。

仍是单源 SSRC 的线上转发；多源切换仅有原生适配器单测，真实 simulcast
发现/信令/选层、带宽分配、弱网和容量验收均未完成。完整平台目标保持进行中。

## 历史增量：下行原生 pacing / GoogCC（2026-10-23）

Windows Release 构建成功，CTest **3/3**、浏览器 **8 组**、两套原生 SDK
集成测试全部通过。原媒体帧数、非静音音频和录制断言保留。

- 新增 `native-send` 使用模拟时钟驱动真实 libwebrtc pacing、GoogCC 和反馈
  模块。验证音频先于视频、视频分时发送、音视频/RTX/padding 共享传输序号、
  TWCC 关联发送记录、REMB 降低目标速率、RTX 经过同一队列、静音/析构清队列、
  不可用传输的媒体入队容量保护。
- 浏览器检查新增实际下行反馈断言，并在删房后验证队列与活动控制器归零。
  最后一次审计为 `sfu_paced_sent=346`、`sfu_downstream_feedback=94`、
  `sfu_feedback_packets=345`、`sfu_padding_sent=18`、
  `sfu_pacer_admission_drops=0`。测试结束时媒体队列字节与带宽预算为零，
  服务端视频 Encode/Decode 调用仍为零。
- 上行和下行分别维护自己的原生反馈状态，实际 socket 发送时间进入下行
  TransportFeedbackAdapter。RTX 序号在出队发送时由原生 PacketSequencer
  分配；初始重复探测使用 GoogCC 原生机制，未自写 pacing 或带宽估计算法。
- 新增 CLI 运行指标采样 `metrics.jsonl`，保留反馈、发送预算和排队状态，
  便于对照媒体帧数分析。一次较早的四轨测试出现帧数不足；增加原生初始重复
  探测并修正发送时序后，两轮完整 CLI 回归通过。这不代替长期弱网验收。

最终二进制 SHA-256：
`CAAF50643732ECEF54D86524B45E8C66D8EAA01223550D75052D0F4DF9F9A037`。

证据：`out/build-paced.log`、`out/windows-x64/Testing/Temporary/LastTest.log`、
`tests/results/integration.log`、
`tests/results/sdk-cli-integration/20261023-184413/`、
`tests/results/sdk-cli-stream/20261023-184413/`。

尚未完成：带宽预算到订阅/选层的分配、simulcast/SVC、拥塞时的帧级准入与恢复、
长断线的音频新鲜度、真实弱网/100 会话/24 小时测试，以及 Linux/mobile 等平台
交付。原生队列的超时和媒体入队上限仅是保护措施，不代表上述质量门槛已通过。

## 前一增量：libwebrtc 原生包级 SFU（2026-10-23）

本节记录当前默认 `media_mode=sfu`；后面的旧记录是解码重编码 relay
的历史基线，不代表当前媒体实现。

- Windows x64 Release 构建成功：`livekit-server`、`livekit-tests`、
  `livekit-native-rtp-tests`，复用相邻 M137 WebRTC 静态库。
- CTest **2/2 通过**。新增测试使用原生 RTP/RTCP 构造器、模拟时钟与原生
  任务队列，验证负载不变、SSRC/PT/MID/旋转扩展映射、序号跨界/乱序、NACK
  缓存重传、RTX 恢复、PLI 的 SSRC 隔离、静音缓存清理、SR 时钟映射，以及
  主动丢包后的原生 NACK→RTX 恢复。跨音视频的 TWCC 反馈包含正确的缺包信息。
- 浏览器集成 **8 组通过**，未降低媒体断言。最后一次初始接收检查为视频
  6 帧、音频 16320 个采样、能量 0.0026305。整个测试结束后审计为
  `sfu_rtp_forwarded=291`、`sfu_rtp_dropped=0`、`video_encode_calls=0`、
  `video_decode_calls=0`；房间、参与者、轨道均归零。
- 原生 SDK 两套集成测试通过，媒体帧数和录制断言保持原样：合成音视频、数据、
  文件四轨发布、中文路径、EOF 退出、TCP H.264 输入转 VP8 发布、选择性订阅、
  静音控制和移除参与者。流测试全量接收端解码视频 **229 帧**，选择接收端
  解码视频 **145 帧**。

验证中发现并修正了 SRTP 发送标志、协商了未接入的 RED/ULPFEC、以及上行
拥塞反馈缺失造成的降码率问题。当前协议处理使用 libwebrtc 原生组件；首阶段
协商限定 Opus/VP8/RTX。上行使用 ReceiveSideCongestionController 产生
TWCC/REMB，下行 pacing、拥塞控制、带宽分配和 simulcast 尚未完成。

证据（本地运行产物）：

- `out/build-sfu.log`、`out/build-native-rtp.log`
- `tests/results/integration.log`、`tests/results/server.log`
- `tests/results/sdk-cli-integration/20261023-182148/result.json` 及收发日志
- `tests/results/sdk-cli-stream/20261023-182148/result.json`

这些结果仅覆盖同机 Windows 和定向丢包单测；不表示通过真实弱网、跨 NAT/TURN、
Linux/mobile、100 会话或 24 小时稳定性验收。完整平台任务仍在实施中。

## 历史基线：解码重编码 relay

日期：2026-10-22（Asia/Shanghai）。

## 环境与构建

- Windows x64，Visual Studio 2022 / MSVC 19.44，Release，静态 CRT。
- C++20，WebRTC 来自 `D:/workspace/webrtc` 当前工作区及 `out/windows-x64` 的 Release 静态库。
- `cmake --build --preset windows-release --target livekit-server livekit-tests` 成功。
- 产物：`out/windows-x64/Release/livekit-server.exe`。
- 独立源码集成配置 `out/source-config` 已成功 configure/generate；本次运行验证复用本地 WebRTC 静态库，未在该新目录重复全量编译 WebRTC。
- 原 `D:/workspace/livekit` 仓库无改动。

## CTest

`ctest --preset windows-release`：1/1 通过。

检查 JWT 签名、过期和生效时间、权限、protobuf、原生数据通道 SDP、房间容量、重复身份替换、删除房间和资源清理。

## 官方 SDK 集成

`cd tests; npm test`：8 组通过。

固定依赖：`livekit-client 2.15.6`、`livekit-server-sdk 2.13.3`、测试端 `@livekit/protocol 1.39.3`、`playwright 1.56.1`。服务端消息定义使用当前 Go 仓库的 `livekit/protocol f09f87aab29b`。

1. 官方 RoomService SDK 建房、列房、JWT 拒绝；Twirp 二进制 protobuf 请求/响应。
2. 两个浏览器 SDK 客户端通过二进制信令、ICE、DTLS 入房。
3. 原生音视频转发：接收端实际解码视频，440 Hz 合成音频有可测能量。最后一次运行视频 `framesDecoded=6`，音频 `totalSamplesReceived=11520`、`totalAudioEnergy=0.0015532859643880986`。
4. 可靠与非可靠数据通道消息，以及认证发送者身份。
5. 定向 SendData、参与者元数据和房间元数据更新。
6. 静音/取消静音、退订/重订及重订后的持续视频解码。
7. 后加入且 `autoSubscribe=false` 的手动订阅，以及反向视频发布。
8. 权限限制、运行中撤销发布权限、重复 identity、移除参与者、删除房间。

最后一轮详细输出在 `tests/results/integration.log` 和 `tests/results/server.log`（运行产物，不纳入版本管理）。

## 尚未验证或尚未实现

本次使用同机浏览器和合成媒体，没有做跨 NAT/TURN、长时间压力、多节点、真实设备全矩阵或其他语言 SDK 的联调。当前是解码/重编码 relay，非原版包级 SFU；完整支持范围和未实现功能见 README。

## C++ SDK CLI 联调（2026-10-23）

使用当前 `livekit-cpp 0.1.0` 服务端与同级 `livekit-sdk-cpp` 的 `lk-cpp 0.3.0`，SDK commit 为 `5b0bdc8446d5c92e191f0bca15abf0fd5f4c84b6`。两端均重新执行 Windows Release 增量构建成功；`ctest --preset windows-release` 为 **1/1 通过**。本轮服务端源码未修改，新增了可复用的 CLI 测试脚本。

测试脚本来自 SDK 的 `tests/integration.py`、`tests/stream_integration.py`，适配当前 C++ 服务端的 JSON 配置、独立 ICE 端口段与结果目录。原媒体质量和帧数断言保留。测试启动自己的 C++ 服务端及 CLI 子进程，结束后已清理；未使用机器上另一个正在运行的 Go 服务端。

| 测试 | 结果 | 实际证据 |
| --- | --- | --- |
| JWT、房间 API、错误输入 | 通过 | JWT 独立 HMAC 验签，建房/列房/更新/参与者查询/删房，无效 Token 和非法视频尺寸拒绝 |
| CLI 合成 VP8 / Opus 收发 | 通过 | 接收视频 171 帧，音频 609 个回调帧；WAV 为 48 kHz 单声道、292320 个采样，峰值 6241、RMS 4224.07 |
| 可靠数据 | 通过 | 收到 `hello native`，topic=`test`，identity=`publisher`，reliable=`true` |
| FFmpeg 文件、中文路径、四轨发布 | 通过 | 两条视频分别解码 90 / 93 帧，两条音频分别 407 / 408 个回调帧，均有非静音 WAV；EOF 后正常退出 |
| TCP H.264 输入 | 通过 | CLI 将 H.264 输入解码后以 VP8 发布，全量接收端解码 233 帧视频 |
| 指定轨道订阅 | 通过 | 只接收到所选视频轨道，解码 147 帧 |
| 静音 / 恢复 | 通过 | 发布端收到 `track-muted` 的 true / false 事件 |
| 服务端踢人 | 通过 | CLI 收到 `server-leave`，reason=4，并以非零状态退出 |

流测试第一次运行因测试直接查找 `type == "AUDIO"` 而失败。当前服务端使用标准 Protobuf JSON 默认序列化，`AUDIO=0` 的 `type` 字段可省略；测试改为按协议读取默认值后完整通过。原始失败日志保留在 `tests/results/sdk-cli-stream/20261023-142854/`，成功运行的 `participants.json` 保留了实际 API 响应。这是测试适配，不涉及服务端媒体逻辑变更。

成功结果与证据：

- `tests/results/sdk-cli-integration/20261023-142813/result.json`
- `tests/results/sdk-cli-integration/20261023-142813/evidence.json`（二进制 SHA-256、录制采样统计、轨道统计与数据事件）
- `tests/results/sdk-cli-stream/20261023-142922/result.json`
- 构建及单测日志：`out/build-cli-validation.log`、`out/build-sdk-cli-validation.log`、`out/ctest-cli-validation.log`。

本轮验证范围为同机 Windows CLI 联调；未新增跨 NAT/TURN、长时间压力或物理设备验证。静音检查验证了控制事件，未量化静音期间的 PCM 能量；H.264 是输入格式，网络发布编码仍为 VP8。本轮未重跑上方的浏览器集成测试。
