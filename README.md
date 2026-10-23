# livekit-cpp

使用 C++20 实现的 LiveKit **单机服务端兼容项目**。原生 WebRTC 直接来自 `D:\workspace\webrtc`，不启动 Go 服务，也不经由 Rust FFI。

当前媒体链路是 **解码后重新编码的 WebRTC relay**。它支持基础发布/订阅，但不是原版 LiveKit 的 RTP 包级 SFU；CPU 占用、时延和吞吐量不能按原版估算。开发版本，不应在未经过部署环境联调和负载测试前替换生产服务。

## 构建（Windows x64）

依赖 Visual Studio 2022 C++ 工具、CMake 3.24+、Git、vcpkg，以及相邻的 WebRTC 源码及其 CMake 依赖。使用静态 CRT `/MT`，Debug 对应 `/MTd`。

```powershell
cd D:\workspace\livekit-cpp
.\scripts\bootstrap.ps1 -VcpkgRoot D:\vcpkg
$env:VCPKG_ROOT = 'D:\vcpkg'
cmake --preset windows-x64
cmake --build --preset windows-release
ctest --preset windows-release
```

默认在本项目的 `out` 目录编译 WebRTC，不修改 `D:\workspace\webrtc` 的源文件。服务端构建排除了本地摄像头采集模块。修改依赖位置可传 `-DWEBRTC_ROOT=其他路径`。

如果指定目录中已经成功构建了 WebRTC 的 Release 静态库，可以复用：

```powershell
cmake --preset windows-x64 -DWEBRTC_PREBUILT_DIR=D:/workspace/webrtc/out/windows-x64
```

预编译导入只支持本地 WebRTC CMake 工程的 Windows Release 布局；Debug 或其他布局使用默认源码构建。

## 启动与连接

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
  publishDefaults: { simulcast: false, videoCodec: 'vp8', red: false },
});
await room.connect('ws://127.0.0.1:7880', token);
await room.localParticipant.setCameraEnabled(true);
await room.localParticipant.setMicrophoneEnabled(true);
```

## 兼容范围

| 功能 | 实现情况 |
|---|---|
| JWT HS256 | 验签、issuer、过期/生效时间、房间授权、发布/订阅/数据权限 |
| 信令 | `/rtc` 双 PeerConnection，官方 protobuf 二进制消息，JSON 请求，ICE trickle、ping/pong |
| RoomService | Create/List/DeleteRoom、List/Get/RemoveParticipant、MutePublishedTrack、UpdateParticipant、UpdateSubscriptions、SendData、UpdateRoomMetadata |
| API 编码 | Twirp JSON / protobuf，Bearer token，标准错误结构 |
| 音视频 | Opus / VP8，发布/订阅、后加入订阅、静音、退订、重新协商 |
| 数据 | `_reliable` / `_lossy`，认证发送者身份、指定接收人、可靠通道有界发送队列 |
| 生命周期 | 重复 identity 替换、房间容量、空房超时、心跳超时、关闭清理 |
| 运维 | JSON 配置、`/healthz`、需 roomList 权限的 `/metrics`、退出信号处理 |

暂不支持原版包级 SFU、simulcast/SVC 层选择、dynacast、E2EE 媒体转发、无缝会话恢复、单 PeerConnection `/rtc/v1`、Redis 集群、内置 TURN、Egress/Ingress、SIP、Agent 调度、Webhook、说话人检测及新式 data track/blob API。断线恢复使用完整重连。隐藏参与者和逐发布者订阅 ACL 尚未实现；请求不受支持的限制时明确拒绝，不会静默绕过限制。

新 SDK 若默认启用 `/rtc/v1` 单连接协议，需要切换为兼容的 `/rtc` 双连接模式。兼容声明以集成测试中固定的 SDK 版本为准。

## 验证

```powershell
ctest --preset windows-release
cd tests
npm ci
npm test
```

集成测试使用本机 Chrome 的无头模式、Canvas 视频和 440 Hz 合成音频，自动启动独立测试端口 17880 的服务进程，结束后清理进程。它检查官方 SDK 的连接、实际视频解码帧、非静音音频、数据、权限、静音、订阅与房间删除；日志在 `tests/results/`。可通过 `LK_SERVER` 和 `LK_TEST_PORT` 指定二进制及端口。当前验证结果见 [VALIDATION.md](VALIDATION.md)。

也可使用同级 `livekit-sdk-cpp` 的原生 CLI 验证（需要 Python 3、FFmpeg/ffprobe，以及已构建的 `lk.exe`）：

```powershell
cd D:\workspace\livekit-cpp
$lk = 'D:\workspace\livekit-sdk-cpp\out\windows-x64\Release\lk.exe'
$server = 'D:\workspace\livekit-cpp\out\windows-x64\Release\livekit-server.exe'
python tests/sdk_cli_integration.py --lk $lk --server $server
python tests/sdk_cli_stream_integration.py --lk $lk --server $server
```

这两个脚本改编自该 SDK 的 CLI 集成测试，使用 C++ 服务端 JSON 配置和随机 HTTP 端口，ICE UDP 分别使用 51000–51100、51200–51300。检查真实音视频录制、可靠数据、文件四轨发布、TCP H.264 输入、指定轨道订阅、静音恢复与踢人；结束后清理子进程。结果、日志和录制保存在 `tests/results/sdk-cli-integration/` 和 `tests/results/sdk-cli-stream/`。

## 代码结构

- `src/auth.cpp`：JWT 与 ID 生成。
- `src/rtc.cpp`：原生 WebRTC 工厂、双 PeerConnection、媒体轨和数据通道。
- `src/rooms.cpp`：单线程房间状态、权限、发布订阅与 RoomService。
- `src/http.cpp`：Boost.Beast HTTP/WebSocket 服务、Twirp 编解码及限流边界。
- `third_party/protocol`：固定到 Go 项目使用的 `f09f87aab29b`，构建时生成 C++ protobuf。
- `third_party/protobuf`：v21.12，避免与 WebRTC 自带 Abseil 的 ABI 冲突。

所有房间状态只在一个 Asio 事件循环中变更；WebRTC 自身运行于 network/worker/signaling 线程，回调投递回事件循环。模块间通过弱引用防止断开后的回调访问已释放会话。

原项目参考版本：`livekit/livekit` commit `a390d03d28c895742d9b13c0894880ee67733801`。沿用 Apache-2.0，第三方组件保留各自许可证，见 `NOTICE`。
