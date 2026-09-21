#pragma once

#include <string>
#include <vector>

#include "sw/config.hpp"
#include "sw/http.hpp"
#include "sw/portal.hpp"

// Huawei-style BYOD portals (Agile Controller / iMaster NCE). The page the
// gateway redirects to is an empty shell: no form, no redirect hint, and all
// of the decision made in /byod/resources/byod/index.js. That script reads the
// gateway's parameters out of the URL, calls /byod/byodrs/init, and navigates
// to whatever URL the response names. This reproduces that one exchange so the
// real login page can be reached without running JavaScript.
namespace sw::byod {

bool looks_like_byod(const std::string &url, const std::string &html);

struct InitResult {
  bool ok = false;
  std::string next_url;  // where the page's JavaScript would have gone
  std::string message;   // the portal's own msg, when it sent one
  std::string raw;       // the untouched response, for diagnostics
  long status = 0;
  bool already_registered = false;  // portal sent us to its "result" page
};

InitResult init(http::Client &client, const Config &cfg, const std::string &page_url);

// Splits out the pure part: given the init response and the page URL, work out
// where to go next, following the same rules as index.js.
InitResult interpret_init(const std::string &raw, const std::string &page_url);

// The login page itself. Its three inputs are all type=hidden: the visible
// boxes carry only `id`, and templatePc.js copies their values across before
// POSTing JSON to an API. The <form> is never submitted, so the generic form
// path cannot work here either.
bool looks_like_login_page(const std::string &url, const std::string &html);

// Encodes the password the way imc_byod_function_base64_with_chinese does.
// ASCII only -- the portal's own escape for non-ASCII is not reproduced, and
// `ascii_only` reports whether that matters for this password.
std::string encode_password(const std::string &password, bool *ascii_only);

// The services (operators, usually) the portal offers. serviceSuffixId must
// name one of these; -1 means "the portal offers no choice". Getting it wrong
// produces E63018, which also happens to be what a wrong account produces.
struct Service {
  std::string value;
  std::string label;
};
std::vector<Service> parse_service_list(const std::string &policy_json);

// Fetches /byod/byodrs/login/init. Exposed so `diagnose` can show what the
// login would have to choose between without attempting a login.
struct Policy {
  bool ok = false;
  std::string raw;
  std::string default_service_id;
  std::vector<Service> services;
  std::string message;
};
Policy fetch_policy(http::Client &client, const Config &cfg, const std::string &page_url);

portal::LoginResult login(http::Client &client, const Config &cfg, const portal::LoginPage &page,
                          const std::string &password);

} // namespace sw::byod
