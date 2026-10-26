# livekit-cpp

[媒体服务设计](docs/design.md)说明 SFU 模块、原生依赖、业务服务边界和后续 worker 架构；[平台总体设计](../docs/platform-design.md)定义跨仓库契约。

当前开发按用户要求暂停（2026-10-26），完整目标尚未完成。已实现功能、未交付能力、
最新失败及后续开发入口见 [功能现状与暂停交接](docs/STATUS_AND_HANDOFF.md)。

使用 C++20 实现的 LiveKit **单机服务端兼容项目**。原生 WebRTC 直接来自 `D:\workspace\webrtc`，不启动 Go 服务，也不经由 Rust FFI。

默认媒体链路是 **基于 libwebrtc 原生模块的 RTP 包级 SFU**，支持 Opus / VP8 / RTX，以及最多三层 VP8 simulcast 的订阅选层。转发保留编码帧内容，服务端视频编解码调用数通过集成测试检查为零。NACK、RTX、PLI、重传缓存、RTCP 解析和上行拥塞反馈复用 libwebrtc。下行复用原生 pacing 和 GoogCC，应用层根据其预算进行音频预留、视频优先级分配和升降层。容量及完整弱网验收仍待完成，当前吞吐量不能按商业 RTC 产品估算。完整实施范围见 [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)。

配置 `"media_mode": "relay"` 可运行原先的解码重编码链路，默认值为 `"sfu"`。SFU 内部适配器依赖相邻 M137 WebRTC 的内部接口；升级 WebRTC 时必须重新验证，当前不宣称生产就绪。

## 构建（Windows x64）

先按 [原生依赖补丁说明](patches/README.md) 将队列暂停/TTL 修复应用到固定版本的
WebRTC。使用预编译库时也必须重新构建原生库；CMake 检查源码，`native-send`
回归测试检查实际链接的库。RTP/RTCP、重传、拥塞控制和 pacing 仍复用 libwebrtc。

依赖 Visual Studio 2022 C++ 工具、CMake 3.24+、Git、vcpkg，以及相邻的 WebRTC 源码及其 CMake 依赖。使用静态 CRT `/MT`，Debug 对应 `/MTd`。

```powershell
cd D:\workspace\livekit-cpp
.\scripts\bootstrap.ps1 -VcpkgRoot D:\vcpkg
$env:VCPKG_ROOT = 'D:\vcpkg'
cmake --preset windows-x64
cmake --build --preset windows-release
ctest --preset windows-release
```

默认在本项目的 `out` 目录编译已应用补丁的 WebRTC；构建过程不会自动修改依赖源码。
服务端构建排除了本地摄像头采集模块。修改依赖位置可传 `-DWEBRTC_ROOT=其他路径`。

如果指定目录中已经成功构建了 WebRTC 的 Release 静态库，可以复用：

```powershell
cmake --preset windows-x64 -DWEBRTC_PREBUILT_DIR=D:/workspace/webrtc/out/windows-x64
```

预编译导入只支持本地 WebRTC CMake 工程的 Windows Release 布局；Debug 或其他布局使用默认源码构建。

Windows 构建另外编译 libwebrtc 自带的 `task_queue_win.cc`，显式使用其 Windows
计时队列。相邻 WebRTC 清单默认选择通用 stdlib 队列；在本机这会使探测发送
出现约 15 ms 的时间粒度并截断探测簇。SFU 用一条共享原生计时队列唤醒网络
线程，媒体状态仍只在网络线程操作，退出时用原生任务安全标记取消回调。

## Linux 源码构建（本机浏览器通过，跨系统待验收）

Ubuntu 22.04 x64 已完成原生构建、CTest 4/4 和 Linux 本机浏览器九组媒体测试。
本机 coturn UDP/TCP relay-only 媒体也已实测。Windows 到 WSL 的浏览器与四轨
测试仍失败，原生流测试也有复测失败，不能据此声明 Linux 整体媒体支持通过。
详细证据见 [VALIDATION.md](VALIDATION.md)。WSL 下建议将源码及
构建目录放在 Linux 文件系统。

```bash
sudo apt-get install git pkg-config ninja-build clang-15 libc++-15-dev libc++abi-15-dev libssl-dev libboost-dev \
  nlohmann-json3-dev libvpx-dev nasm python3-venv meson curl
bash scripts/bootstrap-linux.sh
bash scripts/build-linux-deps.sh
bash scripts/build-linux.sh \
  -U'DAV1D_*' -DCMAKE_PREFIX_PATH="$PWD/out/linux-deps"
./out/linux-x64/bin/livekit-server --dev
```

WebRTC 默认位于相邻的 `../webrtc`，也可追加 `-DWEBRTC_ROOT=/path/to/webrtc`。
脚本依次配置、编译服务端与原生测试、运行 CTest。系统 CMake 低于 3.24 时，
在 `out/linux-tools` 中安装固定的 CMake 3.31.6；可用 `CMAKE_COMMAND` 指定工具。
Linux 的 libvpx 通过 pkg-config 映射到原生清单所需的目标名称。
源码引导脚本按完整提交号获取 LiveKit protocol 和 Protobuf 21.12；已有依赖
若版本不符或有本地修改会报错并保留原文件，不会自动重置。
离线环境可设置 `LK_SOURCE_MIRROR=/path/to/mirror`，其中包含 `protocol` 和
`protobuf` Git 仓库及固定提交；脚本验证相同提交，网络获取有明确超时。
Linux preset 选择 Clang 15 与 libc++；Clang 14 / libstdc++ 11 无法编译本版本
WebRTC 使用的嵌套 optional 类型。更换工具链时使用新目录，例如
`LK_BUILD_DIR="$PWD/out/linux-clang15" bash scripts/build-linux.sh ...`。
Ubuntu 22.04 的 dav1d 0.9.2 不具备所需 API；依赖脚本校验 SHA-512 后构建
固定的 dav1d 1.5.3，安装到本项目 `out/linux-deps`。可设置 `DAV1D_ARCHIVE`
复用离线归档；配置阶段会检查实际选中的头文件，避免编译中途才发现版本不兼容。
构建适配还隔离 AOM 汇编与 C 特性宏的名称冲突，并为 Boost 1.74 显式选择
C++20 的 `invoke_result`。实际 ELF 动态依赖包含 OpenSSL 3、libvpx 7、libc++、
libc++abi 和系统运行库；复制单个可执行文件不等于完成离线部署。

## 启动与连接

可选的 `rtc.turn_rest` 在每次入房时签发有有效期的 coturn REST 凭证，支持从
环境变量加载共享密钥，保留原有静态 `ice_servers` 配置。配置示例、生命周期
及尚未交付的刷新/轮换边界见 [coturn 会话凭证](docs/TURN.md)。

```powershell
.\out\windows-x64\Release\livekit-server.exe --dev
# 开发密钥：devkey / secret；默认只监听 127.0.0.1:7880

.\out\windows-x64\Release\livekit-server.exe --dev --token --room demo --identity alice
```

SDK 连接地址为 `ws://127.0.0.1:7880`，token 使用上面命令的输出。访问局域网可用 `--bind 0.0.0.0`；HTTP/WebSocket TCP 端口默认为 7880，ICE UDP 端口为 50000–50100。生产配置使用自己的 API key/secret：

```powershell
Copy-Item config.example.json config.local.json
# 编辑 config.local.json
.\out\windows-x64\Release\livekit-server.exe --config config.local.json
```

公网部署需在反向代理配置 HTTPS/WSS，并根据网络情况配置可达的 STUN/TURN。当前不含内置 TURN 或公网 IP 重写功能。不要将示例配置中的占位密钥直接用于部署。

```js
import { Room } from 'livekit-client';
const room = new Room({
  adaptiveStream: false,
  dynacast: false,
  publishDefaults: { simulcast: true, videoCodec: 'vp8', red: false },
});
await room.connect('ws://127.0.0.1:7880', token);
await room.localParticipant.setCameraEnabled(true);
await room.localParticipant.setMicrophoneEnabled(true);
```

## 兼容范围

固定版本、平台验证状态和媒体边界见 [兼容性矩阵](docs/COMPATIBILITY.md)。

| 功能 | 实现情况 |
|---|---|
| JWT HS256 | 验签、issuer、过期/生效时间、房间授权、发布/订阅/数据权限 |
| 信令 | `/rtc` 双 PeerConnection，官方 protobuf 二进制消息，JSON 请求，ICE trickle、ping/pong |
| RoomService | Create/List/DeleteRoom、List/Get/RemoveParticipant、MutePublishedTrack、UpdateParticipant、UpdateSubscriptions、SendData、UpdateRoomMetadata |
| API 编码 | Twirp JSON / protobuf，Bearer token，标准错误结构 |
| 音视频 | Opus / VP8，发布/订阅、后加入订阅、静音、退订、重新协商 |
| Simulcast | VP8 最多三层，LOW/MEDIUM/HIGH 或目标宽高，关键帧切层、独立订阅设置、停发层回退 |
| 带宽分配 | 每条传输共享原生 GoogCC 预算，音频预留、视频优先级、带宽暂停/恢复及升层等待 |
| 数据 | `_reliable` / `_lossy`，认证发送者身份、指定接收人、可靠通道有界发送队列 |
| 生命周期 | 重复 identity 替换、房间容量、空房超时、心跳超时、关闭清理 |
| 运维 | JSON 配置、`/healthz`、需 roomList 权限的 `/metrics`、退出信号处理 |

暂不支持订阅帧率限制、RED/ULPFEC、SVC 时空层选择、dynacast、E2EE 媒体转发、无缝会话恢复、单 PeerConnection `/rtc/v1`、Redis 集群、内置 TURN、Egress/Ingress、SIP、Agent 调度、Webhook、说话人检测及新式 data track/blob API。SFU 通过原生 CodecPreferences 明确协商 Opus / VP8 / RTX，不宣告尚未接入的编码或 FEC 能力。断线恢复使用完整重连。隐藏参与者和逐发布者订阅 ACL 尚未实现；请求不受支持的限制时明确拒绝，不会静默绕过限制。

订阅者可调用官方 Web SDK 的 `publication.setVideoQuality(VideoQuality.LOW)` 或 `publication.setVideoDimensions({width:600,height:300})`。宽高优先于 quality，以满足尺寸的最小可用层作为请求上限；实际层还受共享带宽限制，单层发布无法凭空产生低清层。LOW/MEDIUM/HIGH 对应发布端从低到高的协商层顺序，与官方 LiveKit SDK 一致。等待目标层关键帧时，仅在旧层仍符合预算的情况下继续转发；订阅 SSRC 不变。层连续 2 秒无完整帧时回退到可用层。设置跨退订/重订保留。

每 200 ms 分配一次原生目标码率的 85%，先为每条音频预留至少 64 kbps（测得需求更高时按需求），再分配视频基础层，最后升级高优先级视频。`TrackSettings.priority=1` 最高；显式优先级优先于未设置的轨道，未设置或同优先级按订阅顺序处理。各视频层通过原生 `RateStatistics` 测量唯一完整编码帧的码率，含估算传输开销；初始使用尺寸/层级对应的保守需求值。降层在下一分配周期执行，升层/带宽暂停后的恢复要求额外 15% 余量持续一秒。音频需求超过总预算时停止分配视频，但无法保证超出链路容量的全部音频都能送达。

带宽暂停会清除该视频的排队和重传缓存，恢复必须等待关键帧。用户静音停发媒体和探测；带宽暂停保留原生 padding，并通过 `StreamsConfig.max_padding_rate` 提供上限，具体发送速率、探测和反馈处理仍由 libwebrtc 决定。当已分配需求低于原生目标的 60% 时不额外填充，保留原生 ALR 探测所需的空闲带宽。`sfu_media_budget_bps`、`sfu_audio_reserved_bps`、`sfu_video_allocated_bps` 是活动传输最近一次分配的合计；`sfu_video_budget_paused` 为当前无视频分配的订阅数，`sfu_bandwidth_pauses` 为已分配视频变为暂停的累计次数。

恢复有一项大余量例外：若使用一半视频预算仍能分配到恢复目标层，持续 200 ms
即可恢复；余量等级或目标层变化都会重置等待。普通恢复及升层仍等待一秒，
所有分配始终受同一个原生目标预算约束。用户尺寸上限或源层失活导致当前层
消失时，直接回退到剩余可用且可负担的层，避免先回基础层再等待升层。

`sfu_layers_discovered` 统计已发现的视频源 SSRC，`sfu_layer_switches` 统计订阅切层，`sfu_unmapped_packets` 统计尚未能绑定到协商层的包（例如主流 RID 首包前到达的 RTX）。接收协商和 RID 扩展识别均使用原生 JSEP/RTP 对象；RID 替代层、超过三层和 SVC 发布模式不支持。

下行指标包括 `sfu_target_bps` / `sfu_pacing_bps`（活动传输的合计预算）、`sfu_downstream_feedback` / `sfu_feedback_packets`（已关联发送记录的 TWCC 反馈）、`sfu_padding_sent`、`sfu_pacer_queue_bytes` / `sfu_pacer_queue_ms`（合计排队字节 / 最大预计等待时间），以及 `sfu_pacer_admission_drops`。每条传输的媒体入队预算为 4 MiB；视频帧整批接受或拒绝，拒绝后请求关键帧恢复，视频与重传超时由原生队列处理。该保护不代替带宽选层，不能据此宣称弱网质量已达标。

VP8 转发复用 `RtpVideoFrameAssembler` 和 `RtpPacketizerVp8`：完整组帧后重新打包，保持编码帧内容不变；订阅端独立管理 picture ID、RTP 序号和时间戳映射，首次订阅及恢复等待关键帧。`sfu_video_frames_assembled` / `sfu_video_frames_forwarded` / `sfu_video_frames_gated` 分别记录组帧、各订阅入队和依赖检查等原因拒绝的帧数，入队不等于实际送达。组帧引入等待完整帧的延迟，尚未做端到端延迟验收。

`sfu_probe_clusters` / `sfu_probe_packets` / `sfu_probe_feedback_packets` 分别记录原生控制器创建的探测簇、成功发出的探测包和收到反馈的探测包。GoogCC 初始化在传输可用前完成，以确保原生重复初始探测配置生效。排查原生探测时可临时设置 `LK_WEBRTC_LOG=info`；默认仅输出 WebRTC 错误日志。

新 SDK 若默认启用 `/rtc/v1` 单连接协议，需要切换为兼容的 `/rtc` 双连接模式。兼容声明以集成测试中固定的 SDK 版本为准。

## 验证

```powershell
ctest --preset windows-release
cd tests
npm ci
npm test
```

集成测试使用本机 Chrome 的无头模式、Canvas 视频和 440 Hz 合成音频，自动启动独立测试端口 17880 的服务进程，结束后清理进程。它检查官方 SDK 的连接、实际视频解码帧、非静音音频、数据、权限、静音、订阅与房间删除；日志在 `tests/results/`。可通过 `LK_SERVER` 和 `LK_TEST_PORT` 指定二进制及端口。当前验证结果见 [VALIDATION.md](VALIDATION.md)。

测试已启动的专用 Linux 实例时，可设置 `LK_EXTERNAL_SERVER=1` 和
`LK_TEST_URL=http://服务器地址:端口`；脚本使用开发密钥并创建/删除 `integration`
房间，外部进程由调用方管理。`LK_TEST_RESULTS` 可指定独立结果目录，避免覆盖
其他平台的证据。外部模式仍执行同一套媒体解码、订阅及清理断言。

也可使用同级 `livekit-sdk-cpp` 的原生 CLI 验证（需要 Python 3、FFmpeg/ffprobe，以及已构建的 `lk.exe`）：

```powershell
cd D:\workspace\livekit-cpp
$lk = 'D:\workspace\livekit-sdk-cpp\out\windows-x64\Release\lk.exe'
$server = 'D:\workspace\livekit-cpp\out\windows-x64\Release\livekit-server.exe'
python tests/sdk_cli_integration.py --lk $lk --server $server
python tests/sdk_cli_stream_integration.py --lk $lk --server $server
```

这两个脚本改编自该 SDK 的 CLI 集成测试，使用 C++ 服务端 JSON 配置和随机 HTTP 端口，ICE UDP 分别使用 51000–51100、51200–51300。检查真实音视频录制、可靠数据、文件四轨发布、TCP H.264 输入、指定轨道订阅、静音恢复与踢人；结束后清理子进程。结果、日志和录制保存在 `tests/results/sdk-cli-integration/` 和 `tests/results/sdk-cli-stream/`。

测试使用更新后的原生 CLI `--stats-interval-ms 500`，记录原生编码/解码、带宽
限制和连接统计；流测试另存 `native-stats.json`，用于区分源端编码降帧和下行
转发问题。测试结果保留服务端及 SDK 二进制哈希，原有帧数和排队门槛不变。
这两个 CLI 脚本也支持 `--external-url http://服务器地址:端口`，外部专用实例
需配置测试键 `testkey` / `integration-test-secret-32-characters`；`--server`
指定该实例二进制的本地副本用于记录哈希，外部模式不启动或停止它。
可通过 `--output` 将不同平台的结果保存到独立目录。

Windows 上可用以下驱动启动并清理专用 WSL 服务端，依次收集三套测试的结果。
驱动保存 ELF 副本、哈希、动态库和退出状态；任一套失败时整体返回非零。

```powershell
python tests/linux_wsl_integration.py --linux-server /path/to/out/linux-x64/bin/livekit-server --lk D:/workspace/livekit-sdk-cpp/out/windows-x64/Release/lk.exe
python tests/wsl_clock_diagnostics.py
```

`--webrtc-log info` 可启用原生日志。时钟诊断只读采样，不修改系统时钟；主机
时间戳取自管道接收时刻，也包含调度延迟。当前环境观测到约 9% 的跨系统计时
差异，需要在时钟稳定的 Linux 环境重新验证，不能通过放宽媒体断言替代。

Linux 本机浏览器入口为 `scripts/test-linux-browser.sh`；TURN-only 与五秒 UDP
中断恢复入口为 `tests/turn_integration.py --outage`。依赖、命令、实际 relay 路由
检查和证据说明见 [Linux 媒体与 TURN 验证](docs/NETWORK_VALIDATION.md)。

## 代码结构

- `src/auth.cpp`：JWT 与 ID 生成。
- `src/rtc.cpp`：原生 WebRTC 工厂、双 PeerConnection、媒体轨和数据通道。
- `src/rooms.cpp`：单线程房间状态、权限、发布订阅与 RoomService。
- `src/http.cpp`：Boost.Beast HTTP/WebSocket 服务、Twirp 编解码及限流边界。
- `third_party/protocol`：固定到 Go 项目使用的 `f09f87aab29b`，构建时生成 C++ protobuf。
- `third_party/protobuf`：v21.12，避免与 WebRTC 自带 Abseil 的 ABI 冲突。

所有房间状态只在一个 Asio 事件循环中变更；WebRTC 自身运行于 network/worker/signaling 线程，回调投递回事件循环。模块间通过弱引用防止断开后的回调访问已释放会话。

原项目参考版本：`livekit/livekit` commit `a390d03d28c895742d9b13c0894880ee67733801`。沿用 Apache-2.0，第三方组件保留各自许可证，见 `NOTICE`。
