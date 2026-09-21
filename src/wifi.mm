#include "sw/wifi.hpp"

#import <CoreWLAN/CoreWLAN.h>
#import <Foundation/Foundation.h>

#include "sw/util.hpp"

namespace sw::wifi {
namespace {

std::string to_std(NSString *s) { return s ? std::string([s UTF8String]) : std::string(); }

// `ipconfig getsummary <iface>` still reports SSID/BSSID for an ordinary CLI
// process on macOS 26, where CoreWLAN's -ssid returns nil without a Location
// Services grant. Output looks like:
//     SSID : CAMPUS-WIFI
//     BSSID : aa:bb:cc:dd:ee:ff
std::string summary_field(const std::string &summary, const std::string &key) {
  for (const std::string &raw : util::split(summary, '\n')) {
    std::string line = util::trim(raw);
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    if (!util::iequals(util::trim(line.substr(0, colon)), key)) continue;

    std::string value = util::trim(line.substr(colon + 1));
    // BSSID values contain colons, so re-join everything after the first one.
    if (util::iequals(key, "BSSID")) value = util::trim(line.substr(colon + 1));
    if (!value.empty()) return value;
  }
  return "";
}

std::string ipv4_of(const std::string &iface) {
  std::string out = util::exec_capture("ipconfig getifaddr " + iface + " 2>/dev/null");
  return util::trim(out);
}

} // namespace

std::vector<std::string> interfaces() {
  std::vector<std::string> names;
  @autoreleasepool {
    // Instance method: the class method of the same name is deprecated in 13.0.
    NSArray<NSString *> *found = [[CWWiFiClient sharedWiFiClient] interfaceNames];
    for (NSString *n in found) names.push_back(to_std(n));
  }
  return names;
}

Info current(const std::string &interface_hint) {
  Info info;
  info.interface = interface_hint;

  @autoreleasepool {
    CWWiFiClient *client = [CWWiFiClient sharedWiFiClient];
    CWInterface *iface = interface_hint.empty()
                             ? [client interface]
                             : [client interfaceWithName:@(interface_hint.c_str())];
    if (!iface) iface = [client interface];

    if (iface) {
      std::string name = to_std([iface interfaceName]);
      if (!name.empty()) info.interface = name;
      info.power_on = [iface powerOn];

      // Present on some systems, nil on others -- treated as a bonus, never
      // as the only source.
      info.ssid = to_std([iface ssid]);
      info.bssid = to_std([iface bssid]);
    }
  }

  if (info.interface.empty()) info.interface = "en0";

  if (info.ssid.empty() || info.bssid.empty()) {
    std::string summary =
        util::exec_capture("ipconfig getsummary " + info.interface + " 2>/dev/null");
    if (info.ssid.empty()) info.ssid = summary_field(summary, "SSID");
    if (info.bssid.empty()) info.bssid = summary_field(summary, "BSSID");
  }

  info.ipv4 = ipv4_of(info.interface);
  info.associated = !info.ssid.empty() || !info.ipv4.empty();
  return info;
}

} // namespace sw::wifi
