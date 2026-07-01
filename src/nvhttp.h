/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <string>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "crypto.h"
#include "thread_safe.h"

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief HTTPS 端口，相对于配置端口的偏移量。
   *        以 base=41200 为例，最终端口 = 41200 + 0 = 41200（网关在此监听）。
   */
  constexpr auto PORT_HTTPS = 0;

  /**
   * @brief HTTP 端口（仅中间件模式下禁用）。
   */
  constexpr auto PORT_HTTP = 5;

  /**
   * @brief 内部 HTTPS 端口偏移量，供网关隧道使用。
   *        nvhttp 绑定到 127.0.0.1:base+100（41300），网关将 TLS 流量转发至此。
   */
  constexpr auto PORT_HTTPS_INTERNAL = 100;

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

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

  enum class PAIR_PHASE {
    NONE,  ///< Sunshine is not in a pairing phase
    GETSERVERCERT,  ///< Sunshine is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sunshine is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sunshine is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sunshine is in the client pairing secret phase
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
   * @brief Enable or disable a client.
   * @param uuid The UUID of the client.
   * @param enabled Whether the client should be enabled.
   * @return true if the client was found and updated.
   */
  bool set_client_enabled(std::string_view uuid, bool enabled);
  std::string get_cert_by_uuid(std::string_view uuid);

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

  /**
   * @brief Add a trusted client certificate (memory-only, not persisted).
   * @param uuid The unique identifier for the client.
   * @param cert The PEM-encoded X.509 certificate.
   */
  void add_trusted_client(std::string uuid, std::string cert);

  /**
   * @brief Start a stream session directly (bypasses /launch HTTP endpoint).
   *
   * For architectures where game lifecycle is managed externally.
   * Creates a launch session and registers it for RTSP.
   *
   * @param stream_params JSON with optional: width, height, fps, appid, rikey, rikeyid,
   *                       surroundAudioInfo, gcmap, enable_hdr, enable_sops, uniqueid
   * @return RTSP session URL on success, empty string on failure
   */
  std::string start_stream_session(const nlohmann::json &stream_params);

  /**
   * @brief Store pending stream config from middleware.
   *
   * Set by client_connect handler before serverinfo runs.
   * Consumed and cleared by serverinfo to auto-create RTSP session.
   */
  void set_pending_stream_config(const nlohmann::json &config);

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
}  // namespace nvhttp
