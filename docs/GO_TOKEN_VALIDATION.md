# Go 主服务替代：JWT 刷新与动态 claims

日期：2026-10-27（Asia/Shanghai）。继续 R02；完整主服务替代仍未完成。

后续双 PC 信令 resume、SyncState 和 ICE restart 的独立证据见
[恢复验证](GO_RESUME_VALIDATION.md)；本报告的完整再入房不等同于信令 resume。

## 实现与固定基线

Go 1.13.10 源码仍为 `a390d03d28c895742d9b13c0894880ee67733801`，protocol
`f09f87aab29b865850a967924726bf76b7db4755`。按 `roommanager.go::refreshToken`、
`rtcSessionWorker`、`ParticipantImpl` 的 claims 变更及 protocol/auth verifier 实现：

- 入房后发送 `SignalResponse.refresh_token`，会话按单调时钟每 5 分钟续发。
  新 token 有效期至少 10 分钟；原入房 token 的到期时间更晚时予以保留。
  资料或权限变化也触发刷新，不重置周期计时。退出后不再续发。
- HS256 签名使用配置中的 API key；携带 sub/identity、规范化 kind、最新 name、
  metadata、attributes 和 video grant。保留可选布尔值的缺省与显式 false 区别。
  不重新签回未知字段、独立 SIP/Agent 等 grant、aud/jti/sha256 或 kindDetails，
  与固定 Go 刷新行为一致。
- 保留安全的 roomConfig/roomPreset；与 Go 一样，输出存储凭证或房间 RTMP
  stream key 阻止刷新，不把凭证发给客户端，也不因此关闭已建立的会话。
  这里只实现 JWT 中配置的保留/检查，尚未实现按 roomConfig 创建完整房间配置。
- 管理 API 与 SDK 自身资料更新共用 claims 更新路径。按 name、metadata、
  attributes 顺序逐项刷新；空 name/metadata 表示保持原值，空属性值删除该键。
  相同 name/metadata 不触发刷新；非空属性更新即使值相同仍触发，匹配 Go。
- 修改权限后刷新 token 携带新的发布、订阅、数据、自改资料、来源及附属标记。
  使用该 token 完整退房再入房，撤销的权限不会恢复，资料和属性保持最新。
- 未指定或 null 的 canPublishData 继承 canPublish；显式值独立生效。
  修正此前 canPublish=false 时默认仍允许数据发布的差异。
- JWT 必须带 exp，exp/nbf 接受 60 秒时钟偏差。sub 为空时兼容旧 jti identity。
  签名检查仍使用常量时间比较，非 HS256 和错误签名仍拒绝。

## 验证与产物

C++ 为平台子模块 `a21f3c0` 加未提交修改，使用专用 Windows Release 构建。
Chrome 156.0.8078.47 无头、Web SDK 2.15.6、Server SDK 2.13.3、Node 24.13.0。
不使用桌面自动化；测试以临时密钥、自建进程与 loopback 连接运行。

| 产物 | SHA-256 |
| --- | --- |
| Go 参考服务 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| C++ 修复前快照 | `7de3e48401a24449321cee063d546378e9d5b23393e731ed82d4d932a6aca7df` |
| 当前 C++ | `41f284f5b9d3e0204f384be5b330bf1673280fdc1c7738e659859df899f02c8f` |

旧二进制保存在 `out/parity-reference/livekit-server-cpp-pre-token.exe`。
编译日志 `out/token-build.log`，CTest 日志 `out/token-ctest.log`。

新增 `tests/go_token_parity.mjs`，通过官方 SDK 建立真实 PeerConnection，读取
WebSocket protobuf 中的 refresh_token，独立 HMAC 验签并检查 claims；不是仅
在 C++ 生成函数上做自测。测试不保存原始 JWT 或临时签名密钥，保存脱敏断言、
二进制与相关源码 SHA-256。可选周期检查保留生产的五分钟定时器。

| 检查 | 当前结果 | 证据（相对仓库） |
| --- | --- | --- |
| 初版 token 行为对照 | 18/18 | `tests/results/go-token-parity/2026-10-27T03-35-31-075Z-014748/report.json`（上一构建，非最终产物） |
| 最终扩展对照及真实五分钟周期 | 21/21 | `tests/results/go-token-parity/2026-10-27T03-37-56-990Z-0672b4/report.json` |
| CTest | 6/6 | `out/token-ctest.log` |
| 房间 API | 36/36 | `tests/results/go-room-api-token-regression/20261027T033758Z-419aef/report.json` |
| 参与者/生命周期 | 19/19 | `tests/results/go-participant-token-regression/2026-10-27T03-43-50-909Z-dd70a6/report.json` |
| 媒体 ACL | 23/23 | `tests/results/go-acl-token-regression/2026-10-27T03-46-24-384Z-c1b51f/report.json` |
| RPC / Forward / Move | 50/50 契约 | `tests/results/go-rpc-token-regression/2026-10-27T03-53-04-790Z-82f908/report.json` |
| 无头媒体 | 9/9 | `tests/results/go-token-media-regression-20261027/result.json` |

真实周期观测为 Go **299966 ms**、C++ **300420 ms**，两端新 token 的有效期
均为 600 秒，客户端仍处于 connected。没有加速时钟或修改生产刷新间隔。
最终报告内 `sourceHashes` 保存相关源码和测试脚本的 SHA-256。
同目录 `provenance.json` 还包含全部 src/include、原生测试和回归入口哈希。
上表除标明“上一构建”的初版对照外，均验证同一最终 C++ 二进制。
RPC 的 49 项要求响应深度相等，1 项采用下述已验证退出竞态契约；最终这次运行
实际 50 项响应也相同，不能由此删除前次出现的 404/503 分支证据。

## 失败证据与边界

- `tests/results/go-token-parity/2026-10-27T03-32-51-893Z-a17bdb`：Go 拒绝没有
  exp 的 token，暴露测试原先“无 exp 也可刷新”的错误预期；旧 C++ 没有发送刷新。
- `tests/results/go-token-parity/2026-10-27T03-33-22-364Z-bc4f7f`：修正预期后
  Go 15 项通过；旧 C++ 在第一项等待 refresh_token 超时。保留修复前证据。
- `tests/results/go-rpc-token-regression/2026-10-27T03-47-27-335Z-7c8b9c`：Go
  首个 callee 的 PeerConnection 建立失败，尚无 RPC 用例执行；报告 exitCode=2，
  不计为 C++ 通过。不修改代码、超时、断言即重新运行；未确定首次连接失败根因。
- `tests/results/go-rpc-token-regression/2026-10-27T03-48-16-236Z-8abee6`：严格
  对照 **49/50**，唯一差异为 recipient disconnect：Go 返回 404/not_found/
  participant does not exist，C++ 返回 503/unavailable/no response from servers。
  前一增量 `go-rpc-participant-regression/2026-10-27T03-23-07-957Z-2c9a3f` 中
  同一 Go 基线返回 503。源码中 Participant.Close 结束 RPC 后，PSRPC 重试与
  room map 删除/endpoint 注销并发，重试可能到达仍注册但参与者已删除的 handler
  返回 404，也可能找不到 endpoint 返回 503。这不是本次 token 更改引入的确定
  性错误；C++ 目前采用后一种结果，尚未实现真实 PSRPC 生命周期竞态。
  RPC 测试现仅对这一项接受两种**完整、精确**的已观察错误，新增五秒内完成、
  参与者消失和 C++ pending 请求清零检查，保留原始响应与 exactMatch 标记。
  其余 49 项保持响应深度相等。不能把此后契约通过表述为 50 项全部字节等价。
- R02 仍有完整 grant/来源权限、边界输入、通知时序与完整动态撤权矩阵待验证。
  metrics/agent-session grant 在本轮只做到解析、ParticipantPermission 和刷新
  携带，相关服务/数据能力没有因此交付。大 metadata 令牌受现有 32 KiB JWT
  与 HTTP 头限制，尚需和 Go 的完整请求边界对照；多 key 选择/轮转未验收。
- 完整退房再入房验证不等于信令 resume。R03 `/rtc/v1`、会话恢复、SyncState、
  ICE restart 仍未完成。Redis/PSRPC、外部 worker 实际联调、配置、Webhook、
  扩展编码及弱网/容量/稳定性验收继续按总计划推进。
- 旧四轨、跨系统 LOW/401 失败未关闭；没有设备人工验收、Linux 重跑、部署、
  提交或推送。自动化通过不代表可以替换生产 Go 入口。

```powershell
node tests/go_token_parity.mjs `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/windows-parity/Release/livekit-server.exe `
  --node-modules D:/workspace/livekit-cpp/tests/node_modules `
  --periodic true
```

periodic 模式同时运行两份相互独立的服务，用不同 UDP 范围等待真实五分钟；
不与使用 59800–60180 的其他媒体差分套件同时运行。未指定 periodic 时只跑
非周期场景。服务、浏览器上下文和临时配置在脚本退出时清理，报告保留失败。
