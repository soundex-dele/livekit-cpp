# coturn 会话凭证

服务端可为每次认证入房签发 coturn REST 凭证。ICE、TURN 连接、DTLS/SRTP 和
媒体传输由 libwebrtc 处理；应用仅生成 coturn 规定的用户名及认证密码。
`rtc.turn_rest` 默认关闭，已有 `rtc.ice_servers` 静态配置保持兼容。

## 服务端配置

参考 [config.turn.example.json](../config.turn.example.json)：

```json
{
  "rtc": {
    "turn_rest": {
      "urls": [
        "turn:turn.example.com:3478?transport=udp",
        "turn:turn.example.com:3478?transport=tcp"
      ],
      "shared_secret_env": "LIVEKIT_TURN_SECRET",
      "credential_ttl_seconds": 86400
    }
  }
}
```

这是完整配置中 `rtc` 的片段；API keys、绑定地址和端口仍按项目配置提供。
将服务进程环境变量 `LIVEKIT_TURN_SECRET` 设为与 coturn 一致的随机共享密钥。
也支持 `shared_secret` 文件内配置，两种方式必须且只能选择一种。环境变量在
启动时读取，未设置、空值或长度不在 32–1024 字节范围内会报错；错误不回显密钥。
共享密钥应与 API JWT 签名密钥分开保存。

`urls` 可含 1–8 个 `turn:` / `turns:` 地址，详细格式和 transport 参数由
libwebrtc 原生解析器校验。TLS 地址需要 coturn 正确配置证书；当前真实验收覆盖
UDP/TCP，尚未声明 TURN/TLS 通过。`credential_ttl_seconds` 默认 86400，可设
60–604800 秒；非整数和越界值在启动时拒绝。

用户名为 `<Unix 到期秒>:<随机 Participant SID>`，不使用用户的 identity、
房间名称或 JWT。密码是标准 Base64 编码的 HMAC-SHA1；这是 coturn REST 认证
要求的算法，不改变 WebRTC 的加密。每次重新入房会产生新 SID 和新凭证，同一
会话的两条原生连接与客户端获得相同凭证。共享密钥不进入 JoinResponse。

## coturn 对应配置

以下是需要与服务端保持一致的认证部分，替换占位值后并入运营方的 coturn
配置；监听地址、外部地址映射、TLS 证书、relay 端口、防火墙与容量另行配置：

```ini
realm=rtc.example.com
fingerprint
use-auth-secret
static-auth-secret=REPLACE_WITH_THE_SAME_RANDOM_SECRET
no-cli
no-multicast-peers
```

不要同时配置测试驱动的固定 `user=` 账号。允许的 peer 地址应限制为实际 SFU
网段，按并发预算设置 `user-quota`、`total-quota` 和带宽限制。TURN 的客户端
TCP 接入与 TURN 到 SFU 的 UDP relay 是两段链路，需要分别放通相应端口。
测试工具为了本机验证开放 loopback peer，不能将其配置直接用于公网。

## 生命周期与当前边界

当前在入房/完整重连时签发凭证，**没有在线推送刷新活跃会话的 TURN 凭证**。
应按最长会话及恢复窗口选择 TTL；过期后新建 allocation 需要重新入房获得新
凭证。现有 allocation 的跨到期 refresh 行为和长时间通话仍需独立验收。
踢人会关闭 RTC 会话，但不会立刻吊销已经签发、尚未过期的 coturn 凭证。

共享密钥从配置加载，当前没有热更新或多密钥协调接口。更换 coturn 密钥与
服务端密钥需要统一安排，并考虑旧凭证的有效窗口；这不构成无损轮换支持声明。
项目隔离、配额、即时撤销、凭证刷新和完整生产部署仍在实施清单内。

## 真实验证

Linux 环境运行以下命令，其他依赖和证据目录见
[NETWORK_VALIDATION.md](NETWORK_VALIDATION.md)：

```bash
python3 tests/turn_integration.py \
  --server /path/to/livekit-server \
  --turnserver /path/to/turnserver \
  --node /path/to/node --chrome /path/to/chrome \
  --rest-auth --outage
```

该模式使用独立 coturn REST 密钥，服务端通过环境变量读取。测试在 Node 端
独立核对签名、到期时间和每次入房的唯一性，确认实际 relay 路由和完整媒体，
再用浏览器原生 PeerConnection 检查错误密码及过期凭证不能得到 relay 候选。
证据只保存认证核对结果，不输出密码或共享密钥。

最近一次完整通过记录为 `tests/results/turn-rest/20261023-224659/`：UDP 12 组、
TCP 11 组，错误密码和过期凭证均返回 401；共享密钥日志扫描通过。此前两次
运行仍有未解决的 simulcast 媒体失败，详见 [VALIDATION.md](../VALIDATION.md)。
这些测试不构成稳定性、跨到期长通话或公网部署的验收。
