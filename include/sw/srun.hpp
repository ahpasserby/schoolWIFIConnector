#pragma once

#include <string>

#include "sw/config.hpp"
#include "sw/http.hpp"
#include "sw/portal.hpp"

// Support for Srun (深澜) portals, which are single-page apps with no HTML
// form at all: the login parameters are computed in JavaScript from a
// server-issued challenge, so neither form submission nor a replayed request
// can work. See tests/srun_reference.py for the algorithm and the source of
// the test vectors.
namespace sw::srun {

bool looks_like_srun(const std::string &url, const std::string &html);

struct PortalInfo {
  bool ok = false;
  std::string origin;     // e.g. "https://w.bnbu.edu.cn"
  std::string ac_id;      // the portal's "acid", from the page or the URL
  std::string client_ip;  // the address the portal believes we are
  std::string note;
};

PortalInfo parse_portal_info(const std::string &page_url, const std::string &html);

// --- primitives, exported for testing ------------------------------------
std::string base64(const std::string &data);  // portal's own alphabet
std::string x_encode(const std::string &data, const std::string &key);
std::string hmac_md5_hex(const std::string &key, const std::string &data);
std::string sha1_hex(const std::string &data);

std::string build_info_json(const std::string &username, const std::string &password,
                            const std::string &ip, const std::string &ac_id);
std::string build_info_param(const std::string &info_json, const std::string &token);
std::string build_chksum(const std::string &token, const std::string &username,
                         const std::string &hmd5, const std::string &ac_id,
                         const std::string &ip, const std::string &n, const std::string &type,
                         const std::string &info_param);

portal::LoginResult login(http::Client &client, const Config &cfg, const PortalInfo &info,
                          const std::string &password);
portal::LoginResult logout(http::Client &client, const Config &cfg, const PortalInfo &info);

} // namespace sw::srun
