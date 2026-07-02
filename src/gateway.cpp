/**
 * @file src/gateway.cpp
 * @brief In-process single-port gateway implementation.
 */

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "config.h"
#include "gateway.h"
#include "logging.h"
#include "network.h"
#include "rtsp.h"
#include "stream.h"
#include "utility.h"

using namespace std::literals;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using udp = asio::ip::udp;

namespace gateway {

  using socket_ptr = std::shared_ptr<tcp::socket>;

  constexpr std::size_t UDP_GATEWAY_HEADER_SIZE = 3;
  constexpr std::size_t UDP_MAX_AUDIO_QUEUE_BYTES = 128 * 1024;
  constexpr std::size_t UDP_MAX_AUDIO_QUEUE_PACKETS = 128;
  constexpr std::size_t UDP_MAX_VIDEO_QUEUE_BYTES = 512 * 1024;
  constexpr std::size_t UDP_MAX_VIDEO_QUEUE_PACKETS = 384;
  constexpr std::size_t UDP_CONTROL_WARN_QUEUE_PACKETS = 512;
  constexpr std::uint64_t UDP_DIAG_SAMPLE_LIMIT = 32;
  constexpr std::uint64_t UDP_DIAG_SAMPLE_INTERVAL = 1000;
  constexpr auto UDP_IDLE_STATS_INTERVAL = std::chrono::seconds(1);

  struct udp_queued_packet_t {
    std::uint8_t stream_id;
    udp::endpoint endpoint;
    std::shared_ptr<std::vector<std::uint8_t>> data;
  };

  struct udp_raw_packet_t {
    std::uint8_t stream_id;
    std::shared_ptr<udp::socket> socket;
    udp::endpoint endpoint;
    std::shared_ptr<std::vector<char>> data;
  };

  void close_socket_pair(const socket_ptr &a, const socket_ptr &b) {
    boost::system::error_code ec;
    if (a && a->is_open()) {
      a->shutdown(boost::asio::socket_base::shutdown_type::shutdown_both, ec);
      ec.clear();
      a->close(ec);
    }

    ec.clear();
    if (b && b->is_open()) {
      b->shutdown(boost::asio::socket_base::shutdown_type::shutdown_both, ec);
      ec.clear();
      b->close(ec);
    }
  }

  static std::string g_client_ip;
  static std::map<std::uint8_t, std::uint16_t> g_client_ports;

  std::string get_client_ip() {
    return g_client_ip;
  }

  std::uint16_t get_client_port(std::uint8_t stream_id) {
    auto it = g_client_ports.find(stream_id);
    return it != g_client_ports.end() ? it->second : 0;
  }

  static std::uint16_t udp_target_port(std::uint8_t stream_id) {
    switch (stream_id) {
      case STREAM_VIDEO:
        return net::map_port(stream::VIDEO_STREAM_PORT);
      case STREAM_CONTROL:
        return net::map_port(stream::CONTROL_PORT);
      case STREAM_AUDIO:
        return net::map_port(stream::AUDIO_STREAM_PORT);
      default:
        return 0;
    }
  }

  std::string_view udp_stream_name(std::uint8_t stream_id) {
    switch (stream_id) {
      case STREAM_VIDEO:
        return "video"sv;
      case STREAM_CONTROL:
        return "control"sv;
      case STREAM_AUDIO:
        return "audio"sv;
      default:
        return "unknown"sv;
    }
  }

  std::string udp_endpoint_to_string(const udp::endpoint &endpoint) {
    return endpoint.address().to_string() + ':' + std::to_string(endpoint.port());
  }

  bool should_log_udp_sample(std::uint64_t count) {
    return count <= UDP_DIAG_SAMPLE_LIMIT || count % UDP_DIAG_SAMPLE_INTERVAL == 0;
  }

  class gateway_t {
  public:
    gateway_t():
        ioc {},
        acceptor {ioc},
        udp_socket {ioc},
        udp_stats_timer {ioc},
        udp_public_endpoint {},
        stopping {false} {}

    ~gateway_t() {
      stopping.store(true);
      boost::system::error_code ec;
      acceptor.close(ec);
      udp_socket.close(ec);
      udp_stats_timer.cancel();
      for (auto &[_, socket] : udp_internal_sockets) {
        if (socket && socket->is_open()) {
          socket->close(ec);
          ec.clear();
        }
      }
      ioc.stop();
      if (worker.joinable()) {
        worker.join();
      }
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
    void schedule_udp_stats_timer();

    void handle_tcp_connection(tcp::socket client);

    void start_tls_tunnel(socket_ptr client, std::uint8_t first_byte);
    void pump_tcp(socket_ptr from, socket_ptr to, std::string_view direction);

    void start_framed_tcp(socket_ptr client, std::uint8_t stream_id);
    void do_framed_tcp_read(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, bool skip_stream_id = false);
    void read_framed_length_and_payload(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port);
    void forward_to_internal(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, std::vector<std::uint8_t> payload);

    void start_https_framed_tunnel(socket_ptr client, std::uint16_t target_port);
    void do_https_c2i_read(socket_ptr client, socket_ptr internal, bool skip_stream_id = false);
    void read_length_and_payload(socket_ptr client, socket_ptr internal);
    void do_https_i2c_read(socket_ptr client, socket_ptr internal);

    void start_udp_forwarder(std::uint16_t target_port, std::uint8_t stream_id);
    void enqueue_control_client_send(const udp::endpoint &client_ep, const char *payload, std::size_t payload_len);
    void enqueue_control_internal_send(const std::shared_ptr<udp::socket> &socket, const udp::endpoint &internal_ep, const char *payload, std::size_t payload_len);
    void enqueue_udp_send(std::uint8_t stream_id, const udp::endpoint &client_ep, const char *payload, std::size_t payload_len);
    void record_udp_client(std::uint8_t stream_id, const udp::endpoint &client_ep);
    bool get_udp_client(std::uint8_t stream_id, udp::endpoint &client_ep);
    void mark_control_activity();
    void log_udp_stats();
    bool has_pending_control_out() const;
    bool has_pending_audio_out() const;
    void schedule_udp_client_sends();
    void send_next_control_client_packet();
    void send_next_audio_packet();
    void send_next_video_packet();
    void send_next_control_internal_packet();

    asio::io_context ioc;
    tcp::acceptor acceptor;
    udp::socket udp_socket;
    asio::steady_timer udp_stats_timer;
    udp::endpoint udp_public_endpoint;
    std::thread worker;
    std::atomic<bool> stopping;

    std::map<std::uint8_t, udp::endpoint> udp_clients;
    std::mutex udp_mutex;

    std::map<std::uint8_t, std::shared_ptr<udp::socket>> udp_internal_sockets;

    std::deque<udp_queued_packet_t> udp_control_out_queue;
    std::deque<udp_queued_packet_t> udp_audio_out_queue;
    std::deque<udp_queued_packet_t> udp_video_out_queue;
    std::deque<udp_raw_packet_t> udp_control_in_queue;
    std::size_t udp_audio_out_bytes {};
    std::size_t udp_video_out_bytes {};
    std::uint64_t udp_audio_drop_count {};
    std::uint64_t udp_video_drop_count {};
    std::chrono::steady_clock::time_point udp_last_control_activity {};
    std::chrono::steady_clock::time_point udp_last_stats_log {};
    std::uint64_t udp_control_in_packets {};
    std::uint64_t udp_audio_in_packets {};
    std::uint64_t udp_video_in_packets {};
    std::uint64_t udp_invalid_in_packets {};
    std::uint64_t udp_control_out_packets {};
    std::uint64_t udp_audio_out_packets {};
    std::uint64_t udp_video_out_packets {};
    std::uint64_t udp_control_in_bytes {};
    std::uint64_t udp_audio_in_bytes {};
    std::uint64_t udp_video_in_bytes {};
    std::uint64_t udp_control_out_bytes {};
    std::uint64_t udp_audio_out_payload_bytes {};
    std::uint64_t udp_video_out_payload_bytes {};
    std::size_t udp_control_in_peak {};
    std::size_t udp_control_out_peak {};
    std::size_t udp_audio_out_peak {};
    std::size_t udp_video_out_peak {};
    bool udp_control_out_send_active {};
    bool udp_audio_out_send_active {};
    bool udp_video_out_send_active {};
    bool udp_control_in_send_active {};
  };

  void gateway_t::start_listeners() {
    auto base_port = config::sunshine.port;
    auto address = asio::ip::make_address("0.0.0.0");

    tcp::endpoint tcp_ep {address, base_port};
    acceptor.open(tcp_ep.protocol());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(tcp_ep);
    acceptor.listen(asio::socket_base::max_listen_connections);
    do_accept();

    udp::endpoint udp_ep {address, base_port};
    udp_socket.open(udp_ep.protocol());
    udp_socket.set_option(udp::socket::reuse_address(true));
    udp_socket.bind(udp_ep);
    udp_public_endpoint = udp_socket.local_endpoint();
    BOOST_LOG(info) << "[gateway][udp-listen] public="sv << udp_endpoint_to_string(udp_public_endpoint);
    do_udp_receive();
    schedule_udp_stats_timer();

    start_udp_forwarder(net::map_port(stream::VIDEO_STREAM_PORT), STREAM_VIDEO);
    start_udp_forwarder(net::map_port(stream::CONTROL_PORT), STREAM_CONTROL);
    start_udp_forwarder(net::map_port(stream::AUDIO_STREAM_PORT), STREAM_AUDIO);

    BOOST_LOG(info) << "Gateway listening on 0.0.0.0:"sv << base_port << " (TCP+UDP)"sv;
  }

  void gateway_t::do_accept() {
    acceptor.async_accept([this](boost::system::error_code ec, tcp::socket client) {
      if (ec) {
        if (!stopping.load()) {
          BOOST_LOG(warning) << "[gateway] accept failed: "sv << ec.message();
        }
      } else {
        handle_tcp_connection(std::move(client));
      }
      if (!stopping.load()) {
        do_accept();
      }
    });
  }

  void gateway_t::schedule_udp_stats_timer() {
    udp_stats_timer.expires_after(UDP_IDLE_STATS_INTERVAL);
    udp_stats_timer.async_wait([this](boost::system::error_code ec) {
      if (ec || stopping.load()) {
        return;
      }

      BOOST_LOG(info) << "[gateway][udp-idle] public="sv << udp_endpoint_to_string(udp_public_endpoint)
                      << " clients(video/control/audio)="sv << get_client_port(STREAM_VIDEO) << '/' << get_client_port(STREAM_CONTROL) << '/' << get_client_port(STREAM_AUDIO)
                      << " in(video/control/audio/invalid)="sv << udp_video_in_packets << '/' << udp_control_in_packets << '/' << udp_audio_in_packets << '/'
                      << udp_invalid_in_packets
                      << " out(control/audio/video)="sv << udp_control_out_packets << '/' << udp_audio_out_packets << '/' << udp_video_out_packets
                      << " bytes_in(video/control/audio)="sv << udp_video_in_bytes << '/' << udp_control_in_bytes << '/' << udp_audio_in_bytes;

      schedule_udp_stats_timer();
    });
  }

  void gateway_t::handle_tcp_connection(tcp::socket client) {
    auto client_ptr = std::make_shared<tcp::socket>(std::move(client));
    auto remote_ep = client_ptr->remote_endpoint();

    auto client_ip_str = remote_ep.address().to_string();
    if (client_ip_str != "127.0.0.1" && g_client_ip.empty()) {
      g_client_ip = client_ip_str;
    }
    BOOST_LOG(info) << "[gateway] new TCP connection from "sv << client_ip_str << ':' << remote_ep.port();

    auto buf = std::make_shared<std::uint8_t>();
    client_ptr->async_receive(asio::buffer(buf.get(), 1), [this, client_ptr, buf, remote_ep](boost::system::error_code ec, std::size_t) mutable {
      if (ec) {
        BOOST_LOG(warning) << "[gateway] failed to read first TCP byte: "sv << ec.message()
                           << " (client "sv << remote_ep.address().to_string() << ':' << remote_ep.port() << ')';
        return;
      }
      if (stopping.load()) {
        return;
      }

      std::uint8_t first_byte = *buf;
      if (first_byte == TLS_MAGIC) {
        BOOST_LOG(info) << "[gateway] TLS passthrough from "sv << remote_ep.address().to_string()
                        << ':' << remote_ep.port() << " to nvhttp"sv;
        start_tls_tunnel(client_ptr, first_byte);
      } else if (first_byte >= STREAM_HTTPS && first_byte <= STREAM_RTSP) {
        BOOST_LOG(info) << "[gateway] framed TCP stream 0x"sv << util::hex(first_byte).to_string_view()
                        << " from "sv << remote_ep.address().to_string();
        start_framed_tcp(client_ptr, first_byte);
      } else {
        BOOST_LOG(warning) << "[gateway] unknown TCP first byte 0x"sv
                           << util::hex(first_byte).to_string_view() << ", closing"sv;
      }
    });
  }

  void gateway_t::start_tls_tunnel(socket_ptr client, std::uint8_t first_byte) {
    auto internal = std::make_shared<tcp::socket>(ioc);
    auto remote_ep = client->remote_endpoint();
    auto internal_port = static_cast<std::uint16_t>(config::sunshine.port + INTERNAL_HTTPS_OFFSET);

    internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), internal_port},
      [this, client, internal, first_byte, remote_ep, internal_port](boost::system::error_code ec) {
        if (ec) {
          BOOST_LOG(warning) << "[gateway] TLS tunnel failed to connect nvhttp 127.0.0.1:"sv << internal_port
                             << ": "sv << ec.message() << " (client "sv
                             << remote_ep.address().to_string() << ':' << remote_ep.port() << ')';
          return;
        }

        auto fb = std::make_shared<std::uint8_t>(first_byte);
        asio::async_write(*internal, asio::buffer(fb.get(), 1), [this, client, internal, fb](boost::system::error_code ec, std::size_t) {
          if (ec) {
            BOOST_LOG(warning) << "[gateway] TLS tunnel failed to forward first byte: "sv << ec.message();
            close_socket_pair(client, internal);
            return;
          }
          pump_tcp(client, internal, "client->internal"sv);
          pump_tcp(internal, client, "internal->client"sv);
        });
      }
    );
  }

  void gateway_t::pump_tcp(socket_ptr from, socket_ptr to, std::string_view direction) {
    auto buf = std::make_shared<std::array<char, 65536>>();
    from->async_read_some(asio::buffer(*buf), [this, from, to, buf, direction = std::string(direction)](boost::system::error_code ec, std::size_t n) {
      if (ec == asio::error::eof || n == 0) {
        BOOST_LOG(info) << "[gateway] TCP tunnel closed ("sv << direction << ')';
        close_socket_pair(from, to);
        return;
      }
      if (ec) {
        BOOST_LOG(info) << "[gateway] TCP tunnel read failed ("sv << direction << "): "sv << ec.message();
        close_socket_pair(from, to);
        return;
      }

      asio::async_write(*to, asio::buffer(buf->data(), n), [this, from, to, buf, direction](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(info) << "[gateway] TCP tunnel write failed ("sv << direction << "): "sv << ec.message();
          close_socket_pair(from, to);
          return;
        }
        pump_tcp(from, to, direction);
      });
    });
  }

  void gateway_t::start_framed_tcp(socket_ptr client, std::uint8_t stream_id) {
    std::uint16_t target_port = 0;
    switch (stream_id) {
      case STREAM_HTTPS:
        target_port = config::sunshine.port + INTERNAL_HTTPS_OFFSET;
        break;
      case STREAM_RTSP:
        target_port = net::map_port(rtsp_stream::RTSP_SETUP_PORT);
        break;
      default:
        return;
    }

    if (stream_id == STREAM_HTTPS) {
      start_https_framed_tunnel(client, target_port);
    } else {
      do_framed_tcp_read(client, stream_id, target_port, true);
    }
  }

  void gateway_t::start_https_framed_tunnel(socket_ptr client, std::uint16_t target_port) {
    auto internal = std::make_shared<tcp::socket>(ioc);
    internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), target_port},
      [this, client, internal](boost::system::error_code ec) {
        if (ec) {
          BOOST_LOG(warning) << "[gateway] HTTPS framed tunnel failed to connect nvhttp: "sv << ec.message();
          return;
        }
        do_https_c2i_read(client, internal, true);
        do_https_i2c_read(client, internal);
      }
    );
  }

  void gateway_t::do_https_c2i_read(socket_ptr client, socket_ptr internal, bool skip_stream_id) {
    if (skip_stream_id) {
      read_length_and_payload(client, internal);
      return;
    }

    auto sid_buf = std::make_shared<std::uint8_t>();
    asio::async_read(*client, asio::buffer(sid_buf.get(), 1), [this, client, internal, sid_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        close_socket_pair(client, internal);
        return;
      }

      if (*sid_buf != STREAM_HTTPS) {
        BOOST_LOG(warning) << "[gateway] HTTPS framed tunnel got unexpected stream id 0x"sv
                           << util::hex(*sid_buf).to_string_view();
        close_socket_pair(client, internal);
        return;
      }

      read_length_and_payload(client, internal);
    });
  }

  void gateway_t::read_length_and_payload(socket_ptr client, socket_ptr internal) {
    auto len_buf = std::make_shared<std::array<std::uint8_t, 2>>();
    asio::async_read(*client, asio::buffer(*len_buf), [this, client, internal, len_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        close_socket_pair(client, internal);
        return;
      }

      std::uint16_t payload_len = (static_cast<std::uint16_t>(len_buf->at(0)) << 8) |
                                  static_cast<std::uint16_t>(len_buf->at(1));
      if (payload_len == 0) {
        BOOST_LOG(warning) << "[gateway] HTTPS framed tunnel got empty payload"sv;
        close_socket_pair(client, internal);
        return;
      }

      auto payload = std::make_shared<std::vector<std::uint8_t>>(payload_len);
      asio::async_read(*client, asio::buffer(*payload), [this, client, internal, payload, payload_len](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(info) << "[gateway] HTTPS framed tunnel payload read failed: "sv << ec.message()
                          << " (expected "sv << payload_len << " bytes)"sv;
          close_socket_pair(client, internal);
          return;
        }

        asio::async_write(*internal, asio::buffer(*payload), [this, client, internal, payload](boost::system::error_code ec, std::size_t) {
          if (ec) {
            close_socket_pair(client, internal);
            return;
          }
          do_https_c2i_read(client, internal);
        });
      });
    });
  }

  void gateway_t::do_https_i2c_read(socket_ptr client, socket_ptr internal) {
    auto buf = std::make_shared<std::array<char, 65536>>();
    internal->async_read_some(asio::buffer(*buf), [this, client, internal, buf](boost::system::error_code ec, std::size_t n) {
      if (ec == asio::error::eof || n == 0) {
        close_socket_pair(client, internal);
        return;
      }
      if (ec) {
        BOOST_LOG(info) << "[gateway] HTTPS framed tunnel internal read failed: "sv << ec.message();
        close_socket_pair(client, internal);
        return;
      }

      auto wrapped = std::make_shared<std::vector<std::uint8_t>>();
      wrapped->reserve(UDP_GATEWAY_HEADER_SIZE + n);
      wrapped->push_back(STREAM_HTTPS);
      wrapped->push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
      wrapped->push_back(static_cast<std::uint8_t>(n & 0xFF));
      wrapped->insert(wrapped->end(), buf->data(), buf->data() + n);

      asio::async_write(*client, asio::buffer(*wrapped), [this, client, internal, wrapped](boost::system::error_code ec, std::size_t) {
        if (ec) {
          close_socket_pair(client, internal);
          return;
        }
        do_https_i2c_read(client, internal);
      });
    });
  }

  void gateway_t::do_framed_tcp_read(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, bool skip_stream_id) {
    if (skip_stream_id) {
      read_framed_length_and_payload(client, stream_id, target_port);
      return;
    }

    auto sid_buf = std::make_shared<std::uint8_t>();
    asio::async_read(*client, asio::buffer(sid_buf.get(), 1), [this, client, stream_id, target_port, sid_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        return;
      }
      if (*sid_buf != stream_id) {
        BOOST_LOG(warning) << "[gateway] framed TCP expected stream 0x"sv << util::hex(stream_id).to_string_view()
                           << " but got 0x"sv << util::hex(*sid_buf).to_string_view();
        return;
      }
      read_framed_length_and_payload(client, stream_id, target_port);
    });
  }

  void gateway_t::read_framed_length_and_payload(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port) {
    auto len_buf = std::make_shared<std::array<std::uint8_t, 2>>();
    asio::async_read(*client, asio::buffer(*len_buf), [this, client, stream_id, target_port, len_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        BOOST_LOG(debug) << "[gateway] framed TCP length read failed: "sv << ec.message();
        return;
      }

      std::uint16_t payload_len = (static_cast<std::uint16_t>(len_buf->at(0)) << 8) |
                                  static_cast<std::uint16_t>(len_buf->at(1));
      if (payload_len == 0) {
        BOOST_LOG(warning) << "[gateway] framed TCP got empty payload"sv;
        return;
      }

      auto payload = std::make_shared<std::vector<std::uint8_t>>(payload_len);
      asio::async_read(*client, asio::buffer(*payload), [this, client, stream_id, target_port, payload](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(debug) << "[gateway] framed TCP payload read failed: "sv << ec.message();
          return;
        }
        forward_to_internal(client, stream_id, target_port, std::move(*payload));
      });
    });
  }

  void gateway_t::forward_to_internal(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, std::vector<std::uint8_t> payload) {
    auto internal = std::make_shared<tcp::socket>(ioc);
    auto payload_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
    internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), target_port},
      [this, client, stream_id, target_port, internal, payload_ptr](boost::system::error_code ec) mutable {
        if (ec) {
          BOOST_LOG(debug) << "[gateway] framed TCP failed to connect internal service: "sv << ec.message();
          return;
        }

        asio::async_write(*internal, asio::buffer(*payload_ptr), [this, client, stream_id, target_port, internal, payload_ptr](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            return;
          }

          auto resp = std::make_shared<std::vector<std::uint8_t>>(65536);
          internal->async_read_some(asio::buffer(*resp), [this, client, stream_id, target_port, internal, resp](boost::system::error_code ec, std::size_t n) mutable {
            if (ec || n == 0) {
              return;
            }
            resp->resize(n);

            auto wrapped = std::make_shared<std::vector<std::uint8_t>>();
            wrapped->reserve(UDP_GATEWAY_HEADER_SIZE + n);
            wrapped->push_back(stream_id);
            wrapped->push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
            wrapped->push_back(static_cast<std::uint8_t>(n & 0xFF));
            wrapped->insert(wrapped->end(), resp->begin(), resp->end());

            asio::async_write(*client, asio::buffer(*wrapped), [this, client, stream_id, target_port, wrapped](boost::system::error_code ec, std::size_t) {
              if (ec) {
                return;
              }
              do_framed_tcp_read(client, stream_id, target_port, false);
            });
          });
        });
      }
    );
  }

  void gateway_t::record_udp_client(std::uint8_t stream_id, const udp::endpoint &client_ep) {
    {
      std::lock_guard lock {udp_mutex};
      udp_clients[stream_id] = client_ep;
    }

    g_client_ports[stream_id] = client_ep.port();
    auto client_ip = client_ep.address().to_string();
    if (!client_ep.address().is_loopback() && g_client_ip.empty()) {
      g_client_ip = client_ip;
    }
  }

  bool gateway_t::get_udp_client(std::uint8_t stream_id, udp::endpoint &client_ep) {
    std::lock_guard lock {udp_mutex};
    auto it = udp_clients.find(stream_id);
    if (it == udp_clients.end()) {
      return false;
    }

    client_ep = it->second;
    return true;
  }

  void gateway_t::enqueue_control_client_send(const udp::endpoint &client_ep, const char *payload, std::size_t payload_len) {
    if (payload_len == 0 || payload_len > 0xFFFF) {
      return;
    }

    auto data = std::make_shared<std::vector<std::uint8_t>>();
    data->reserve(UDP_GATEWAY_HEADER_SIZE + payload_len);
    data->push_back(STREAM_CONTROL);
    data->push_back(static_cast<std::uint8_t>((payload_len >> 8) & 0xFF));
    data->push_back(static_cast<std::uint8_t>(payload_len & 0xFF));
    data->insert(data->end(), payload, payload + payload_len);

    mark_control_activity();
    ++udp_control_out_packets;
    udp_control_out_bytes += payload_len;
    udp_control_out_queue.emplace_back(udp_queued_packet_t {STREAM_CONTROL, client_ep, std::move(data)});
    udp_control_out_peak = std::max(udp_control_out_peak, udp_control_out_queue.size());
    log_udp_stats();
    if (udp_control_out_queue.size() == UDP_CONTROL_WARN_QUEUE_PACKETS) {
      BOOST_LOG(warning) << "[gateway] control UDP client queue reached "sv << udp_control_out_queue.size() << " packets"sv;
    }

    schedule_udp_client_sends();
  }

  void gateway_t::enqueue_control_internal_send(const std::shared_ptr<udp::socket> &socket, const udp::endpoint &internal_ep, const char *payload, std::size_t payload_len) {
    if (payload_len == 0) {
      return;
    }

    mark_control_activity();
    auto data = std::make_shared<std::vector<char>>(payload, payload + payload_len);
    udp_control_in_queue.emplace_back(udp_raw_packet_t {STREAM_CONTROL, socket, internal_ep, std::move(data)});
    udp_control_in_peak = std::max(udp_control_in_peak, udp_control_in_queue.size());
    log_udp_stats();
    if (udp_control_in_queue.size() == UDP_CONTROL_WARN_QUEUE_PACKETS) {
      BOOST_LOG(warning) << "[gateway] control UDP internal queue reached "sv << udp_control_in_queue.size() << " packets"sv;
    }

    send_next_control_internal_packet();
  }

  void gateway_t::enqueue_udp_send(std::uint8_t stream_id, const udp::endpoint &client_ep, const char *payload, std::size_t payload_len) {
    if (payload_len == 0 || payload_len > 0xFFFF) {
      return;
    }

    if (stream_id == STREAM_CONTROL) {
      enqueue_control_client_send(client_ep, payload, payload_len);
      return;
    }

    auto data = std::make_shared<std::vector<std::uint8_t>>();
    data->reserve(UDP_GATEWAY_HEADER_SIZE + payload_len);
    data->push_back(stream_id);
    data->push_back(static_cast<std::uint8_t>((payload_len >> 8) & 0xFF));
    data->push_back(static_cast<std::uint8_t>(payload_len & 0xFF));
    data->insert(data->end(), payload, payload + payload_len);

    udp_queued_packet_t packet {stream_id, client_ep, std::move(data)};

    switch (stream_id) {
      case STREAM_AUDIO:
        ++udp_audio_out_packets;
        udp_audio_out_payload_bytes += payload_len;
        udp_audio_out_bytes += packet.data->size();
        udp_audio_out_queue.emplace_back(std::move(packet));
        while (udp_audio_out_queue.size() > UDP_MAX_AUDIO_QUEUE_PACKETS || udp_audio_out_bytes > UDP_MAX_AUDIO_QUEUE_BYTES) {
          udp_audio_out_bytes -= udp_audio_out_queue.front().data->size();
          udp_audio_out_queue.pop_front();
          ++udp_audio_drop_count;
        }
        break;
      case STREAM_VIDEO:
        ++udp_video_out_packets;
        udp_video_out_payload_bytes += payload_len;
        udp_video_out_bytes += packet.data->size();
        udp_video_out_queue.emplace_back(std::move(packet));
        while (udp_video_out_queue.size() > UDP_MAX_VIDEO_QUEUE_PACKETS || udp_video_out_bytes > UDP_MAX_VIDEO_QUEUE_BYTES) {
          udp_video_out_bytes -= udp_video_out_queue.front().data->size();
          udp_video_out_queue.pop_front();
          ++udp_video_drop_count;
        }
        break;
      default:
        return;
    }

    schedule_udp_client_sends();
  }

  void gateway_t::log_udp_stats() {
    auto now = std::chrono::steady_clock::now();
    if (udp_last_stats_log.time_since_epoch().count() != 0 && now - udp_last_stats_log < 1s) {
      return;
    }
    udp_last_stats_log = now;

    auto control_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - udp_last_control_activity).count();
    udp_audio_out_peak = std::max(udp_audio_out_peak, udp_audio_out_queue.size());
    udp_video_out_peak = std::max(udp_video_out_peak, udp_video_out_queue.size());
    BOOST_LOG(info) << "[gateway][udp-stats] in(video/control/audio/invalid)="sv << udp_video_in_packets << '/' << udp_control_in_packets << '/' << udp_audio_in_packets << '/'
                    << udp_invalid_in_packets
                    << " out(control/audio/video)="sv << udp_control_out_packets << '/' << udp_audio_out_packets << '/' << udp_video_out_packets
                    << " drops(audio/video)="sv << udp_audio_drop_count << '/' << udp_video_drop_count
                    << " q(ctrl_in/ctrl_out/audio/video)="sv << udp_control_in_queue.size() << '/' << udp_control_out_queue.size() << '/'
                    << udp_audio_out_queue.size() << '/' << udp_video_out_queue.size()
                    << " peaks(ctrl_in/ctrl_out/audio/video)="sv << udp_control_in_peak << '/' << udp_control_out_peak << '/'
                    << udp_audio_out_peak << '/' << udp_video_out_peak
                    << " bytes_in(video/control/audio)="sv << udp_video_in_bytes << '/' << udp_control_in_bytes << '/' << udp_audio_in_bytes
                    << " bytes_out(control/audio/video)="sv << udp_control_out_bytes << '/' << udp_audio_out_payload_bytes << '/' << udp_video_out_payload_bytes
                    << " active(ctrl/audio/video)="sv << udp_control_out_send_active << '/' << udp_audio_out_send_active << '/' << udp_video_out_send_active
                    << " control_idle_ms="sv << control_idle_ms;
  }

  void gateway_t::mark_control_activity() {
    udp_last_control_activity = std::chrono::steady_clock::now();
  }

  bool gateway_t::has_pending_control_out() const {
    return udp_control_out_send_active || !udp_control_out_queue.empty();
  }

  bool gateway_t::has_pending_audio_out() const {
    return udp_audio_out_send_active || !udp_audio_out_queue.empty();
  }

  void gateway_t::schedule_udp_client_sends() {
    send_next_control_client_packet();
    send_next_audio_packet();
    send_next_video_packet();
  }

  void gateway_t::send_next_control_client_packet() {
    if (udp_control_out_send_active || stopping.load()) {
      return;
    }

    if (udp_control_out_queue.empty()) {
      return;
    }

    auto packet = std::move(udp_control_out_queue.front());
    udp_control_out_queue.pop_front();
    udp_control_out_send_active = true;
    udp_socket.async_send_to(asio::buffer(*packet.data), packet.endpoint, [this, packet](boost::system::error_code ec, std::size_t) mutable {
      udp_control_out_send_active = false;
      if (ec && !stopping.load()) {
        log_udp_stats();
        BOOST_LOG(warning) << "[gateway] control UDP client send failed: "sv << ec.message();
      }
      schedule_udp_client_sends();
    });
  }

  void gateway_t::send_next_audio_packet() {
    if (udp_audio_out_send_active || stopping.load()) {
      return;
    }

    if (has_pending_control_out()) {
      send_next_control_client_packet();
      return;
    }

    if (udp_audio_out_queue.empty()) {
      return;
    }

    auto packet = std::move(udp_audio_out_queue.front());
    udp_audio_out_bytes -= packet.data->size();
    udp_audio_out_queue.pop_front();
    udp_audio_out_send_active = true;
    udp_socket.async_send_to(asio::buffer(*packet.data), packet.endpoint, [this, packet](boost::system::error_code ec, std::size_t) mutable {
      udp_audio_out_send_active = false;
      if (ec && !stopping.load()) {
        log_udp_stats();
        BOOST_LOG(warning) << "[gateway] audio UDP client send failed: "sv << ec.message();
      }
      schedule_udp_client_sends();
    });
  }

  void gateway_t::send_next_video_packet() {
    if (udp_video_out_send_active || stopping.load()) {
      return;
    }

    if (has_pending_control_out()) {
      send_next_control_client_packet();
      return;
    }

    if (has_pending_audio_out()) {
      send_next_audio_packet();
      return;
    }

    if (udp_video_out_queue.empty()) {
      return;
    }

    auto packet = std::move(udp_video_out_queue.front());
    udp_video_out_bytes -= packet.data->size();
    udp_video_out_queue.pop_front();
    udp_video_out_send_active = true;
    udp_socket.async_send_to(asio::buffer(*packet.data), packet.endpoint, [this, packet](boost::system::error_code ec, std::size_t) mutable {
      udp_video_out_send_active = false;
      if (ec && !stopping.load()) {
        log_udp_stats();
        BOOST_LOG(warning) << "[gateway] video UDP client send failed: "sv << ec.message();
      }
      schedule_udp_client_sends();
    });
  }

  void gateway_t::send_next_control_internal_packet() {
    if (udp_control_in_send_active || stopping.load()) {
      return;
    }

    if (udp_control_in_queue.empty()) {
      return;
    }

    auto packet = std::move(udp_control_in_queue.front());
    udp_control_in_queue.pop_front();
    udp_control_in_send_active = true;
    auto packet_count = udp_control_in_packets;
    if (should_log_udp_sample(packet_count)) {
      BOOST_LOG(info) << "[gateway][udp-in-forward] queued stream=control payload="sv << packet.data->size()
                      << " to="sv << udp_endpoint_to_string(packet.endpoint)
                      << " via="sv << udp_endpoint_to_string(packet.socket->local_endpoint());
    }
    packet.socket->async_send_to(asio::buffer(*packet.data), packet.endpoint, [this, packet, packet_count](boost::system::error_code ec, std::size_t bytes_sent) mutable {
      udp_control_in_send_active = false;
      if (ec && !stopping.load()) {
        log_udp_stats();
        BOOST_LOG(warning) << "[gateway][udp-in-forward] failed stream=control payload="sv << packet.data->size()
                           << " to="sv << udp_endpoint_to_string(packet.endpoint)
                           << " via="sv << udp_endpoint_to_string(packet.socket->local_endpoint())
                           << " err="sv << ec.message();
      } else if (should_log_udp_sample(packet_count)) {
        BOOST_LOG(info) << "[gateway][udp-in-forward] ok stream=control sent="sv << bytes_sent
                        << " to="sv << udp_endpoint_to_string(packet.endpoint)
                        << " via="sv << udp_endpoint_to_string(packet.socket->local_endpoint());
      }
      send_next_control_internal_packet();
    });
  }

  void gateway_t::start_udp_forwarder(std::uint16_t target_port, std::uint8_t stream_id) {
    if (stream_id == STREAM_CONTROL) {
      auto sock = std::make_shared<udp::socket>(ioc);
      sock->open(udp::v4());
      sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), 0});

      BOOST_LOG(info) << "[gateway] UDP forwarder stream=0x03 (control) internal 127.0.0.1:"sv << sock->local_endpoint().port();

      auto buf = std::make_shared<std::array<char, 65507>>();
      auto do_read = std::make_shared<std::function<void()>>();
      *do_read = [this, sock, buf, do_read]() mutable {
        sock->async_receive(asio::buffer(*buf), [this, sock, buf, do_read](boost::system::error_code ec, std::size_t n) mutable {
          if (ec) {
            if (!stopping.load()) {
              (*do_read)();
            }
            return;
          }
          if (n == 0) {
            (*do_read)();
            return;
          }

          udp::endpoint client_ep;
          if (!get_udp_client(STREAM_CONTROL, client_ep)) {
            BOOST_LOG(info) << "[gateway][udp-out-drop] stream=control reason=no_client len="sv << n;
            (*do_read)();
            return;
          }

          if (should_log_udp_sample(udp_control_out_packets + 1)) {
            BOOST_LOG(info) << "[gateway][udp-out] stream=control payload="sv << n
                            << " to="sv << udp_endpoint_to_string(client_ep)
                            << " from_internal="sv << udp_endpoint_to_string(sock->local_endpoint());
          }
          enqueue_control_client_send(client_ep, buf->data(), n);
          (*do_read)();
        });
      };
      (*do_read)();
      udp_internal_sockets[STREAM_CONTROL] = sock;
      return;
    }

    auto out_sock = std::make_shared<udp::socket>(ioc);
    out_sock->open(udp::v4());
    auto fwd_port = static_cast<std::uint16_t>(target_port + UDP_FWD_OFFSET);
    out_sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), fwd_port});

    auto in_sock = std::make_shared<udp::socket>(ioc);
    in_sock->open(udp::v4());
    in_sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), 0});

    BOOST_LOG(info) << "[gateway] UDP forwarder stream=0x"sv << util::hex(stream_id).to_string_view()
                    << " outbound 127.0.0.1:"sv << fwd_port
                    << " inbound 127.0.0.1:"sv << in_sock->local_endpoint().port();

    auto out_buf = std::make_shared<std::array<char, 65507>>();
    auto do_read = std::make_shared<std::function<void()>>();
    *do_read = [this, out_sock, out_buf, stream_id, do_read]() mutable {
      out_sock->async_receive(asio::buffer(*out_buf), [this, out_sock, out_buf, stream_id, do_read](boost::system::error_code ec, std::size_t n) mutable {
        if (ec) {
          if (!stopping.load()) {
            (*do_read)();
          }
          return;
        }
        if (n == 0) {
          (*do_read)();
          return;
        }

        udp::endpoint client_ep;
        if (!get_udp_client(stream_id, client_ep)) {
          BOOST_LOG(info) << "[gateway][udp-out-drop] stream="sv << udp_stream_name(stream_id)
                          << " reason=no_client len="sv << n;
          (*do_read)();
          return;
        }

        auto out_count = stream_id == STREAM_AUDIO ? udp_audio_out_packets + 1 : udp_video_out_packets + 1;
        if (should_log_udp_sample(out_count)) {
          BOOST_LOG(info) << "[gateway][udp-out] stream="sv << udp_stream_name(stream_id)
                          << " payload="sv << n
                          << " to="sv << udp_endpoint_to_string(client_ep)
                          << " from_internal="sv << udp_endpoint_to_string(out_sock->local_endpoint());
        }
        enqueue_udp_send(stream_id, client_ep, out_buf->data(), n);
        (*do_read)();
      });
    };
    (*do_read)();

    udp_internal_sockets[stream_id] = in_sock;
  }

  void gateway_t::do_udp_receive() {
    auto buf = std::make_shared<std::array<char, 65507>>();
    auto sender = std::make_shared<udp::endpoint>();
    udp_socket.async_receive_from(asio::buffer(*buf), *sender, [this, buf, sender](boost::system::error_code ec, std::size_t n) {
      if (stopping.load()) {
        return;
      }
      if (ec) {
        BOOST_LOG(warning) << "[gateway] UDP receive failed: code="sv << ec.value() << " message="sv << ec.message();
        do_udp_receive();
        return;
      }

      if (n < UDP_GATEWAY_HEADER_SIZE) {
        ++udp_invalid_in_packets;
        BOOST_LOG(info) << "[gateway] UDP datagram too short ("sv << n << " bytes), dropping from "sv << udp_endpoint_to_string(*sender);
        do_udp_receive();
        return;
      }

      std::uint8_t stream_id = static_cast<std::uint8_t>(buf->at(0));
      std::uint16_t payload_len = (static_cast<std::uint16_t>(buf->at(1)) << 8) |
                                  static_cast<std::uint16_t>(buf->at(2));

      if (payload_len > n - UDP_GATEWAY_HEADER_SIZE) {
        ++udp_invalid_in_packets;
        BOOST_LOG(warning) << "[gateway][udp-in-drop] reason=bad_length stream=0x"sv << util::hex(stream_id).to_string_view()
                           << " declared="sv << payload_len
                           << " available="sv << (n - UDP_GATEWAY_HEADER_SIZE)
                           << " datagram="sv << n
                           << " from="sv << udp_endpoint_to_string(*sender);
        do_udp_receive();
        return;
      }

      if (payload_len == 0) {
        ++udp_invalid_in_packets;
        BOOST_LOG(info) << "[gateway][udp-in-drop] reason=empty stream=0x"sv << util::hex(stream_id).to_string_view()
                        << " from="sv << udp_endpoint_to_string(*sender);
        do_udp_receive();
        return;
      }

      auto target_port = udp_target_port(stream_id);
      if (target_port == 0) {
        ++udp_invalid_in_packets;
        BOOST_LOG(warning) << "[gateway] unknown UDP stream_id 0x"sv << util::hex(stream_id).to_string_view()
                           << " len="sv << payload_len
                           << " from="sv << udp_endpoint_to_string(*sender);
        do_udp_receive();
        return;
      }

      auto it = udp_internal_sockets.find(stream_id);
      if (it == udp_internal_sockets.end() || !it->second) {
        ++udp_invalid_in_packets;
        BOOST_LOG(warning) << "[gateway] no internal UDP socket for stream_id 0x"sv << util::hex(stream_id).to_string_view()
                           << " len="sv << payload_len
                           << " from="sv << udp_endpoint_to_string(*sender);
        do_udp_receive();
        return;
      }

      record_udp_client(stream_id, *sender);
      udp::endpoint internal_ep {asio::ip::make_address("127.0.0.1"), target_port};
      auto internal_socket = it->second;
      auto payload_copy = std::make_shared<std::vector<char>>(
        buf->data() + UDP_GATEWAY_HEADER_SIZE,
        buf->data() + UDP_GATEWAY_HEADER_SIZE + payload_len
      );

      std::uint64_t inbound_count = 0;
      switch (stream_id) {
        case STREAM_CONTROL:
          inbound_count = ++udp_control_in_packets;
          udp_control_in_bytes += payload_len;
          break;
        case STREAM_AUDIO:
          inbound_count = ++udp_audio_in_packets;
          udp_audio_in_bytes += payload_len;
          break;
        case STREAM_VIDEO:
          inbound_count = ++udp_video_in_packets;
          udp_video_in_bytes += payload_len;
          break;
        default:
          break;
      }

      auto trailing_bytes = n - UDP_GATEWAY_HEADER_SIZE - payload_len;
      if (should_log_udp_sample(inbound_count)) {
        BOOST_LOG(info) << "[gateway][udp-in] stream="sv << udp_stream_name(stream_id)
                        << " sid=0x"sv << util::hex(stream_id).to_string_view()
                        << " datagram="sv << n
                        << " payload="sv << payload_len
                        << " trailing="sv << trailing_bytes
                        << " from="sv << udp_endpoint_to_string(*sender)
                        << " to_internal="sv << udp_endpoint_to_string(internal_ep)
                        << " via="sv << udp_endpoint_to_string(internal_socket->local_endpoint());
      }
      if (trailing_bytes != 0) {
        ++udp_invalid_in_packets;
        BOOST_LOG(warning) << "[gateway][udp-in] trailing bytes ignored stream="sv << udp_stream_name(stream_id)
                           << " trailing="sv << trailing_bytes
                           << " datagram="sv << n
                           << " payload="sv << payload_len
                           << " from="sv << udp_endpoint_to_string(*sender);
      }
      log_udp_stats();

      do_udp_receive();

      if (stream_id == STREAM_CONTROL) {
        enqueue_control_internal_send(internal_socket, internal_ep, payload_copy->data(), payload_copy->size());
      } else {
        internal_socket->async_send_to(
          asio::buffer(*payload_copy),
          internal_ep,
          [payload_copy, stream_id_capture = stream_id, internal_ep, local_ep = internal_socket->local_endpoint(), inbound_count](boost::system::error_code ec, std::size_t bytes_sent) {
            if (ec) {
              BOOST_LOG(warning) << "[gateway][udp-in-forward] failed stream="sv << udp_stream_name(stream_id_capture)
                                 << " payload="sv << payload_copy->size()
                                 << " to="sv << udp_endpoint_to_string(internal_ep)
                                 << " via="sv << udp_endpoint_to_string(local_ep)
                                 << " err="sv << ec.message();
              return;
            }
            if (should_log_udp_sample(inbound_count)) {
              BOOST_LOG(info) << "[gateway][udp-in-forward] ok stream="sv << udp_stream_name(stream_id_capture)
                              << " sent="sv << bytes_sent
                              << " to="sv << udp_endpoint_to_string(internal_ep)
                              << " via="sv << udp_endpoint_to_string(local_ep);
            }
          }
        );
      }
    });
  }

  class deinit_t: public platf::deinit_t {
  public:
    deinit_t(std::unique_ptr<gateway_t> &&impl):
        impl {std::move(impl)} {}

    ~deinit_t() override {
      impl.reset();
    }

  private:
    std::unique_ptr<gateway_t> impl;
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> start() {
    auto impl = std::make_unique<gateway_t>();
    impl->run();
    return std::make_unique<deinit_t>(std::move(impl));
  }

}  // namespace gateway
