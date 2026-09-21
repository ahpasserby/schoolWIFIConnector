#pragma once

#include <string>
#include <vector>

namespace sw::dns {

// Nameservers the system resolver is actually using, as reported by
// `scutil --dns`. A manually configured DNS (System Settings > Network > DNS)
// shows up here and overrides whatever DHCP offered.
std::vector<std::string> system_nameservers();

// Nameservers this network handed out over DHCP. These are the ones that can
// resolve a campus portal's internal-only hostname; the system resolver will
// ignore them entirely if the user pinned a public DNS on the interface.
std::vector<std::string> dhcp_nameservers(const std::string &interface_name);

// Split out for testing: parses `ipconfig getpacket <iface>` output.
std::vector<std::string> parse_dhcp_nameservers(const std::string &packet);

// Resolves `host` to IPv4 addresses by querying `server` directly, bypassing
// the system resolver configuration.
std::vector<std::string> resolve_a(const std::string &server, const std::string &host,
                                   int timeout_sec = 3);

// True when a libcurl error string describes a name-resolution failure rather
// than a connection or TLS failure.
bool is_resolve_failure(const std::string &curl_error);

} // namespace sw::dns
