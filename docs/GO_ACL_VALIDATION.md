# Go 主服务替代：媒体订阅 ACL

日期：2026-10-27（Asia/Shanghai）。本轮完成 R02 的媒体订阅 ACL 增量，完整
主服务替代仍未完成；范围见 [替代计划](GO_REPLACEMENT.md)。
隐藏参与者、kind 与附属房间生命周期的后续进展见
[参与者验证记录](GO_PARTICIPANT_VALIDATION.md)，下文保留本增量当时的边界。

## 代码与行为

此前 C++ 收到 `all_participants=false` 的 SubscriptionPermission 会直接断开
发布者。现在按 Go UpTrackManager 的规则保存发布者 ACL，并在自动订阅、管理
API、重新授权、后加入和新轨道到达时统一检查：

- 默认允许所有参与者；`all_participants=true` 优先于其他条目，包括无效条目。
- 按 subscriber identity 授权全部或指定 track SID；未列出的订阅者及轨道拒绝。
- SID 条目更新时解析为 identity；未知 SID 跳过。解析成功的授权在订阅者退出、
  以相同 identity 重入后仍有效，直到发布者替换 ACL。
- 显式 identity 优先于 SID，即使两者指向不同参与者；重复 identity 使用最后
  一项，整份更新替换旧规则。缺少 identity 和 SID 的条目使整次更新无效，保持
  之前的 ACL，且不断开连接。这些均来自固定 Go 实现并经差分检查。
- 撤销 ACL 移除原生 Peer 的发送轨，保留订阅意图。再次授权自动恢复；显式取消
  订阅的轨不会被 ACL 更新重新开启。SDK 收到 SubscriptionPermissionUpdate。
- 每个发布者独立保存规则。Track ACL 不用于控制 reliable/lossy data channel；
  这类数据仍由 token/participant 数据权限与定向路由控制。
- 解析 `video.recorder` 及不区分大小写的 `kind=egress`，对齐录制参与者绕过
  Track ACL 的行为。管理 API 可动态更新 recorder 标记。

特殊时序：Go `SetPermission` 仅重新尝试待订阅轨道；移除 recorder 标记本身
不会立即重新检查已经建立的订阅。下一次发布者 ACL 更新会重新检查并撤销。
C++ 对齐这一行为；`canSubscribe=false` 则立即移除所有订阅，包括录制参与者，
再开启时重新检查授权。不能把“只移除 recorder 标记”当成立即停止媒体的操作。

代码位置：`include/lk/subscription_permissions.hpp` 保存规则；`Rooms::subscribe`
统一执行授权并通知 SDK；`update_subscription_permissions` 更新规则及已有订阅。
发布者退出/轨道取消发布时清理接收者权限状态。ACL 状态与房间操作均在同一
io_context 线程，媒体转发和编解码仍复用现有 libwebrtc 路径。

## 基线与构建

- Go：1.13.10，`a390d03d28c895742d9b13c0894880ee67733801`；protocol
  `f09f87aab29b865850a967924726bf76b7db4755`，未修改参考源码。
- C++：平台子模块 `a21f3c0` 加工作区增量，专用 `out/windows-parity` Release，
  未覆盖其他任务使用的正式服务产物。
- Chrome 156.0.8078.47 无头模式、Web SDK 2.15.6、Server SDK 2.13.3、Node
  24.13.0。Go/C++ 都使用 loopback、客户端 `iceServers=[]`；不依赖外部 STUN。

| 产物 | SHA-256 |
| --- | --- |
| Go 参考服务 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| ACL 修复前 C++ | `56707dca60b95b586aa9f870e1516672dce9ffb329543cf43d63d830501b91e5` |
| 本轮最终 C++ | `64760f8d86195104981b47c77560603c75530a9b73bb82e14412b5f43490d215` |

构建/原生测试日志：`out/acl-final-build.log`、`out/acl-final-ctest.log`。
最终 ACL 报告同目录的 `provenance.json` 保存基线提交与源文件哈希。

## 自动化证据

新增 `tests/go_acl_parity.mjs`，以真实合成音视频和官方 SDK 对照同一组场景。
允许状态必须观察到视频解码帧、音频能量相对于采样点继续增长；不能仅看轨道
对象或历史计数。拒绝状态检查 SDK 权限及 TrackUnsubscribed，在无其他允许
媒体的场景还比较原生接收 PeerConnection 的 RTP 字节计数，确认停止增长。
所有测试只操作自建进程、临时房间及合成媒体，结束后清理进程和测试密钥配置。

| 检查 | 结果 | 证据（相对仓库） |
| --- | --- | --- |
| 修复前 17 场景 | Go 17 通过；C++ 首项通过，撤权场景失败后停止 | `tests/results/go-acl-parity/2026-10-27T02-53-31-760Z-4ff373/report.json` |
| 扩展 23 场景，最终产物 | **23/23** | `tests/results/go-acl-parity/2026-10-27T03-00-02-934Z-288b4e/report.json` |
| 原生 CTest，最终产物 | **6/6** | `out/acl-final-ctest.log` |
| 房间 API 回归，最终产物 | **36/36** | `tests/results/go-room-api-acl-regression/20261027T030057Z-8d43cc/report.json` |
| RPC / Forward / Move 回归，最终产物 | **50/50** | `tests/results/go-rpc-acl-regression/2026-10-27T03-01-40-975Z-080f47/report.json` |
| 原有无头 SDK 媒体回归，最终产物 | **9/9** | `tests/results/go-acl-media-regression-20261027/result.json` |

23 场景覆盖默认允许、动态拒绝/恢复、单轨选择、规则替换、SID/identity 优先级、
无效条目原子拒绝、重复条目、未知 SID、预授权后加入、显式退订保留、管理 API
不能越权、新轨道默认拒绝、recorder/EGRESS 豁免及撤销时序、全局 canSubscribe
撤权、发布者隔离和订阅者会话替换。原生测试额外隔离规则解析与状态更新，避免
只由浏览器集成间接判断这些语义。

原有媒体回归继续覆盖 VP8 三层、音频、重订、反向发布、数据和权限清理，SFU
服务端视频编解码零调用断言保留。脚本与 lockfile 已确认和依赖安装位置的版本
一致，仅换行不同；通过 LK_SERVER 指向本轮专用产物。

## 保留失败与未覆盖项

- 初始脚本 `2026-10-27T02-52-03-077Z-9da8f9` 错用不存在的 SDK 属性
  `isSubscriptionAllowed`，导致 Go 也无法通过拒绝断言。按 SDK 源码改为
  `permissionStatus`，不计这次结果为产品差异。
- `2026-10-27T02-56-45-731Z-88e6b1` 记录对 recorder 移除时序的错误预期：
  Go 在第 18 项失败，首版 C++ 立即撤权因而与 Go 不同。查明 SetPermission /
  SubscriptionManager 行为后修正 C++ 和用例，同时保留全局撤权的独立断言。
  最终套件验证实际 Go 契约，没有删除 recorder 撤权场景。
- R02 仍未完成：隐藏参与者、完整 grant/kind 字段与生命周期、刷新 token、完整
  动态权限矩阵、data track/blob ACL 等仍待实现。当前 recorder 支持仅覆盖
  媒体 ACL 豁免，不代表录制任务、房间计数/容量/activeRecording 已兼容。
- R03 信令恢复、Redis/PSRPC、外部 worker 实际联调和其他工作包仍待完成。
  本轮没有验证 Linux、跨系统、文件四轨、弱网、容量或长期稳定性；旧失败保留。
- 这是自动化验证，没有使用桌面自动化，没有实际设备人工验收，没有部署、
  提交或推送代码。

复现命令（不要并行运行多份 ACL 套件，UDP 范围固定为 59400–59580 和
59600–59780；HTTP 端口自动分配）：

```powershell
cmake --build out/windows-parity --config Release --parallel 6
ctest --test-dir out/windows-parity -C Release --output-on-failure
node tests/go_acl_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/windows-parity/Release/livekit-server.exe `
  --node-modules D:/workspace/livekit-cpp/tests/node_modules
```

报告与日志目录被 Git 忽略；依赖复用旧仓库已安装的 node_modules，不修改旧仓库
源码。每条用例需要两端断言都成立才算匹配，参考服务或候选未完成都会返回非零。
