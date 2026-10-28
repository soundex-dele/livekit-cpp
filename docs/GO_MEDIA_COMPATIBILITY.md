# 完整媒体兼容实施与统一验证

状态：实现集中落地，统一验证进行中；尚未宣称 R04/R05/R06 完整验收。
参考版本沿用 GO_REPLACEMENT.md 固定的 Go 1.13.10 提交。

## 已接入的功能路径

- 编码协商：VP8、VP9 profile 0–3、AV1 profile 0–2、H.264 profile/mode 0/1、H.265 SRST，以及 Opus/PCMU/PCMA。
- 音频 RED：校验 RFC 2198 封装，重映射内部 PT，RED 转 Opus，普通 Opus 生成最多两包冗余；编码内容保持不变。
- 加密音频：发布时独立登记加密状态，保留认证过的 Opus/RED 原始格式；不把密文 Opus 合成为 RED，也不拆解密文 RED。密文 RED 的跨 PT/客户端组合仍需单独验收。
- 不解密转发：保留原始 RTP 负载，单独解析 DD/通用描述符及必要的公开 RTP 载荷头；有界乱序缓存、首包结构重传恢复、原生 NACK/RTX 和依赖门控。
- 描述符连续性：VP8 时间层信息、VP9 7 位 ID 回绕及源切换重映射，DD 帧号、有效解码目标和下游空间层 marker。
- 自适应：按空间/时间层统计累计码率、独立带宽分配、FPS 选层、画面内固定层上限、队列取消和关键帧恢复。
- 拥塞配置：`rtc.congestion_control.enabled` 控制层分配，`allow_pause` 控制预算不足时是否暂停基础层；默认值对齐固定 Go 的 `true/false`。关闭暂停时允许基础层需求超出预算，实际发包仍由原生 pacing 控制。
- 备用编码：原 SID 下的额外 CID 发布，协商 codec/profile 匹配、动态绑定/撤销、各编码独立 SSRC 元数据和 dynacast 需求反馈。
- 备用策略：SIMULCAST 保持各编码独立；PREFER_REGRESSION/REGRESSION 收到备用发布后锁定回退编码并重绑订阅。
- 备用切换连续性：同一 SSRC 重建发送器时保留 RTP/RTX 序号、输出时间戳和 RTCP 累计计数，仍等待新源关键帧；单 PC 可切换到已有协商列表内的备用 PT。
- 会议事件：400 ms 音量观察窗、活跃度和音量量化、撤订后的说话人清除、订阅范围内连接质量、stream state，以及恢复后的状态重放。
- 质量评分：原生 RTP/RTCP 采样、R-factor/MOS、丢包权重、恢复/下降平滑和空间层降级。统计窗口输入仍需与 Go 弱网场景逐项比较，不能仅凭相同公式宣称评分完全一致。

## 验证状态

遵循用户要求，先集中实现功能，再统一运行验证。期间编译仅用于检查接口和链接。

- `out/media-complete-build-1.log`、`-2.log`：实现阶段编译成功。
- `out/media-complete-build-3.log`：重复成员声明导致编译失败；`-4.log` 修正后成功。
- `out/media-complete-ctest-1.log`：9/11；DD 测试/生产扩展写入顺序问题，以及旧 FPS 拒绝断言。
- `out/media-complete-build-5.log`、`out/media-complete-ctest-2.log`：修正后构建成功，原生 **11/11**。
- 新增 `tests/unit/media_compat_test.cpp` 覆盖 RED 转换、DD 首包乱序、AV1/H26x 不透明载荷、密文保持、VP8/VP9 描述符和事件算法。这些是传输级合成数据检查，不能替代真实编码解码。
- 官方 SDK 无窗口验证脚本已扩展编码、加密密钥、RED、会议事件、FPS、dynacast 和备用编码选项。实际结果在后续验证完成后补入。
- `out/media-complete-build-9.log`、`out/media-complete-ctest-3.log`：补齐评分默认开关、RED/FEC 权重、撤销发布的加密状态清理后，构建成功、原生 **11/11**。
- `media-v4` 固定产物与源码/补丁哈希保存在 `out/parity-reference/media-v4-provenance`；SDK 测试使用冻结二进制，不引用持续变化的构建输出。

## 已保留的差异与失败

- `tests/results/media-v1-av1/2026-10-28T03-56-24-437Z-c4bc66/report.json`：AV1 L1T1 单/双 PC 实际解码 **14/14** 对照通过。
- `media-v1-adaptive`：Go VP9 L3T3_KEY 在请求 4 FPS 后仍解码约 57 帧/4秒；C++ 存在不同的降帧行为。测试记录实际帧数并比较效果，不能用单边降帧断言判定 Go 通过。
- `media-v2-backup`：默认 Go 备用策略会让既有订阅回退为 VP8。脚本现显式区分三种策略；旧 C++ 单 PC 切换停流、主/备用 MIME 覆盖问题保留在原报告，修复后需要重新对照。
- `media-v2-e2ee-h264`：Go **16/16**，旧 C++ 在重订阅检查中的反向 H.264 停流。
- `media-v3-e2ee-h264`：该次重订阅检查通过，但迟加入者 VP8 无入站统计；`media-v3-e2ee-diagnostic` 还出现带宽探测超时。以上运行与构建重叠，尚不能排除调度影响，也不能当作修复通过的证据。后续统一串行复测。
- `media-v7-backup-simulcast/2026-10-28T05-08-09-637Z-c48a2e/report.json`：VP9 L1T3 + VP8 SIMULCAST 备用策略，单/双 PC **16/16** 对照通过。
- `media-v7-h265/2026-10-28T05-09-33-135Z-2d6da0/report.json`：双方都通过初始 H.265 解码、元数据、混合 VP8、反向 H.265；双方都未通过静音恢复。Go 发布端统计显示主轨随后变成 VP8，C++ 发布端 H.265 编码帧停在 56。不能将该组合宣布为 H.265 完整通过。
- `media-v7-audio-red-events/2026-10-28T05-10-43-766Z-d9cf59/report.json`：C++ 完成单/双 PC **22/22** 音频、会议事件检查，但 Go 在 RED MIME 元数据断言处失败（实际 `audio/red`，旧断言预期 `audio/opus`）。已对齐断言和 C++ 元数据，必须重新对照后才能计入验收。
- 原生走时回归：`out/media-probe-regression-before.log` 稳定复现旧调度器探测饥饿；修正时间读取顺序后，`out/media-complete-ctest-5.log` **11/11**。此修复不能替代加密端到端验证；`media-v7-e2ee-h264` 仍有失败，后续继续对齐默认不暂停基础层的策略。

未完成整体验收：全部编码/profile/设备组合真实解码、弱网评分差分、历史间歇停流、100 会话容量、24 小时稳定性、Linux 重建和实际设备人工验收。生产完整替代的其余 R00–R15 门槛继续以主计划为准。

## 后续统一验证与修复（media-v8 / media-v9）

以下均为 Windows、官方 Web SDK 2.23.0、无窗口 Chromium 的合成媒体自动化对照，不是实际设备人工验收。报告位于 `tests/results/`，原始失败不覆盖。

| 固定产物与场景 | 对照结果 | 报告目录 |
| --- | --- | --- |
| v8 H.264 E2EE | 16/16，包括错误密钥不能解码、换回密钥恢复 | `media-v8-e2ee-h264/2026-10-28T05-17-22-427Z-8a8d0e` |
| v8 H.264 mode 0 | 14/14 | `media-v8-h264-mode0/2026-10-28T05-21-49-667Z-2d71b7` |
| v8 H.264 high profile | 14/14，允许协商降低 level，检查 profile/constraints | `media-v8-h264-high/2026-10-28T05-22-29-053Z-54e174` |
| v8 VP9 L1T3 E2EE | 16/16 | `media-v8-e2ee-vp9/2026-10-28T05-23-05-090Z-0f4bc7` |
| v8 AV1 L3T3_KEY | 12/12，SVC 专项，不包含混合 VP8 场景 | `media-v8-av1-svc/2026-10-28T05-24-21-857Z-32b372` |
| v9 VP9/VP8 REGRESSION | 16/16 | `media-v9-backup-regression/2026-10-28T05-33-40-491Z-a1432b` |
| v9 VP9/VP8 PREFER_REGRESSION | 16/16 | `media-v9-backup-prefer/2026-10-28T05-34-46-431Z-c18c83` |
| v9 RED + Opus/G.711 + 会议事件 | 22/22 | `media-v9-red-events/2026-10-28T05-36-03-903Z-114fda` |
| v9 VP9 L3T3_KEY 自适应 | 23/24 差分一致，两端各完成 24 个场景 | `media-v9-adaptive/2026-10-28T05-38-46-541Z-83645f` |

v9 精确源码、工作树补丁、构建配置及二进制哈希：`out/parity-reference/media-v9-provenance/manifest.json`。构建日志 `out/media-complete-build-16.log`；原生 `out/media-complete-ctest-7.log` **11/11**，新增发送器重建连续性和加密音频不改写断言。v8 没有独立的完整源码快照，报告只可证明其记录哈希的二进制行为；不将当前工作树回填为 v8 源码。

修复依据及仍未关闭项：

- v8 REGRESSION **15/16**、PREFER_REGRESSION **7/16** 的既有订阅停流：单 PC 的零新增媒体段请求不会触发 SDK offer；重建同 SSRC 发送器又重置了序号/时间戳。v9 同场景通过，历史结果保留。
- v8 RED 元数据失败：上行 answer 把 Opus 排在 RED 前。v9 保持 publisher offer 的音频优先顺序，并报告实际协商 MIME。音频 Go 夹具新增 loopback IP 限定，以排除虚拟网卡候选造成的参考建连失败。
- v8 自适应首项 C++ 超时：实际持续解码 320×180，未达到期望 1280×720。v9 达到完整分辨率，但没有足够证据据此关闭历史层升级间歇失败。
- v9 FPS 差异：双 PC 请求 4 FPS 后，Go 为 57 帧/4秒，C++ 为 15 帧；单 PC 分别为 14/14 帧。固定 Go 使用 RTP 时钟及缓存的依赖/连续性估计，C++ 当前使用滑动到达时间统计，尚未逐项对齐。测试保留差异判定，不删除断言。
- v9 加密音频 **2/22**：Go **22/22**，C++ Opus 解密失败。虽然已有密文不合成 RED 的转发保护，音频发布尚未登记加密状态，导致该保护没有生效。后续已修正状态登记，须以新产物验证结果为准。

## media-v10：音频加密状态修复

`Peer::publication_codec` 将可选加密状态与 codec 约束分别处理：音频没有显式 codec 请求时仍登记加密标志；撤销发布时清理状态。此改动和 v9 的密文格式保护共同修复加密 Opus。

- `tests/results/media-v10-audio-e2ee/2026-10-28T05-48-27-386Z-e7d021/report.json`：双方 **22/22**，包含加密 Opus/PCMU/PCMA 混合实际音调、会议事件、静音恢复、重订阅、迟加入及信令恢复。
- `out/media-complete-build-17.log`：构建成功；`out/media-complete-ctest-8.log`：原生 **11/11**。
- 精确产物及源码快照：`out/parity-reference/media-v10-provenance/manifest.json`；后续文档更新不改变已冻结源码。
- RoomService JSON/protobuf 契约 **234/234**：`tests/results/media-v10-room-contract/2026-10-28T05-50-28-885Z-5a619f/report.json`。
- 既有无窗口媒体集成 **9/9**：`tests/results/media-v10-integration-frozen/result.json`；记录 SHA256 `509db2c834dc8ee8f923657a5a8f6d50de3a08619d07558530cec8a6e4b287f4`，与 v10 冻结产物一致。此前 `media-v10-integration` 使用了错误的环境变量名，实际测试了默认旧产物；该轮保留但不计作 v10 证据。
- H.265 未通过：`tests/results/media-v10-h265/2026-10-28T05-49-22-896Z-842a1a/report.json`。两端初始四项通过，均在静音恢复失败；Go 发布端随后输出 VP8，C++ 发布端 H.265 编码停在 46 帧，另一路反向 H.265 仍推进。此证据不足以归因服务端，也不足以证明 H.265 生命周期兼容。
- `git diff --check` 通过。未运行桌面自动化；未进行真实设备人工验收。

剩余实现/兼容边界：FPS 统计和基线缓存行为尚未一致；未协商 `extmap-allow-mixed` 时超过 16 字节的 DD 当前安全拒绝；H26x 无 DD 的多 NAL 启动/乱序边界需要更完整的弱网覆盖；弱网连接质量统计窗还没有完整 Go 差分。上述边界加上 H.265、历史间歇停流和部署/容量门槛，意味着“完整媒体兼容”仍不能标记完成。
