# Linux 媒体与 TURN 验证

以下是开发验收入口，完整交付范围仍以 [IMPLEMENTATION.md](IMPLEMENTATION.md)
为准。服务端继续使用 libwebrtc 的 ICE、DTLS/SRTP、RTP/RTCP、重传、拥塞控制和
pacing；中继使用 coturn。测试代理仅转发或丢弃完整 UDP 数据报，不解析 RTC 协议。

## Linux 本机浏览器

需要 Linux Node.js 18+、Chrome 及 `tests/package-lock.json` 对应的 npm 依赖。
在 `tests` 目录运行 `npm ci`，然后从项目根目录执行：

```bash
LK_SERVER=/path/to/build/bin/livekit-server \
LK_CHROME=/path/to/chrome \
bash scripts/test-linux-browser.sh
```

默认服务端为 `out/linux-x64/bin/livekit-server`，Chrome 从 PATH 中查找。
可用 `LK_TEST_RESULTS` 指定证据目录、`LK_TEST_PORT` 指定独立 HTTP 端口。
脚本记录实际二进制哈希、Node/Chrome 版本；九组测试包含实际音视频解码、数据、
三层 simulcast、独立订阅、静音/重订、权限及清理。Linux 服务端退出码必须为零。

## TURN-only 与中断恢复

需要 Python 3、可执行的 coturn，以及上面的浏览器环境。驱动创建独立配置、
随机测试凭证、独立 HTTP/TURN 监听端口，并依次启动 UDP、TCP 两组服务进程：

```bash
python3 tests/turn_integration.py \
  --server /path/to/build/bin/livekit-server \
  --turnserver /path/to/turnserver \
  --node /path/to/node \
  --chrome /path/to/chrome \
  --outage
```

每套测试用新的服务端与 coturn 进程。RTC UDP 范围为 54000–54499，TURN relay
范围为 54500–54999，运行时应保证可用。测试仅监听本机地址；为本机媒体设置了
coturn 的 loopback peer 许可，**这些配置不适合作为公网部署模板**。凭证仅用于
此次测试，配置保存在结果目录，进程停止后不再生效。

- UDP/TCP 两组均使用浏览器 `iceTransportPolicy=relay`，执行完整媒体断言。
  额外读取实际选中候选对：至少五条活动发布/订阅连接必须使用 relay，
  `relayProtocol` 必须匹配该组的 UDP 或 TCP，并且双向字节数大于零。
- TCP 组关闭 coturn 的 UDP 客户端监听，只提供 TCP 接入。这证明客户端到
  TURN 的 TCP 路径；TURN 到 SFU 仍使用 UDP，不代表 TURN/TLS 或公网防火墙验收。
- `--outage` 在 UDP 组插入一个本机数据报代理。媒体正常后，连续五秒丢弃
  两个方向的所有数据报；不缓存、补发或解析 RTP。断言两个方向都实际丢包、
  视频在中断中停止推进，并在放行后的十秒内收到至少六个新解码视频帧和新的
  非静音音频。整个检查使用 libwebrtc 原有恢复逻辑。
  注入持续时间和恢复十秒门槛使用 Linux `CLOCK_MONOTONIC_RAW`，并同时保存
  Node 侧观察时间，避免把已发现的 guest 时钟校准差异带入验收计时。
- 浏览器退出失败、媒体断言失败、代理错误或服务端不能正常退出，均令整套
  驱动返回非零。超时终止驱动持有的浏览器进程组，不停止系统中的其他 coturn。

证据位于 `tests/results/turn/<时间>/`：`result.json`、实际二进制哈希、逐套日志，
以及 `browser/turn-routes.json` 和 UDP 组的 `browser/recovery.json`。恢复时间为
当前样本的观测值；仍需重复测试统计 P95、跨网络、Wi-Fi/蜂窝切换、TURN/TLS、
凭证轮换、10% 随机丢包、容量与长期稳定性验收。

## 重复订阅回归

需要专门检查协商暂停后的恢复时，可在浏览器或 TURN 驱动前设置
`LK_RESUBSCRIBE_ROUNDS=5`（支持 1–20，默认 1）。每轮都要求新订阅恢复 360p，
且同一接收端已有音频采样和其他视频的解码帧继续增加，断言超时保持不变。
结果 JSON 保存请求轮数，`integration.log` 保存每轮完成记录；失败后的轮数
不能当作已完成轮数。此测试不能替代换网、容量或长期稳定性验收。

## 跨系统时钟诊断

Windows 到 WSL 的媒体测试继续使用 `tests/linux_wsl_integration.py`。
`python tests/wsl_clock_diagnostics.py` 只读记录 Linux MONOTONIC/RAW/REALTIME
和 Windows 的接收时间。当前环境曾出现约 9% 的 MONOTONIC 速率差，而 RAW 接近
主机时间；这些跨系统测试尚未通过。Linux 本机与 TURN 测试通过不能替代它们，
也不能把所有失败都归为时钟问题。具体版本、成功和失败记录见
[VALIDATION.md](../VALIDATION.md)。
