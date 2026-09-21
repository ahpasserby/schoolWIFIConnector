#pragma once

#include <string>
#include <vector>

namespace sw::netenv {

// Things in the local network environment that break captive-portal
// authentication: a system proxy, proxy environment variables, or a VPN/tunnel
// holding the default route. schoolwifi bypasses proxies for its own requests,
// but the browser that `schoolwifi open` hands the URL to does not, and a
// tunnel that owns the default route defeats both.
struct Interference {
  bool system_proxy = false;
  std::string system_proxy_detail;

  bool env_proxy = false;
  std::string env_proxy_detail;

  std::string default_route_interface;
  bool tunnelled_default_route = false;

  std::vector<std::string> tunnel_interfaces;

  bool any() const;
};

Interference detect();

// Split out for testing.
std::string parse_default_route_interface(const std::string &netstat_output);
std::string parse_system_proxy(const std::string &scutil_proxy_output);
bool is_tunnel_interface(const std::string &name);

} // namespace sw::netenv
