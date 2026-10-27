# 配置入口兼容验证

2026-10-27，Windows x64 自动化。Go 1.13.10 固定提交 `a390d03d…`。
本增量补齐配置读取入口，R09 和主服务完整替代仍未完成。

## 实现范围

- `--config` 读取 YAML 和 JSON；采用固定 yaml-cpp 0.8.0
  `f7320141120f720aecc4c32be25586e7da9eb978`，与服务端使用同一 C++ 运行时构建。
- `--config-body` / `LIVEKIT_CONFIG` 提供配置正文。显式正文参数覆盖对应环境
  变量；非空正文优先于文件，文件不存在时仍可使用正文启动。
- `--keys` / `LIVEKIT_KEYS` 覆盖整个密钥表；显式参数优先于环境变量。
  依据 Go `unmarshalKeys`，命令行密钥只保留 YAML 字符串值，数值/布尔值不
  转成密钥；正文的 `keys` 是有类型的字符串映射，保留标量文字。
- `--dev` 可与配置合用，最终密钥表为空时才补入 `devkey/secret`。
- 支持单个 `bind_addresses`，与已有 `bind` 不能同时配置。多地址明确拒绝。
- 顶层、room、rtc 和 turn_rest 的未知字段继续拒绝；重复映射键、多个文档、
  非有限数和过深/过大的别名展开会拒绝，语法错误不回显可能包含密钥的原文。

参考位置：Go `cmd/server/main.go` 的 `getConfigString/getConfig`，
`pkg/config/config.go` 的 `updateFromCLI/unmarshalKeys`。

## 行为证据

`tests/go_config_parity.py` 对两端分别启动独立进程，通过真实 RoomService
认证请求验证配置生效和被覆盖密钥失效，而不是只检查进程能否启动。

旧 C++ 固定产物在最初 14 项中只通过 4 项拒绝输入检查，Go **14/14**：
`tests/results/go-config-before/20261027T095235Z-a66465/report.json`。
新增两个 YAML 密钥标量边界后，当前 Go/C++ **16/16**：
`tests/results/go-config-yaml-verified/20261027T100326Z-b37dd0/report.json`。
覆盖文件、JSON、正文、环境变量、覆盖顺序、开发模式、空密钥、未知字段、
无效布尔、重复字段，以及 `on` 字符串和名为 `keys` 的非字符串密钥。

原生 CTest **9/9**，`out/config-yaml-ctest.log`；新增配置测试还覆盖标量别名、
循环别名限制、JSON 字段保留、带引号布尔拒绝和错误日志不泄漏输入。

房间 API 首轮 **35/36**：Go 的 unknown method 请求出现 Windows 10054
连接重置；C++ 返回预期的 404/bad_route。原报告保留：
`tests/results/go-room-config-yaml/20261027T100414Z-07d89c/report.json`。
保持原请求和门槛复查 **36/36**：
`tests/results/go-room-config-yaml-recheck/20261027T100447Z-69a179/report.json`。

同产物静音策略 JSON/protobuf 共 **63/63**，确认默认、开启、关闭三种配置
仍具有相应的管理员静音/恢复语义，并检查 SDK 状态和实际恢复音频：

- `tests/results/go-mute-config-yaml-default/2026-10-27T10-05-00-733Z-96257d/report.json`
- `tests/results/go-mute-config-yaml-true/2026-10-27T10-07-06-126Z-950d6d/report.json`
- `tests/results/go-mute-config-yaml-false/2026-10-27T10-08-02-661Z-e86cbf/report.json`

同产物无头媒体集成 **9/9**，包含十轮三层 VP8 重订阅、既有音视频推进、
LOW/HIGH/尺寸切换及高层关闭/恢复：
`tests/results/go-config-yaml-media-20261027/result.json`。
该报告同时保存本地二进制/测试哈希和三端传输检查点。一次回归通过不关闭
之前的间歇视频问题。

固定产物：`out/parity-reference/livekit-server-cpp-config-yaml-v1.exe`。
SHA-256：`d0aabec0e6fc6549da18b79fdb34c66e56bafa2b76592f2c64d31a7fa3a04f82`。
源码、测试、构建脚本、yaml-cpp 源码及依赖哈希：
`out/parity-reference/config-yaml-v1-provenance/`。
其中 `regressions.json` 关联本产物通过/失败报告，并核对源码快照之后的漂移；
本轮结束时漂移为空。
构建记录：`out/config-yaml-build-final.log`；首次配置因 CMake 4 不再接受
依赖声明的旧策略最低版本而失败，保留 `out/config-yaml-build.log`，兼容设置
仅作用于 yaml-cpp 子目录，没有改写固定依赖源码。

```powershell
python tests/go_config_parity.py `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-config-yaml-v1.exe
```

## 尚未完成

这是当前已实现字段的 YAML/JSON 入口兼容，不能直接迁移任意 Go 配置。
尚缺多地址监听、完整自动生成 CLI/env 参数、key-file、RTC 网络/TURN、Redis、
日志/限制等配置语义；默认监听地址和 RTC 端口仍采用 C++ 默认值。
YAML 合并键、多文档及完整标量格式兼容未交付。`--dev` 的调试服务、日志和
网络默认值也未完整对齐。Linux 引导/构建文件已更新，本轮没有进行 Linux
构建或实际设备人工验收。

媒体历史间歇失败继续由 [视频稳定性记录](GO_VIDEO_STABILITY.md) 跟踪。
配置通过不代表媒体稳定性、外部 worker 或完整替代通过。
