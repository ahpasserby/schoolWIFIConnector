#include "sw/keychain.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace sw::keychain {
namespace {

std::string status_message(OSStatus status) {
  CFStringRef msg = SecCopyErrorMessageString(status, nullptr);
  if (!msg) return "OSStatus " + std::to_string(status);

  char buf[512];
  std::string out;
  if (CFStringGetCString(msg, buf, sizeof(buf), kCFStringEncodingUTF8)) {
    out = buf;
  } else {
    out = "OSStatus " + std::to_string(status);
  }
  CFRelease(msg);
  return out;
}

CFDictionaryRef make_query(const std::string &service, const std::string &account,
                           bool want_data) {
  CFStringRef svc = CFStringCreateWithCString(nullptr, service.c_str(), kCFStringEncodingUTF8);
  CFStringRef acc = CFStringCreateWithCString(nullptr, account.c_str(), kCFStringEncodingUTF8);

  CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, svc);
  CFDictionarySetValue(q, kSecAttrAccount, acc);
  if (want_data) {
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  }

  CFRelease(svc);
  CFRelease(acc);
  return q;
}

} // namespace

bool set_password(const std::string &service, const std::string &account,
                  const std::string &password, std::string *err) {
  CFDictionaryRef query = make_query(service, account, false);

  CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(password.data()),
                                static_cast<CFIndex>(password.size()));
  CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attrs, kSecValueData, data);

  // Update in place when an item already exists, otherwise add a new one.
  OSStatus status = SecItemUpdate(query, attrs);
  if (status == errSecItemNotFound) {
    CFMutableDictionaryRef add = CFDictionaryCreateMutableCopy(nullptr, 0, query);
    CFDictionarySetValue(add, kSecValueData, data);
    status = SecItemAdd(add, nullptr);
    CFRelease(add);
  }

  CFRelease(data);
  CFRelease(attrs);
  CFRelease(query);

  if (status != errSecSuccess) {
    if (err) *err = status_message(status);
    return false;
  }
  return true;
}

bool get_password(const std::string &service, const std::string &account, std::string *out,
                  std::string *err) {
  CFDictionaryRef query = make_query(service, account, true);
  CFTypeRef result = nullptr;
  OSStatus status = SecItemCopyMatching(query, &result);
  CFRelease(query);

  if (status != errSecSuccess) {
    if (err) {
      *err = status == errSecItemNotFound ? "no keychain entry for " + service + "/" + account
                                          : status_message(status);
    }
    return false;
  }

  auto data = static_cast<CFDataRef>(result);
  if (out) {
    out->assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                static_cast<std::size_t>(CFDataGetLength(data)));
  }
  CFRelease(result);
  return true;
}

bool delete_password(const std::string &service, const std::string &account, std::string *err) {
  CFDictionaryRef query = make_query(service, account, false);
  OSStatus status = SecItemDelete(query);
  CFRelease(query);

  if (status != errSecSuccess && status != errSecItemNotFound) {
    if (err) *err = status_message(status);
    return false;
  }
  return true;
}

} // namespace sw::keychain
