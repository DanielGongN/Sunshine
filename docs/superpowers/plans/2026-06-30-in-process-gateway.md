# In-Process Gateway Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate SteamGateway port-multiplexing into ws_engine.exe so port 41200 is the single external port for all traffic (TLS + gateway-framed TCP/UDP), eliminating the standalone Go proxy.

**Architecture:** A boost::asio gateway listens on 0.0.0.0:41200 TCP+UDP. TCP connections peek the first byte: 0x16 (TLS) → raw tunnel to nvhttp on 127.0.0.1:41300; 0x01/0x05 → framed gateway TCP proxy to internal ports. UDP datagrams carry a 3-byte header [stream_id:1B][length:2B BE] before payload; the gateway strips/adds headers between client and internal UDP ports.

**Tech Stack:** C++23, boost::asio (already in project), Ninja build system

## Global Constraints

- Single port 41200 for all external traffic (TCP + UDP)
- nvhttp HTTPS server moves to 127.0.0.1:41300 (internal only)
- Gateway protocol header: [stream_id:1B][length:2B big-endian][payload:length bytes]
- TLS passthrough: first byte 0x16 → raw bidirectional TCP tunnel
- Existing streaming ports (41214/41215/41216/41226) unchanged but bound to 127.0.0.1 only
- `BOOST_USE_STATIC=ON`, `SUNSHINE_ENABLE_TRAY=OFF`, `SUNSHINE_BUILD_ASSETS=OFF`

---

### Task 1: Add internal HTTPS port constant and move nvhttp to 127.0.0.1

**Files:**
- Modify: `src/nvhttp.h:40-46` (PORT_HTTPS comment, internal port)
- Modify: `src/nvhttp.cpp:1319-1321` (bind address)
- Modify: `src/config.cpp:1329` (port validation range)

**Interfaces:**
- Produces: `nvhttp::PORT_HTTPS = 0` (stays, 41200), new `GATEWAY_LISTEN_PORT` offset
- Produces: nvhttp binds to 127.0.0.1 on port `config::sunshine.port + 100` (41300)
- Produces: port validation range accepts internal HTTPS port (+100)

- [ ] **Step 1: Add internal HTTPS port constant to nvhttp.h**

In `src/nvhttp.h`, add the internal port offset:

```cpp
  /**
   * @brief The HTTPS port, as a difference from the config port.
   *        With base=41200, final port = 41200 + 0 = 41200 (gateway listens here).
   */
  constexpr auto PORT_HTTPS = 0;

  /**
   * @brief The HTTP port (disabled in middleware-only mode).
   */
  constexpr auto PORT_HTTP = 5;

  /**
   * @brief Internal HTTPS port offset for gateway tunneling.
   *        nvhttp binds to 127.0.0.1:base+100 (41300), gateway forwards TLS here.
   */
  constexpr auto PORT_HTTPS_INTERNAL = 100;
```

- [ ] **Step 2: Change nvhttp HTTPS bind address to 127.0.0.1 and use internal port**

In `src/nvhttp.cpp`, change the https_server config:

```cpp
    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto port_https_internal = (std::uint16_t)(config::sunshine.port + PORT_HTTPS_INTERNAL);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    // ... later:
    https_server.config.reuse_address = true;
    https_server.config.address = "127.0.0.1";
    https_server.config.port = port_https_internal;
```

- [ ] **Step 3: Update port validation range in config.cpp**

In `src/config.cpp`, update the validation to allow the internal HTTPS port:

```cpp
    int port = sunshine.port;
    // Allow range from base+1024 to 65535, ensuring internal HTTPS (base+100) fits
    int_between_f(vars, "port"s, port, {1024, 65535 - rtsp_stream::RTSP_SETUP_PORT});
    sunshine.port = (std::uint16_t) port;
```

- [ ] **Step 4: Build and verify**

Run: `./build.sh`
Expected: Build succeeds, no compile errors.

- [ ] **Step 5: Commit**

```bash
git add src/nvhttp.h src/nvhttp.cpp src/config.cpp
git commit -m "feat: move nvhttp HTTPS to 127.0.0.1:41300 for in-process gateway"
```

---

### Task 2: Create gateway.h header

**Files:**
- Create: `src/gateway.h`

**Interfaces:**
- Produces: `namespace gateway` with `std::unique_ptr<platf::deinit_t> start()` function
- Produces: stream ID constants 0x01-0x05, TLS_MAGIC=0x16

- [ ] **Step 1: Write gateway.h**

```cpp
/**
 * @file src/gateway.h
 * @brief In-process port-multiplexing gateway for single-port Sunshine access.
 *
 * Listens on 0.0.0.0:<base> TCP+UDP.  Demultiplexes by first byte:
 *   0x16 (TLS ClientHello) → raw TCP tunnel to 127.0.0.1:<base+100> (nvhttp)
 *   0x01–0x05            → gateway protocol [id:1B][len:2B BE][payload]
 */
#pragma once

#include "platform/common.h"

namespace gateway {

  // Stream IDs in the gateway protocol
  constexpr std::uint8_t STREAM_HTTPS  = 0x01;
  constexpr std::uint8_t STREAM_VIDEO  = 0x02;
  constexpr std::uint8_t STREAM_CONTROL = 0x03;
  constexpr std::uint8_t STREAM_AUDIO  = 0x04;
  constexpr std::uint8_t STREAM_RTSP   = 0x05;

  // First byte of a TLS ClientHello
  constexpr std::uint8_t TLS_MAGIC = 0x16;

  // Internal HTTPS port offset (nvhttp binds to base + 100)
  constexpr int INTERNAL_HTTPS_OFFSET = 100;

  /**
   * @brief Start the in-process gateway.
   * @return A deinit guard that stops the gateway on destruction.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();

}  // namespace gateway
```

- [ ] **Step 2: Commit**

```bash
git add src/gateway.h
git commit -m "feat: add gateway.h with stream ID constants and start() declaration"
```

---

### Task 3: Implement gateway TCP listener and demux

**Files:**
- Create: `src/gateway.cpp`

**Interfaces:**
- Consumes: `gateway::stream constants`, `gateway::INTERNAL_HTTPS_OFFSET`
- Consumes: `net::map_port()` for internal port calculation
- Produces: `gateway::start()` creates io_context, acceptor, UDP socket, worker thread

- [ ] **Step 1: Create file with includes, constants, and start() skeleton**

```cpp
/**
 * @file src/gateway.cpp
 * @brief In-process port-multiplexing gateway implementation.
 */
#include "gateway.h"
#include "config.h"
#include "logging.h"
#include "network.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

using namespace std::literals;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using udp = asio::ip::udp;

namespace gateway {

  class gateway_t {
  public:
    gateway_t()
        : ioc {}
        , acceptor {ioc}
        , udp_socket {ioc}
        , stopping {false} {}

    ~gateway_t() {
      stopping.store(true);
      beast::error_code ec;
      acceptor.close(ec);
      udp_socket.close(ec);
      ioc.stop();
      if (worker.joinable()) worker.join();
    }

    void run() {
      worker = std::thread([this]() {
        platf::set_thread_name("gateway");
        try {
          start_listeners();
          ioc.run();
        } catch (const std::exception &e) {
          BOOST_LOG(fatal) << "Gateway terminated: "sv << e.what();
        }
      });
    }

  private:
    void start_listeners();
    void do_accept();
    void do_udp_receive();

    // TCP session: peek first byte, then tunnel or proxy
    void handle_tcp_connection(tcp::socket client);

    asio::io_context ioc;
    tcp::acceptor acceptor;
    udp::socket udp_socket;
    std::thread worker;
    std::atomic<bool> stopping;
  };

  class deinit_t : public platf::deinit_t {
  public:
    deinit_t(std::unique_ptr<gateway_t> &&impl) : impl {std::move(impl)} {}
    ~deinit_t() override { impl.reset(); }
  private:
    std::unique_ptr<gateway_t> impl;
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> start() {
    auto impl = std::make_unique<gateway_t>();
    impl->run();
    return std::make_unique<deinit_t>(std::move(impl));
  }

}  // namespace gateway
```

- [ ] **Step 2: Implement start_listeners()**

```cpp
void gateway_t::start_listeners() {
  auto base_port = config::sunshine.port;  // 41200
  auto address = asio::ip::make_address("0.0.0.0");

  // TCP acceptor
  tcp::endpoint tcp_ep {address, base_port};
  acceptor.open(tcp_ep.protocol());
  acceptor.set_option(tcp::acceptor::reuse_address(true));
  acceptor.bind(tcp_ep);
  acceptor.listen(asio::socket_base::max_listen_connections);
  do_accept();

  // UDP socket
  udp::endpoint udp_ep {address, base_port};
  udp_socket.open(udp_ep.protocol());
  udp_socket.set_option(udp::socket::reuse_address(true));
  udp_socket.bind(udp_ep);
  do_udp_receive();

  BOOST_LOG(info) << "Gateway listening on 0.0.0.0:"sv << base_port << " (TCP+UDP)"sv;
}
```

- [ ] **Step 3: Implement do_accept()**

```cpp
void gateway_t::do_accept() {
  acceptor.async_accept([this](beast::error_code ec, tcp::socket client) {
    if (!ec) {
      handle_tcp_connection(std::move(client));
    }
    if (!stopping.load()) {
      do_accept();
    }
  });
}
```

- [ ] **Step 4: Commit**

```bash
git add src/gateway.cpp
git commit -m "feat: gateway TCP listener skeleton with start/stop lifecycle"
```

---

### Task 4: Implement TCP session handler (peek first byte)

**Files:**
- Modify: `src/gateway.cpp`

**Interfaces:**
- Consumes: `gateway_t::handle_tcp_connection(tcp::socket)`
- Produces: First-byte detection routes to `tls_tunnel()` or `framed_tcp_session()`

- [ ] **Step 1: Implement handle_tcp_connection with first-byte peek**

```cpp
void gateway_t::handle_tcp_connection(tcp::socket client) {
  auto buf = std::make_shared<std::uint8_t>();
  client.async_receive(asio::buffer(buf.get(), 1),
      [this, client = std::move(client), buf](beast::error_code ec, std::size_t) mutable {
    if (ec || stopping.load()) return;

    std::uint8_t first_byte = *buf;
    if (first_byte == TLS_MAGIC) {
      BOOST_LOG(debug) << "Gateway: TLS tunnel to nvhttp"sv;
      start_tls_tunnel(std::move(client), first_byte);
    } else if (first_byte >= STREAM_HTTPS && first_byte <= STREAM_RTSP) {
      BOOST_LOG(debug) << "Gateway: framed TCP stream 0x"sv
                       << util::hex(first_byte).to_string_view();
      start_framed_tcp(std::move(client), first_byte);
    } else {
      BOOST_LOG(warning) << "Gateway: unknown first byte 0x"sv
                         << util::hex(first_byte).to_string_view() << ", dropping"sv;
    }
  });
}
```

- [ ] **Step 2: Commit**

```bash
git add src/gateway.cpp
git commit -m "feat: gateway TCP first-byte peek and routing"
```

---

### Task 5: Implement TLS raw tunnel (0x16 passthrough)

**Files:**
- Modify: `src/gateway.cpp`

**Interfaces:**
- Consumes: `first_byte` from peek
- Produces: `start_tls_tunnel(tcp::socket, uint8_t first_byte)` — bidirectional raw TCP proxy to 127.0.0.1:41300

- [ ] **Step 1: Add shared_ptr aliases and start_tls_tunnel**

Before `handle_tcp_connection`, add:

```cpp
    // Forward-declare
    void start_tls_tunnel(tcp::socket client, std::uint8_t first_byte);
    void start_framed_tcp(tcp::socket client, std::uint8_t stream_id);

    // Shared socket alias for async ownership management
    using socket_ptr = std::shared_ptr<tcp::socket>;
```

- [ ] **Step 2: Implement start_tls_tunnel**

```cpp
void gateway_t::start_tls_tunnel(tcp::socket client, std::uint8_t first_byte) {
  auto internal = std::make_shared<tcp::socket>(ioc);
  auto client_ptr = std::make_shared<tcp::socket>(std::move(client));

  auto internal_port = (std::uint16_t)(config::sunshine.port + INTERNAL_HTTPS_OFFSET);
  internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), internal_port},
      [this, client_ptr, internal, first_byte](beast::error_code ec) {
    if (ec) {
      BOOST_LOG(warning) << "Gateway TLS tunnel: connect to nvhttp failed: "sv << ec.message();
      return;
    }

    // Send the first byte that we already peeked, then start bidir relay
    asio::async_write(*internal, asio::buffer(&first_byte, 1),
        [this, client_ptr, internal](beast::error_code ec, std::size_t) {
      if (ec) return;
      start_bidir_relay(client_ptr, internal);
    });
  });
}
```

- [ ] **Step 3: Implement start_bidir_relay (bidirectional raw relay)**

```cpp
void gateway_t::start_bidir_relay(socket_ptr a, socket_ptr b) {
  // Client → Internal
  auto buf_c2i = std::make_shared<std::array<char, 65536>>();
  a->async_read_some(asio::buffer(*buf_c2i),
      [this, a, b, buf_c2i](beast::error_code ec, std::size_t n) {
    if (ec || n == 0) return;
    asio::async_write(*b, asio::buffer(buf_c2i->data(), n),
        [this, a, b, buf_c2i](beast::error_code ec, std::size_t) {
      if (ec) return;
      start_bidir_relay(a, b);
    });
  });

  // Internal → Client
  auto buf_i2c = std::make_shared<std::array<char, 65536>>();
  b->async_read_some(asio::buffer(*buf_i2c),
      [this, a, b, buf_i2c](beast::error_code ec, std::size_t n) {
    if (ec || n == 0) return;
    asio::async_write(*a, asio::buffer(buf_i2c->data(), n),
        [this, a, b, buf_i2c](beast::error_code ec, std::size_t) {
      if (ec) return;
      start_bidir_relay(a, b);
    });
  });
}
```

- [ ] **Step 4: Commit**

```bash
git add src/gateway.cpp
git commit -m "feat: gateway TLS raw tunnel (0x16) to nvhttp internal port"
```

---

### Task 6: Implement framed TCP sessions (0x01, 0x05)

**Files:**
- Modify: `src/gateway.cpp`

**Interfaces:**
- Consumes: `first_byte` (stream_id) from peek, `STREAM_HTTPS`, `STREAM_RTSP`
- Produces: `start_framed_tcp(tcp::socket, uint8_t stream_id)` — framed proxy with gateway protocol

- [ ] **Step 1: Add target port map and start_framed_tcp**

```cpp
void gateway_t::start_framed_tcp(tcp::socket client, std::uint8_t stream_id) {
  // Map stream_id to internal target port
  std::uint16_t target_port = 0;
  switch (stream_id) {
    case STREAM_HTTPS:
      target_port = config::sunshine.port + INTERNAL_HTTPS_OFFSET;  // 41300
      break;
    case STREAM_RTSP:
      target_port = net::map_port(rtsp_stream::RTSP_SETUP_PORT);    // 41226
      break;
    default:
      return;  // Should not reach here
  }

  auto client_ptr = std::make_shared<tcp::socket>(std::move(client));
  do_framed_tcp_read(client_ptr, stream_id, target_port);
}
```

- [ ] **Step 2: Implement do_framed_tcp_read (read length + payload)**

```cpp
void gateway_t::do_framed_tcp_read(socket_ptr client, std::uint8_t stream_id,
                                   std::uint16_t target_port) {
  // Read 2-byte length header (big-endian)
  auto len_buf = std::make_shared<std::array<std::uint8_t, 2>>();
  asio::async_read(*client, asio::buffer(*len_buf),
      [this, client, stream_id, target_port, len_buf](beast::error_code ec, std::size_t) {
    if (ec) {
      if (!stopping.load() && ec != asio::error::eof) {
        BOOST_LOG(debug) << "Gateway framed TCP: read length error: "sv << ec.message();
      }
      return;
    }

    std::uint16_t payload_len = (static_cast<std::uint16_t>(len_buf->at(0)) << 8)
                              |  static_cast<std::uint16_t>(len_buf->at(1));

    // Read payload
    auto payload = std::make_shared<std::vector<std::uint8_t>>(payload_len);
    asio::async_read(*client, asio::buffer(*payload),
        [this, client, stream_id, target_port, payload](beast::error_code ec, std::size_t) {
      if (ec) return;

      forward_to_internal(client, stream_id, target_port, std::move(*payload));
    });
  });
}
```

- [ ] **Step 3: Implement forward_to_internal (dial, send, read response, wrap, reply)**

```cpp
void gateway_t::forward_to_internal(socket_ptr client, std::uint8_t stream_id,
                                    std::uint16_t target_port,
                                    std::vector<std::uint8_t> payload) {
  auto internal = std::make_shared<tcp::socket>(ioc);
  internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), target_port},
      [=](beast::error_code ec) mutable {
    if (ec) {
      BOOST_LOG(debug) << "Gateway framed TCP: internal connect failed: "sv << ec.message();
      return;
    }

    // Forward payload to internal server
    asio::async_write(*internal, asio::buffer(payload),
        [=](beast::error_code ec, std::size_t) mutable {
      if (ec) return;

      // Read response from internal server
      auto resp = std::make_shared<std::vector<std::uint8_t>>(65536);
      internal->async_read_some(asio::buffer(*resp),
          [=](beast::error_code ec, std::size_t n) mutable {
        if (ec || n == 0) return;
        resp->resize(n);

        // Wrap in gateway protocol header and send to client
        std::vector<std::uint8_t> wrapped;
        wrapped.reserve(3 + n);
        wrapped.push_back(stream_id);
        wrapped.push_back((n >> 8) & 0xFF);
        wrapped.push_back(n & 0xFF);
        wrapped.insert(wrapped.end(), resp->begin(), resp->end());

        asio::async_write(*client, asio::buffer(wrapped),
            [=](beast::error_code ec, std::size_t) {
          if (ec) return;
          // Continue reading next framed message from client
          const_cast<gateway_t*>(this)->do_framed_tcp_read(
              client, stream_id, target_port);
        });
      });
    });
  });
}
```

- [ ] **Step 4: Commit**

```bash
git add src/gateway.cpp
git commit -m "feat: gateway framed TCP sessions for HTTPS (0x01) and RTSP (0x05)"
```

---

### Task 7: Implement UDP demux (0x02, 0x03, 0x04)

**Files:**
- Modify: `src/gateway.cpp`

**Interfaces:**
- Consumes: `STREAM_VIDEO`, `STREAM_CONTROL`, `STREAM_AUDIO`, stream port constants
- Produces: UDP receive/send with gateway header wrapping

- [ ] **Step 1: Add UDP session tracking and target port map**

Before the gateway_t class, add:

```cpp
  // Map stream_id to internal UDP target port
  static std::uint16_t udp_target_port(std::uint8_t stream_id) {
    switch (stream_id) {
      case STREAM_VIDEO:  return net::map_port(stream::VIDEO_STREAM_PORT);   // 41214
      case STREAM_CONTROL: return net::map_port(stream::CONTROL_PORT);        // 41215
      case STREAM_AUDIO:  return net::map_port(stream::AUDIO_STREAM_PORT);   // 41216
      default: return 0;
    }
  }
```

Add to gateway_t private members:

```cpp
    // UDP client tracking: remember the last external endpoint per stream_id
    std::map<std::uint8_t, udp::endpoint> udp_clients;
    std::mutex udp_mutex;
```

- [ ] **Step 2: Implement do_udp_receive() and internal UDP readers**

```cpp
void gateway_t::do_udp_receive() {
  auto buf = std::make_shared<std::array<char, 65507>>();
  auto sender = std::make_shared<udp::endpoint>();
  udp_socket.async_receive_from(asio::buffer(*buf), *sender,
      [this, buf, sender](beast::error_code ec, std::size_t n) {
    if (ec || stopping.load()) return;

    if (n < 3) {
      BOOST_LOG(debug) << "Gateway UDP: packet too short ("sv << n << " bytes)"sv;
      do_udp_receive();
      return;
    }

    std::uint8_t stream_id = static_cast<std::uint8_t>(buf->at(0));
    std::uint16_t payload_len = (static_cast<std::uint16_t>(buf->at(1)) << 8)
                              |  static_cast<std::uint16_t>(buf->at(2));

    if (payload_len > n - 3) {
      BOOST_LOG(debug) << "Gateway UDP: payload_len "sv << payload_len
                       << " exceeds available "sv << (n - 3);
      do_udp_receive();
      return;
    }

    // Remember client endpoint for return path
    {
      std::lock_guard lock {udp_mutex};
      udp_clients[stream_id] = *sender;
    }

    auto target_port = udp_target_port(stream_id);
    if (target_port == 0) {
      BOOST_LOG(warning) << "Gateway UDP: unknown stream_id 0x"sv
                         << util::hex(stream_id).to_string_view();
    } else {
      // Forward payload to internal UDP port
      udp::endpoint internal_ep {asio::ip::make_address("127.0.0.1"), target_port};
      udp_socket.async_send_to(
          asio::buffer(buf->data() + 3, payload_len), internal_ep,
          [](beast::error_code, std::size_t) {});
    }

    do_udp_receive();
  });
}
```

- [ ] **Step 3: Add internal UDP forwarders (ephemeral sockets for bidirectional UDP)**

Sunshine's stream code sends UDP to the client address (which is the gateway's ephemeral socket).
So the gateway creates one dedicated ephemeral socket per internal stream — it forwards client
packets *from* this socket to Sunshine, and reads Sunshine's replies *on* this socket, wraps
them, and sends them to the external client via the main `udp_socket`.

Add to gateway_t private members:

```cpp
    // Dedicated ephemeral sockets for internal UDP forwarding
    std::map<std::uint8_t, std::shared_ptr<udp::socket>> udp_internal_sockets;
```

Add the forwarder setup function and call from start_listeners:

```cpp
    void start_udp_forwarder(std::uint16_t target_port, std::uint8_t stream_id) {
      auto sock = std::make_shared<udp::socket>(ioc);
      sock->open(udp::v4());
      // Bind to ephemeral port — Sunshine replies here
      sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), 0});

      auto buf = std::make_shared<std::array<char, 65507>>();

      std::function<void()> do_read = [=]() mutable {
        sock->async_receive(asio::buffer(*buf),
            [=](beast::error_code ec, std::size_t n) mutable {
          if (ec || stopping.load()) return;
          if (n == 0) { do_read(); return; }

          // Wrap with gateway header
          std::array<std::uint8_t, 3> header {
            stream_id,
            static_cast<std::uint8_t>((n >> 8) & 0xFF),
            static_cast<std::uint8_t>(n & 0xFF)
          };

          udp::endpoint client_ep;
          {
            std::lock_guard lock {udp_mutex};
            auto it = udp_clients.find(stream_id);
            if (it == udp_clients.end()) { do_read(); return; }
            client_ep = it->second;
          }

          // Send header + payload to external client via main socket
          std::vector<asio::const_buffer> buffers {
            asio::buffer(header),
            asio::buffer(buf->data(), n)
          };
          udp_socket.async_send_to(buffers, client_ep,
              [=](beast::error_code, std::size_t) { do_read(); });
        });
      };
      do_read();

      udp_internal_sockets[stream_id] = sock;
    }
```

And call from `start_listeners()` after UDP socket binding:

```cpp
    start_udp_forwarder(net::map_port(stream::VIDEO_STREAM_PORT),  STREAM_VIDEO);
    start_udp_forwarder(net::map_port(stream::CONTROL_PORT),       STREAM_CONTROL);
    start_udp_forwarder(net::map_port(stream::AUDIO_STREAM_PORT),  STREAM_AUDIO);
```

Update `do_udp_receive()` Step 2 to forward payloads through the ephemeral socket:

```cpp
    auto target_port = udp_target_port(stream_id);
    if (target_port == 0) {
      // ... (unchanged warning)
    } else {
      auto &sock = udp_internal_sockets[stream_id];
      udp::endpoint internal_ep {asio::ip::make_address("127.0.0.1"), target_port};
      sock->async_send_to(
          asio::buffer(buf->data() + 3, payload_len), internal_ep,
          [](beast::error_code, std::size_t) {});
    }
```

- [ ] **Step 4: Include missing headers**

At the top of gateway.cpp, add:

```cpp
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "stream.h"
#include "rtsp.h"
```

- [ ] **Step 5: Commit**

```bash
git add src/gateway.cpp
git commit -m "feat: gateway UDP demux for video/control/audio streams"
```

---

### Task 8: Add gateway.cpp to CMake source list

**Files:**
- Modify: `cmake/compile_definitions/common.cmake`

- [ ] **Step 1: Add gateway source files to SUNSHINE_TARGET_FILES**

In `cmake/compile_definitions/common.cmake`, add after the middleware entry:

```cmake
        "${CMAKE_SOURCE_DIR}/src/middleware.cpp"
        "${CMAKE_SOURCE_DIR}/src/middleware.h"
        "${CMAKE_SOURCE_DIR}/src/gateway.cpp"
        "${CMAKE_SOURCE_DIR}/src/gateway.h"
```

- [ ] **Step 2: Build and verify**

Run: `./build.sh`
Expected: Build succeeds, gateway.o compiled and linked.

- [ ] **Step 3: Commit**

```bash
git add cmake/compile_definitions/common.cmake
git commit -m "build: add gateway.cpp/h to CMake source list"
```

---

### Task 9: Wire gateway::start() into main.cpp

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add gateway include**

```cpp
#include "gateway.h"
```

- [ ] **Step 2: Start gateway after nvhttp initialization**

After `nvhttp::start` thread and before the main event loop, add:

```cpp
  // Start in-process gateway (listens on 0.0.0.0:41200 TCP+UDP)
  std::unique_ptr<platf::deinit_t> gateway_deinit_guard;
  gateway_deinit_guard = gateway::start();
  if (!gateway_deinit_guard) {
    BOOST_LOG(error) << "Gateway failed to initialize"sv;
  }
  BOOST_LOG(info) << "Gateway listening for client connections"sv;
```

This should go after the `httpThread` and before the main loop's `shutdown_event->view()`.

- [ ] **Step 3: Build and verify**

Run: `./build.sh`
Expected: Build succeeds with gateway wired into main.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire gateway::start() into main, single port 41200"
```

---

### Task 10: End-to-end verification

- [ ] **Step 1: Run ws_engine.exe**

```powershell
cd "C:\Program Files\SteamEngine"
.\ws_engine.exe
```

Check logs for:
```
Gateway listening on 0.0.0.0:41200 (TCP+UDP)
Gateway listening for client connections
nvhttp ready, middleware can now send stream_engine_info
```

- [ ] **Step 2: Test TLS passthrough (browser/curl)**

```powershell
curl -k https://127.0.0.1:41200/serverinfo
```

Expected: Returns XML serverinfo response (TLS 0x16 → tunnel → nvhttp).

- [ ] **Step 3: Test gateway protocol with custom Moonlight**

Connect custom Moonlight to `gateway:41200`. Verify: video/audio/control flow through gateway, rtsp handshake works.

- [ ] **Step 4: Test TCP stream framing**

Send a framed packet (e.g., 0x01 for HTTPS):
```
[0x01][0x00 0x3C][60 bytes HTTPS request data]
```

Expected: Response comes back wrapped with gateway header.

- [ ] **Step 5: Commit final verified state**

```bash
git add -A
git commit -m "feat: in-process gateway on port 41200 — verified end-to-end"
```
