#pragma once

#include <string>
#include <utility>
#include <vector>

#include "sw/config.hpp"
#include "sw/http.hpp"

namespace sw::portal {

enum class State {
  Online,   // probe returned the expected success payload
  Captive,  // probe was intercepted: a portal is in the way
  Offline,  // probe could not reach anything at all
};

const char *state_name(State s);

struct Probe {
  State state = State::Offline;
  std::string probe_url;
  long status = 0;
  std::string location;  // Location header when the probe was redirected
  std::string body;
  std::string error;
  std::vector<std::string> redirects;
};

// Fetches a connectivity-check URL without following redirects so the portal's
// own address is visible in the Location header.
Probe probe(http::Client &client, const Config &cfg);

struct LoginPage {
  std::string url;
  std::string html;
  std::vector<std::string> trail;  // every hop taken to get here
  std::string note;
  // Intermediate API responses fetched during discovery -- the calls the
  // page's JavaScript would have made. `diagnose` writes these into its dump,
  // because when discovery goes wrong they are the only record of why.
  std::vector<std::pair<std::string, std::string>> captures;  // filename -> body
};

// Walks meta-refresh / JS / iframe hops from the probe response until a page
// containing a password field is found (or the hop budget runs out).
LoginPage resolve_login_page(http::Client &client, const Config &cfg, const Probe &pr);

struct LoginResult {
  bool success = false;
  std::string message;
  std::string posted_to;
  std::string method;
  long status = 0;
  std::string response_body;
  // Field names/values actually submitted. The password is masked before it is
  // recorded here, so this is safe to print in diagnostics.
  std::vector<std::pair<std::string, std::string>> sent_fields;
};

// Performs a full login: probe -> resolve page -> submit -> verify by
// re-probing. `password` is supplied by the caller (Keychain, env or config).
LoginResult login(http::Client &client, const Config &cfg, const std::string &password);

// Best-effort logout; requires `logout_url` in the config.
LoginResult logout(http::Client &client, const Config &cfg);

// Picks the form most likely to be the login form, and works out which of its
// fields carry the username and the password.
struct FormPlan {
  bool ok = false;
  std::string reason;
  std::string action_url;
  std::string method;
  std::string username_field;
  std::string password_field;
  std::vector<std::pair<std::string, std::string>> fields;  // ready to encode
};

// Why a login that was accepted at the protocol level still left the machine
// offline. Exposed for testing; distinguishes a rejected credential from a
// second portal taking over, which is what chained logins look like.
std::string explain_failed_verification(const Probe &last, const std::string &submitted_to);

FormPlan plan_form_login(const Config &cfg, const LoginPage &page, const std::string &username,
                         const std::string &password);

} // namespace sw::portal
