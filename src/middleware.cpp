/**
 * @file src/middleware.cpp
 * @brief Definitions for the middle-platform WebSocket client.
 */

// standard includes
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

// lib includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "file_handler.h"
#include "gateway.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "middleware.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"

using namespace std::literals;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

namespace middleware {

  // Constants
  constexpr auto RECONNECT_BASE_DELAY = 1s;
  constexpr auto RECONNECT_MAX_DELAY = 60s;
  constexpr int RECONNECT_BACKOFF_MULTIPLIER = 2;

  // Global pointers for send_to_upstream (set/cleared by middleware_t lifecycle)
  asio::io_context *g_ioc = nullptr;
  websocket::stream<tcp::socket> *g_ws = nullptr;
  safe::queue_t<std::string> g_outgoing_queue {30};
  std::atomic<bool> g_read_in_progress {false};
  std::atomic<int> g_active_sessions {0};
  constexpr auto HEARTBEAT_INTERVAL = 30s;
  constexpr auto CONNECTED_RETRY_INTERVAL = 30s;
  std::mutex g_connected_ack_mutex;
  std::optional<std::int64_t> g_pending_connected_message_id;
  std::optional<json> g_pending_connected_payload;
  asio::steady_timer *g_connected_retry_timer = nullptr;

  namespace detail {
    std::int64_t make_timestamp() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
      )
        .count();
    }

    std::optional<std::int64_t> get_int64_field(const json &msg, const char *field) {
      if (!msg.contains(field)) {
        return std::nullopt;
      }

      const auto &value = msg[field];
      if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return static_cast<std::int64_t>(unsigned_value);
        }
      }

      if (value.is_number_integer()) {
        return value.get<std::int64_t>();
      }

      return std::nullopt;
    }

    json make_connected_payload(std::int64_t message_id) {
      json msg;
      msg["event"] = "connected";
      msg["message_id"] = message_id;
      msg["data"] = {
        {"type", 1}
      };
      return msg;
    }

    json make_disconnected_payload(std::string_view message, disconnect_notification_type_e type) {
      json msg;
      msg["event"] = "disconnected";
      msg["message"] = std::string {message};
      msg["data"] = {
        {"type", static_cast<int>(type)}
      };
      return msg;
    }

    bool is_connected_ack_success(const json &msg, std::int64_t pending_message_id) {
      if (!msg.contains("event") || !msg["event"].is_string()) {
        return false;
      }

      if (msg["event"].get<std::string>() != "consume_record_create_success") {
        return false;
      }

      const auto msg_id = get_int64_field(msg, "message_id");
      if (!msg_id || *msg_id != pending_message_id) {
        return false;
      }

      if (!msg.contains("data") || !msg["data"].is_object()) {
        return false;
      }

      const auto code = get_int64_field(msg["data"], "code");
      return code && *code == 0;
    }
  }  // namespace detail

  std::optional<std::int64_t> pending_connected_message_id() {
    std::scoped_lock lock {g_connected_ack_mutex};
    return g_pending_connected_message_id;
  }

  void set_pending_connected_payload(json msg) {
    auto msg_id = detail::get_int64_field(msg, "message_id");
    if (!msg_id) {
      return;
    }

    std::scoped_lock lock {g_connected_ack_mutex};
    g_pending_connected_message_id = *msg_id;
    g_pending_connected_payload = std::move(msg);
  }

  void clear_pending_connected_payload() {
    std::scoped_lock lock {g_connected_ack_mutex};
    g_pending_connected_message_id.reset();
    g_pending_connected_payload.reset();
  }

  void schedule_connected_retry();

  void cancel_connected_retry() {
    auto *ioc = g_ioc;
    auto *timer = g_connected_retry_timer;
    if (!ioc || !timer) {
      return;
    }

    boost::asio::post(*ioc, [timer]() {
      if (timer != g_connected_retry_timer) {
        return;
      }

      timer->cancel();
    });
  }

  void retry_pending_connected() {
    std::optional<json> payload;
    std::optional<std::int64_t> msg_id;
    {
      std::scoped_lock lock {g_connected_ack_mutex};
      if (!g_pending_connected_message_id || !g_pending_connected_payload) {
        return;
      }

      msg_id = g_pending_connected_message_id;
      payload = g_pending_connected_payload;
    }

    BOOST_LOG(info) << "Retrying connected event: msg_id="sv << *msg_id;
    send_to_upstream(std::move(*payload));
    schedule_connected_retry();
  }

  void schedule_connected_retry() {
    auto *ioc = g_ioc;
    auto *timer = g_connected_retry_timer;
    if (!ioc || !timer) {
      return;
    }

    boost::asio::post(*ioc, [timer]() {
      if (timer != g_connected_retry_timer) {
        return;
      }

      {
        std::scoped_lock lock {g_connected_ack_mutex};
        if (!g_pending_connected_message_id || !g_pending_connected_payload) {
          return;
        }
      }

      timer->cancel();
      timer->expires_after(CONNECTED_RETRY_INTERVAL);
      timer->async_wait([](const boost::system::error_code &ec) {
        if (ec == asio::error::operation_aborted) {
          return;
        }

        if (ec) {
          BOOST_LOG(warning) << "connected retry timer error: "sv << ec.message();
          return;
        }

        retry_pending_connected();
      });
    });
  }

  void notify_client_state(bool connected) {
    if (connected) {
      g_active_sessions.fetch_add(1, std::memory_order_release);
    } else {
      g_active_sessions.fetch_sub(1, std::memory_order_release);
    }
  }

  void notify_client_connected() {
    auto msg = detail::make_connected_payload(detail::make_timestamp());
    set_pending_connected_payload(msg);
    send_to_upstream(std::move(msg));
    schedule_connected_retry();
  }

  void notify_client_disconnected(std::string_view message, disconnect_notification_type_e type) {
    send_to_upstream(detail::make_disconnected_payload(message, type));
  }

  void notify_client_disconnected(std::u8string_view message, disconnect_notification_type_e type) {
    notify_client_disconnected(std::string_view {reinterpret_cast<const char *>(message.data()), message.size()}, type);
  }

  // Events to subscribe to
  constexpr const char *SUBSCRIBED_EVENTS[] = {
    "force_disconnected_time",
    "standby_disconnected_time",
    "display_config",
    "click_gamepad",
    "disconnected",
    "client_connect",
    "client_disconnect",
    "consume_record_create_success"
  };

  class middleware_t {
  public:
    middleware_t():
        ioc {},
        ws {ioc},
        resolver {ioc},
        reconnect_delay {RECONNECT_BASE_DELAY},
        stopping {false} {
      g_ioc = &ioc;
      g_ws = &ws;
      g_connected_retry_timer = &connected_retry_timer;
    }

    ~middleware_t() {
      stopping.store(true);
      g_ioc = nullptr;
      g_ws = nullptr;
      g_connected_retry_timer = nullptr;
      clear_pending_connected_payload();
      beast::error_code ec;
      ws.close(websocket::close_code::normal, ec);  // ignore errors during shutdown
      ioc.stop();
      if (worker.joinable()) {
        worker.join();
      }
    }

    void run() {
      worker = std::thread([this]() {
        try {
          platf::set_thread_name("middleware");
          connect_and_subscribe();
          ioc.run();
        } catch (const std::exception &e) {
          BOOST_LOG(fatal) << "Middleware thread terminated: "sv << e.what();
        }
      });
    }

  private:
    static std::int64_t make_timestamp() {
      return detail::make_timestamp();
    }

    void send_subscribe_msg(const std::string &topic) {
      json root;
      root["message_id"] = make_timestamp();
      root["event"] = "subscribe";
      json data;
      data["topic"] = topic;
      root["data"] = data;

      std::string msg = root.dump();
      beast::error_code ec;
      ws.write(asio::buffer(msg), ec);
      if (ec) {
        BOOST_LOG(warning) << "Failed to subscribe to "sv << topic << ": "sv << ec.message();
      } else {
        BOOST_LOG(info) << "Subscribing to event: "sv << topic;
      }
    }

    void subscribe_all_events() {
      for (const auto &event : SUBSCRIBED_EVENTS) {
        send_subscribe_msg(event);
      }
    }

    void on_message_event(json &msg) {
      if (!msg.contains("event")) {
        BOOST_LOG(warning) << "No event field in middleware message"sv;
        return;
      }

      if (!msg["event"].is_string()) {
        BOOST_LOG(warning) << "event field is not a string, dropping message"sv;
        return;
      }

      std::string event_type = msg["event"].get<std::string>();
      std::int64_t msg_id = msg.value("message_id", std::int64_t {0});

      BOOST_LOG(info) << "Middleware event received: "sv << event_type << " (msg_id="sv << msg_id << ')';

      if (event_type == "force_disconnected_time") {
        BOOST_LOG(info) << "handling force_disconnected_time"sv;
        // Store AFK timeout (seconds) for use by stream idle detection
        // data can be a number directly, or an object with a "timeout" key
        std::optional<int> timeout;
        if (msg.contains("data")) {
          if (msg["data"].is_number()) {
            timeout = msg["data"].get<int>();
          } else if (msg["data"].is_object()) {
            const auto &data = msg["data"];
            if (data.contains("timeout") && data["timeout"].is_number()) {
              timeout = data["timeout"].get<int>();
            }
          }
        }
        if (timeout && *timeout >= 0) {
          config::sunshine.middleware.force_disconnected_timeout.store(*timeout, std::memory_order_release);
          BOOST_LOG(info) << "force_disconnected_time: timeout="sv << *timeout;
        }
      } else if (event_type == "standby_disconnected_time") {
        BOOST_LOG(info) << "handling standby_disconnected_time"sv;
        // data can be a number directly, or an object with a "timeout" key
        std::optional<int> timeout;
        if (msg.contains("data")) {
          if (msg["data"].is_number()) {
            timeout = msg["data"].get<int>();
          } else if (msg["data"].is_object()) {
            const auto &data = msg["data"];
            if (data.contains("timeout") && data["timeout"].is_number()) {
              timeout = data["timeout"].get<int>();
            }
          }
        }
        if (timeout && *timeout >= 0) {
          config::sunshine.middleware.standby_disconnected_timeout.store(*timeout, std::memory_order_release);
          BOOST_LOG(info) << "standby_disconnected_time: timeout="sv << *timeout;
        }
      } else if (event_type == "display_config") {
        BOOST_LOG(info) << "handling display_config"sv;
        auto &data = msg["data"];
        int qp_high = data.value("high", 0);
        int qp_normal = data.value("normal", 0);
        int qp_low = data.value("low", 0);
        if (qp_high > 0 && qp_normal > 0 && qp_low > 0) {
          BOOST_LOG(info) << "display_config: qp_high="sv << qp_high
                          << " qp_normal="sv << qp_normal
                          << " qp_low="sv << qp_low;
        }
      } else if (event_type == "click_gamepad") {
        BOOST_LOG(info) << "handling click_gamepad"sv;
        std::string button = msg["data"].value("button", "");
        if (button.empty()) {
          BOOST_LOG(warning) << "click_gamepad: missing button field"sv;
          return;
        }

        // Map button name to platf:: flag
        static const std::unordered_map<std::string, std::uint32_t> button_map {
          {"A", platf::A},
          {"B", platf::B},
          {"X", platf::X},
          {"Y", platf::Y},
          {"DPAD_UP", platf::DPAD_UP},
          {"DPAD_DOWN", platf::DPAD_DOWN},
          {"DPAD_LEFT", platf::DPAD_LEFT},
          {"DPAD_RIGHT", platf::DPAD_RIGHT},
          {"START", platf::START},
          {"BACK", platf::BACK},
          {"HOME", platf::HOME},
          {"LEFT_STICK", platf::LEFT_STICK},
          {"RIGHT_STICK", platf::RIGHT_STICK},
          {"LEFT_BUTTON", platf::LEFT_BUTTON},
          {"RIGHT_BUTTON", platf::RIGHT_BUTTON},
        };

        auto it = button_map.find(button);
        if (it == button_map.end()) {
          BOOST_LOG(warning) << "click_gamepad: unknown button: "sv << button;
          return;
        }

        BOOST_LOG(info) << "click_gamepad: injecting button="sv << button;
        input::click_gamepad(0, it->second);
      } else if (event_type == "disconnected") {
        BOOST_LOG(info) << "Middleware requested client disconnect"sv;
        auto force_event = mail::man->event<bool>(mail::force_disconnect);
        force_event->raise(true);
      } else if (event_type == "consume_record_create_success") {
        auto pending_msg_id = pending_connected_message_id();
        if (!pending_msg_id) {
          BOOST_LOG(debug) << "consume_record_create_success received with no pending connected event"sv;
        } else if (detail::is_connected_ack_success(msg, *pending_msg_id)) {
          BOOST_LOG(info) << "connected event acknowledged: msg_id="sv << *pending_msg_id;
          clear_pending_connected_payload();
          cancel_connected_retry();
        } else {
          BOOST_LOG(debug) << "consume_record_create_success did not match pending connected event"sv;
        }
      } else if (event_type == "client_connect") {
        BOOST_LOG(info) << "handling client_connect"sv;
        auto &data = msg["data"];
        BOOST_LOG(info) << "client_connect data="sv << data.dump();
        std::string uuid = data.value("uuid", "");
        std::string cert = data.value("cert", "");
        std::string user_uuid = data.value("user_uuid", "");
        int status = -1;
        if (uuid.empty() || cert.empty()) {
          BOOST_LOG(warning) << "client_connect: missing uuid or cert"sv;
        } else if (!nvhttp::add_trusted_client(uuid, cert)) {
          BOOST_LOG(warning) << "client_connect: failed to add trusted client certificate, uuid="sv << uuid;
        } else {
          if (data.contains("rikey")) {
            nvhttp::set_pending_stream_config(data);
            BOOST_LOG(info) << "client_connect: stream config stored, waiting for serverinfo"sv;
          }
          status = 0;
        }

        // Send operate_result acknowledgment back to upstream
        json ack;
        ack["event"] = "operate_result";
        ack["message_id"] = make_timestamp();
        ack["data"] = {
          {"event", "client_connect"},
          {"user_uuid", user_uuid},
          {"status", status}
        };
        send_to_upstream(std::move(ack));
      } else if (event_type == "client_disconnect") {
        BOOST_LOG(info) << "handling client_disconnect"sv;
        std::string uuid = msg.value("data", json::object()).value("uuid", "");
        if (uuid.empty()) {
          BOOST_LOG(warning) << "client_disconnect: missing uuid"sv;
        } else {
          nvhttp::remove_trusted_client(uuid);
        }
      } else {
        BOOST_LOG(info) << "Unknown middleware event: "sv << event_type;
      }
    }

    void send_stream_engine_info() {
      // Read server cert from disk
      auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
      if (cert.empty()) {
        BOOST_LOG(error) << "Failed to read server cert for stream_engine_info"sv;
        return;
      }

      auto gateway_port = (std::uint16_t) config::sunshine.port;
      auto internal_https_port = (std::uint16_t) (config::sunshine.port + nvhttp::PORT_HTTPS_INTERNAL);

      json msg;
      msg["message_id"] = make_timestamp();
      msg["event"] = "stream_engine_info";
      json data;
      data["cert"] = cert;

      json gateway_streams;
      gateway_streams["https"] = static_cast<int>(gateway::STREAM_HTTPS);
      gateway_streams["video"] = static_cast<int>(gateway::STREAM_VIDEO);
      gateway_streams["control"] = static_cast<int>(gateway::STREAM_CONTROL);
      gateway_streams["audio"] = static_cast<int>(gateway::STREAM_AUDIO);
      gateway_streams["rtsp"] = static_cast<int>(gateway::STREAM_RTSP);
      gateway_streams["client_mic"] = static_cast<int>(gateway::STREAM_CLIENT_MIC);

      json gateway_info;
      gateway_info["port"] = gateway_port;
      gateway_info["protocol"] = "steam_gateway_v1";
      gateway_info["udp_header_bytes"] = gateway::UDP_HEADER_SIZE;
      gateway_info["streams"] = std::move(gateway_streams);

      // Gateway external single port (handles 0x01/0x16 protocol split).
      data["gateway"] = std::move(gateway_info);
      // Internal HTTPS port, for reference only; external clients should not connect directly.
      data["internal_https_port"] = internal_https_port;
      data["port"] = gateway_port;  // Backward compatibility: the default port is now the gateway port.

      BOOST_LOG(info) << "[stream_engine_info] gateway端口="sv << gateway_port
                      << " 内部HTTPS端口="sv << internal_https_port;

      msg["data"] = data;

      send_to_upstream(std::move(msg));
    }

    void drain_outgoing() {
      while (g_outgoing_queue.peek()) {
        auto payload = g_outgoing_queue.pop();
        if (!payload) {
          continue;
        }
        beast::error_code ec;
        ws.write(asio::buffer(*payload), ec);
        if (ec) {
          BOOST_LOG(warning) << "Failed to send to upstream: "sv << ec.message();
          break;
        }
      }
    }

    void do_async_read() {
      g_read_in_progress.store(true, std::memory_order_release);
      auto buffer = std::make_shared<beast::flat_buffer>();
      ws.async_read(*buffer, [this, buffer](beast::error_code ec, std::size_t bytes) {
        g_read_in_progress.store(false, std::memory_order_release);

        if (stopping.load()) {
          return;
        }

        if (ec) {
          BOOST_LOG(warning) << "Middleware read error: "sv << ec.message();
          // Connection broken; schedule reconnect.
          schedule_reconnect();
          return;
        }

        BOOST_LOG(debug) << "async_read received "sv << bytes << " bytes"sv;

        try {
          std::string payload = beast::buffers_to_string(buffer->data());
          buffer->consume(buffer->size());
          json msg = json::parse(payload);
          on_message_event(msg);
        } catch (const json::exception &e) {
          BOOST_LOG(warning) << "Middleware JSON error: "sv << e.what();
        }

        // Process any outgoing messages queued between reads
        drain_outgoing();

        // Continue async read loop
        if (!stopping.load()) {
          do_async_read();
        }
      });
    }

    void connect_and_subscribe() {
      if (stopping.load()) {
        return;
      }

      auto &cfg = config::sunshine.middleware;
      beast::error_code ec;

      // Resolve and connect
      auto results = resolver.resolve(cfg.address, std::to_string(cfg.port), ec);
      if (ec) {
        BOOST_LOG(warning) << "Middleware resolve failed: "sv << ec.message();
        schedule_reconnect();
        return;
      }

      asio::connect(beast::get_lowest_layer(ws), results, ec);
      if (ec) {
        BOOST_LOG(warning) << "Middleware connect failed: "sv << ec.message();
        schedule_reconnect();
        return;
      }

      // WebSocket handshake
      ws.handshake(cfg.address + ":" + std::to_string(cfg.port), "/ws", ec);
      if (ec) {
        BOOST_LOG(warning) << "Middleware handshake failed: "sv << ec.message();
        schedule_reconnect();
        return;
      }

      BOOST_LOG(info) << "Middleware connected to "sv << cfg.address << ':' << cfg.port;
      reconnect_delay = RECONNECT_BASE_DELAY;  // reset backoff

      // Subscribe to events
      subscribe_all_events();

      // Wait for nvhttp HTTPS server to be ready before sending stream_engine_info.
      auto nvhttp_ready = mail::man->event<bool>(mail::nvhttp_ready);
      if (!nvhttp_ready->peek()) {
        BOOST_LOG(info) << "Waiting for nvhttp server to be ready..."sv;
        nvhttp_ready->view();  // block until nvhttp::start() raises the event
      }
      send_stream_engine_info();

      // Drain outgoing before starting async read (no read pending, safe to write)
      drain_outgoing();

      // Start async read loop (no cancel in send_to_upstream, so it won't be interrupted)
      do_async_read();

      // Start heartbeat timer
      start_heartbeat();
    }

    void send_heartbeat(const boost::system::error_code &ec) {
      if (ec == asio::error::operation_aborted || stopping.load()) {
        return;
      }

      json msg;
      msg["event"] = "client_heartbeat";
      msg["message_id"] = make_timestamp();
      msg["data"] = {
        {"net_connected", g_active_sessions.load(std::memory_order_acquire) > 0},
        {"type", 1}  // 1 = sunshine client
      };

      // Heartbeats are sent directly and do not use the send_to_upstream envelope.
      std::string payload = msg.dump();
      g_outgoing_queue.raise(std::move(payload));
      boost::asio::post(ioc, [this]() {
        drain_outgoing();
      });

      // Schedule the next heartbeat.
      heartbeat_timer.expires_after(HEARTBEAT_INTERVAL);
      heartbeat_timer.async_wait([this](const auto &ec) {
        send_heartbeat(ec);
      });
    }

    void start_heartbeat() {
      heartbeat_timer.expires_after(HEARTBEAT_INTERVAL);
      heartbeat_timer.async_wait([this](const auto &ec) {
        send_heartbeat(ec);
      });
    }

    void schedule_reconnect() {
      if (stopping.load()) {
        return;
      }

      BOOST_LOG(info) << "Middleware reconnecting in "sv
                      << std::chrono::duration_cast<std::chrono::seconds>(reconnect_delay).count()
                      << " seconds"sv;

      auto timer = std::make_shared<asio::steady_timer>(ioc, reconnect_delay);
      timer->async_wait([this, timer](beast::error_code ec) {
        if (ec == asio::error::operation_aborted || stopping.load()) {
          return;
        }
        connect_and_subscribe();
      });

      // Increase backoff
      reconnect_delay = std::min<std::chrono::milliseconds>(
        reconnect_delay * RECONNECT_BACKOFF_MULTIPLIER,
        RECONNECT_MAX_DELAY
      );
    }

    asio::io_context ioc;
    websocket::stream<tcp::socket> ws;
    tcp::resolver resolver;
    asio::steady_timer heartbeat_timer {ioc};
    asio::steady_timer connected_retry_timer {ioc};
    std::thread worker;
    std::chrono::milliseconds reconnect_delay;
    std::atomic<bool> stopping;
  };

  class deinit_t: public platf::deinit_t {
  public:
    deinit_t(std::unique_ptr<middleware_t> &&impl):
        impl {std::move(impl)} {
    }

    ~deinit_t() override {
      impl.reset();  // destroy middleware_t first (joins thread, closes socket)
    }

  private:
    std::unique_ptr<middleware_t> impl;
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> start() {
    auto impl = std::make_unique<middleware_t>();
    impl->run();

    return std::make_unique<deinit_t>(std::move(impl));
  }

  void send_to_upstream(nlohmann::json inner_msg) {
    // Log the internal event type for tracing.
    std::string inner_event = inner_msg.value("event", "unknown");
    BOOST_LOG(info) << "send_to_upstream: event="sv << inner_event;
    if (!g_ioc || !g_ws) {
      BOOST_LOG(debug) << "send_to_upstream: middleware not connected, dropping message"sv;
      return;
    }

    // Wrap the internal event in a send_to_upstream envelope.
    json envelope;
    envelope["event"] = "send_to_upstream";
    envelope["message_id"] = detail::make_timestamp();
    envelope["data"] = std::move(inner_msg);

    std::string payload = envelope.dump();
    g_outgoing_queue.raise(std::move(payload));

    // Post to the io_context and try to drain the outgoing queue.
    // If the read loop is active, its completion callback will drain the queue.
    // If no read is active, drain immediately.
    boost::asio::post(*g_ioc, []() {
      if (!g_ws) {
        return;
      }
      if (g_read_in_progress.load(std::memory_order_acquire)) {
        // The active read loop will call drain_outgoing().
        return;
      }
      // No active read operation; drain safely now.
      while (g_outgoing_queue.peek()) {
        auto payload = g_outgoing_queue.pop();
        if (!payload) {
          continue;
        }
        beast::error_code wec;
        g_ws->write(asio::buffer(*payload), wec);
        if (wec) {
          BOOST_LOG(warning) << "send_to_upstream write failed: "sv << wec.message();
          break;
        }
      }
    });
  }

}  // namespace middleware
