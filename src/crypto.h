/**
 * @file src/crypto.h
 * @brief 加密函数的声明
 * 基于OpenSSL实现，提供SHA-256哈希、AES加解密、X.509证书、RSA签名等功能
 * 用于Moonlight客户端配对和串流数据加密
 */
#pragma once

// 标准库头文件
#include <array>

// OpenSSL加密库头文件
#include <openssl/evp.h>  // 加密算法接口
#include <openssl/rand.h> // 随机数生成
#include <openssl/sha.h>  // SHA哈希算法
#include <openssl/x509.h> // X.509证书处理

// 本地项目头文件
#include "utility.h" // 智能指针工具等

namespace crypto {
  /**
   * @brief 凭证结构体（包含X.509证书和私钥的PEM字符串）
   */
  struct creds_t {
    std::string x509; // X.509证书PEM字符串
    std::string pkey;  // 私钥PEM字符串
  };

  void md_ctx_destroy(EVP_MD_CTX *); // 消息摘要上下文的自定义释放函数

  // 类型别名定义
  using sha256_t = std::array<std::uint8_t, SHA256_DIGEST_LENGTH>; // SHA-256哈希值（32字节）

  using aes_t = std::vector<std::uint8_t>;  // AES密钥字节数组
  // OpenSSL对象的智能指针（自动释放内存）
  using x509_t = util::safe_ptr<X509, X509_free>;                    // X.509证书
  using x509_store_t = util::safe_ptr<X509_STORE, X509_STORE_free>;  // 证书存储
  using x509_store_ctx_t = util::safe_ptr<X509_STORE_CTX, X509_STORE_CTX_free>; // 证书验证上下文
  using cipher_ctx_t = util::safe_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>; // 加密上下文
  using md_ctx_t = util::safe_ptr<EVP_MD_CTX, md_ctx_destroy>;       // 消息摘要上下文
  using bio_t = util::safe_ptr<BIO, BIO_free_all>;                   // BIO I/O对象
  using pkey_t = util::safe_ptr<EVP_PKEY, EVP_PKEY_free>;            // 公私钥对
  using pkey_ctx_t = util::safe_ptr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>; // 密钥上下文
  using bignum_t = util::safe_ptr<BIGNUM, BN_free>;                  // 大数对象

  /**
   * @brief 使用SHA-256算法计算哈希值
   * @param plaintext 待哈希的明文数据
   * @return 32字节的SHA-256哈希值
   */
  sha256_t hash(const std::string_view &plaintext);

  /**
   * @brief 从salt和PIN码派生AES密钥（用于配对加密）
   */
  aes_t gen_aes_key(const std::array<uint8_t, 16> &salt, const std::string_view &pin);

  x509_t x509(const std::string_view &x);   // 从PEM字符串解析X.509证书
  pkey_t pkey(const std::string_view &k);    // 从PEM字符串解析私钥
  std::string pem(x509_t &x509);             // 将X.509证书序列化为PEM字符串
  std::string pem(pkey_t &pkey);             // 将私钥序列化为PEM字符串

  /**
   * @brief 使用私钥对数据进行SHA-256签名
   */
  std::vector<uint8_t> sign256(const pkey_t &pkey, const std::string_view &data);

  /**
   * @brief 使用X.509证书验证SHA-256签名
   */
  bool verify256(const x509_t &x509, const std::string_view &data, const std::string_view &signature);

  /**
   * @brief 生成自签名的X.509证书和私钥对（用于Sunshine的HTTPS和配对加密）
   * @param cn 证书主体名称（Common Name）
   * @param key_bits RSA密钥位数
   */
  creds_t gen_creds(const std::string_view &cn, std::uint32_t key_bits);

  /**
   * @brief 获取X.509证书的签名数据
   */
  std::string_view signature(const x509_t &x);

  /**
   * @brief 生成加密安全的随机字节串
   */
  std::string rand(std::size_t bytes);

  /**
   * @brief 从指定字符集生成随机字符串（用于PIN码生成等）
   */
  std::string rand_alphabet(std::size_t bytes, const std::string_view &alphabet = std::string_view {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!%&()=-"});

  /**
   * @brief 证书链验证类
   * 管理可信证书列表，用于验证Moonlight客户端证书是否已配对
   */
  class cert_chain_t {
  public:
    KITTY_DECL_CONSTR(cert_chain_t)

    void add(x509_t &&cert);   // 添加可信证书到链中

    void clear();               // 清空所有可信证书

    /**
     * @brief 验证证书是否在可信链中
     * @return nullptr=验证通过, 否则返回错误信息
     */
    const char *verify(x509_t::element_type *cert);

  private:
    std::vector<std::pair<x509_t, x509_store_t>> _certs; // 可信证书列表（证书+证书存储对）
    x509_store_ctx_t _cert_ctx; // 证书验证上下文
  };

  /**
   * @brief AES加密命名空间
   * 提供ECB、GCM、CBC三种加密模式，用于GameStream协议中的数据加密
   */
  namespace cipher {
    constexpr std::size_t tag_size = 16; // GCM认证标签大小（16字节）

    /**
     * @brief 计算PKCS#7填充后的数据大小（16字节对齐）
     */
    constexpr std::size_t round_to_pkcs7_padded(std::size_t size) {
      return ((size + 15) / 16) * 16;
    }

    /**
     * @brief AES加密器基类
     * 包含加密和解密上下文、密钥和填充设置
     */
    class cipher_t {
    public:
      cipher_ctx_t decrypt_ctx; // 解密上下文
      cipher_ctx_t encrypt_ctx; // 加密上下文

      aes_t key;    // AES密钥

      bool padding;  // 是否启用PKCS#7填充
    };

    /**
     * @brief AES-ECB模式加解密器
     * ECB为电子密码本模式，用于GameStream配对过程中的加密
     */
    class ecb_t: public cipher_t {
    public:
      ecb_t() = default;
      ecb_t(ecb_t &&) noexcept = default;
      ecb_t &operator=(ecb_t &&) noexcept = default;

      ecb_t(const aes_t &key, bool padding = true); // 使用指定AES密钥初始化

      int encrypt(const std::string_view &plaintext, std::vector<std::uint8_t> &cipher); // ECB加密
      int decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext); // ECB解密
    };

    /**
     * @brief AES-GCM模式加解密器
     * GCM提供认证加密（AEAD），是串流数据传输的主要加密模式
     */
    class gcm_t: public cipher_t {
    public:
      gcm_t() = default;
      gcm_t(gcm_t &&) noexcept = default;
      gcm_t &operator=(gcm_t &&) noexcept = default;

      gcm_t(const crypto::aes_t &key, bool padding = true); // 使用指定AES密钥初始化

      /**
       * @brief AES-GCM加密（分离输出标签和密文）
       * @param plaintext 明文数据
       * @param tag GCM认证标签输出缓冲区
       * @param ciphertext 密文输出缓冲区
       * @param iv 初始化向量
       * @return 密文+标签总长度，错误返回-1
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *tag, std::uint8_t *ciphertext, aes_t *iv);

      /**
       * @brief AES-GCM加密（标签和密文连续写入同一缓冲区）
       * tagged_cipher缓冲区大小至少为: round_to_pkcs7_padded(plaintext.size()) + tag_size
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *tagged_cipher, aes_t *iv);

      int decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext, aes_t *iv); // GCM解密
    };

    /**
     * @brief AES-CBC模式加密器
     * CBC为密码块链接模式，用于GameStream协议中的部分加密
     */
    class cbc_t: public cipher_t {
    public:
      cbc_t() = default;
      cbc_t(cbc_t &&) noexcept = default;
      cbc_t &operator=(cbc_t &&) noexcept = default;

      cbc_t(const crypto::aes_t &key, bool padding = true); // 使用指定AES密钥初始化

      /**
       * @brief AES-CBC加密
       * cipher缓冲区大小至少为: round_to_pkcs7_padded(plaintext.size())
       * @param plaintext 明文数据
       * @param cipher 密文输出缓冲区
       * @param iv 初始化向量
       * @return 密文长度，错误返回-1
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *cipher, aes_t *iv);
    };
  }  // namespace cipher
}  // namespace crypto
