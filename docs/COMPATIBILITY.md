# 兼容性与依赖基线

截至 2026-10-26。这里区分已经实测的组合和仍在实施的目标；完整验收范围见
[IMPLEMENTATION.md](IMPLEMENTATION.md)，运行证据见 [VALIDATION.md](../VALIDATION.md)。

## 协议与依赖

| 组件 | 当前基线 | 约束 |
| --- | --- | --- |
| 原生 WebRTC | M137，提交 `aad183504be13d5a9c529581a27c53767570b5d6` + 本地 pacer TTL 补丁 | 见 [补丁说明](../patches/README.md)；需重建原生库并运行 native-send；升级需重新验证 |
| LiveKit protocol | `f09f87aab29b865850a967924726bf76b7db4755` | Linux 引导脚本按完整提交获取并核对已有仓库 |
| Protobuf | 21.12，`f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c` | 生成器与静态库同源构建 |
| 官方 Web SDK | `livekit-client` 2.15.6 | 浏览器媒体集成测试使用该固定版本 |
| 官方 Server SDK | `livekit-server-sdk` 2.13.3 | 测试房间 API、权限和令牌 |
| JS protocol 包 | `@livekit/protocol` 1.39.3 | 二进制 API 互通测试；不表示整个 schema 均已实现 |
| 原生 C++ SDK | 相邻 `livekit-sdk-cpp` 工作树 | 集成报告保存实际 CLI SHA-256；SDK 正在共同演进 |
| dav1d | 1.5.3 | Linux 引导脚本校验发行归档 SHA-512；Ubuntu 22.04 的 0.9.2 API 不适用 |
| coturn 本机验收 | Ubuntu 4.5.2-3.1~ubuntu22.04.1 | 独立测试进程；UDP/TCP relay-only，非公网生产部署 |
| Linux 浏览器环境 | Chrome 156.0.8078.47 / Node 22.16.0 | 实际可执行文件版本和哈希保存在测试目录 |

Linux 当前构建环境为 WSL Ubuntu 22.04 x64，Clang/libc++/libc++abi
15.0.7，CMake 3.31.6，Boost 1.74，libvpx 1.11.0，nlohmann-json 3.10.5，
OpenSSL 开发包 `3.0.2-0ubuntu1.30`。这是当前实际环境记录，尚不是完整的
可重现系统镜像或所有版本组合的支持承诺。Clang 14 搭配 libstdc++ 11 的
嵌套 `std::optional` 编译问题已由最小样例确认，配置阶段会检查此能力。

## 平台与互通

| 组合 | 状态 | 已有证据或尚缺的验收 |
| --- | --- | --- |
| Windows x64 服务端 + Windows C++ SDK | 已实测，最新文件回归未通过 | 流输入、定向订阅、数据、静音和踢人通过；最新文件四轨视频 108/78 帧，第二路未达 80 帧门槛 |
| Windows x64 服务端 + Chrome / 官方 Web SDK | 已实测 | 九组集成，含三个真实 VP8 simulcast 层和独立订阅 |
| Linux x64 服务端 + Linux Chrome | 本机通过，稳定性待验收 | ELF、CTest 4/4；原生暂停/TTL 缺陷已复现修复，REST UDP/TCP 各五轮重订通过；高层恢复曾耗时约 14.7 s，非完整时延验收 |
| Linux x64 服务端 + Windows 客户端 | 未通过整体验收 | 最新补丁后仍有 LOW simulcast 超时、四轨视频 108/40 帧；流测试在入房 HTTP 401 处失败 |
| Android arm64 SDK / JNI / Kotlin | 未交付 | 当前外部原生构建清单尚不支持 arm64；需真实构建及设备互通 |
| iOS arm64 SDK / Objective-C++ / Swift | 未交付 | 同上，另需 Apple 工具链、签名及设备验证 |
| macOS、其他桌面与语言封装 | 后续阶段 | 无编译或运行支持声明 |

Windows 显式选择 libwebrtc 自带的 Windows 计时队列；Linux 选择原生默认
队列。RTP/RTCP、NACK/RTX、PLI/FIR、拥塞估计、pacing 和探测不跨平台重写。

## 当前媒体和信令边界

| 能力 | 当前范围 |
| --- | --- |
| RTC 编码 | Opus / VP8 / RTX；转发路径保持编码帧内容，视频 Encode/Decode 调用通过审计为零 |
| 分层 | 最多三层 VP8 simulcast；按质量、目标尺寸和共享预算选择；SVC 尚未交付 |
| H.264 输入 | CLI 可通过 FFmpeg 解码文件或 TCP H.264，再发布 VP8；不表示 SFU 已支持 H.264 RTP 转发 |
| FEC、RED、其他编解码器 | 尚未作为此 SFU 的协商能力开放；后续适配仍须使用原生模块 |
| 信令 | 已验证 `/rtc` 双 PeerConnection 模式；`/rtc/v1` 单连接模式未声明支持 |
| 服务端 API | 已测试的房间、参与者、轨道、权限与数据操作；不等同于完整 LiveKit 商业 API |
| 网络 | 本地真实媒体、coturn UDP/TCP relay-only、单次五秒 UDP 中断及原生模拟链路；公网、TURN/TLS、换网、P95 和完整弱网仍待完成 |
| TURN 凭证 | 静态 ICE 与按会话签发的 coturn REST 临时凭证；真实错误密码/过期拒绝已测；在线刷新、即时吊销、无损轮换及跨到期长通话未完成 |
| 规模与稳定性 | 2–16 人房间、100 并发真实媒体会话与 24 小时验收仍待完成 |

升级任何基线后，至少重新运行原生 CTest、浏览器互通及两套 CLI 媒体集成，
保留二进制哈希、客户端版本、配置和结果。Linux 外部实例使用相同媒体断言，
不能以 API 健康检查代替音视频验证。
