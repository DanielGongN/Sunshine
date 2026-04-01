/**
 * @file src/platform/windows/publish.cpp
 * @brief Windows mDNS服务注册实现。使用Windows DNS-SD API发布Sunshine服务。
 */
// platform includes
// WinSock2.h must be included before Windows.h
// clang-format off
#include <WinSock2.h>
#include <Windows.h>
// clang-format on
#include <WinDNS.h>
#include <winerror.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/thread_safe.h"
#include "utf_utils.h"

#define _FN(x, ret, args) \
  typedef ret(*x##_fn) args; \
  static x##_fn x

using namespace std::literals;

#define __SV(quote) L##quote##sv
#define SV(quote) __SV(quote)

extern "C" {
#ifndef __MINGW32__
  constexpr auto DNS_REQUEST_PENDING = 9506L;
  constexpr auto DNS_QUERY_REQUEST_VERSION1 = 0x1;
  constexpr auto DNS_QUERY_RESULTS_VERSION1 = 0x1;
#endif

  constexpr auto SERVICE_DOMAIN = "local";
  const auto SERVICE_TYPE_DOMAIN = std::format("{}.{}"sv, platf::SERVICE_TYPE, SERVICE_DOMAIN);

#ifndef __MINGW32__
  typedef struct _DNS_SERVICE_INSTANCE {
    LPWSTR pszInstanceName;
    LPWSTR pszHostName;

    IP4_ADDRESS *ip4Address;
    IP6_ADDRESS *ip6Address;

    WORD wPort;
    WORD wPriority;
    WORD wWeight;

    // Property list
    DWORD dwPropertyCount;

    PWSTR *keys;
    PWSTR *values;

    DWORD dwInterfaceIndex;
  } DNS_SERVICE_INSTANCE, *PDNS_SERVICE_INSTANCE;
#endif

  typedef VOID WINAPI
    DNS_SERVICE_REGISTER_COMPLETE(
      _In_ DWORD Status,
      _In_ PVOID pQueryContext,
      _In_ PDNS_SERVICE_INSTANCE pInstance
    );

  typedef DNS_SERVICE_REGISTER_COMPLETE *PDNS_SERVICE_REGISTER_COMPLETE;

#ifndef __MINGW32__
  typedef struct _DNS_SERVICE_CANCEL {
    PVOID reserved;
  } DNS_SERVICE_CANCEL, *PDNS_SERVICE_CANCEL;

  typedef struct _DNS_SERVICE_REGISTER_REQUEST {
    ULONG Version;
    ULONG InterfaceIndex;
    PDNS_SERVICE_INSTANCE pServiceInstance;
    PDNS_SERVICE_REGISTER_COMPLETE pRegisterCompletionCallback;
    PVOID pQueryContext;
    HANDLE hCredentials;
    BOOL unicastEnabled;
  } DNS_SERVICE_REGISTER_REQUEST, *PDNS_SERVICE_REGISTER_REQUEST;
#endif

  _FN(_DnsServiceFreeInstance, VOID, (_In_ PDNS_SERVICE_INSTANCE pInstance));
  _FN(_DnsServiceDeRegister, DWORD, (_In_ PDNS_SERVICE_REGISTER_REQUEST pRequest, _Inout_opt_ PDNS_SERVICE_CANCEL pCancel));
  _FN(_DnsServiceRegister, DWORD, (_In_ PDNS_SERVICE_REGISTER_REQUEST pRequest, _Inout_opt_ PDNS_SERVICE_CANCEL pCancel));
} /* extern "C" */

namespace platf::publish {
  /**
   * @brief mDNS注册/注销完成回调，通过alarm通知等待线程
   */
  VOID WINAPI register_cb(DWORD status, PVOID pQueryContext, PDNS_SERVICE_INSTANCE pInstance) {
    auto alarm = (safe::alarm_t<PDNS_SERVICE_INSTANCE>::element_type *) pQueryContext;

    if (status) {
      print_status("register_cb()"sv, status);
    }

    alarm->ring(pInstance);
  }

  /**
   * @brief 注册或注销mDNS服务：构建服务实例→调用DnsServiceRegister/DeRegister→等待回调完成
   */
  static int service(bool enable, PDNS_SERVICE_INSTANCE &existing_instance) {
    // 创建信号量，用于等待异步注册/注销回调完成
    auto alarm = safe::make_alarm<PDNS_SERVICE_INSTANCE>();

    // 构建mDNS服务域名："_nvstream._tcp.local"
    std::wstring domain = utf_utils::from_utf8(SERVICE_TYPE_DOMAIN);

    // 构建完整服务实例名："<主机名>._nvstream._tcp.local"
    auto hostname = platf::get_host_name();
    auto name = utf_utils::from_utf8(net::mdns_instance_name(hostname) + '.') + domain;
    // 构建主机名："<主机名>.local" 用于SRV记录
    auto host = utf_utils::from_utf8(hostname + ".local");

    DNS_SERVICE_INSTANCE instance {};
    instance.pszInstanceName = name.data();
    instance.wPort = net::map_port(nvhttp::PORT_HTTP);
    instance.pszHostName = host.data();

    // 设置空TXT记录以符合RFC 1035规范。
    // 若不设置，Windows会发送零字符串的TXT记录（违规）；
    // 设置单个空值后，Windows会发送单个空字符串（合规）。
    // Apple的mDNS解析器会严格校验，非法TXT记录会导致整个应答被拒绝。
    PWCHAR keys[] = {nullptr};
    PWCHAR values[] = {nullptr};
    instance.dwPropertyCount = 1;
    instance.keys = keys;
    instance.values = values;

    // 构建DNS-SD注册/注销请求结构
    DNS_SERVICE_REGISTER_REQUEST req {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.pQueryContext = alarm.get();  // 回调上下文指向alarm，用于通知完成
    req.pServiceInstance = enable ? &instance : existing_instance;  // 注册用新实例，注销用已有实例
    req.pRegisterCompletionCallback = register_cb;

    DNS_STATUS status {};

    // 异步调用注册或注销API，成功时返回DNS_REQUEST_PENDING
    if (enable) {
      status = _DnsServiceRegister(&req, nullptr);
      if (status != DNS_REQUEST_PENDING) {
        print_status("DnsServiceRegister()"sv, status);
        return -1;
      }
    } else {
      status = _DnsServiceDeRegister(&req, nullptr);
      if (status != DNS_REQUEST_PENDING) {
        print_status("DnsServiceDeRegister()"sv, status);
        return -1;
      }
    }

    // 阻塞等待回调函数ring信号量
    alarm->wait();

    auto registered_instance = alarm->status();
    if (enable) {
      // 保存返回的实例指针，后续注销时使用
      existing_instance = registered_instance;
    } else if (registered_instance) {
      // 注销成功，释放实例内存并清空指针
      _DnsServiceFreeInstance(registered_instance);
      existing_instance = nullptr;
    }

    return registered_instance ? 0 : -1;
  }

  class mdns_registration_t: public ::platf::deinit_t {
  public:
    mdns_registration_t():
        existing_instance(nullptr) {
      if (service(true, existing_instance)) {
        BOOST_LOG(error) << "Unable to register Sunshine mDNS service"sv;
        return;
      }

      BOOST_LOG(info) << "Registered Sunshine mDNS service"sv;
    }

    ~mdns_registration_t() override {
      if (existing_instance) {
        if (service(false, existing_instance)) {
          BOOST_LOG(error) << "Unable to unregister Sunshine mDNS service"sv;
          return;
        }

        BOOST_LOG(info) << "Unregistered Sunshine mDNS service"sv;
      }
    }

  private:
    PDNS_SERVICE_INSTANCE existing_instance;
  };

  /**
   * @brief 从dnsapi.dll动态加载mDNS函数指针（DnsServiceRegister/DeRegister/FreeInstance）
   */
  int load_funcs(HMODULE handle) {
    // 失败守卫：若任一函数加载失败则自动释放DLL句柄
    auto fg = util::fail_guard([handle]() {
      FreeLibrary(handle);
    });

    // 从dnsapi.dll动态获取三个DNS-SD函数的地址
    _DnsServiceFreeInstance = (_DnsServiceFreeInstance_fn) GetProcAddress(handle, "DnsServiceFreeInstance");
    _DnsServiceDeRegister = (_DnsServiceDeRegister_fn) GetProcAddress(handle, "DnsServiceDeRegister");
    _DnsServiceRegister = (_DnsServiceRegister_fn) GetProcAddress(handle, "DnsServiceRegister");

    if (!(_DnsServiceFreeInstance && _DnsServiceDeRegister && _DnsServiceRegister)) {
      BOOST_LOG(error) << "mDNS service not available in dnsapi.dll"sv;
      return -1;
    }

    fg.disable();
    return 0;
  }

  std::unique_ptr<::platf::deinit_t> start() {
    HMODULE handle = LoadLibrary("dnsapi.dll");

    if (!handle || load_funcs(handle)) {
      BOOST_LOG(error) << "Couldn't load dnsapi.dll, You'll need to add PC manually from Moonlight"sv;
      return nullptr;
    }

    return std::make_unique<mdns_registration_t>();
  }
}  // namespace platf::publish
