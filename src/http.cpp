#include "sw/http.hpp"

#include <curl/curl.h>

#include <cstring>

#include "sw/log.hpp"
#include "sw/util.hpp"

namespace sw::http {
namespace {

std::size_t write_body(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::size_t write_header(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  auto *resp = static_cast<Response *>(userdata);
  std::string line(ptr, size * nmemb);
  std::size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = util::lower(util::trim(line.substr(0, colon)));
    std::string value = util::trim(line.substr(colon + 1));
    resp->headers[key] = value;
    if (key == "location") resp->redirects.push_back(value);
  }
  return size * nmemb;
}

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_global() { static CurlGlobal g; }

} // namespace

std::string Response::header(const std::string &key) const {
  auto it = headers.find(util::lower(key));
  return it == headers.end() ? std::string() : it->second;
}

Client::Client() {
  ensure_global();
  handle_ = curl_easy_init();
  user_agent_ = "schoolwifi";
  if (handle_) {
    // An empty cookie file switches on libcurl's in-memory cookie engine.
    curl_easy_setopt(static_cast<CURL *>(handle_), CURLOPT_COOKIEFILE, "");
  }
}

Client::~Client() {
  if (handle_) curl_easy_cleanup(static_cast<CURL *>(handle_));
}

void Client::set_user_agent(const std::string &ua) { user_agent_ = ua; }
void Client::set_interface(const std::string &iface) { interface_ = iface; }
void Client::set_insecure(bool insecure) { insecure_ = insecure; }

void Client::add_resolve(const std::string &host, const std::string &port,
                         const std::string &address) {
  std::string entry = host + ":" + port + ":" + address;
  for (const std::string &existing : resolve_entries_) {
    if (existing == entry) return;
  }
  resolve_entries_.push_back(entry);
  log::debug("pinning " + entry);
}

bool Client::has_resolve_for(const std::string &host) const {
  for (const std::string &entry : resolve_entries_) {
    if (util::starts_with(entry, host + ":")) return true;
  }
  return false;
}

void Client::clear_cookies() {
  if (handle_) curl_easy_setopt(static_cast<CURL *>(handle_), CURLOPT_COOKIELIST, "ALL");
}

Response Client::send(const Request &req) {
  Response resp;
  if (!handle_) {
    resp.error = "curl handle unavailable";
    return resp;
  }

  auto *curl = static_cast<CURL *>(handle_);
  curl_easy_reset(curl);
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // survives the reset

  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = '\0';

  curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(req.timeout_sec));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(req.timeout_sec));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.follow ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Ignore http_proxy/ALL_PROXY from the environment. Captive-portal detection
  // and the login itself must talk to the local gateway directly; routing them
  // through a proxy (or a VPN's proxy) either fails outright or, worse, makes a
  // captive network look online.
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");

  // Portal gateways very often present a self-signed or expired certificate.
  // Verification is off by default because refusing to talk to them would make
  // the tool useless; the only secret sent is the campus password the browser
  // would have sent to the same host anyway.
  if (insecure_) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  if (!interface_.empty()) {
    curl_easy_setopt(curl, CURLOPT_INTERFACE, interface_.c_str());
  }
  if (!req.referer.empty()) {
    curl_easy_setopt(curl, CURLOPT_REFERER, req.referer.c_str());
  }

  // Re-applied on every request: curl_easy_reset() drops these along with
  // everything else.
  curl_slist *resolve_list = nullptr;
  for (const std::string &entry : resolve_entries_) {
    resolve_list = curl_slist_append(resolve_list, entry.c_str());
  }
  if (resolve_list) curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);

  curl_slist *headers = nullptr;
  if (util::iequals(req.method, "POST")) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    headers = curl_slist_append(headers, ("Content-Type: " + req.content_type).c_str());
  } else if (!util::iequals(req.method, "GET")) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    if (!req.body.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }
  }
  for (const std::string &h : req.extra_headers) {
    headers = curl_slist_append(headers, h.c_str());
  }
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  log::debug(req.method + " " + req.url);
  CURLcode rc = curl_easy_perform(curl);
  if (headers) curl_slist_free_all(headers);
  if (resolve_list) curl_slist_free_all(resolve_list);

  if (rc != CURLE_OK) {
    resp.ok = false;
    resp.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    log::debug("  -> transport error: " + resp.error);
    return resp;
  }

  resp.ok = true;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  char *eff = nullptr;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
  resp.final_url = eff ? eff : req.url;
  log::debug("  -> " + std::to_string(resp.status) + " " + resp.final_url + " (" +
             std::to_string(resp.body.size()) + " bytes)");
  return resp;
}

Response Client::get(const std::string &url, bool follow, int timeout_sec) {
  Request req;
  req.url = url;
  req.method = "GET";
  req.follow = follow;
  req.timeout_sec = timeout_sec;
  return send(req);
}

Response Client::post(const std::string &url, const std::string &body, const std::string &referer) {
  Request req;
  req.url = url;
  req.method = "POST";
  req.body = body;
  req.referer = referer;
  req.follow = true;
  return send(req);
}

} // namespace sw::http
