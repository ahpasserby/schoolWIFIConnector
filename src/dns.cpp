#include "sw/dns.hpp"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netinet/in.h>
#include <resolv.h>

#include <cstring>

#include "sw/log.hpp"
#include "sw/util.hpp"

namespace sw::dns {
namespace {

bool valid_ipv4(const std::string &s) {
  struct in_addr addr {};
  return inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

} // namespace

std::vector<std::string> parse_dhcp_nameservers(const std::string &packet) {
  // `ipconfig getpacket en0` prints the option as:
  //     domain_name_server (ip_mult): {10.253.0.1, 10.253.0.2}
  // and, when a single server was offered, sometimes as:
  //     domain_name_server (ip): 10.253.0.1
  std::vector<std::string> servers;

  for (const std::string &raw : util::split(packet, '\n')) {
    std::string line = util::trim(raw);
    if (!util::icontains(line, "domain_name_server")) continue;

    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string value = util::trim(line.substr(colon + 1));

    std::size_t open = value.find('{');
    if (open != std::string::npos) {
      std::size_t close = value.find('}', open);
      if (close == std::string::npos) continue;
      value = value.substr(open + 1, close - open - 1);
    }

    for (const std::string &token : util::split(value, ',')) {
      std::string ip = util::trim(token);
      if (valid_ipv4(ip)) servers.push_back(ip);
    }
  }
  return servers;
}

std::vector<std::string> dhcp_nameservers(const std::string &interface_name) {
  std::string iface = interface_name.empty() ? "en0" : interface_name;
  return parse_dhcp_nameservers(util::exec_capture("ipconfig getpacket " + iface + " 2>/dev/null"));
}

std::vector<std::string> system_nameservers() {
  std::vector<std::string> servers;
  std::string out = util::exec_capture("scutil --dns 2>/dev/null");

  for (const std::string &raw : util::split(out, '\n')) {
    std::string line = util::trim(raw);
    if (!util::starts_with(line, "nameserver[")) continue;
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;

    std::string ip = util::trim(line.substr(colon + 1));
    if (!valid_ipv4(ip)) continue;
    bool seen = false;
    for (const std::string &s : servers) {
      if (s == ip) seen = true;
    }
    if (!seen) servers.push_back(ip);
  }
  return servers;
}

std::vector<std::string> resolve_a(const std::string &server, const std::string &host,
                                   int timeout_sec) {
  std::vector<std::string> addresses;

  struct in_addr server_addr {};
  if (inet_pton(AF_INET, server.c_str(), &server_addr) != 1) return addresses;

  struct __res_state state;
  std::memset(&state, 0, sizeof(state));
  if (res_ninit(&state) != 0) return addresses;

  // Point this resolver instance at exactly one server, replacing whatever the
  // system configuration says.
  state.nsaddr_list[0].sin_family = AF_INET;
  state.nsaddr_list[0].sin_addr = server_addr;
  state.nsaddr_list[0].sin_port = htons(53);
  state.nsaddr_list[0].sin_len = sizeof(struct sockaddr_in);
  state.nscount = 1;
  state.retrans = timeout_sec;
  state.retry = 1;

  unsigned char answer[NS_PACKETSZ];
  int len = res_nquery(&state, host.c_str(), ns_c_in, ns_t_a, answer, sizeof(answer));
  if (len <= 0) {
    res_ndestroy(&state);
    return addresses;
  }

  ns_msg msg;
  if (ns_initparse(answer, len, &msg) < 0) {
    res_ndestroy(&state);
    return addresses;
  }

  // The answer section carries the CNAME chain as well as the A records; take
  // every A record and ignore the rest.
  int count = ns_msg_count(msg, ns_s_an);
  for (int i = 0; i < count; ++i) {
    ns_rr rr;
    if (ns_parserr(&msg, ns_s_an, i, &rr) < 0) continue;
    if (ns_rr_type(rr) != ns_t_a || ns_rr_rdlen(rr) != 4) continue;

    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, ns_rr_rdata(rr), buf, sizeof(buf))) addresses.emplace_back(buf);
  }

  res_ndestroy(&state);
  return addresses;
}

bool is_resolve_failure(const std::string &curl_error) {
  return util::icontains(curl_error, "resolve host") ||
         util::icontains(curl_error, "resolve proxy") ||
         util::icontains(curl_error, "name or service not known") ||
         util::icontains(curl_error, "nodename nor servname");
}

} // namespace sw::dns
