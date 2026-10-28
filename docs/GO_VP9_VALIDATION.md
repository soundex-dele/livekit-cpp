# VP9 与空间层转发验证

2026-10-28，Windows x64 自动化；本增量推进 R04/R05。**完整媒体兼容尚未完成。**

## 实现

- 协商 VP9 profile 0，区分缺省 profile 0 与不支持的 profile，公布 video/VP9。
- 使用 libwebrtc 原生 VP9 depacketizer、frame assembler、reference finder 和
  packetizer；不调用视频编码器或解码器。保留压缩内容、flexible mode、GOF、
  temporal index、跨帧依赖、picture ID 和 TL0 信息。
- 支持同一 RTP timestamp 的多个空间层；按依赖顺序转发，过滤掉的层不消耗
  下游 RTP 序列号。每张图像只在订阅者选中的最高层设置 marker。
- 独立订阅者可按 LOW/MEDIUM/HIGH 或目标尺寸选 VP9 空间层，保留全部较低层
  依赖；按图像固定选择，重写 scalability structure 的层数。调整订阅时请求关键帧。
- 在接受完整帧批次后才提交 picture/TL0 offset 和空间层状态。队列溢出导致
  新源切换失败时，当前源的依赖与编号仍然有效。
- 接受 VP9 的 MULTIPLE_SPATIAL_LAYERS_PER_STREAM；其他未实现的 SVC 编码
  仍然拒绝。沿用原生 SFU 路径，不引入采集、转码或 Go 代理。

## 测试方法

测试使用官方 Web SDK 2.23.0 和无窗口 Chrome，合成 canvas 视频。
断言实际入站/出站 codec、持续增长的 framesDecoded、精确解码尺寸以及出站
scalabilityMode；VP8 回退不能通过。单 PC 检查实际 publisher-only 模式。

通用套件单/双 PC 共 14 项：解码、轨道 metadata、VP8 混合、双向发布、静音恢复、
重复订阅、迟加入。L3 模式增加每种 PC 的独立 LOW/HIGH、中间尺寸及升回 HIGH，
合计 20 项。服务端视频编解码审计必须保持零。

原生测试使用合成的 VP9 payload（**不声称这些测试字节可解码**），分别验证：
两张图像 × 三空间层、分片丢失及 NACK/RTX 修复、逐字节内容保持、RTP 序列号和
picture ID 回绕、独立 base-only 订阅、SS 层数、marker 和拒绝切源后的状态保护。
真实可解码性由浏览器套件另外检查。

## 元数据修复

进一步检查发现 vp9-v2 将 SVC 错报为 simulcast，且三层 spatialLayer 均为 0。
加强原有 metadata 用例后，旧产物在 go-vp9-metadata-before/2026-10-28T02-43-36-159Z-ab69f3/
通过首项解码、随后稳定断言失败。vp9-v3 对省略 video_layer_mode 的 VP9 推断多空间层，
区分显式 ONE_SPATIAL_LAYER_PER_STREAM，按分辨率规范化层顺序与空间层索引。
此前 metadata 用例只检查 MIME；下表中的 vp9-v2 通过项不代表此元数据差异已关闭。

## 最终 vp9-v3 产物

| 检查（含增强的元数据断言） | 结果 | 报告目录（tests/results/ 下） |
| --- | --- | --- |
| L3T3_KEY 混合三轨、双向/迟加入、独立选层，单/双 PC | **20/20** | go-vp9-svc-v3/2026-10-28T02-45-31-080Z-ab0c53/ |
| L1T3 与 VP8 混合，单/双 PC | **14/14** | go-vp9-temporal-v3/2026-10-28T02-46-17-543Z-1072f3/ |
| L3T3 独立空间层选择，单/双 PC | **12/12** | go-vp9-l3t3-v3/2026-10-28T02-47-36-086Z-4a95de/ |
| H.264/VP8 混合，单/双 PC | **14/14** | go-h264-vp9-v3/2026-10-28T02-48-07-014Z-2def41/ |
| 常规音视频/VP8 simulcast 集成 | **9/9** | vp9-v3-media-integration-20261028/result.json |
| RoomService JSON/protobuf | **234/234** | go-room-contract-vp9-v3/2026-10-28T02-45-39-542Z-a982cf/ |
| 原生 CTest | **10/10** | out/vp9-ctest-v3.log（项目目录） |

上述媒体测试逐项同时通过固定 Go 和 C++，记录实际浏览器解码和服务端零编解码调用。
历史失败仍保留，完整稳定性与弱网验收未完成。

## vp9-v2 产物的自动化门禁

| 固定 vp9-v2 产物 | 结果 | 报告 |
| --- | --- | --- |
| L3T3_KEY 混合三轨及选层，单/双 PC | **20/20 Go/C++ 对照** | tests/results/go-vp9-svc-idle/2026-10-28T02-38-51-191Z-31a90c/ |
| L3T3_KEY 独立空间层选择，单/双 PC | **12/12 Go/C++ 对照** | tests/results/go-vp9-spatial-v2/2026-10-28T02-33-55-421Z-51cd89/ |
| L1T3 与 VP8 混合，单/双 PC | **14/14 Go/C++ 对照** | tests/results/go-vp9-temporal-v2/2026-10-28T02-35-47-866Z-9a907a/ |
| H.264/VP8 混合回归，单/双 PC | **14/14 Go/C++ 对照** | tests/results/go-h264-vp9-regression/2026-10-28T02-37-04-017Z-98c9c6/ |
| RoomService JSON/protobuf | **234/234** | tests/results/go-room-contract-vp9/2026-10-28T02-34-44-358Z-6d9447/ |
| L3T3 独立空间层选择，单/双 PC | **12/12 Go/C++ 对照** | tests/results/go-vp9-l3t3-v2/2026-10-28T02-39-50-257Z-f95b44/ |
| 常规音视频/VP8 simulcast 集成 | **9/9** | tests/results/vp9-media-integration-20261028/result.json |
| 原生 CTest | **10/10** | out/vp9-ctest-final.log |

独立选层拓扑使用 --svc-only true：一名发布者和两名订阅者，不发布混合 VP8 或
反向视频。每种 PC 的 6 项为解码、metadata、第二订阅者、LOW/HIGH 独立选择、
640×360 中层、恢复 1280×720。此结果不能替代完整混合三轨三空间层门禁。
L1T3 的通过验证保留时间依赖的完整转发，不表示实现了时间层筛选。

## 保留的失败与诊断

结果目录均在 tests/results/，包含 report.json、测试源码副本和服务日志。

| 目录 | 结果与解释 |
| --- | --- |
| go-vp9-before/2026-10-28T01-58-11-860Z-97fcea | 旧 limits-v2 无 VP9 解码；Go 首次建连失败，不能算有效差分通过 |
| go-vp9-before-recheck/2026-10-28T02-03-00-308Z-ac8e00 | 同样的参考建连失败及旧产物 VP9 失败 |
| go-vp9-diagnostic/2026-10-28T02-06-01-005Z-455e96 | 初版 C++ L1T1 单/双 PC 14/14；仅 C++ 诊断，不是差分验收 |
| go-vp9-svc-diagnostic/2026-10-28T02-10-03-213Z-741de6 | C++ L3T3_KEY 双 PC 10/10；单 PC 6 项通过后迟加入 VP8 尺寸失败。入站帧数仍增长，发布端降为 960×540 |
| go-vp9-connect-diagnostic/2026-10-28T02-13-02-761Z-4d0a2a | Go L1T1 双 PC 7/7，单 PC 建连失败；保留建连期间 ICE 轨迹，STUN 请求无响应 |
| go-vp9-loopback-diagnostic/2026-10-28T02-16-17-052Z-2c8a2b | Go L3T3_KEY 双 PC 10/10，单 PC 6 项通过后迟加入尺寸失败；发布端关闭高空间层 |
| go-vp9-svc-v2/2026-10-28T02-23-58-375Z-f0095f | Go 信令超时，C++ 3 项后双向媒体尺寸超时；整轮失败 |
| go-vp9-svc-final/2026-10-28T02-29-26-699Z-2609f3 | 3/5 已执行项匹配，整轮未通过；两端入站帧数增长但分别停在 640×360 / 320×180 |

测试修正：参考 Go 限制 rtc.ips.includes 为 127.0.0.1/32，避免多网卡上全部
NAT 映射为环回 IP 的歧义；仍使用固定 Go 二进制。选层测试明确设置
maintain-resolution，保留原有解码/尺寸断言。运行时观察到大量其他编译进程；
这只记录环境干扰，不能据此排除服务器缺陷或关闭历史视频稳定性问题。

原生验证保留 out/vp9-build-v1.log 的测试构造函数编译错误，随后已修复。
首次构建失败后的旧 CTest 10/10 不能算新增 VP9 测试证据。
随后一次新 CTest 的 native-rtp 通过，但 RPC 短时间等待失败；
out/vp9-ctest-v2.log 的新 native-rtp（含队列回归）通过，RPC 再现，core 因
仍在链接而未启动。RPC 测试已改为排空异步计时器，未修改生产 RPC 的期限或逻辑。

最终 out/vp9-ctest-final.log：**10/10**，包含新增 VP9 与修正的 RPC 测试。

## 固定产物与复现

- 最终 C++：out/parity-reference/livekit-server-cpp-vp9-v3.exe
- SHA-256：d6b13632b97121b77c54abc2bbb755191afb084d0a904cf7c6a06c0cdade23b4
- 元数据修复前 vp9-v2 SHA-256：2911a5e1796035fca9311009463ba5003d6fdf4888a7bc0f82d80c46dcf58705
- Go：out/parity-reference/livekit-server-go.exe
- Go SHA-256：c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98
- Go 基线：a390d03d28c895742d9b13c0894880ee67733801（1.13.10）。
- C++ 基于 36c85467fe753a5b03b2f9968c376424fbccb00b 加工作区变更，包含先前内容
  上限与轨道更新。最终文件快照、哈希和补丁在 out/parity-reference/vp9-v3-provenance/。
  修复前快照保留在 out/parity-reference/vp9-v2-provenance/。

~~~powershell
node tests/go_codec_parity.mjs --codec vp9 --scalability-mode L3T3_KEY --go-server out/parity-reference/livekit-server-go.exe --cpp-server out/parity-reference/livekit-server-cpp-vp9-v3.exe
ctest --test-dir out/windows-parity -C Release --output-on-failure
~~~

## 未完成边界

当前带宽分配仍按整个源 SSRC 计费；尚无按 SVC 空间/时间层码率的拥塞自适应、
订阅帧率或 dynacast。VP9 profile 1/2/3、7-bit picture ID 回绕、其他 SVC 模式及客户端平台矩阵尚未验收。
完整 TrackInfo 字段（例如实际源 SSRC 的填充）尚未逐项对齐。
AV1、RED、H.265、完整 H.264 profile/分层和 E2EE 仍待实现与验证。
历史间歇视频失败、弱网/容量/24 小时稳定性门槛仍开放。
未做 Linux 重建、真摄像头、硬件编码器或实际设备人工验收。
