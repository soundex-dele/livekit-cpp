# 数据流与数据包兼容验证

2026-10-27，Windows x64 自动化。Go 基线仍为 1.13.10 / `a390d03d…`，协议
`f09f87aa…`。本次只推进 R02/R03/R07；主服务完整替代尚未完成。

## 实现

- 保留官方 SDK 文本和字节流的 header/chunk/trailer 透传；用实际分块内容检查
  Unicode 文本及定向 256 KiB 二进制传输。服务端不拼装或解压流内容。
- 顶层 destination identities 优先于旧 user identities，向两处规范化；目的
  identity 与旧 destination SID 按并集匹配。媒体 `canSubscribe=false` 不禁止
  接收数据，`canPublishData=false` 仍阻止发布数据。
- 普通发送者不能伪造身份。Agent kind 和旧 agent grant 可代发聊天、文本头，
  保留被代理身份并标记 generated；字节头仍标记实际 Agent。隐藏发送者沿用
  Go 的外层 SID/空公开身份语义。
- 普通参与者的 transcription 不转发；metrics 不再泄漏给普通数据接收者。
  **完整 metrics 订阅、聚合和时间戳路径尚未实现**，有权限的 metrics 消费者
  也尚未交付，不能把拒绝广播等同于完整 metrics 支持。
- 可靠消息按发布者的非零序号去重，保留原始序号；零序号保持旧协议语义。
  lossy 通道决定 kind，不推进可靠序号。信令 resume 返回 last_message_seq，
  并保留同一参与者的去重状态。
- user nonce 采用 Go 的房间级 100 项 FIFO UUID 缓存，包括初始零 UUID、
  非法长度不缓存、重复查询不刷新顺序等边界。

## 自动化结果

官方 Web SDK **2.23.0**，无头 Chrome **156.0.8078.48**：

| 模式 | 结果 | 报告 |
| --- | --- | --- |
| 单 PC | 22/22 | `tests/results/go-data-single/2026-10-27T06-18-34-022Z-b47839/report.json` |
| 双 PC | 22/22 | `tests/results/go-data-dual/2026-10-27T06-20-57-390Z-3cd061/report.json` |

测试检查真实 PC 模式，不能靠自动回退通过。原始数据通道旁路采样先于 SDK 的
去重，避免客户端替服务器掩盖重复包。恢复检查包含原 SID/PC 保持、回传发布
序号、拒绝重放旧包与允许新包。

同一产物的 CTest **8/8**，含新增的数据序号、路由与 nonce 淘汰边界；房间
API **36/36**，报告为
`tests/results/go-room-api-data-regression/20261027T061931Z-eadb49/report.json`。
原有无头媒体 **9/9**，含此前失败的 720p/LOW/HIGH 切换，记录为
`tests/results/go-data-media-regression-20261027/result.json`。

同一产物单 PC 媒体/扩容/恢复回归 **13/13**：
`tests/results/go-single-pc-data-regression/2026-10-27T06-24-47-101Z-db1d54/report.json`。
RPC **50/50 契约**（49 项原始响应相同）：
`tests/results/go-rpc-data-regression/2026-10-27T06-23-35-335Z-eefd8f/report.json`。
退出竞态仍按此前验证的 Go `404/not_found/participant does not exist` 或
`503/unavailable/no response from servers` 两种完整错误检查，不声称 PSRPC 等价。

本次链接了另一平台应用任务修复的原生 GoogCC 拥塞原因状态通知；补丁说明见
`patches/README.md`。纯视频测试使用 MTU 大小的真实 RTP 负载，并检查实际视频
送达；当前复测通过，见 `out/data-native-result.log`。此前的恢复失败、过大
合成包问题及中间实验日志仍保留，不能将当前结果归给此前的二进制。

## 产物与失败记录

- C++ 固定副本：`out/parity-reference/livekit-server-cpp-data.exe`。
  SHA-256：`2869defdb4689c613f32811e55499a0533df33f6ff3e303d715523140ba6fe72`。
- Go SHA-256：`c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98`。
- WebRTC 静态库 SHA-256：`58437f9607ce7a2ba02a539cfac977fd40fde9ed313ebd817d86b130443c21ec`。
- 原生发送测试 SHA-256：`3163bcebbf3b563c17dbae1e188a8a77a8fb873354fb45bbf44d1a69c25b74ec`。
- 单 PC 报告目录附 `provenance.json`、源码快照 `sources.zip`；构建、CTest 与
  原生恢复日志分别为 `out/data-build.log`、`out/data-ctest.log`、
  `out/data-native-result.log`。这是未提交工作树，不把 HEAD 当成完整源码版本。

保留失败：

1. `go-data-before/2026-10-27T05-50-11-019Z-d103e3`：Go 健康检查误用 `/healthz`，
   因测试入口错误而超时；不算对照通过。C++ 的具体失败仍保留。
2. 修正入口后 `go-data-before/2026-10-27T05-57-00-511Z-7f7145`：Go **17/17**，
   旧 C++ **6/17**。旧 C++ SHA 为 `2081f3a7…`，11 项差异均保留原始断言。
3. `go-data-parity/2026-10-27T06-15-35-595Z-180362`：**21/22**；新增恢复检查
   错误假定新版 SDK 总有 subscriber PC，双方测试均抛异常。改为识别单/双 PC。
4. `go-data-dual/2026-10-27T06-18-59-731Z-7d2329`：Go ICE 建连失败；C++ 的
   模式断言发现 singlePeerConnection 设置位置错误。将其放入 Room 构造选项
   后双 PC 完整重跑；未删除模式断言或降低连接检查。

## 剩余边界

本次没有实现 data-track 专用通道、WebTransport、跨节点可靠数据缓存/迁移、
全部重连重放与缓冲溢出契约、限流/背压参数兼容、授权 metrics 汇聚、数据
E2EE 实际密钥互通或所有旧 SDK 组合。流的取消、丢块与全部压缩扩展仍需
补充差分，不能用上述内容完整性检查替代 R07 整体验收。

720p 与纯视频模拟恢复在当前产物上通过；历史四轨帧数、Linux/Windows
跨系统 LOW/401、长时间弱网、容量、真机和灰度仍未验收。本次没有部署、提交
或推送，也没有启动 Go 来为 C++ 主服务补功能；Go 仅用于差分参考。

```powershell
node tests/go_data_parity.mjs --mode single `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-data.exe
node tests/go_data_parity.mjs --mode dual `
  --go-server out/parity-reference/livekit-server-go.exe `
  --cpp-server out/parity-reference/livekit-server-cpp-data.exe
```

测试依赖目录可通过 `--node-modules` 与 `--modern-node-modules` 指定；同机串行
运行，Go/C++ UDP 范围分别为 61000–61180 / 61200–61380。测试删除临时密钥
配置并清理自建服务和无头浏览器；保留日志、二进制哈希和每项断言。
