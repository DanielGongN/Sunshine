/**
 * @file src/middleware.cpp
 * @brief Definitions for the middle-platform WebSocket client.
 */

// standard includes
#include <chrono>
#include <string>
#include <thread>

// lib includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "file_handler.h"
#include "globals.h"
#include "logging.h"
#include "middleware.h"
#include "nvhttp.h"

using namespace std::literals;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace middleware {

  // Constants
  constexpr auto RECONNECT_BASE_DELAY = 1s;
  constexpr auto RECONNECT_MAX_DELAY = 60s;
  constexpr int RECONNECT_BACKOFF_MULTIPLIER = 2;

  // Global pointers for send_to_upstream (set/cleared by middleware_t lifecycle)
  net::io_context* g_ioc = nullptr;
  websocket::stream<tcp::socket>* g_ws = nullptr;
  safe::queue_t<std::string> g_outgoing_queue {30};

  // Events to subscribe to
  constexpr const char* SUBSCRIBED_EVENTS[] = {
    "force_disconnected_time",
    "standby_disconnected_time",
    "display_config",
    "click_gamepad",
    "disconnected"
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
    }

    ~middleware_t() {
      stopping.store(true);
      g_ioc = nullptr;
      g_ws = nullptr;
      beast::error_code ec;
      ws.close(websocket::close_code::normal, ec);  // ignore errors during shutdown
      ioc.stop();
      if (worker.joinable()) {
        worker.join();
      }
    }

    void run() {
      worker = std::thread([this]() {
        platf::set_thread_name("middleware");
        connect_and_subscribe();
        ioc.run();
      });
    }

  private:
    static std::string make_timestamp() {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      return std::to_string(now);
    }

    void send_subscribe_msg(const std::string& topic) {
      json root;
      root["message_id"] = make_timestamp();
      root["event"] = "subscribe";
      json data;
      data["topic"] = topic;
      root["data"] = data;

      std::string msg = root.dump();
      BOOST_LOG(info) << "Subscribing to event: "sv << topic;
      ws.write(net::buffer(msg));
    }

    void subscribe_all_events() {
      for (const auto& event : SUBSCRIBED_EVENTS) {
        send_subscribe_msg(event);
      }
    }

    void on_message_event(json& msg) {
      if (!msg.contains("event")) {
        BOOST_LOG(warning) << "No event field in middleware message"sv;
        return;
      }

      std::string event_type = msg["event"].get<std::string>();
      std::string msg_id = msg.value("message_id", "");

      BOOST_LOG(info) << "Middleware event received: "sv << event_type << " (msg_id="sv << msg_id << ')';

      if (event_type == "force_disconnected_time") {
        // Store AFK timeout (seconds) for use by stream idle detection
        int timeout = msg.value("data", json::object()).value("timeout", 0);
        if (timeout <= 0) timeout = msg.value("data", 0);
        if (timeout > 0) {
          config::sunshine.middleware.force_disconnected_timeout = timeout;
          BOOST_LOG(info) << "force_disconnected_time: timeout="sv << timeout;
        }
      }
      else if (event_type == "standby_disconnected_time") {
        int timeout = msg.value("data", json::object()).value("timeout", 0);
        if (timeout <= 0) timeout = msg.value("data", 0);
        if (timeout > 0) {
          config::sunshine.middleware.standby_disconnected_timeout = timeout;
          BOOST_LOG(info) << "standby_disconnected_time: timeout="sv << timeout;
        }
      }
      else if (event_type == "display_config") {
        auto& data = msg["data"];
        int qp_high   = data.value("high", 0);
        int qp_normal = data.value("normal", 0);
        int qp_low    = data.value("low", 0);
        if (qp_high > 0 && qp_normal > 0 && qp_low > 0) {
          BOOST_LOG(info) << "display_config: qp_high="sv << qp_high
                          << " qp_normal="sv << qp_normal
                          << " qp_low="sv << qp_low;
        }
      }
      else if (event_type == "click_gamepad") {
        std::string button = msg["data"].value("button", "");
        BOOST_LOG(info) << "click_gamepad: button="sv << button;
      }
      else if (event_type == "disconnected") {
        BOOST_LOG(info) << "Middleware requested client disconnect"sv;
        auto force_event = mail::man->event<bool>(mail::force_disconnect);
        force_event->raise(true);
      }
      else if (event_type == "client_connect") {
        auto &data = msg["data"];
        std::string uuid = data.value("uuid", "");
        std::string cert = data.value("cert", "");
        if (uuid.empty() || cert.empty()) {
          BOOST_LOG(warning) << "client_connect: missing uuid or cert"sv;
        } else {
          nvhttp::add_trusted_client(uuid, cert);
        }
      }
      else if (event_type == "client_disconnect") {
        std::string uuid = msg.value("data", json::object()).value("uuid", "");
        if (uuid.empty()) {
          BOOST_LOG(warning) << "client_disconnect: missing uuid"sv;
        } else {
          nvhttp::remove_trusted_client(uuid);
        }
      }
      else {
        BOOST_LOG(debug) << "Unknown middleware event: "sv << event_type;
      }
    }

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

    void drain_outgoing() {
      while (g_outgoing_queue.peek()) {
        auto payload = g_outgoing_queue.pop();
        beast::error_code ec;
        ws.write(net::buffer(payload), ec);
        if (ec) {
          BOOST_LOG(warning) << "Failed to send to upstream: "sv << ec.message();
          break;
        }
      }
    }

    void do_async_read() {
      auto buffer = std::make_shared<beast::flat_buffer>();
      ws.async_read(*buffer, [this, buffer](beast::error_code ec, std::size_t /*bytes*/) {
        if (ec == net::error::operation_aborted || stopping.load()) {
          return;
        }

        if (ec) {
          BOOST_LOG(warning) << "Middleware read error: "sv << ec.message();
          // Connection broken — schedule reconnect
          schedule_reconnect();
          return;
        }

        try {
          std::string payload = beast::buffers_to_string(buffer->data());
          buffer->consume(buffer->size());
          json msg = json::parse(payload);
          on_message_event(msg);
        } catch (const json::parse_error& e) {
          BOOST_LOG(warning) << "Middleware JSON parse error: "sv << e.what();
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
      if (stopping.load()) return;

      auto& cfg = config::sunshine.middleware;
      beast::error_code ec;

      // Resolve and connect
      auto results = resolver.resolve(cfg.address, std::to_string(cfg.port), ec);
      if (ec) {
        BOOST_LOG(warning) << "Middleware resolve failed: "sv << ec.message();
        schedule_reconnect();
        return;
      }

      net::connect(beast::get_lowest_layer(ws), results, ec);
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

      // Wait for nvhttp HTTPS server to be ready before sending stream_engine_info
      auto nvhttp_ready = mail::man->event<bool>(mail::nvhttp_ready);
      if (!nvhttp_ready->peek()) {
        BOOST_LOG(info) << "Waiting for nvhttp server to be ready..."sv;
        nvhttp_ready->view();  // block until nvhttp::start() raises the event
      }
      send_stream_engine_info();

      // Drain any outgoing messages queued before we start reading
      drain_outgoing();

      // Start async read loop (non-blocking)
      do_async_read();
    }

    void schedule_reconnect() {
      if (stopping.load()) return;

      BOOST_LOG(info) << "Middleware reconnecting in "sv
                      << std::chrono::duration_cast<std::chrono::seconds>(reconnect_delay).count()
                      << " seconds"sv;

      auto timer = std::make_shared<net::steady_timer>(ioc, reconnect_delay);
      timer->async_wait([this, timer](beast::error_code ec) {
        if (ec == net::error::operation_aborted || stopping.load()) {
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

    net::io_context ioc;
    websocket::stream<tcp::socket> ws;
    tcp::resolver resolver;
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

  void send_to_upstream(nlohmann::json msg) {
    if (!g_ioc || !g_ws) {
      BOOST_LOG(debug) << "send_to_upstream: middleware not connected, dropping message"sv;
      return;
    }

    msg["event"] = "send_to_upstream";
    msg["message_id"] = std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count()
    );

    std::string payload = msg.dump();
    g_outgoing_queue.push(std::move(payload));

    // Post to io_context to drain and send immediately.
    // Cancel in-flight async_read, drain outgoing, then restart.
    boost::asio::post(*g_ioc, []() {
      if (!g_ws) return;
      beast::error_code ec;
      g_ws->cancel(ec);  // Cancel any in-flight async_read (safe on io_context thread)
      // Note: drain_outgoing will be called when the cancelled read completes
      // and before the next async_read starts, via do_async_read's completion handler.
      // For immediate send, drain here too (the cancel above ensures no concurrent read):
      while (g_outgoing_queue.peek()) {
        auto payload = g_outgoing_queue.pop();
        beast::error_code wec;
        g_ws->write(net::buffer(payload), wec);
        if (wec) {
          BOOST_LOG(warning) << "send_to_upstream write failed: "sv << wec.message();
          break;
        }
      }
    });
  }

}  // namespace middleware
