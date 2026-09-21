#include "sw/netenv.hpp"

#include <arpa/inet.h>

#include <cctype>
#include <cstdlib>

#include "sw/util.hpp"

namespace sw::netenv {
namespace {

// Interfaces created by VPN, tunnel and TUN-mode proxy software.
const char *kTunnelPrefixes[] = {"utun", "ipsec", "ppp", "tun", "tap", "gpd", "wg"};

std::string scutil_value(const std::string &output, const std::string &key) {
  for (const std::string &raw : util::split(output, '\n')) {
    std::string line = util::trim(raw);
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    if (!util::iequals(util::trim(line.substr(0, colon)), key)) continue;
    return util::trim(line.substr(colon + 1));
  }
  return "";
}

} // namespace

bool Interference::any() const {
  return system_proxy || env_proxy || tunnelled_default_route;
}

bool is_tunnel_interface(const std::string &name) {
  for (const char *prefix : kTunnelPrefixes) {
    if (util::starts_with(util::lower(name), prefix)) return true;
  }
  return false;
}

std::string parse_default_route_interface(const std::string &netstat_output) {
  // `netstat -rn -f inet` prints, among headers and other routes:
  //     default            172.19.9.90        UGScg                 en0
  for (const std::string &raw : util::split(netstat_output, '\n')) {
    std::string line = util::trim(raw);
    if (!util::starts_with(line, "default")) continue;

    // The interface is the last field that looks like an interface name.
    std::vector<std::string> fields;
    std::string cur;
    for (char c : line) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!cur.empty()) fields.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) fields.push_back(cur);

    for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
      const std::string &f = *it;
      if (f.size() < 3) continue;
      bool ends_with_digit = std::isdigit(static_cast<unsigned char>(f.back())) != 0;
      bool alpha_start = std::isalpha(static_cast<unsigned char>(f.front())) != 0;
      if (ends_with_digit && alpha_start && f.find('.') == std::string::npos) return f;
    }
  }
  return "";
}

std::string parse_default_gateway(const std::string &netstat_output) {
  for (const std::string &raw : util::split(netstat_output, '\n')) {
    std::string line = util::trim(raw);
    if (!util::starts_with(line, "default")) continue;

    std::vector<std::string> fields;
    std::string cur;
    for (char c : line) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!cur.empty()) fields.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) fields.push_back(cur);

    // "default  <gateway>  <flags>  <iface>" - the gateway is a dotted quad
    // when it is an address at all ("link#14" when the route is on-link).
    for (std::size_t i = 1; i < fields.size(); ++i) {
      struct in_addr addr {};
      if (inet_pton(AF_INET, fields[i].c_str(), &addr) == 1) return fields[i];
    }
    return "";
  }
  return "";
}

std::string default_gateway() {
  return parse_default_gateway(util::exec_capture("netstat -rn -f inet 2>/dev/null"));
}

std::string parse_system_proxy(const std::string &scutil_proxy_output) {
  std::vector<std::string> active;

  if (scutil_value(scutil_proxy_output, "HTTPEnable") == "1") {
    std::string host = scutil_value(scutil_proxy_output, "HTTPProxy");
    std::string port = scutil_value(scutil_proxy_output, "HTTPPort");
    active.push_back("HTTP " + host + (port.empty() ? "" : ":" + port));
  }
  if (scutil_value(scutil_proxy_output, "HTTPSEnable") == "1") {
    std::string host = scutil_value(scutil_proxy_output, "HTTPSProxy");
    std::string port = scutil_value(scutil_proxy_output, "HTTPSPort");
    active.push_back("HTTPS " + host + (port.empty() ? "" : ":" + port));
  }
  if (scutil_value(scutil_proxy_output, "SOCKSEnable") == "1") {
    std::string host = scutil_value(scutil_proxy_output, "SOCKSProxy");
    std::string port = scutil_value(scutil_proxy_output, "SOCKSPort");
    active.push_back("SOCKS " + host + (port.empty() ? "" : ":" + port));
  }
  if (scutil_value(scutil_proxy_output, "ProxyAutoConfigEnable") == "1") {
    std::string url = scutil_value(scutil_proxy_output, "ProxyAutoConfigURLString");
    active.push_back("PAC " + url);
  }

  return util::join(active, ", ");
}

Interference detect() {
  Interference result;

  result.system_proxy_detail = parse_system_proxy(util::exec_capture("scutil --proxy 2>/dev/null"));
  result.system_proxy = !result.system_proxy_detail.empty();

  static const char *kProxyVars[] = {"http_proxy",  "https_proxy", "all_proxy",
                                     "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"};
  std::vector<std::string> found;
  for (const char *name : kProxyVars) {
    const char *value = std::getenv(name);
    if (value && *value) found.push_back(std::string(name) + "=" + value);
  }
  // Report each variable once; the upper/lower-case pairs are usually the same.
  std::vector<std::string> unique;
  for (const std::string &entry : found) {
    bool dup = false;
    std::string value = entry.substr(entry.find('=') + 1);
    for (const std::string &kept : unique) {
      if (kept.substr(kept.find('=') + 1) == value) dup = true;
    }
    if (!dup) unique.push_back(entry);
  }
  result.env_proxy_detail = util::join(unique, ", ");
  result.env_proxy = !result.env_proxy_detail.empty();

  result.default_route_interface =
      parse_default_route_interface(util::exec_capture("netstat -rn -f inet 2>/dev/null"));
  result.tunnelled_default_route = is_tunnel_interface(result.default_route_interface);

  std::string interfaces = util::exec_capture("ifconfig -l 2>/dev/null");
  for (const std::string &name : util::split(util::trim(interfaces), ' ')) {
    if (is_tunnel_interface(name)) result.tunnel_interfaces.push_back(name);
  }

  return result;
}

} // namespace sw::netenv
