# 视频稳定性排查记录

2026-10-27，Windows x64、无头 Chrome 156.0.8078.48。完整替代与视频稳定性
仍未验收；本记录不代表实际设备人工验收。主服务代码未因本轮复跑而修改。

## 固定对象与未关闭问题

当前 C++ 测试产物：`out/parity-reference/livekit-server-cpp-g711-mute-v1.exe`，
SHA-256：`ceb77e887bedfe9727bfce18dc2f7f41a0dfbb4d64e69cff485d5d473845a307`。
Go 固定基线及生产源码快照见 [静音验证](GO_MUTE_VALIDATION.md)。

此前出现单 PC 混合 H.264/VP8 停流，以及三层 VP8 的 720p 切换超时。
历史失败、旧产物对照和 BUNDLE 警告排查见
[音频编码验证](GO_AUDIO_CODEC_VALIDATION.md)。这些问题的根因尚未证明，
不能用重复执行后的通过结果覆盖失败。

720p 失败时，发布者的高层编码停止，低层继续，浏览器报告 bandwidth 限制；
SFU 下行总预算充足且未暂停视频。这只说明应进一步检查发布端、上行反馈和
每条传输的统计，不能证明是某个具体拥塞算法或 G.711 改动导致。

## 补充证据

下列路径均相对 `tests/results/`，测试保持原超时与解码断言：

| 场景 | 结果 | 报告目录 |
| --- | --- | --- |
| C++ 开启原生日志的混合视频 | 14 项通过；仅诊断 | `go-video-stall-diagnostic/2026-10-27T09-13-44-762Z-b60d5e` |
| C++ 不开启原生日志的混合视频 | 14 项通过；仅诊断 | `go-video-stall-metrics/2026-10-27T09-15-17-414Z-fabfdd` |
| C++ 连续三轮普通混合视频 | 每轮 14 项通过；仅诊断 | `go-video-stall-round-1/2026-10-27T09-18-30-891Z-dd7df5`、`go-video-stall-round-2/2026-10-27T09-18-50-694Z-2f69ce`、`go-video-stall-round-3/2026-10-27T09-19-08-635Z-65b129` |
| C++ 混合/迟加入后各持续观察 10 秒 | 14 项通过；仅诊断 | `go-video-stall-sustained/2026-10-27T09-22-37-003Z-82c58a` |
| 原媒体套件，开启原生日志 | 9/9 | `go-video-simulcast-diagnostic-20261027` |
| 原媒体套件，十轮 simulcast 重订阅 | 9/9；十轮均恢复，原有音视频继续推进 | `go-video-resubscribe-stress-20261027` |
| Go/C++ 混合/迟加入后各持续观察 10 秒 | 14/14 差分通过 | `go-video-sustained-differential/2026-10-27T09-29-18-278Z-89c5ee` |
| Go/C++ 十轮混合编码重订阅首轮 | 未完成；浏览器关闭，随后 C++ 启动退出 `3221225794` | `go-video-renegotiation-differential/2026-10-27T09-33-58-379Z-a2f1a0` |
| Go/C++ 十轮混合编码重订阅复查 | 14/14；单/双 PC 各十轮，含 10 秒持续解码 | `go-video-renegotiation-recheck/2026-10-27T09-43-19-925Z-08428b` |
| 带传输诊断的原媒体套件 | 9/9；十轮 simulcast 重订阅，三端检查点可解析 | `go-video-transport-integration-20261027` |

被中断的一轮不能计入差分通过，也不能用来推断媒体停流根因。`--only cpp` 的
报告显式标记 `differential:false`，它们不是 Go/C++ 对照验收。

## 可复现诊断入口

`tests/go_codec_parity.mjs` 新增的观察与压力选项：

- `--sustain-ms 10000`：混合发布和迟加入后每 500 ms 采样，每 2 秒检查每条
  轨道的解码帧数继续增加；失败采样也保留，不只记录首帧。
- `--resubscribe-rounds 10`：单/双 PC 各执行十轮 H.264 撤订/重订，每轮检查
  H.264 解码恢复、同连接 VP8 和反向 H.264 继续推进。
- `tests/rtc_diagnostics.mjs`：记录每个实际 PC 的 ICE/DTLS 状态、候选对、
  可用带宽、RTT、双向 RTP/RTCP、编码来源和 codec 关联。单 PC 不重复采集。
- 超时保存逐次解码观察、各端统计和 C++ metrics；成功保存传输检查点。
  混合编码报告自动保存测试脚本及 SHA-256。
- `tests/integration.mjs`：720p 超时另外保存发布端和目标订阅端的传输统计；
  成功保存三端检查点。新报告记录测试源码和本地服务二进制哈希。

```powershell
node tests/go_codec_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-g711-mute-v1.exe `
  --sustain-ms 10000 --resubscribe-rounds 10 `
  --output tests/results/go-video-renegotiation
```

下一步依据失败时的逐轨计数、候选对和 SFU 统计定位停流，再构造能稳定失败的
回归检查。只有证实原因、实施修复并完成相应对照后，才能关闭原有问题。
