#include "sw/byod.hpp"

#include "sw/log.hpp"
#include "sw/util.hpp"

namespace sw::byod {
namespace {

// The gateway hands these to the portal in the query string; index.js passes
// them straight through to /byod/byodrs/init.
const char *kGatewayParams[] = {"wlannasid", "usermac", "userurl", "userip", "ssid"};

} // namespace

bool looks_like_byod(const std::string &url, const std::string &html) {
  bool path_matches = util::icontains(url, "/byod/");
  bool page_matches = util::icontains(html, "/byod/byodrs/") ||
                      util::icontains(html, "/byod/resources/byod/") ||
                      (util::icontains(html, "<title>BYOD</title>") && path_matches);
  return path_matches && page_matches;
}

InitResult interpret_init(const std::string &raw, const std::string &page_url) {
  InitResult result;
  result.raw = raw;

  std::string body = util::strip_jsonp(raw);
  std::string code = util::json_field(body, "code");
  result.message = util::json_field(body, "msg");

  // code -1 / -2: the portal refused, and `data` is either empty or a URL to
  // show the failure on. index.js still navigates there, so follow it.
  if (code == "-1" || code == "-2") {
    std::string data = util::json_field(body, "data");
    if (data.empty()) {
      result.message = result.message.empty() ? "portal returned code " + code : result.message;
      return result;
    }
    result.next_url = util::resolve_url(page_url, data);
    result.ok = true;
    return result;
  }

  std::string url = util::json_field(body, "url");
  if (url.empty()) {
    result.message = result.message.empty()
                         ? "init response carried no url (code " + code + ")"
                         : result.message;
    return result;
  }

  // "…Result…" means the device is already registered; there is no login form
  // to fill in on this leg.
  if (util::icontains(url, "Result")) {
    result.already_registered = true;
  }

  // index.js appends the gateway's own query string and the page it should
  // return to. Reproduced exactly, including the '?' vs '&' choice.
  std::string query = util::query_string(page_url);
  std::string redirect = util::url_encode(page_url);

  if (url.find('?') != std::string::npos) {
    if (!query.empty()) url += "&" + query;
    url += "&nasRedirectUrl=" + redirect;
  } else {
    url += "?";
    if (!query.empty()) url += query + "&";
    url += "nasRedirectUrl=" + redirect;
  }

  result.next_url = util::resolve_url(page_url, url);
  result.ok = true;
  return result;
}

InitResult init(http::Client &client, const Config &cfg, const std::string &page_url) {
  InitResult result;

  std::string origin = util::url_origin(page_url);
  if (origin.empty()) {
    result.message = "could not determine the portal's origin from " + page_url;
    return result;
  }

  std::string url = origin + "/byod/byodrs/init?";
  bool first = true;
  for (const char *name : kGatewayParams) {
    if (!first) url += "&";
    first = false;
    url += std::string(name) + "=" + util::url_encode(util::query_param(page_url, name));
  }

  log::info("byod: asking " + origin + "/byod/byodrs/init where the login page is");

  http::Request req;
  req.url = url;
  req.method = "GET";
  req.follow = true;
  req.timeout_sec = cfg.probe_timeout > 0 ? cfg.probe_timeout * 2 : 10;
  req.referer = page_url;
  req.extra_headers.push_back("X-Requested-With: XMLHttpRequest");
  req.extra_headers.push_back("Accept: application/json, text/javascript, */*; q=0.01");

  http::Response resp = client.send(req);
  if (!resp.ok) {
    result.message = "byod init request failed: " + resp.error;
    return result;
  }

  InitResult parsed = interpret_init(resp.body, page_url);
  parsed.status = resp.status;
  if (parsed.ok) {
    log::info("byod: portal says the login page is " + parsed.next_url);
  } else {
    log::warn("byod: " + parsed.message);
  }
  return parsed;
}

} // namespace sw::byod
