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
#include "globals.h"
#include "logging.h"
#include "middleware.h"

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
    }

    ~middleware_t() {
      stopping.store(true);
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
        // Store AFK timeout (seconds) for use by stream logic
        int timeout = msg.value("data", json::object()).value("timeout", 0);
        if (timeout <= 0) timeout = msg.value("data", 0);
        if (timeout > 0) {
          BOOST_LOG(info) << "force_disconnected_time: timeout="sv << timeout;
        }
      }
      else if (event_type == "standby_disconnected_time") {
        int timeout = msg.value("data", json::object()).value("timeout", 0);
        if (timeout <= 0) timeout = msg.value("data", 0);
        if (timeout > 0) {
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
      }
      else {
        BOOST_LOG(debug) << "Unknown middleware event: "sv << event_type;
      }
    }

    void read_loop() {
      beast::flat_buffer buffer;
      while (!stopping.load()) {
        beast::error_code ec;
        ws.read(buffer, ec);

        if (ec == net::error::operation_aborted || stopping.load()) {
          break;
        }

        if (ec) {
          BOOST_LOG(warning) << "Middleware read error: "sv << ec.message();
          break;
        }

        try {
          std::string payload = beast::buffers_to_string(buffer.data());
          buffer.consume(buffer.size());
          json msg = json::parse(payload);
          on_message_event(msg);
        } catch (const json::parse_error& e) {
          BOOST_LOG(warning) << "Middleware JSON parse error: "sv << e.what();
        }
      }
    }

    void connect_and_subscribe() {
      auto& cfg = config::sunshine.middleware;

      while (!stopping.load()) {
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
        ws.handshake(cfg.address + ":" + std::to_string(cfg.port), "/", ec);
        if (ec) {
          BOOST_LOG(warning) << "Middleware handshake failed: "sv << ec.message();
          schedule_reconnect();
          return;
        }

        BOOST_LOG(info) << "Middleware connected to "sv << cfg.address << ':' << cfg.port;
        reconnect_delay = RECONNECT_BASE_DELAY;  // reset backoff

        // Subscribe to events
        subscribe_all_events();

        // Enter read loop
        read_loop();

        // Read loop exited — reconnect
        BOOST_LOG(info) << "Middleware disconnected, will reconnect..."sv;
      }
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

}  // namespace middleware
