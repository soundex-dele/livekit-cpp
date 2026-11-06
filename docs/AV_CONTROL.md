# AV 业务控制面接入

`AV_MEDIA_MODE=true` 启用 AV 平台的媒体节点模式。`livekit-server` Go 业务服务拥有组织、会话和授权；本进程只执行媒体准入与媒体操作。未启用该模式时保留原 LiveKit 兼容入口。

## 配置与节点身份

| 环境变量 | 含义 |
| --- | --- |
| `AV_MEDIA_MODE` | `true` 启用本协议 |
| `AV_NODE_ID` | 部署内唯一、稳定的节点 ID |
| `AV_RTC_KEY` | `keys` 配置中的 RTC 签发者名称 |
| `AV_MANAGEMENT_KEY` | `keys` 中独立的管理签发者名称 |
| `AV_ADMISSION_URL` | 业务服务的 `/internal/v1/media/admissions/check` HTTPS 地址，可使用内部服务域名 |
| `AV_ADMISSION_SECRET` | 此节点专用的准入认证秘密，至少 32 字符 |
| `AV_ADMISSION_CA_FILE` | 可选，自建 CA 的 PEM 文件；始终校验证书与主机名 |
| `AV_ALLOW_INSECURE_ADMISSION` | 仅开发允许设为 `true` 使用 HTTP |

RTC 与管理签发者名称及秘密必须分别不同，两个签名秘密均至少 32 字符。秘密沿用 JSON/YAML `keys` 或 `LIVEKIT_KEYS` 载入，不在日志、节点信息中返回。生产将管理端口限制为受信控制面访问；需要外部 TLS 入口时由经过配置的网关承接。

每次启动生成新的 `node_incarnation`，不允许用环境变量恢复旧代次。控制面从节点信息读取代次；重启前签发的媒体凭证与媒体操作在新进程中失效。

`GET /internal/v1/media/node` 使用管理 JWT 且要求 `video.roomList=true`，返回 `node_id`、`node_incarnation`、`av_mode`、`ready`、`metrics`。

## 准入与 Token

媒体 JWT 使用 RTC 签发者，并包含标准 `sub`、`video.roomJoin`、`video.room`、发布/订阅权限和以下声明：

```json
{
  "av_grant": "opaque-grant-id",
  "av_policy_version": 1,
  "av_binding_generation": 1,
  "av_node_id": "media-1",
  "av_node_incarnation": "NI_process-generation",
  "av_grant_expires_at": 1800000000
}
```

每个 `RTCGrant` 使用独立参与者 identity；撤销后创建新授权时不得复用旧 identity。RTC JWT 不能附带管理权限；管理 JWT 不能用于 RTC 入房。

SFU 在 `/rtc`、`/rtc/v1` 及其 `/validate` 入口、resume 与 Token 刷新时调用准入服务，使用 `Authorization: Bearer <AV_ADMISSION_SECRET>`。请求：

```json
{
  "node_id": "media-1",
  "node_incarnation": "NI_process-generation",
  "room": "immutable-room-name",
  "identity": "unique-grant-participant",
  "grant_id": "opaque-grant-id",
  "policy_version": 1,
  "binding_generation": 1
}
```

仅接受 HTTP 200 和显式响应：

```json
{"allowed":true,"policy_version":1,"binding_generation":1,"expires_at":1800000000}
```

两个版本必须与请求一致，`expires_at` 是 Unix 秒，须尚未到期且不得大于 JWT 中业务授权的绝对期限。网络/DNS/TLS、两秒整体超时、非 200、空或无效响应都拒绝。

服务端每 20 秒重新核验已连接参与者，并刷新最长 60 秒的 RTC JWT。刷新保持原 RTC 签发者与权限，不扩展业务授权期限。迟到回调仅能作用于同一参与者和授权版本。resume 必须匹配原 grant、权限版本和房间/节点代次；新 Token 不能覆盖服务端已收紧的权限。

## 条件媒体操作

AV 模式下普通 Twirp 的变更接口关闭；只保留受管理凭证保护的 `ListRooms`、`ListParticipants`、`GetParticipant` 查询。所有变更使用 `POST /internal/v1/media/operations`：

```json
{
  "operation_id": "stable-operation-id",
  "node_id": "media-1",
  "node_incarnation": "NI_process-generation",
  "room": "immutable-room-name",
  "binding_generation": 1,
  "target_policy_revision": 2,
  "method": "RemoveParticipant",
  "payload": {"room":"immutable-room-name","identity":"unique-grant-participant"}
}
```

支持 `CreateRoom`、`DeleteRoom`、`RemoveParticipant`、`UpdateParticipant`、`MutePublishedTrack`、`UpdateRoomMetadata`、`UpdateSubscriptions`；payload 沿用 RoomService protobuf JSON。

- Create/Delete 要求管理 JWT 的 `video.roomCreate=true`。
- 参与者操作、房间资料、ListParticipants/GetParticipant 要求 `video.roomAdmin=true` 且 `video.room` 等于目标房间。
- 房间操作以房间为目标，参与者操作以房间加 identity 为目标；同目标 revision 单调递增。相同 operation ID、revision 与完整请求重试返回原结果，旧 revision 或同 revision 不同内容返回 409。
- `UpdateParticipant` 的 revision 必须与新业务授权的 `policy_version` 一致。普通移除/清理操作可以使用控制面的递增操作序号。
- 删除房间永久封闭实际房间名；移除参与者永久封闭该 identity。新的房间绑定/授权使用新的名字/identity，防止迟到请求恢复旧权限。
- 每个节点最多保留 100,000 个房间生命周期记录，每个房间最多 100,000 个目标记录。达到上限返回明确 503，不清除仍能保护旧请求的记录；通过新节点代次及新房间绑定滚动更换。

成功响应包含 `operation_id`、`state: succeeded` 与 RoomService `result`。本地执行记录随节点进程存活；跨重启由节点代次和控制面的持久任务共同处理，不宣称 SFU 拥有持久业务数据库。

## 验证

```powershell
cmake --build out/windows-x64 --config Release --parallel 3
ctest --test-dir out/windows-x64 -C Release --output-on-failure
python tests/av_control_contract.py --binary out/windows-x64/Release/livekit-server.exe
python tests/av_control_contract.py --binary out/windows-x64/Release/livekit-server.exe --go ../out/tools/go/bin/go.exe --tls
node tests/av_signal_contract.mjs --binary out/windows-x64/Release/livekit-server.exe
```

这些检查使用原生测试、隔离 HTTP/TLS 准入服务和无窗口信令连接，不操作桌面、不使用实际采集设备。真实媒体容量、公网 TURN 和设备人工验收另行报告。

2026-11-06 自动化记录：Windows Release 全目标构建通过；CTest 12/12 通过；HTTP 与 HTTPS 准入/条件操作测试通过；原始信令的入房、刷新签发者/TTL、resume 版本隔离和周期撤权测试通过。新增原生文件通过 clang-format 20.1.8 检查，`git diff --check` 通过。此记录不覆盖真实设备和生产容量验收。
