# 轨道信息更新兼容验证

2026-10-28，Asia/Shanghai，Windows x64 自动化。接续工作区已有轨道更新实现，
补完单／双 PC、最终产物回归及证据归档；推进 R03/R05，未完成整个工作包。

## 实现与范围

- `UpdateLocalAudioTrack` 更新 audio_features，排序/去重，保留未知枚举值，
  同步 stereo 和 disable_dtx；空列表清除这些字段。
- `UpdateLocalVideoTrack` 更新 width/height，接受零值；真实客户端调整画布
  到 640×360 后，订阅端解码统计确认新尺寸到达。
- 仅允许更新自身轨道；缺失或他人 SID 返回 `RequestResponse/NOT_FOUND`，
  包含原请求，不发送成功确认。成功更新广播 ParticipantInfo。
- 已发布轨道收到错误类型的更新时，不改变该类字段，但仍有参与者更新事件。
  等待媒体到达的 pending 轨道保留更新，暂不发送成功响应或广播；此路径有原生单测。
- 继续忽略基线已废弃的 `update_layers`。不把以上字段更新解释为音频编码器
  重配置、SVC/dynacast 或完整动态编码参数支持。

参考固定 Go 提交 `a390d03d28c895742d9b13c0894880ee67733801` 中的
`UpdateLocalAudioTrack`、`UpdateLocalVideoTrack` 及信令分派行为。

## 差分和回归

`tests/go_track_update_parity.mjs` 使用官方 Web SDK 2.23.0、无窗口 Chrome、
合成 VP8 视频和 440 Hz 音频。每项检查错误/事件/状态后，继续检查视频帧数、
音频采样及能量推进，确认原 PC 和已连接状态保持；单 PC 断言实际 PC 模式。

| 检查 | 结果 | 报告目录，含 report.json |
| --- | --- | --- |
| 修复前 signal-metadata-v1，最终同套测试 | Go 12/12，C++ 1/12 | `tests/results/go-track-update-baseline-final/2026-10-28T01-43-50-897Z-a3e518/` |
| 原 track-update-v1，双 PC | 12/12 | `tests/results/go-track-update-dual/2026-10-28T01-12-06-804Z-16d8d8/` |
| 接续验证 track-update-v1，单 PC | 12/12 | `tests/results/go-track-update-single/2026-10-28T01-30-04-563Z-17b29d/` |
| 最终 limits-v2，双 PC | **12/12** | `tests/results/go-track-update-limits-dual/2026-10-28T01-47-48-216Z-b4dbf7/` |
| 最终 limits-v2，单 PC | **12/12** | `tests/results/go-track-update-limits-single/2026-10-28T01-49-43-476Z-8c5521/` |

同一 limits-v2 产物原生 CTest 10/10、RoomService 234/234、配置入口 16/16、
内容上限 118/118、非默认上限媒体单／双 PC 各 15/15、常规无窗口媒体集成 9/9。
完整路径和源码/依赖快照见 [内容上限验证](GO_LIMITS_VALIDATION.md)。

早期 `go-track-update-before-corrected/2026-10-28T01-02-36-350Z-6dd453/` 和
`go-track-update-diagnostic/2026-10-28T01-05-30-585Z-c0adbb/` 中，Go 10/12，
错误类型更新的测试把服务端与订阅者视图直接比较，造成两项参考断言超时。
当前改为分别比较 API 和对端各自的更新前视图；上述失败保留，不作为服务端差异。
最终同输入的旧产物对照仍为 C++ 1/12，因此并未放宽断言来迁就 C++ 实现。

```powershell
node tests/go_track_update_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-limits-v2.exe
# 单 PC 增加 --single-pc true
```

最终二进制为 `out/parity-reference/livekit-server-cpp-limits-v2.exe`，SHA-256：
`aaed01162b256491e9b4a6e2831122a67805061e37f35191f24f60a0b8e08352`。
本轮未做实际设备人工验收，也不据重复通过关闭历史间歇停流、四轨帧数或跨系统失败。
