#pragma once

#include <string>
#include <vector>

namespace sw::wifi {

struct Info {
  std::string interface;
  std::string ssid;
  std::string bssid;
  std::string ipv4;
  bool power_on = false;
  bool associated = false;
};

// Names of the Wi-Fi interfaces CoreWLAN reports (usually just "en0").
std::vector<std::string> interfaces();

// CoreWLAN supplies the interface name and power state. It does NOT supply the
// SSID on macOS 14+ unless the caller holds a Location Services grant, so the
// SSID/BSSID come from `ipconfig getsummary`, which still reports them for a
// plain CLI process. Both sources are best-effort: an empty ssid is normal and
// callers must degrade rather than fail.
Info current(const std::string &interface_hint = "");

} // namespace sw::wifi
