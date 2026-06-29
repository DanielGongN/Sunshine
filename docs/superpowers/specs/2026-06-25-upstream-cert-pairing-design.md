# Design: 通过上游平台免 PIN 配对的证书流转

## 概述

通过中间平台 WebSocket 连接，将 Sunshine 服务端证书和 Moonlight 客户端证书进行免 PIN 交换和预授权。Sunshine 固定使用一组预定义端口，移除 Web UI 管理后台，使用固定用户凭证。客户端证书为一次性信任：断开即删，每次重连必须重新通过上游平台授权。

---

## 架构

```
                    固定端口 41200 (HTTPS)
                   + TLS 握手 + 流式连接
┌──────────────┐ ────────────────────────────> ┌──────────────┐
│  Moonlight   │                                │   Sunshine   │
│              │ <──────────────────────────── │              │
│ 客户端生成自   │   服务端 cert (用于 TLS 验证)     │ 生成 server cert
│ 签名 client   │                                │ → cakey.pem
│ cert         │                                │ → cacert.pem
└──────┬───────┘                                └──────┬───────┘
       │                                               │
       │ ① 上报 client cert                 ② 上报 server cert (PEM)
       │    请求 server cert + port                   + port 41200
       │                                               │
       └──────────────────>  ┌──────────┐  <───────────┘
                             │  上游平台  │
                             │ (WebSocket)│
                             └──────────┘
```

## 固定端口配置

基准端口: **41205**

| 端口 | 偏移 | 用途 | 协议 |
|------|------|------|------|
| 41200 | -5 | GameStream HTTPS (TLS + 配对) | TCP |
| 41205 | 0 | GameStream HTTP | TCP |
| 41214 | +9 | Video Stream | UDP |
| 41215 | +10 | Control | UDP |
| 41216 | +11 | Audio Stream | UDP |
| 41226 | +21 | RTSP Setup | TCP |

Web UI HTTPS（偏移 +1，原 47990）随 confighttp 移除而废弃。

## 数据流

### 方向1: Sunshine → 上游 → Moonlight

1. Sunshine 启动，`http::init()` 生成/加载 server cert (`cacert.pem` + `cakey.pem`)
2. `nvhttp::start()` 加载 cert 到内存
3. Middleware WebSocket 连接上游成功后，发送 `stream_engine_info`:
   ```json
   {
     "event": "stream_engine_info",
     "data": {
       "cert": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
       "port": 41200
     }
   }
   ```
4. 上游按需转发给 Moonlight
5. Moonlight 用该 cert 完成 TLS 握手 + 直连 Sunshine

### 方向2: Moonlight → 上游 → Sunshine

1. Moonlight 生成自签名 client certificate
2. Moonlight 将 client cert 上报给上游平台
3. 上游通过 WebSocket 发送给 Sunshine:
   ```json
   {
     "event": "client_connect",
     "data": {
       "uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
       "cert": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----"
     }
   }
   ```
4. Sunshine middleware 收到后调用 `nvhttp::add_trusted_client(uuid, cert)`
5. 证书加入内存中的 `cert_chain_t`，不持久化到磁盘
6. Moonlight 直连 Sunshine:41200，TLS 出示 client cert → 匹配信任列表 → 免 PIN 通过

### 方向3: 客户端断开

Sunshine 有两种方式触发证书清理：

**自动检测（流断开）:**
1. Sunshine 已有 ping timeout 机制（`config::stream.ping_timeout`，默认 10s），网络不好或 Moonlight 关闭时自动超时
2. `session::stop()` → 状态变为 `STOPPING` → session 从列表擦除
3. 在 session 清理路径中 hook，读取 `session->client_cert`，调用 `nvhttp::remove_trusted_client_by_cert(cert)`
4. 证书从内存中移除，无需上游介入

**上游主动断开:**
1. 上游平台主动发送断开事件:
   ```json
   {
     "event": "client_disconnect",
     "data": {
       "uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
     }
   }
   ```
2. Sunshine middleware 调用 `nvhttp::remove_trusted_client(uuid)`
3. 证书从内存中移除，该客户端后续无法直连 Sunshine

---

## 组件改动

### 1. `config.cpp` / `config.h`

- `sunshine.port` 默认值改为 `41205`
- 移除 confighttp 相关配置项（或保留不启动）
- 固定用户凭证: 用户名 `wusuan_admin`，密码 `2BTtaMdVvb2`

### 2. `httpcommon.cpp`

- 固定凭证逻辑: 用户凭证使用硬编码值 `wusuan_admin:2BTtaMdVvb2`
- 跳过 web 管理后台相关的凭证初始化（`confighttp` 不再需要）

### 3. `middleware.cpp`

新增 WebSocket 事件:

- **`stream_engine_info` 发送**: Middleware 连接上游成功后，发送 server cert PEM + 端口 41200。
  - cert 读取自 `cacert.pem` 文件
  - 发送时机: `nvhttp::start()` 完成后触发（详见互斥与通知机制）

- **`client_connect` 接收**: 解析 `data.uuid`、`data.cert`，调用 `nvhttp::add_trusted_client(uuid, cert)`。

- **`client_disconnect` 接收**: 解析 `data.uuid`，调用 `nvhttp::remove_trusted_client(uuid)`。

### 4. `nvhttp.h` / `nvhttp.cpp`

新增接口:

```cpp
// 一次性信任: 添加客户端证书到内存中的 cert_chain，不持久化
void add_trusted_client(std::string uuid, std::string cert);

// 移除客户端证书 by uuid（上游主动断开时调用）
void remove_trusted_client(std::string_view uuid);

// 移除客户端证书 by cert PEM（stream 自动检测断开时调用）
void remove_trusted_client_by_cert(std::string_view cert);
```

实现要点:
- `add_trusted_client`: 构造 `named_cert_t {uuid, cert, enabled=true}`，加入 `client_root.named_devices` 和 `cert_chain_t`，**不调用 `save_state()`**
- `remove_trusted_client`: 从 `named_devices` 移除指定 uuid，重建 `cert_chain_t`
- `remove_trusted_client_by_cert`: 从 `named_devices` 移除匹配 cert PEM 的条目，重建 `cert_chain_t`。用于 stream session 清理 hook（session 已有 `client_cert` 字段）
- Sunshine 重启后所有内存中的证书自动清除
- 使用 `std::mutex` 保护跨线程访问

禁用 PIN 配对:
- 当 middleware 启用时，`getservercert` 端点返回 403:
  ```json
  {"root": {"paired": 0, "<xmlattr>.status_code": 403}}
  ```
- 其余配对阶段端点 (`clientchallenge`, `serverchallengeresp`, `clientpairingsecret`) 同样拒绝

### 5. `stream.cpp`

在 session 清理路径（ping timeout / RTSP TEARDOWN → `session::stop()` → `STOPPING` 状态擦除）中，新增 hook:

- 读取 `session->client_cert`（该字段已存在）
- 调用 `nvhttp::remove_trusted_client_by_cert(session->client_cert)` 清理内存中的证书

### 6. `main.cpp`

- 取消 middleware 启动代码注释
- 移除/注释 `confighttp::start` 线程（Web UI 不再需要）
- 启动顺序:
  1. `http::init()` — 生成/加载 cert 到 disk + 固定凭证
  2. `nvhttp::start()` — 加载 cert 到内存，启动 HTTPS server
  3. middleware 连接上游后发送 `stream_engine_info`

### 7. 互斥与通知机制

Middleware 发送 `stream_engine_info` 需要满足两个条件:
1. WebSocket 连接成功
2. `nvhttp::start()` 中 HTTPS server 已启动就绪

方案: 使用 `safe::event_t` 通知机制:
- `nvhttp::start()` 启动完成后 set 事件
- middleware `connect_and_subscribe()` 成功后等待该事件，然后发送 `stream_engine_info`

---

## 实现注意事项

- **内存泄漏**: 所有涉及 OpenSSL 对象（`X509`、`EVP_PKEY`、`BIO`）、`std::unique_ptr`、`std::shared_ptr` 的代码必须严格检查生命周期。新增的 `cert_chain_t` 操作（添加/重建）、WebSocket 消息的 JSON 对象、session 清理 hook 中的字符串拷贝等都需要确保异常安全，使用 RAII 包装（项目已有 `util::safe_ptr` 模式）。
- **证书内存管理**: `named_devices` 重建 `cert_chain_t` 时需先 `clear()` 再重新 `add()`，避免残留已删除证书的 `x509_store_t`。

## 安全考量

- **一次性证书**: 客户端证书仅在内存中，Sunshine 重启后自动清除，防止残留授权
- **固定凭证**: 用户名密码硬编码在二进制中，仅用于内部通信，不暴露到网络
- **证书验证**: Moonlight 直连时必须出示已预授权的 client cert，TLS 层验证通过才允许流式连接
- **无 PIN 攻击面**: PIN 配对端点被禁用，减少暴力破解风险
- **WebSocket 安全**: 假设上游平台为可信基础设施，通信在受控网络内

---

## 错误处理

- Middleware 连接失败: Sunshine 继续运行但无法获取客户端预授权（日志记录 warning）
- `client_connect` 收到无效 cert PEM: 日志记录 error，拒绝添加
- `client_disconnect` 收到未知 uuid: 日志记录 warning，忽略
- `stream_engine_info` 发送失败: 重试逻辑放在 `connect_and_subscribe()` 的重连循环中
- 上游断线重连: `connect_and_subscribe()` 已有 while 循环重连机制，重连后重新发送 `stream_engine_info`。断线期间内存中的客户端证书保持不变，不影响正在流式连接的 Moonlight
