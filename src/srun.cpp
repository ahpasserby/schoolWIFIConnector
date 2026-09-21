#include "sw/srun.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

#include "sw/log.hpp"
#include "sw/util.hpp"

namespace sw::srun {
namespace {

const char kAlphabet[] = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";

std::string to_hex(const unsigned char *bytes, std::size_t len) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

// Packs a byte string into little-endian 32-bit words, optionally appending
// the original length as a final word (the JS `s(a, true)`).
std::vector<std::uint32_t> to_words(const std::string &data, bool append_length) {
  std::vector<std::uint32_t> words;
  for (std::size_t i = 0; i < data.size(); i += 4) {
    std::uint32_t n = 0;
    for (std::size_t j = 0; j < 4; ++j) {
      if (i + j < data.size()) {
        n |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + j])) << (8 * j);
      }
    }
    words.push_back(n);
  }
  if (append_length) words.push_back(static_cast<std::uint32_t>(data.size()));
  return words;
}

std::string from_words(const std::vector<std::uint32_t> &words) {
  std::string out;
  out.reserve(words.size() * 4);
  for (std::uint32_t w : words) {
    out += static_cast<char>(w & 0xFF);
    out += static_cast<char>((w >> 8) & 0xFF);
    out += static_cast<char>((w >> 16) & 0xFF);
    out += static_cast<char>((w >> 24) & 0xFF);
  }
  return out;
}

std::string now_millis() {
  return std::to_string(static_cast<long long>(std::time(nullptr)) * 1000);
}

// Reads `key : "value"` out of the page's inline `var CONFIG = {...}` block.
std::string js_config_string(const std::string &html, const std::string &key) {
  std::size_t pos = 0;
  while ((pos = util::ifind(html, key, pos)) != std::string::npos) {
    std::size_t after = pos + key.size();
    // The key must stand alone, not be a suffix of a longer identifier.
    if (pos > 0 && (std::isalnum(static_cast<unsigned char>(html[pos - 1])) || html[pos - 1] == '_')) {
      pos = after;
      continue;
    }
    std::size_t i = after;
    while (i < html.size() && std::isspace(static_cast<unsigned char>(html[i]))) ++i;
    if (i >= html.size() || html[i] != ':') {
      pos = after;
      continue;
    }
    ++i;
    while (i < html.size() && std::isspace(static_cast<unsigned char>(html[i]))) ++i;
    if (i >= html.size() || (html[i] != '"' && html[i] != '\'')) {
      pos = after;
      continue;
    }
    char quote = html[i++];
    std::size_t close = html.find(quote, i);
    if (close == std::string::npos) return "";
    return html.substr(i, close - i);
  }
  return "";
}

} // namespace

std::string base64(const std::string &data) {
  std::string out;
  std::size_t imax = data.size() - data.size() % 3;

  for (std::size_t i = 0; i < imax; i += 3) {
    std::uint32_t n = (static_cast<unsigned char>(data[i]) << 16) |
                      (static_cast<unsigned char>(data[i + 1]) << 8) |
                      static_cast<unsigned char>(data[i + 2]);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }

  std::size_t rest = data.size() - imax;
  if (rest == 1) {
    std::uint32_t n = static_cast<std::uint32_t>(static_cast<unsigned char>(data[imax])) << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += "==";
  } else if (rest == 2) {
    std::uint32_t n = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[imax])) << 16) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(data[imax + 1])) << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

std::string x_encode(const std::string &data, const std::string &key) {
  if (data.empty()) return "";

  std::vector<std::uint32_t> v = to_words(data, true);
  std::vector<std::uint32_t> k = to_words(key, false);
  while (k.size() < 4) k.push_back(0);

  const std::size_t n = v.size() - 1;
  std::uint32_t z = v[n];
  std::uint32_t y = 0;
  const std::uint32_t c = 0x9E3779B9u;
  std::uint32_t d = 0;

  // uint32_t arithmetic wraps, which is exactly the masking the JS does
  // explicitly with `& 0xFFFFFFFF`.
  int q = static_cast<int>(6 + 52 / (n + 1));
  while (q-- > 0) {
    d += c;
    std::uint32_t e = (d >> 2) & 3;

    std::size_t p = 0;
    for (; p < n; ++p) {
      y = v[p + 1];
      std::uint32_t m = (z >> 5) ^ (y << 2);
      m += ((y >> 3) ^ (z << 4)) ^ (d ^ y);
      m += k[(p & 3) ^ e] ^ z;
      v[p] += m;
      z = v[p];
    }

    y = v[0];
    std::uint32_t m = (z >> 5) ^ (y << 2);
    m += ((y >> 3) ^ (z << 4)) ^ (d ^ y);
    m += k[(p & 3) ^ e] ^ z;
    v[n] += m;
    z = v[n];
  }

  return from_words(v);
}

std::string hmac_md5_hex(const std::string &key, const std::string &data) {
  unsigned char digest[CC_MD5_DIGEST_LENGTH];
  CCHmac(kCCHmacAlgMD5, key.data(), key.size(), data.data(), data.size(), digest);
  return to_hex(digest, sizeof(digest));
}

std::string sha1_hex(const std::string &data) {
// The portal's protocol mandates SHA-1; CryptoKit is not reachable from C++,
// and this is a protocol transcription, not a security choice.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  unsigned char digest[CC_SHA1_DIGEST_LENGTH];
  CC_SHA1(data.data(), static_cast<CC_LONG>(data.size()), digest);
#pragma clang diagnostic pop
  return to_hex(digest, sizeof(digest));
}

std::string build_info_json(const std::string &username, const std::string &password,
                            const std::string &ip, const std::string &ac_id) {
  // Key order matches JSON.stringify in the portal's JavaScript.
  return "{\"username\":\"" + username + "\",\"password\":\"" + password + "\",\"ip\":\"" + ip +
         "\",\"acid\":\"" + ac_id + "\",\"enc_ver\":\"srun_bx1\"}";
}

std::string build_info_param(const std::string &info_json, const std::string &token) {
  return "{SRBX1}" + base64(x_encode(info_json, token));
}

std::string build_chksum(const std::string &token, const std::string &username,
                         const std::string &hmd5, const std::string &ac_id, const std::string &ip,
                         const std::string &n, const std::string &type,
                         const std::string &info_param) {
  std::string chkstr = token + username + token + hmd5 + token + ac_id + token + ip + token + n +
                       token + type + token + info_param;
  return sha1_hex(chkstr);
}

bool looks_like_srun(const std::string &url, const std::string &html) {
  return util::icontains(url, "srun_portal") || util::icontains(html, "srunsoft") ||
         util::icontains(html, "srun_bx1") || util::icontains(html, "/cgi-bin/srun_portal");
}

PortalInfo parse_portal_info(const std::string &page_url, const std::string &html) {
  PortalInfo info;
  info.origin = util::url_origin(page_url);

  info.ac_id = js_config_string(html, "acid");
  if (info.ac_id.empty()) info.ac_id = util::query_param(page_url, "ac_id");
  if (info.ac_id.empty()) {
    info.ac_id = "1";  // the near-universal default
    info.note = "ac_id not found on the page; assuming 1";
  }

  info.client_ip = js_config_string(html, "ip");

  if (info.origin.empty()) {
    info.note = "could not determine the portal's origin from " + page_url;
    return info;
  }
  info.ok = true;
  return info;
}

portal::LoginResult login(http::Client &client, const Config &cfg, const PortalInfo &info,
                          const std::string &password) {
  portal::LoginResult result;
  result.method = "GET";

  const std::string callback = "jQuery_" + now_millis();
  std::string ip = info.client_ip;

  // Step 1: ask the portal for a challenge token. Its reply also tells us the
  // address it sees us on, which is more authoritative than the page's.
  std::string challenge_url = info.origin + "/cgi-bin/get_challenge?callback=" + callback +
                              "&username=" + util::url_encode(cfg.username) +
                              "&ip=" + util::url_encode(ip) + "&_=" + now_millis();

  http::Response challenge = client.get(challenge_url, /*follow=*/true, 10);
  if (!challenge.ok) {
    result.message = "get_challenge failed: " + challenge.error;
    return result;
  }

  std::string body = util::strip_jsonp(challenge.body);
  std::string token = util::json_field(body, "challenge");
  if (token.empty()) {
    std::string err = util::json_field(body, "error");
    result.message = "portal issued no challenge" + (err.empty() ? "" : " (" + err + ")");
    result.response_body = challenge.body;
    return result;
  }

  std::string online_ip = util::json_field(body, "online_ip");
  if (online_ip.empty()) online_ip = util::json_field(body, "client_ip");
  if (!online_ip.empty()) ip = online_ip;
  if (ip.empty()) {
    result.message = "could not determine this machine's address as the portal sees it";
    return result;
  }

  // Step 2: build the signed payload exactly as the portal's JavaScript does.
  const std::string n = "200";
  const std::string type = "1";

  std::string info_json = build_info_json(cfg.username, password, ip, info.ac_id);
  std::string info_param = build_info_param(info_json, token);
  std::string hmd5 = hmac_md5_hex(token, password);
  std::string chksum = build_chksum(token, cfg.username, hmd5, info.ac_id, ip, n, type, info_param);

  std::string login_url = info.origin + "/cgi-bin/srun_portal?callback=" + callback +
                          "&action=login" + "&username=" + util::url_encode(cfg.username) +
                          "&password=" + util::url_encode("{MD5}" + hmd5) +
                          "&ac_id=" + util::url_encode(info.ac_id) + "&ip=" + util::url_encode(ip) +
                          "&chksum=" + chksum + "&info=" + util::url_encode(info_param) +
                          "&n=" + n + "&type=" + type + "&os=Mac+OS&name=Mac&double_stack=0&_=" +
                          now_millis();

  result.posted_to = info.origin + "/cgi-bin/srun_portal";
  result.sent_fields.emplace_back("action", "login");
  result.sent_fields.emplace_back("username", cfg.username);
  result.sent_fields.emplace_back("password", "{MD5}********");
  result.sent_fields.emplace_back("ac_id", info.ac_id);
  result.sent_fields.emplace_back("ip", ip);
  result.sent_fields.emplace_back("chksum", chksum);

  log::info("srun: challenge obtained, submitting login for " + cfg.username + " (ip " + ip +
            ", acid " + info.ac_id + ")");

  http::Response resp = client.get(login_url, /*follow=*/true, 12);
  if (!resp.ok) {
    result.message = "srun_portal request failed: " + resp.error;
    return result;
  }

  std::string reply = util::strip_jsonp(resp.body);
  result.status = resp.status;
  result.response_body = resp.body;

  std::string error = util::json_field(reply, "error");
  std::string error_msg = util::json_field(reply, "error_msg");
  std::string suc_msg = util::json_field(reply, "suc_msg");

  if (util::iequals(error, "ok")) {
    result.success = true;
    result.message = suc_msg.empty() ? "srun reported ok" : "srun: " + suc_msg;
    return result;
  }

  result.success = false;
  result.message = "srun rejected the login";
  if (!error.empty()) result.message += ": " + error;
  if (!error_msg.empty() && error_msg != error) result.message += " (" + error_msg + ")";
  return result;
}

portal::LoginResult logout(http::Client &client, const Config &cfg, const PortalInfo &info) {
  portal::LoginResult result;
  result.method = "GET";

  std::string url = info.origin + "/cgi-bin/srun_portal?callback=jQuery_" + now_millis() +
                    "&action=logout&username=" + util::url_encode(cfg.username) +
                    "&ip=" + util::url_encode(info.client_ip) +
                    "&ac_id=" + util::url_encode(info.ac_id) + "&_=" + now_millis();
  result.posted_to = url;

  http::Response resp = client.get(url, /*follow=*/true, 10);
  if (!resp.ok) {
    result.message = "logout request failed: " + resp.error;
    return result;
  }

  std::string reply = util::strip_jsonp(resp.body);
  std::string error = util::json_field(reply, "error");
  result.status = resp.status;
  result.response_body = resp.body;
  result.success = util::iequals(error, "ok") || util::icontains(reply, "logout_ok");
  result.message = result.success ? "srun: logged out" : "srun: " + (error.empty() ? "unexpected reply" : error);
  return result;
}

} // namespace sw::srun
