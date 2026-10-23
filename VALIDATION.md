# 验证记录

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
