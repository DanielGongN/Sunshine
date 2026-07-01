/**
 * @file src/gateway.cpp
 * @brief 进程内端口复用网关实现。
 */

// 标准库
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// 第三方库
#include <boost/asio.hpp>

// 本地头文件
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

  // 共享 socket 别名，用于异步操作中的所有权管理
  using socket_ptr = std::shared_ptr<tcp::socket>;

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

  // 第一个外部客户端 IP + UDP 端口（由 handle_tcp_connection / do_udp_receive 记录）
  static std::string g_client_ip;
  static std::map<std::uint8_t, std::uint16_t> g_client_ports;

  std::string get_client_ip() {
    return g_client_ip;
  }

  std::uint16_t get_client_port(std::uint8_t stream_id) {
    auto it = g_client_ports.find(stream_id);
    return it != g_client_ports.end() ? it->second : 0;
  }

  // 将 stream_id 映射到内部 UDP 目标端口
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

  class gateway_t {
  public:
    gateway_t():
        ioc {},
        acceptor {ioc},
        udp_socket {ioc},
        stopping {false} {}

    ~gateway_t() {
      stopping.store(true);
      boost::system::error_code ec;
      acceptor.close(ec);
      udp_socket.close(ec);
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
          BOOST_LOG(fatal) << "网关异常终止: "sv << e.what();
        }
      });
    }

  private:
    void start_listeners();
    void do_accept();
    void do_udp_receive();

    // TCP 会话：窥探首字节，然后决定走隧道还是代理
    void handle_tcp_connection(tcp::socket client);

    // TLS 原始隧道（首字节 0x16）
    void start_tls_tunnel(socket_ptr client, std::uint8_t first_byte);
    void start_bidir_relay(socket_ptr a, socket_ptr b);

    // 带帧头的 TCP 代理（0x01, 0x05）
    void start_framed_tcp(socket_ptr client, std::uint8_t stream_id);
    void do_framed_tcp_read(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, bool skip_stream_id = false);
    void read_framed_length_and_payload(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port);
    void forward_to_internal(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, std::vector<std::uint8_t> payload);

    // HTTPS 帧隧道（0x01 持久版，不复用 forward_to_internal 的短连接模式）
    void start_https_framed_tunnel(socket_ptr client, std::uint16_t target_port);
    void do_https_c2i_read(socket_ptr client, socket_ptr internal, bool skip_stream_id = false);
    void read_length_and_payload(socket_ptr client, socket_ptr internal);
    void do_https_i2c_read(socket_ptr client, socket_ptr internal);

    // UDP 转发器
    void start_udp_forwarder(std::uint16_t target_port, std::uint8_t stream_id);

    asio::io_context ioc;
    tcp::acceptor acceptor;
    udp::socket udp_socket;
    std::thread worker;
    std::atomic<bool> stopping;

    // UDP 客户端追踪：记录每个 stream_id 最后一次收到的外部端点
    std::map<std::uint8_t, udp::endpoint> udp_clients;
    std::mutex udp_mutex;

    // 内部 UDP 转发专用的临时端口 socket
    std::map<std::uint8_t, std::shared_ptr<udp::socket>> udp_internal_sockets;
  };

  // ========== 启动监听 ==========

  void gateway_t::start_listeners() {
    auto base_port = config::sunshine.port;
    auto address = asio::ip::make_address("0.0.0.0");

    // TCP 接收器
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

    // 启动内部 UDP 转发器，用于接收 Sunshine 的回包
    start_udp_forwarder(net::map_port(stream::VIDEO_STREAM_PORT), STREAM_VIDEO);
    start_udp_forwarder(net::map_port(stream::CONTROL_PORT), STREAM_CONTROL);
    start_udp_forwarder(net::map_port(stream::AUDIO_STREAM_PORT), STREAM_AUDIO);

    BOOST_LOG(info) << "网关监听中 0.0.0.0:"sv << base_port << " (TCP+UDP)"sv;
  }

  // ========== TCP 接受循环 ==========

  void gateway_t::do_accept() {
    acceptor.async_accept([this](boost::system::error_code ec, tcp::socket client) {
      if (ec) {
        BOOST_LOG(warning) << "[网关] accept错误: "sv << ec.message();
      } else {
        handle_tcp_connection(std::move(client));
      }
      if (!stopping.load()) {
        do_accept();
      }
    });
  }

  // ========== TCP 首字节检测与分流 ==========

  void gateway_t::handle_tcp_connection(tcp::socket client) {
    // 先用 shared_ptr 接管 socket，避免 lambda 捕获时 move 导致 async_receive 操作在空句柄上执行。
    auto client_ptr = std::make_shared<tcp::socket>(std::move(client));
    auto remote_ep = client_ptr->remote_endpoint();

    auto client_ip_str = remote_ep.address().to_string();
    if (client_ip_str != "127.0.0.1" && g_client_ip.empty()) {
      g_client_ip = client_ip_str;
    }
    BOOST_LOG(info) << "[网关] 新TCP连接: "sv << client_ip_str
                    << ':' << remote_ep.port();

    auto buf = std::make_shared<std::uint8_t>();
    client_ptr->async_receive(asio::buffer(buf.get(), 1), [this, client_ptr, buf, remote_ep](boost::system::error_code ec, std::size_t) mutable {
      if (ec) {
        BOOST_LOG(warning) << "[网关] 首字节读取失败: code="sv << ec.value()
                           << " (客户端="sv << remote_ep.address().to_string()
                           << ':' << remote_ep.port() << ')';
        return;
      }
      if (stopping.load()) {
        return;
      }

      std::uint8_t first_byte = *buf;
      if (first_byte == TLS_MAGIC) {
        BOOST_LOG(info) << "[网关] 检测到 TLS 连接(首字节0x16), 对端: "sv
                        << remote_ep.address().to_string()
                        << ':' << remote_ep.port()
                        << " → 隧道转发至 nvhttp"sv;
        start_tls_tunnel(client_ptr, first_byte);
      } else if (first_byte >= STREAM_HTTPS && first_byte <= STREAM_RTSP) {
        BOOST_LOG(info) << "[网关] 检测到带帧协议 0x"sv
                        << util::hex(first_byte).to_string_view()
                        << " 对端: "sv << remote_ep.address().to_string();
        start_framed_tcp(client_ptr, first_byte);
      } else {
        BOOST_LOG(warning) << "[网关] 未知首字节 0x"sv
                           << util::hex(first_byte).to_string_view()
                           << " 对端: "sv << remote_ep.address().to_string()
                           << " -- 丢弃连接"sv;
      }
    });
  }

  // ========== TLS 原始隧道 (0x16) ==========

  void gateway_t::start_tls_tunnel(socket_ptr client, std::uint8_t first_byte) {
    auto internal = std::make_shared<tcp::socket>(ioc);
    auto remote_ep = client->remote_endpoint();
    auto internal_port = (std::uint16_t) (config::sunshine.port + INTERNAL_HTTPS_OFFSET);

    BOOST_LOG(info) << "[网关] TLS隧道: 连接内部 nvhttp 127.0.0.1:"sv << internal_port;

    internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), internal_port},
      [this, client, internal, first_byte, remote_ep](boost::system::error_code ec) {
        if (ec) {
          BOOST_LOG(warning) << "[网关] TLS隧道: 连接内部 nvhttp 失败: "sv << ec.message()
                             << " (客户端: "sv << remote_ep.address().to_string() << ':' << remote_ep.port() << ')';
          return;
        }

        BOOST_LOG(info) << "[网关] TLS隧道已建立: 客户端 "sv << remote_ep.address().to_string()
                        << ':' << remote_ep.port()
                        << " ↔ nvhttp 127.0.0.1:"sv << internal->remote_endpoint().port()
                        << " -- 开始双向中继"sv;

        // 将已窥探的首字节先发给 nvhttp，然后启动双向中继
        // first_byte 用 shared_ptr 管理，确保异步写回调中 buffer 仍然有效
        auto fb = std::make_shared<std::uint8_t>(first_byte);
        asio::async_write(*internal, asio::buffer(fb.get(), 1), [this, client, internal, fb](boost::system::error_code ec, std::size_t) {
          if (ec) {
            BOOST_LOG(warning) << "[网关] TLS隧道: 首字节写入失败: "sv << ec.message();
            return;
          }
          start_bidir_relay(client, internal);
        });
      }
    );
  }

  void gateway_t::start_bidir_relay(socket_ptr a, socket_ptr b) {
    // 客户端 → 内部服务
    auto buf_c2i = std::make_shared<std::array<char, 65536>>();
    a->async_read_some(asio::buffer(*buf_c2i), [this, a, b, buf_c2i](boost::system::error_code ec, std::size_t n) {
      if (ec == asio::error::eof || n == 0) {
        BOOST_LOG(info) << "[网关] 中继: 客户端主动关闭连接(EOF)"sv;
        return;
      }
      if (ec) {
        BOOST_LOG(info) << "[网关] 中继: 客户端→内部 读错误: "sv << ec.message();
        return;
      }
      asio::async_write(*b, asio::buffer(buf_c2i->data(), n), [this, a, b, buf_c2i](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(info) << "[网关] 中继: 客户端→内部 写错误: "sv << ec.message();
          return;
        }
        start_bidir_relay(a, b);
      });
    });

    // 内部服务 → 客户端
    auto buf_i2c = std::make_shared<std::array<char, 65536>>();
    b->async_read_some(asio::buffer(*buf_i2c), [this, a, b, buf_i2c](boost::system::error_code ec, std::size_t n) {
      if (ec == asio::error::eof || n == 0) {
        BOOST_LOG(info) << "[网关] 中继: nvhttp 内部服务关闭连接(EOF)"sv;
        return;
      }
      if (ec) {
        BOOST_LOG(info) << "[网关] 中继: 内部→客户端 读错误: "sv << ec.message();
        return;
      }
      asio::async_write(*a, asio::buffer(buf_i2c->data(), n), [this, a, b, buf_i2c](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(info) << "[网关] 中继: 内部→客户端 写错误: "sv << ec.message();
          return;
        }
        start_bidir_relay(a, b);
      });
    });
  }

  // ========== 带帧头的 TCP 代理 (0x01, 0x05) ==========

  void gateway_t::start_framed_tcp(socket_ptr client, std::uint8_t stream_id) {
    // 将 stream_id 映射到内部目标端口
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
      // HTTPS 需要持久隧道 —— TLS 握手是多轮往返的，不能在每帧新建 TCP 连接。
      // 改为: 连接一次 nvhttp，然后 framed(client) ↔ raw(internal) 双向中继。
      BOOST_LOG(info) << "[网关] HTTPS 帧隧道: 使用持久连接模式"sv;
      start_https_framed_tunnel(client, target_port);
    } else {
      do_framed_tcp_read(client, stream_id, target_port, true);  // 首帧 stream_id 已消费
    }
  }

  // ========== HTTPS 帧隧道 (0x01 持久版) ==========
  // 与 forward_to_internal 不同，这里只连接一次 nvhttp，
  // 然后把 client 侧的带帧流量和 internal 侧的原始 TCP 流量做双向中继。

  void gateway_t::start_https_framed_tunnel(socket_ptr client, std::uint16_t target_port) {
    auto internal = std::make_shared<tcp::socket>(ioc);
    internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), target_port},
      [this, client, internal](boost::system::error_code ec) {
        if (ec) {
          BOOST_LOG(warning) << "[网关] HTTPS帧隧道: 连接 nvhttp 失败: "sv << ec.message();
          return;
        }
        BOOST_LOG(info) << "[网关] HTTPS帧隧道已建立 → 开始双向中继 (framed↔raw)"sv;
        do_https_c2i_read(client, internal, true);  // 首帧 stream_id 已在 handle_tcp_connection 消费
        do_https_i2c_read(client, internal);
      }
    );
  }

  // Client → Internal: 读取带帧数据 [stream_id:1B][len:2B BE][payload]，剥离头部后原始写入 nvhttp
  // skip_stream_id=true: 首帧 stream_id 已被 handle_tcp_connection 消费，直接读长度
  void gateway_t::do_https_c2i_read(socket_ptr client, socket_ptr internal, bool skip_stream_id) {
    if (skip_stream_id) {
      read_length_and_payload(client, internal);
      return;
    }

    auto sid_buf = std::make_shared<std::uint8_t>();
    asio::async_read(*client, asio::buffer(sid_buf.get(), 1), [this, client, internal, sid_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        if (!stopping.load() && ec != asio::error::eof) {
          BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: failed to read stream id (C->I): "sv << ec.message();
        } else {
          BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: client closed (C->I)"sv;
        }
        close_socket_pair(client, internal);
        return;
      }

      if (*sid_buf != STREAM_HTTPS) {
        BOOST_LOG(warning) << "[gateway] HTTPS framed tunnel: unexpected stream id 0x"sv
                           << util::hex(*sid_buf).to_string_view() << ", closing"sv;
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
        BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: failed to read length (C->I): "sv << ec.message();
        close_socket_pair(client, internal);
        return;
      }

      std::uint16_t payload_len = (static_cast<std::uint16_t>(len_buf->at(0)) << 8) |
                                  static_cast<std::uint16_t>(len_buf->at(1));
      if (payload_len == 0) {
        BOOST_LOG(warning) << "[gateway] HTTPS framed tunnel: invalid empty payload, closing"sv;
        close_socket_pair(client, internal);
        return;
      }

      auto payload = std::make_shared<std::vector<std::uint8_t>>(payload_len);
      asio::async_read(*client, asio::buffer(*payload), [this, client, internal, payload, payload_len](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: failed to read payload (C->I): "sv << ec.message()
                          << " (expected "sv << payload_len << " bytes)"sv;
          close_socket_pair(client, internal);
          return;
        }

        BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: forwarding "sv << payload_len << " bytes to nvhttp"sv;
        asio::async_write(*internal, asio::buffer(*payload), [this, client, internal, payload](boost::system::error_code ec, std::size_t) {
          if (ec) {
            BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: failed to write nvhttp (C->I): "sv << ec.message();
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
        BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: nvhttp closed (I->C, EOF)"sv;
        close_socket_pair(client, internal);
        return;
      }
      if (ec) {
        BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: failed to read nvhttp (I->C): "sv << ec.message();
        close_socket_pair(client, internal);
        return;
      }

      BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: received "sv << n << " bytes from nvhttp"sv;

      auto wrapped = std::make_shared<std::vector<std::uint8_t>>();
      wrapped->reserve(3 + n);
      wrapped->push_back(STREAM_HTTPS);
      wrapped->push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
      wrapped->push_back(static_cast<std::uint8_t>(n & 0xFF));
      wrapped->insert(wrapped->end(), buf->data(), buf->data() + n);

      asio::async_write(*client, asio::buffer(*wrapped), [this, client, internal, wrapped](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(info) << "[gateway] HTTPS framed tunnel: failed to write client (I->C): "sv << ec.message();
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

    // 读 1 字节 stream_id，然后读长度+payload
    auto sid_buf = std::make_shared<std::uint8_t>();
    asio::async_read(*client, asio::buffer(sid_buf.get(), 1), [this, client, stream_id, target_port, sid_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        if (!stopping.load() && ec != asio::error::eof) {
          BOOST_LOG(debug) << "[网关] 帧TCP: 读stream_id失败: "sv << ec.message();
        }
        return;
      }
      read_framed_length_and_payload(client, stream_id, target_port);
    });
  }

  // 读 2 字节长度头 → 读 payload → 转发到内部服务
  void gateway_t::read_framed_length_and_payload(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port) {
    auto len_buf = std::make_shared<std::array<std::uint8_t, 2>>();
    asio::async_read(*client, asio::buffer(*len_buf), [this, client, stream_id, target_port, len_buf](boost::system::error_code ec, std::size_t) {
      if (ec) {
        BOOST_LOG(warning) << "[网关] 帧TCP: 读长度失败: "sv << ec.message();
        return;
      }

      std::uint16_t payload_len = (static_cast<std::uint16_t>(len_buf->at(0)) << 8) | static_cast<std::uint16_t>(len_buf->at(1));

      if (payload_len == 0 || payload_len > 65535) {
        BOOST_LOG(warning) << "[网关] 帧TCP: 非法payload长度 "sv << payload_len << " -- 关闭连接"sv;
        return;
      }

      auto payload = std::make_shared<std::vector<std::uint8_t>>(payload_len);
      asio::async_read(*client, asio::buffer(*payload), [this, client, stream_id, target_port, payload, payload_len](boost::system::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(warning) << "[网关] 帧TCP: 读payload失败: "sv << ec.message();
          return;
        }
        BOOST_LOG(debug) << "[网关] 帧TCP(0x"sv << util::hex(stream_id).to_string_view()
                         << "): 转发 "sv << payload_len << " 字节到内部端口 "sv << target_port;
        forward_to_internal(client, stream_id, target_port, std::move(*payload));
      });
    });
  }

  void gateway_t::forward_to_internal(socket_ptr client, std::uint8_t stream_id, std::uint16_t target_port, std::vector<std::uint8_t> payload) {
    auto internal = std::make_shared<tcp::socket>(ioc);
    // 使用 shared_ptr 管理 payload 生命周期，确保异步回调中数据有效
    auto payload_ptr = std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
    internal->async_connect(
      tcp::endpoint {asio::ip::make_address("127.0.0.1"), target_port},
      [this, client, stream_id, target_port, internal, payload_ptr](boost::system::error_code ec) mutable {
        if (ec) {
          BOOST_LOG(debug) << "网关带帧 TCP: 连接内部服务失败: "sv << ec.message();
          return;
        }

        // 将 payload 转发给内部服务
        asio::async_write(*internal, asio::buffer(*payload_ptr), [this, client, stream_id, target_port, internal, payload_ptr](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            return;
          }

          // 从内部服务读取响应
          auto resp = std::make_shared<std::vector<std::uint8_t>>(65536);
          internal->async_read_some(asio::buffer(*resp), [this, client, stream_id, target_port, internal, resp](boost::system::error_code ec, std::size_t n) mutable {
            if (ec || n == 0) {
              return;
            }
            resp->resize(n);

            // 包装网关协议头并发送给客户端
            // 使用 shared_ptr 确保 wrapped 数据在异步写操作期间有效
            auto wrapped = std::make_shared<std::vector<std::uint8_t>>();
            wrapped->reserve(3 + n);
            wrapped->push_back(stream_id);
            wrapped->push_back((n >> 8) & 0xFF);
            wrapped->push_back(n & 0xFF);
            wrapped->insert(wrapped->end(), resp->begin(), resp->end());

            asio::async_write(*client, asio::buffer(*wrapped), [this, client, stream_id, target_port, wrapped](boost::system::error_code ec, std::size_t) {
              if (ec) {
                return;
              }
              // 继续从客户端读取下一条带帧消息（后续帧含 stream_id）
              do_framed_tcp_read(client, stream_id, target_port, false);
            });
          });
        });
      }
    );
  }

  // ========== UDP 解复用 ==========

  void gateway_t::start_udp_forwarder(std::uint16_t target_port, std::uint8_t stream_id) {
    if (stream_id == STREAM_CONTROL) {
      // 控制流：ENET peer 由收到 ping 的源地址决定，必须和出方向同一端口。
      // 用单 socket 同时收发，async_send_to 冲突时自动重启读。
      auto sock = std::make_shared<udp::socket>(ioc);
      sock->open(udp::v4());
      sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), 0});

      BOOST_LOG(info) << "[网关] UDP转发器: stream=0x03 (控制) 127.0.0.1:"sv << sock->local_endpoint().port();

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

          std::array<std::uint8_t, 3> header {
            STREAM_CONTROL,
            static_cast<std::uint8_t>((n >> 8) & 0xFF),
            static_cast<std::uint8_t>(n & 0xFF)
          };
          udp::endpoint client_ep;
          {
            std::lock_guard lock {udp_mutex};
            auto it = udp_clients.find(STREAM_CONTROL);
            if (it == udp_clients.end()) {
              (*do_read)();
              return;
            }
            client_ep = it->second;
          }
          std::vector<asio::const_buffer> buffers {
            asio::buffer(header),
            asio::buffer(buf->data(), n)
          };
          udp_socket.async_send_to(buffers, client_ep, [do_read](boost::system::error_code, std::size_t) {
            (*do_read)();
          });
        });
      };
      (*do_read)();
      udp_internal_sockets[STREAM_CONTROL] = sock;
      return;
    }

    // 视频/音频：出方向 socket 绑定固定偏移端口，只读 Sunshine 输出
    auto out_sock = std::make_shared<udp::socket>(ioc);
    out_sock->open(udp::v4());
    auto fwd_port = (std::uint16_t) (target_port + UDP_FWD_OFFSET);
    out_sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), fwd_port});

    // 入方向：随机端口，do_udp_receive 通过此发送 Moonlight 数据给 Sunshine
    auto in_sock = std::make_shared<udp::socket>(ioc);
    in_sock->open(udp::v4());
    in_sock->bind(udp::endpoint {asio::ip::make_address("127.0.0.1"), 0});

    BOOST_LOG(info) << "[网关] UDP转发器: stream=0x"sv << util::hex(stream_id).to_string_view()
                    << " 出 127.0.0.1:"sv << fwd_port << " 入端口="sv << in_sock->local_endpoint().port();

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

        std::array<std::uint8_t, 3> header {
          stream_id,
          static_cast<std::uint8_t>((n >> 8) & 0xFF),
          static_cast<std::uint8_t>(n & 0xFF)
        };
        udp::endpoint client_ep;
        {
          std::lock_guard lock {udp_mutex};
          auto it = udp_clients.find(stream_id);
          if (it == udp_clients.end()) {
            (*do_read)();
            return;
          }
          client_ep = it->second;
        }
        std::vector<asio::const_buffer> buffers {
          asio::buffer(header),
          asio::buffer(out_buf->data(), n)
        };
        udp_socket.async_send_to(buffers, client_ep, [do_read](boost::system::error_code, std::size_t) {
          (*do_read)();
        });
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
        BOOST_LOG(warning) << "[网关] UDP主socket接收错误: code="sv << ec.value();
        do_udp_receive();  // 重新开始接收
        return;
      }

      if (n < 3) {
        BOOST_LOG(info) << "[网关] UDP: 数据包太短 ("sv << n << " 字节), 丢弃"sv;
        do_udp_receive();
        return;
      }

      std::uint8_t stream_id = static_cast<std::uint8_t>(buf->at(0));
      std::uint16_t payload_len = (static_cast<std::uint16_t>(buf->at(1)) << 8) | static_cast<std::uint16_t>(buf->at(2));

      if (payload_len > n - 3) {
        BOOST_LOG(debug) << "[网关] UDP: payload_len "sv << payload_len
                         << " 超出可用长度 "sv << (n - 3) << ", 丢弃"sv;
        do_udp_receive();
        return;
      }

      if (payload_len == 0) {
        do_udp_receive();
        return;
      }

      BOOST_LOG(debug) << "[网关] UDP入向: stream=0x"sv << util::hex(stream_id).to_string_view()
                       << " payload="sv << payload_len << "B 来自 "sv << sender->address().to_string() << ':' << sender->port();

      // 记录客户端端点，用于回包路径和 stream engine UDP 出方向
      {
        std::lock_guard lock {udp_mutex};
        udp_clients[stream_id] = *sender;
      }
      g_client_ports[stream_id] = sender->port();

      auto target_port = udp_target_port(stream_id);
      if (target_port == 0) {
        BOOST_LOG(warning) << "网关 UDP: 未知 stream_id 0x"sv
                           << util::hex(stream_id).to_string_view();
      } else {
        // 通过临时端口 socket 将 payload 转发给内部 UDP 端口
        auto &sock = udp_internal_sockets[stream_id];
        udp::endpoint internal_ep {asio::ip::make_address("127.0.0.1"), target_port};
        sock->async_send_to(
          asio::buffer(buf->data() + 3, payload_len),
          internal_ep,
          [stream_id_capture = stream_id, target_port](boost::system::error_code ec, std::size_t) {
            if (ec) {
              BOOST_LOG(warning) << "[网关] UDP入向转发失败: 0x"sv
                                 << util::hex(stream_id_capture).to_string_view()
                                 << " → 127.0.0.1:"sv << target_port
                                 << " err="sv << ec.message();
            }
          }
        );
      }

      do_udp_receive();
    });
  }

  // ========== deinit_t 和 start() ==========

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
