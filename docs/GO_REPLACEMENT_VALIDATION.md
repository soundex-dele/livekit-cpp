# Go 主服务替代：首批控制面验证

日期：2026-10-27（Asia/Shanghai）。本轮只完成首批房间 API 兼容修复及相关回归，
**主服务完整替代尚未完成**。完整范围见 [替代计划](GO_REPLACEMENT.md)。

## 基线和构建

- Go：`D:/workspace/livekit`，`a390d03d28c895742d9b13c0894880ee67733801`。
  `go version -m` 确认 `vcs.modified=false`，`--version` 输出 `1.13.10`。
  使用 Go 1.27.3 从 `./cmd/server` 构建，未修改 Go 源码或依赖版本。
  默认模块代理连接超时后，本次构建进程改用 `https://goproxy.cn,direct`；
  没有修改全局 Go 配置或关闭模块校验。
- C++：平台子模块 `a21f3c0` 加本轮工作区修改，Windows x64 Release。
  独立目录 `out/windows-parity`，复用平台 SDK 的 WebRTC 预编译库。
  未覆盖 `out/windows-x64` 下其他任务使用的二进制。
- CMake 配置及编译成功；保留原生依赖既有编译警告，本轮未修改 WebRTC。
  日志：`out/parity-configure.log`、`out/parity-build-final.log`。

| 产物 | SHA-256 |
| --- | --- |
| Go 参考服务 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| C++ 修复前服务 | `a83bf74d9bcc1fec707801d3ed449305d986910812ba59ab347d36f766f919b3` |
| C++ 修复后服务 | `bf9bd4618a4f26386df7ad390b5fe2aab12c940a4d2b3471b239caf1fe5c5771` |

## 行为差分

同一份 `tests/go_room_api_parity.py` 启动独立 Go/C++ 进程，分别发送相同逻辑请求。
测试只使用自建房间和临时密钥，运行后清理测试进程和密钥配置。

| 检查 | 修复前 | 修复后 |
| --- | --- | --- |
| 36 项 JSON RoomService 用例 | 22 通过，14 失败 | **36/36 通过** |
| CTest 原生测试 | 未在旧二进制上重跑 | **4/4 通过** |
| 官方 Web/Server SDK 无头集成 | 未在旧二进制上重跑 | **9/9 通过** |

差分证据路径（相对本仓库；产物被 Git 忽略，未打包进源码提交）：

- 修复前：`tests/results/go-room-api-before/20261027T015718Z-1ab28d/report.json`。
- 修复后：`tests/results/go-room-api-after/20261027T015745Z-574ada/report.json`。
- 原生测试：`out/parity-ctest.log`，包含 core、TURN credentials、native RTP、native send。
- 浏览器：`tests/results/go-replacement-browser-20261027/result.json`，同目录保留日志。

本轮修正的可观察差异：

1. RoomService grant 拒绝返回 `401/unauthenticated`，与 Go 一致；RTC 信令权限检查
   仍有自身的处理逻辑，未全局替换所有权限错误。
2. 默认 metadata 上限改为 512 KiB；attributes 按键和值的 UTF-8 字节总量单独
   限制为 64 KiB。两者可同时达到上限；拒绝更新不部分修改参与者状态。
3. HTTP body 默认上限对齐为 10 MiB，信令消息为 2 MiB。验证了 JSON 转义后
   超过旧 1 MiB HTTP 限制、但解码 metadata 正好 512 KiB 的有效请求。
4. `SendData.nonce` 必须为空或 16 字节；非法 nonce 返回 `invalid_argument`。
5. JSON 通过 protobuf 描述符解析：接受字段别名、忽略未知字段，字段类型错误
   返回 `malformed`；两种请求编码进入同一 API 表示。
6. HTTP JSON 响应使用 proto 字段名并输出默认标量/列表，与 Go 的默认风格对齐。
7. 默认单机行为下，不存在房间的 `ListParticipants` 返回空列表且不创建房间。
8. `CreateRoom` 对齐 Go 接受空房间名的行为，删除该房间可成功。RTC join 仍独立
   检查房间和 identity，未把管理 API 的这一行为扩展为入房支持声明。

早期 33 项探索运行曾把空房间名误设为应返回 400；Go 实测返回 200，源码也确认
这一行为。最终 36 项套件已修正参考断言，并在修复前/后两份二进制上重新运行；
没有把 Go 的实际行为当成 C++ 缺陷后再放宽成忽略该项。

## SDK 与媒体回归范围

沿用 `tests/integration.mjs`，Chrome 156.0.8078.47 无头模式，Node 24.13.0，
Web SDK 2.15.6、Server SDK 2.13.3。复用了同级旧仓库已有的依赖安装；运行前逐行
确认测试脚本和 lockfile 与本仓库一致（仅文件换行不同），通过 `LK_SERVER` 指向
本轮 C++ 产物，`LK_TEST_RESULTS` 指向本仓库的独立结果目录。未修改旧仓库源码。

通过的九组检查包括官方 API/JWT 与二进制 protobuf 请求，双浏览器信令/ICE/DTLS，
真实合成音视频接收，数据通道与定向 SendData，参与者和房间 metadata，静音/重订，
后加入及反向发布，三层 simulcast 独立选层/恢复，以及权限、重复 identity 和清理。
服务端 SFU 编码/解码审计断言保留，未放宽媒体门槛。

## 未覆盖与剩余风险

- 36 项差分仅覆盖无媒体的 JSON 房间控制面子集。错误比较 HTTP 状态/Twirp code，
  不要求错误文本逐字一致；房间主要业务字段与 SID 稳定性有检查，不比较全部响应。
  Go/C++ 的未设置 message 字段 null/省略、编码列表等仍不能宣称完全一致。
- HTTP JSON 字段输出风格改变了旧 C++ 响应；官方 SDK 已回归，自行按旧 camelCase
  解析的 HTTP 客户端需跟随 Go 的 proto 字段名。配置化限额和完整 HTTP 错误行为
  仍属于 R01/R09 后续工作。
- 本记录对应首批增量；`PerformRpc` 后续进度见 [RPC 验证记录](GO_RPC_VALIDATION.md)。
  完整 JSON/protobuf 差分、隐藏参与者、细粒度 ACL、`/rtc/v1`、
  会话恢复、更多编码、E2EE、data track/blob、Redis/PSRPC 和外部 worker 尚未补齐。
- 没有重跑 CLI 文件四轨、Windows→Linux/WSL、Linux TURN、公网、弱网、容量和
  长期稳定性。上一轮失败继续有效，不能由本次九组浏览器通过替代。
- 全部结果为自动化检查；没有操作用户桌面，没有完成实际设备人工验收。
- 本轮没有部署、切换运行中的媒体服务、提交 Git 或推送远端。
