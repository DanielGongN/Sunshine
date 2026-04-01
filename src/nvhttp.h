/**
 * @file src/nvhttp.h
 * @brief NVIDIA GameStream HTTP服务器的声明
 * 实现与Moonlight客户端完全兼容的GameStream协议
 * 包含设备配对、应用列表、串流会话管理等功能
 */
// 宏定义
#pragma once

// 标准库头文件
#include <string>

// 第三方库头文件
#include <boost/property_tree/ptree.hpp>       // 属性树（用于XML响应生成）
#include <nlohmann/json.hpp>                    // JSON处理
#include <Simple-Web-Server/server_https.hpp>  // HTTPS服务器

// 本地项目头文件
#include "crypto.h"      // 加密函数（配对加密）
#include "thread_safe.h" // 线程安全工具

/**
 * @brief NVIDIA GameStream HTTP协议实现命名空间
 * Sunshine模拟NVIDIA GameStream服务器，让Moonlight客户端认为连接的是官方GameStream
 */
namespace nvhttp {

  /**
   * @brief 模拟的GameStream协议版本
   * 负数第4位告知Moonlight客户端这是Sunshine而非NVIDIA官方
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief 模拟的GeForce Experience版本号
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief HTTP端口偏移量（相对于基础端口）
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief HTTPS端口偏移量（基础端口-5）
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief 启动nvhttp GameStream服务器（在独立线程中运行）
   */
  void start();

  /**
   * @brief 配置nvhttp服务器的SSL证书和私钥
   */
  void setup(const std::string &pkey, const std::string &cert);

  /**
   * @brief 自定义HTTPS连接类
   * 继承Simple-Web-Server的HTTPS连接，析构时优雅关闭TLS连接
   */
  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Gracefully shutdown the TLS connection
      SimpleWeb::error_code ec;
      shutdown(ec);
    }
  };

  /**
   * @brief 配对阶段枚举
   * Moonlight客户端与Sunshine配对时的状态机阶段
   */
  enum class PAIR_PHASE {
    NONE,  ///< 未在配对中
    GETSERVERCERT,  ///< 获取服务器证书阶段
    CLIENTCHALLENGE,  ///< 客户端挑战阶段
    SERVERCHALLENGERESP,  ///< 服务器挑战响应阶段
    CLIENTPAIRINGSECRET  ///< 客户端配对密钥阶段
  };

  struct pair_session_t {
    struct {
      std::string uniqueID = {};
      std::string cert = {};
      std::string name = {};
    } client;

    std::unique_ptr<crypto::aes_t> cipher_key = {};
    std::vector<uint8_t> clienthash = {};

    std::string serversecret = {};
    std::string serverchallenge = {};

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>>
        response;
      std::string salt = {};
    } async_insert_pin;

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
  };

  /**
   * @brief removes the temporary pairing session
   * @param sess
   */
  void remove_session(const pair_session_t &sess);

  /**
   * @brief Pair, phase 1
   *
   * Moonlight will send a salt and client certificate, we'll also need the user provided pin.
   *
   * PIN and SALT will be used to derive a shared AES key that needs to be stored
   * in order to be used to decrypt_symmetric in the next phases.
   *
   * At this stage we only have to send back our public certificate.
   */
  void getservercert(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin);

  /**
   * @brief Pair, phase 2
   *
   * Using the AES key that we generated in phase 1 we have to decrypt the client challenge,
   *
   * We generate a SHA256 hash with the following:
   *  - Decrypted challenge
   *  - Server certificate signature
   *  - Server secret: a randomly generated secret
   *
   * The hash + server_challenge will then be AES encrypted and sent as the `challengeresponse` in the returned XML
   */
  void clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  /**
   * @brief Pair, phase 3
   *
   * Moonlight will send back a `serverchallengeresp`: an AES encrypted client hash,
   * we have to send back the `pairingsecret`:
   * using our private key we have to sign the certificate_signature + server_secret (generated in phase 2)
   */
  void serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  /**
   * @brief Pair, phase 4 (final)
   *
   * We now have to use everything we exchanged before in order to verify and finally pair the clients
   *
   * We'll check the client_hash obtained at phase 3, it should contain the following:
   *   - The original server_challenge
   *   - The signature of the X509 client_cert
   *   - The unencrypted client_pairing_secret
   * We'll check that SHA256(server_challenge + client_public_cert_signature + client_secret) == client_hash
   *
   * Then using the client certificate public key we should be able to verify that
   * the client secret has been signed by Moonlight
   */
  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  /**
   * @brief Compare the user supplied pin to the Moonlight pin.
   * @param pin The user supplied pin.
   * @param name The user supplied name.
   * @return `true` if the pin is correct, `false` otherwise.
   * @examples
   * bool pin_status = nvhttp::pin("1234", "laptop");
   * @examples_end
   */
  bool pin(std::string pin, std::string name);

  /**
   * @brief Remove single client.
   * @param uuid The UUID of the client to remove.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   */
  bool unpair_client(std::string_view uuid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * nlohmann::json clients = nvhttp::get_all_clients();
   * @examples_end
   */
  nlohmann::json get_all_clients();

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void erase_all_clients();
}  // namespace nvhttp
