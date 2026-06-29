# 上游平台免 PIN 证书配对 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 通过上游 WebSocket 平台实现 Sunshine 与 Moonlight 的免 PIN 证书预授权，固定端口 41200+，移除 Web UI，使用硬编码凭证。

**Architecture:** 扩展现有 middleware WebSocket 客户端和 nvhttp GameStream server。新增 `stream_engine_info`/`client_connect`/`client_disconnect` 三个 WebSocket 事件实现证书流转；新增 `add_trusted_client`/`remove_trusted_client` 接口管理内存中的客户端证书；利用现有 session 清理路径自动撤销断线客户端的证书。

**Tech Stack:** C++17, OpenSSL, Boost.Asio/Beast, nlohmann::json, Simple-Web-Server

## Global Constraints

- 基准端口: `41205`（所有端口偏移保持不变: `-5, 0, +9, +10, +11, +21`）
- 固定凭证: 用户名 `wusuan_admin`，密码 `2BTtaMdVvb2`
- 客户端证书仅存内存，不持久化到 `sunshine_state.json`
- PIN 配对端点禁用（返回 403）
- confighttp Web UI 移除
- 所有 OpenSSL 对象必须使用 `util::safe_ptr` RAII 包装
- 跨线程访问 `client_root`/`cert_chain` 使用 `std::mutex` 保护

---

### Task 1: 固定端口与固定用户凭证

**Files:**
- Modify: `src/config.cpp:598` 行附近

**Interfaces:**
- Produces: `config::sunshine.port = 41205`，`config::sunshine.username = "wusuan_admin"`，`config::sunshine.password = hashed("2BTtaMdVvb2")`

- [ ] **Step 1: 修改基准端口默认值**

在 `src/config.cpp` 的 `sunshine_t` 初始化中，将端口从 `47989` 改为 `41205`：

```cpp
// 第 598 行附近
sunshine_t sunshine {
    "en",  // locale
    2,  // min_log_level
    0,  // flags
    {},  // User file
    {},  // Username
    {},  // Password
    {},  // Password Salt
    platf::appdata().string() + "/sunshine.conf",  // config file
    {},  // cmd args
    41205,  // Base port number (was 47989)
    "ipv4",  // Address family
    // ... 后续不变
};
```

- [ ] **Step 2: 固定用户凭证**

同文件同结构体，直接填入用户名和预计算的密码哈希：

生成固定 salt + password hash:
```
salt = crypto::rand_alphabet(16)  // 但我们需要固定值，用 "wusuan_salt_16ch"
password_hash = hex(sha256("2BTtaMdVvb2" + "wusuan_salt_16ch"))
```

但 `http::init()` 中会调用 `user_creds_exist()` 检查 credential 文件，然后调用 `reload_user_creds()`。我们需要绕过这个流程。

更简洁的做法：在 `config.cpp` 的 `sunshine_t` 初始化中直接设置固定凭证：

```cpp
sunshine_t sunshine {
    "en",
    2,
    0,
    {},
    "wusuan_admin",      // Username — 固定
    util::hex(crypto::hash("2BTtaMdVvb2" + std::string("wusuan_salt_16ch"))).to_string(),  // Password hash
    "wusuan_salt_16ch",  // Password Salt — 固定
    // ...
};
```

但 `crypto::hash()` 在构造时还没初始化就不安全。改用 `httpcommon.cpp` 中设置。

- [ ] **Step 3: 在 httpcommon.cpp 的 `init()` 中强制设置固定凭证**

在 `src/httpcommon.cpp` 的 `init()` 函数第 61 行附近，将原来的 credential 检查逻辑改为直接设置固定值：

```cpp
// 原代码（第 57-65 行）:
// if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) && ...
// if (!user_creds_exist(config::sunshine.credentials_file)) { ... }

// 替换为:
// Generate server cert if needed
if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) &&
    create_creds(config::nvhttp.pkey, config::nvhttp.cert)) {
  return -1;
}

// Fixed credentials — no Web UI, no credential file needed
const std::string fixed_salt = "wusuan_salt_16ch";
config::sunshine.username = "wusuan_admin";
config::sunshine.salt = fixed_salt;
config::sunshine.password = util::hex(crypto::hash(std::string("2BTtaMdVvb2") + fixed_salt)).to_string();
```

删除 `credentials_file` 和 `user_creds_exist`/`reload_user_creds` 的调用。

- [ ] **Step 4: 提交**

```bash
git add src/config.cpp src/httpcommon.cpp
git commit -m "feat(config): set fixed port 41205 and hardcoded credentials"
```

---

### Task 2: nvhttp 新增受信任客户端管理 API + 禁用 PIN 配对

**Files:**
- Modify: `src/nvhttp.h:63` 行后，`src/nvhttp.cpp:48` 行附近

**Interfaces:**
- Produces:
  - `void nvhttp::add_trusted_client(std::string uuid, std::string cert)` — 添加证书到内存 `cert_chain`
  - `void nvhttp::remove_trusted_client(std::string_view uuid)` — 按 uuid 移除
  - `void nvhttp::remove_trusted_client_by_cert(std::string_view cert)` — 按 cert PEM 移除

- [ ] **Step 1: 在 nvhttp.cpp 匿名命名空间中添加 mutex 和辅助函数**

在 `src/nvhttp.cpp` 的匿名命名空间（第 48 行 `crypto::cert_chain_t cert_chain;` 之后）添加：

```cpp
// Mutex to protect client_root and cert_chain across threads (nvhttp + middleware)
std::mutex client_mutex;

// Rebuild cert_chain from client_root.named_devices
// Caller must hold client_mutex
void rebuild_cert_chain() {
  cert_chain.clear();
  for (auto &named_cert : client_root.named_devices) {
    if (named_cert.enabled) {
      auto x509 = crypto::x509(named_cert.cert);
      if (x509) {
        cert_chain.add(std::move(x509));
      }
    }
  }
}
```

- [ ] **Step 2: 实现 add_trusted_client**

在 `add_authorized_client` 函数附近（第 288 行后）添加新函数：

```cpp
void add_trusted_client(std::string uuid, std::string cert) {
  std::lock_guard lg {client_mutex};

  // Validate the cert PEM before adding
  auto x509 = crypto::x509(cert);
  if (!x509) {
    BOOST_LOG(error) << "add_trusted_client: invalid cert PEM for uuid "sv << uuid;
    return;
  }

  // Check for duplicate uuid
  for (auto &dev : client_root.named_devices) {
    if (dev.uuid == uuid) {
      BOOST_LOG(warning) << "add_trusted_client: uuid "sv << uuid << " already exists, replacing cert"sv;
      dev.cert = cert;
      rebuild_cert_chain();
      return;
    }
  }

  named_cert_t named_cert;
  named_cert.name = uuid;  // use uuid as name since we don't get name from upstream
  named_cert.uuid = std::move(uuid);
  named_cert.cert = std::move(cert);
  named_cert.enabled = true;

  cert_chain.add(std::move(x509));
  client_root.named_devices.emplace_back(std::move(named_cert));

  // Do NOT call save_state() — certificates are memory-only
  BOOST_LOG(info) << "Trusted client added: "sv << named_cert.uuid;
}
```

- [ ] **Step 3: 实现 remove_trusted_client**

```cpp
void remove_trusted_client(std::string_view uuid) {
  std::lock_guard lg {client_mutex};

  auto &devices = client_root.named_devices;
  auto it = std::find_if(devices.begin(), devices.end(), [&uuid](const named_cert_t &d) {
    return d.uuid == uuid;
  });

  if (it == devices.end()) {
    BOOST_LOG(warning) << "remove_trusted_client: uuid "sv << uuid << " not found"sv;
    return;
  }

  BOOST_LOG(info) << "Trusted client removed: "sv << it->uuid;
  devices.erase(it);
  rebuild_cert_chain();
}
```

- [ ] **Step 4: 实现 remove_trusted_client_by_cert**

```cpp
void remove_trusted_client_by_cert(std::string_view cert) {
  if (cert.empty()) {
    return;
  }

  std::lock_guard lg {client_mutex};

  auto &devices = client_root.named_devices;
  auto it = std::find_if(devices.begin(), devices.end(), [&cert](const named_cert_t &d) {
    return d.cert == cert;
  });

  if (it == devices.end()) {
    BOOST_LOG(debug) << "remove_trusted_client_by_cert: cert not found (may already be removed)"sv;
    return;
  }

  BOOST_LOG(info) << "Trusted client removed by cert (stream ended): "sv << it->uuid;
  devices.erase(it);
  rebuild_cert_chain();
}
```

- [ ] **Step 5: 在 nvhttp.h 中声明新接口**

在 `src/nvhttp.h` 的 `erase_all_clients()` 声明之后添加：

```cpp
/**
 * @brief Add a trusted client certificate (memory-only, not persisted).
 * @param uuid The unique identifier for the client.
 * @param cert The PEM-encoded X.509 certificate.
 */
void add_trusted_client(std::string uuid, std::string cert);

/**
 * @brief Remove a trusted client certificate by uuid.
 * @param uuid The unique identifier for the client.
 */
void remove_trusted_client(std::string_view uuid);

/**
 * @brief Remove a trusted client certificate by cert PEM.
 *        Used by stream cleanup hook when a client disconnects.
 * @param cert The PEM-encoded X.509 certificate.
 */
void remove_trusted_client_by_cert(std::string_view cert);
```

- [ ] **Step 6: 禁用 PIN 配对端点**

在 `src/nvhttp.cpp` 的 `getservercert` 函数开头（第 358 行附近），当 middleware 启用时直接返回 403：

```cpp
void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
  if (sess.last_phase != PAIR_PHASE::NONE) {
    fail_pair(sess, tree, "Out of order call to getservercert");
    return;
  }

  // PIN pairing disabled when middleware is active
  if (config::sunshine.middleware.enabled) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 403);
    BOOST_LOG(warning) << "PIN pairing disabled: use upstream platform for client authorization"sv;
    return;
  }

  // ... 原有逻辑保持不变 ...
}
```

对其他配对阶段函数 (`clientchallenge`, `serverchallengeresp`, `clientpairingsecret`) 同样处理或在调用链上天然被阻隔（因为 `getservercert` 是入口）。

- [ ] **Step 7: 提交**

```bash
git add src/nvhttp.h src/nvhttp.cpp
git commit -m "feat(nvhttp): add trusted client management API, disable PIN pairing"
```

---

### Task 3: 添加 nvhttp_ready 通知事件

**Files:**
- Modify: `src/globals.h`（添加 mail 常量）
- Modify: `src/nvhttp.cpp`（服务器启动后 raise 事件）

**Interfaces:**
- Produces: `mail::nvhttp_ready` — `safe::event_t<bool>` 供 middleware 等待

- [ ] **Step 1: 在 globals.h 中注册新 mail 事件**

在 `src/globals.h` 第 57 行（`MAIL(hdr);` 之后）添加：

```cpp
MAIL(nvhttp_ready);
```

- [ ] **Step 2: 在 nvhttp::start() 中触发事件**

在 `src/nvhttp.cpp` 的 `start()` 函数中，HTTPS server 成功启动后 raise 事件。找到服务器 `start()` 调用（约第 1208 行），在之后添加：

```cpp
auto accept_and_run = [&](auto *http_server) {
  try {
    std::string name = "nvhttp::" + std::to_string(http_server->config.port);
    platf::set_thread_name(name);
    http_server->start();
  } catch (std::exception &e) {
    BOOST_LOG(fatal) << "nvhttp server failed to start: "sv << e.what();
  }
};

// ... 两个 accept_and_run 调用之后 ...

// Signal middleware that nvhttp is ready
auto nvhttp_ready = mail::man->event<bool>(mail::nvhttp_ready);
nvhttp_ready->raise(true);
BOOST_LOG(info) << "nvhttp ready, middleware can now send stream_engine_info"sv;
```

- [ ] **Step 3: 提交**

```bash
git add src/globals.h src/nvhttp.cpp
git commit -m "feat(mail): add nvhttp_ready event for middleware coordination"
```

---

### Task 4: Stream session 清理 hook — 自动撤销证书

**Files:**
- Modify: `src/stream.cpp:1094-1107` 行附近

**Interfaces:**
- Consumes: `nvhttp::remove_trusted_client_by_cert(std::string_view)`
- Consumes: `session->client_cert`（已存在字段）

- [ ] **Step 1: 在 session 清理路径中添加证书撤销调用**

在 `src/stream.cpp` 的 session iterate 循环中，找到 `STOPPING` 状态处理块（约第 1094 行）。在 session 被 erase 之前，读取 `client_cert` 并调用 nvhttp 清理：

```cpp
if (session->state.load(std::memory_order_acquire) == session::state_e::STOPPING) {
  // Revoke trusted client cert when stream ends
  // This handles both ping timeout (network issues) and normal disconnect
  if (!session->client_cert.empty()) {
    nvhttp::remove_trusted_client_by_cert(session->client_cert);
  }

  pos = server->_sessions->erase(pos);

  if (session->control.peer) {
    {
      auto ptslg = server->_peer_to_session.lock();
      server->_peer_to_session->erase(session->control.peer);
    }

    enet_peer_disconnect_now(session->control.peer, 0);
  }

  session->controlEnd.raise(true);
  continue;
}
```

- [ ] **Step 2: 确保 stream.cpp 包含 nvhttp.h**

在 `src/stream.cpp` 头部检查是否有 `#include "nvhttp.h"`，如无则添加：

```cpp
#include "nvhttp.h"
```

- [ ] **Step 3: 提交**

```bash
git add src/stream.cpp
git commit -m "feat(stream): auto-revoke client cert on stream disconnect"
```

---

### Task 5: Middleware 事件 — stream_engine_info / client_connect / client_disconnect

**Files:**
- Modify: `src/middleware.cpp`

**Interfaces:**
- Consumes: `mail::nvhttp_ready`（等待事件）
- Consumes: `nvhttp::add_trusted_client(uuid, cert)`，`nvhttp::remove_trusted_client(uuid)`
- Consumes: `file_handler::read_file(config::nvhttp.cert)`（读取 cacert.pem）
- Produces: WebSocket 消息 `stream_engine_info`（发送），`client_connect`/`client_disconnect`（处理）

- [ ] **Step 1: 添加发送 stream_engine_info 的成员函数**

在 `middleware_t` 类的 `private` 部分，`on_message_event` 之后添加：

```cpp
void send_stream_engine_info() {
  // Read server cert from disk
  auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
  if (cert.empty()) {
    BOOST_LOG(error) << "Failed to read server cert for stream_engine_info"sv;
    return;
  }

  json msg;
  msg["message_id"] = make_timestamp();
  msg["event"] = "stream_engine_info";
  json data;
  data["cert"] = cert;
  data["port"] = 41200;
  msg["data"] = data;

  std::string payload = msg.dump();
  beast::error_code ec;
  ws.write(net::buffer(payload), ec);
  if (ec) {
    BOOST_LOG(warning) << "Failed to send stream_engine_info: "sv << ec.message();
  } else {
    BOOST_LOG(info) << "Sent stream_engine_info to upstream"sv;
  }
}
```

需要添加 include: `#include "file_handler.h"` 和 `#include "nvhttp.h"`。

- [ ] **Step 2: 在 on_message_event 中添加 client_connect 处理**

在 `on_message_event` 的 if-else 链中（`disconnected` 分支之后，`else` 之前）添加：

```cpp
} else if (event_type == "client_connect") {
  auto &data = msg["data"];
  std::string uuid = data.value("uuid", "");
  std::string cert = data.value("cert", "");
  if (uuid.empty() || cert.empty()) {
    BOOST_LOG(warning) << "client_connect: missing uuid or cert"sv;
  } else {
    nvhttp::add_trusted_client(uuid, cert);
  }
} else if (event_type == "client_disconnect") {
  std::string uuid = msg.value("data", json::object()).value("uuid", "");
  if (uuid.empty()) {
    BOOST_LOG(warning) << "client_disconnect: missing uuid"sv;
  } else {
    nvhttp::remove_trusted_client(uuid);
  }
```

- [ ] **Step 3: 在 connect_and_subscribe 中等待 nvhttp_ready 并发送 stream_engine_info**

在 `connect_and_subscribe` 函数的 `subscribe_all_events()` 之后、`read_loop()` 之前，插入：

```cpp
// Subscribe to events
subscribe_all_events();

// Wait for nvhttp HTTPS server to be ready before sending stream_engine_info
auto nvhttp_ready = mail::man->event<bool>(mail::nvhttp_ready);
if (!nvhttp_ready->peek()) {
  BOOST_LOG(info) << "Waiting for nvhttp server to be ready..."sv;
  nvhttp_ready->view();  // block until nvhttp::start() raises the event
}
send_stream_engine_info();

// Enter read loop
read_loop();
```

- [ ] **Step 4: 添加必要的 include**

在 `middleware.cpp` 头部已有 include 之后添加：

```cpp
#include "file_handler.h"
#include "nvhttp.h"
```

- [ ] **Step 5: 更新订阅事件列表（可选，暂不需要订阅新事件）**

当前 `SUBSCRIBED_EVENTS` 数组中 `client_connect` 和 `client_disconnect` 是从上游发来的事件，不需要 Sunshine 主动订阅。保留现有列表不变。

- [ ] **Step 6: 提交**

```bash
git add src/middleware.cpp
git commit -m "feat(middleware): add stream_engine_info, client_connect, client_disconnect events"
```

---

### Task 6: main.cpp 启动顺序调整

**Files:**
- Modify: `src/main.cpp:387-413` 行附近

**Interfaces:**
- Consumes: `middleware::start()`，`nvhttp::start`，`confighttp::start`（移除）

- [ ] **Step 1: 启用 middleware 启动代码**

在 `src/main.cpp` 第 387-394 行，取消 middleware 代码注释并启用：

```cpp
// 启动中间平台 WebSocket 连接
std::unique_ptr<platf::deinit_t> middleware_deinit_guard;
if (config::sunshine.middleware.enabled) {
  middleware_deinit_guard = middleware::start();
  if (!middleware_deinit_guard) {
    BOOST_LOG(error) << "Middleware failed to initialize"sv;
  }
}
```

- [ ] **Step 2: 移除 confighttp 启动**

将第 412 行的 `std::thread configThread {confighttp::start};` 注释掉：

```cpp
std::thread httpThread {nvhttp::start};
// std::thread configThread {confighttp::start};  // Web UI removed
std::thread rtspThread {rtsp_stream::start};
```

- [ ] **Step 3: 更新 thread join**

第 440 行，移除 `configThread.join()`:

```cpp
httpThread.join();
// configThread.join();  // removed
rtspThread.join();
```

- [ ] **Step 4: 确认启动顺序满足 spec 要求**

检查顺序:
1. `http::init()` — 第 376 行，生成/加载 cert + 设置固定凭证 ✓
2. middleware 启动 — 第 389 行，在新位置 ✓
3. `nvhttp::start()` — 第 411 行，加载 cert + 启动 HTTPS server ✓
4. Middleware 连接后等待 nvhttp_ready → 发送 stream_engine_info ✓

注意：middleware 在 `nvhttp::start()` 之前启动是 OK 的，因为 middleware 在 `connect_and_subscribe()` 中会等待 `nvhttp_ready` 事件。但有一个竞态：如果 middleware 连接非常快，`nvhttp_ready` 事件还没被创建（`mail::man->event<bool>(mail::nvhttp_ready)` 在 `nvhttp::start()` 中调用）。

修复方法：在 `main.cpp` 中，middleware 启动之前，先创建 `nvhttp_ready` 事件：

```cpp
// Pre-create the nvhttp_ready event before starting middleware or nvhttp
mail::man->event<bool>(mail::nvhttp_ready);

// 启动中间平台 WebSocket 连接
std::unique_ptr<platf::deinit_t> middleware_deinit_guard;
if (config::sunshine.middleware.enabled) {
  middleware_deinit_guard = middleware::start();
  // ...
}
```

这样 middleware 中的 `mail::man->event<bool>(mail::nvhttp_ready)` 会获取已存在的事件，而不会创建新的。

- [ ] **Step 5: 提交**

```bash
git add src/main.cpp
git commit -m "feat(main): enable middleware, remove confighttp, fix startup ordering"
```

---

### Task 7: 编译验证 + 整体检查

**Files:**
- 无新增文件

- [ ] **Step 1: 编译 Sunshine**

```bash
cd E:\work\c++\Sunshine && cmake --build build --config Release
```

- [ ] **Step 2: 检查编译错误**

修复所有编译错误和警告。预期可能的错误：
- `file_handler` include 路径
- `mail::nvhttp_ready` 未在 `globals.h` 中声明
- `config::sunshine.middleware.enabled` 访问
- `#include "nvhttp.h"` 在 middleware.cpp 中的循环依赖

如果出现 `nvhttp.h` 与 `middleware.h` 循环依赖，改用前向声明或在 middleware.cpp 中延迟 include。

- [ ] **Step 3: 内存泄漏检查（manual review）**

逐文件检查：
- `add_trusted_client`: `crypto::x509(cert)` 返回 `util::safe_ptr`，`cert_chain.add(std::move(x509))` 正确转移所有权 ✓
- `rebuild_cert_chain`: 先 `cert_chain.clear()` 再 add，无泄漏 ✓
- `send_stream_engine_info`: `file_handler::read_file` 返回 `std::string`，无动态分配 ✓
- WebSocket `ws.write(net::buffer(payload))`: payload 是栈上 `std::string`，生命周期覆盖 write 调用 ✓
- `client_mutex`: `std::lock_guard` RAII，自动释放 ✓

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "chore: compilation fixes and memory leak review"
```

---

## Self-Review

### Spec coverage check:

| Spec 需求 | 实现 Task |
|-----------|----------|
| 端口改为 41205 | Task 1 |
| 固定凭证 wusuan_admin / 2BTtaMdVvb2 | Task 1 |
| 移除 confighttp Web UI | Task 6 |
| stream_engine_info 发送 (cert + port) | Task 5 |
| client_connect 接收 (uuid + cert) | Task 5 |
| client_disconnect 接收 (uuid) | Task 5 |
| add_trusted_client API (内存, 不持久化) | Task 2 |
| remove_trusted_client API | Task 2 |
| remove_trusted_client_by_cert API | Task 2 |
| std::mutex 线程安全 | Task 2 |
| 禁用 PIN 配对 (getservercert 403) | Task 2 |
| Stream session 清理 hook 自动撤销证书 | Task 4 |
| 互斥与通知机制 (nvhttp_ready) | Task 3, 5, 6 |
| 启动顺序: http::init → nvhttp::start → middleware send | Task 6 |
| 内存泄漏防范 | Task 2, 7 |
