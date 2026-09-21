#pragma once

#include <map>
#include <string>
#include <vector>

namespace sw::http {

struct Response {
  bool ok = false;  // transport succeeded (an HTTP 404 is still ok = true)
  std::string error;
  long status = 0;
  std::string body;
  std::string final_url;
  std::vector<std::string> redirects;
  std::map<std::string, std::string> headers;  // keys lowercased, last wins
  std::string header(const std::string &key) const;
};

struct Request {
  std::string url;
  std::string method = "GET";
  std::string body;
  std::string content_type = "application/x-www-form-urlencoded";
  std::string referer;
  bool follow = true;
  int timeout_sec = 10;
  std::vector<std::string> extra_headers;
};

// A cookie-preserving libcurl session. Cookies live in memory for the lifetime
// of the object, which is what portal logins need (session cookie set on the
// login page, replayed on the POST).
class Client {
public:
  Client();
  ~Client();
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  Response send(const Request &req);
  Response get(const std::string &url, bool follow = true, int timeout_sec = 10);
  Response post(const std::string &url, const std::string &body, const std::string &referer);

  void set_user_agent(const std::string &ua);
  // Binds requests to a network interface (e.g. "en0") so the probe cannot
  // silently succeed over a different route.
  void set_interface(const std::string &iface);
  void set_insecure(bool insecure);  // many campus portals use self-signed TLS
  void clear_cookies();

  // Pins host:port to an address, bypassing the system resolver for that name
  // (libcurl's CURLOPT_RESOLVE). Used when the portal's hostname only exists
  // in the campus DNS that the system resolver has been configured to ignore.
  void add_resolve(const std::string &host, const std::string &port, const std::string &address);
  bool has_resolve_for(const std::string &host) const;

private:
  void *handle_ = nullptr;  // CURL*
  std::string user_agent_;
  std::string interface_;
  std::vector<std::string> resolve_entries_;  // "host:port:address"
  bool insecure_ = true;
};

} // namespace sw::http
