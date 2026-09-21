#include "sw/byod.hpp"

#include "sw/html.hpp"
#include "sw/log.hpp"
#include "sw/util.hpp"

namespace sw::byod {
namespace {

// The gateway hands these to the portal in the query string; index.js passes
// them straight through to /byod/byodrs/init.
const char *kGatewayParams[] = {"wlannasid", "usermac", "userurl", "userip", "ssid"};

} // namespace

bool looks_like_byod(const std::string &url, const std::string &html) {
  // Only the bootstrap shell, identified by the one script that performs the
  // init call. Matching any page under /byod/ was too loose: the login page
  // the init call sends us to lives there too, and asking init about *it*
  // returns the same answer again, forever, with the query string doubling on
  // every pass.
  if (!util::icontains(url, "/byod/")) return false;
  return util::icontains(html, "byod/resources/byod/index.js") ||
         util::icontains(html, "/byod/byodrs/");
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

bool looks_like_login_page(const std::string &url, const std::string &html) {
  if (!util::icontains(url, "/byod/")) return false;

  // Look for the actual pair of fields, not for the text anywhere on the page:
  // the visible boxes carry ids like "id_userName", and matching those would
  // claim any page that merely mentions them.
  for (const sw::html::Form &form : sw::html::extract_forms(html)) {
    if (form.find("userName") != nullptr && form.find("userPwd") != nullptr) return true;
  }
  return false;
}

std::string encode_password(const std::string &password, bool *ascii_only) {
  bool ascii = true;
  std::string escaped;
  for (char c : password) {
    if (static_cast<unsigned char>(c) > 0x7F) ascii = false;
    // imc_byod_function_base_escape doubles backslashes on ASCII input.
    if (c == '\\') escaped += '\\';
    escaped += c;
  }
  if (ascii_only) *ascii_only = ascii;
  return util::base64_encode(escaped);
}

namespace {

// Copies a value out of the init response verbatim, falling back to a literal
// when the portal did not send it, so the request keeps the portal's own types.
std::string carry_over(const std::string &init_json, const std::string &key,
                       const std::string &fallback) {
  std::string raw = util::json_raw_field(init_json, key);
  return raw.empty() ? fallback : raw;
}

std::string json_string(const std::string &value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

} // namespace

portal::LoginResult login(http::Client &client, const Config &cfg, const portal::LoginPage &page,
                          const std::string &password) {
  portal::LoginResult result;
  result.method = "POST";

  std::string origin = util::url_origin(page.url);
  if (origin.empty()) {
    result.message = "could not determine the portal's origin from " + page.url;
    return result;
  }

  // Step 1: the page's own init call, for the licence and policy identifiers
  // the login request has to echo back.
  std::string scheme = util::parse_url(page.url).scheme;
  std::string init_url = origin + "/byod/byodrs/login/init?hp=" + (scheme == "https" ? "0" : "1");
  log::info("byod: fetching login policy from " + init_url);

  http::Request init_req;
  init_req.url = init_url;
  init_req.method = "GET";
  init_req.follow = true;
  init_req.timeout_sec = 12;
  init_req.referer = page.url;
  init_req.extra_headers.push_back("X-Requested-With: XMLHttpRequest");

  http::Response init_resp = client.send(init_req);
  if (!init_resp.ok) {
    result.message = "byod login/init failed: " + init_resp.error;
    return result;
  }
  const std::string &policy = init_resp.body;

  // serviceSuffixId is -1 unless the portal offers a service list with a
  // default, exactly as templatePc.js decides it.
  std::string service = util::json_field(policy, "defaultServiceTypeId");
  if (service.empty()) service = "-1";

  // shopIdE and wlannasid come from the earlier /byod/byodrs/init response,
  // which resolve_login_page captured on the way here.
  std::string shop_id;
  std::string wlannasid;
  for (const auto &capture : page.captures) {
    if (capture.first != "byod-init.json") continue;
    shop_id = util::json_field(capture.second, "shopIdE");
    wlannasid = util::json_field(capture.second, "wlannasid");
  }

  bool ascii_only = true;
  std::string encoded = encode_password(password, &ascii_only);
  if (!ascii_only) {
    log::warn("byod: this password contains non-ASCII characters, which the portal escapes in a "
              "way schoolwifi does not reproduce; the login may be rejected");
  }

  std::string body = "{";
  body += "\"userName\":" + json_string(cfg.username);
  body += ",\"userPassword\":" + json_string(encoded);
  body += ",\"serviceSuffixId\":" + json_string(service);
  body += ",\"dynamicPwdAuth\":false";
  body += ",\"code\":\"\",\"codeTime\":\"\",\"validateCode\":\"\"";
  body += ",\"licenseCode\":" + carry_over(policy, "licenseCode", "\"\"");
  body += ",\"userGroupId\":" + carry_over(policy, "userGroupId", "\"\"");
  body += ",\"validationType\":" + carry_over(policy, "validationType", "0");
  body += ",\"guestManagerId\":" + carry_over(policy, "guestManagerId", "\"\"");
  body += ",\"shopIdE\":" + json_string(shop_id);
  body += ",\"wlannasid\":" + json_string(wlannasid);
  body += "}";

  http::Request req;
  req.url = origin + "/byod/byodrs/login/defaultLogin";
  req.method = "POST";
  req.body = body;
  req.content_type = "application/json";
  req.follow = true;
  req.timeout_sec = 15;
  req.referer = page.url;
  req.extra_headers.push_back("X-Requested-With: XMLHttpRequest");

  result.posted_to = req.url;
  result.sent_fields.emplace_back("userName", cfg.username);
  result.sent_fields.emplace_back("userPassword", "(base64) ********");
  result.sent_fields.emplace_back("serviceSuffixId", service);
  result.sent_fields.emplace_back("shopIdE", shop_id);
  result.sent_fields.emplace_back("wlannasid", wlannasid);

  log::info("byod: submitting login for " + cfg.username);
  http::Response resp = client.send(req);
  if (!resp.ok) {
    result.message = "byod defaultLogin failed: " + resp.error;
    return result;
  }

  result.status = resp.status;
  result.response_body = resp.body;

  std::string code = util::json_field(resp.body, "code");
  std::string msg = util::json_field(resp.body, "msg");
  if (code == "0") {
    result.success = true;
    result.message = msg.empty() ? "byod accepted the login" : "byod: " + msg;
    return result;
  }

  result.success = false;
  result.message = "byod rejected the login";
  if (!msg.empty()) result.message += ": " + msg;
  else if (!code.empty()) result.message += " (code " + code + ")";
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
