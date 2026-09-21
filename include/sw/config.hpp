#pragma once

#include <map>
#include <string>
#include <vector>

namespace sw {

// Everything the connector needs to authenticate against one campus portal.
// Loaded from an INI-style file; every field has a usable default so a minimal
// config is just `username` plus (optionally) an `ssid` guard.
struct Config {
  // [network]
  std::string ssid;               // when set, login/watch only act on this SSID
  std::string interface = "en0";  // Wi-Fi interface to inspect and bind to
  // Nameserver used when the system resolver cannot resolve the portal's
  // hostname. Empty means "ask whatever this network offered over DHCP".
  std::string dns_server;

  // [account]
  std::string username;
  std::string keychain_service = "schoolwifi";
  std::string password;  // plaintext fallback; Keychain is preferred

  // [portal]
  // "form" discovers the login form on the portal page and submits it.
  // "raw"  submits `post_body` to `login_url` verbatim (for API-style portals).
  std::string login_method = "form";
  std::string login_url;   // override / required when login_method = raw
  std::string logout_url;
  std::string http_method = "POST";  // used by login_method = raw
  std::string post_body;             // template, supports {username} {password}
  std::string username_field;        // override auto-detection
  std::string password_field;
  std::map<std::string, std::string> extra_fields;  // `field.<name> = <value>`
  std::string success_contains;  // substring proving success, checked first
  std::string failure_contains;  // substring proving failure
  std::vector<std::string> probe_urls;
  int probe_timeout = 5;  // seconds per connectivity-check request
  std::string user_agent;

  // [watch]
  int online_interval = 30;   // seconds between probes while online
  int captive_interval = 5;   // seconds between probes while captive
  int max_retries = 3;        // consecutive login failures before backing off
  int retry_backoff = 30;     // seconds to wait after max_retries
  std::string log_file;

  std::string source_path;  // where this config was read from ("" if defaults)
};

std::string default_config_path();
// Returns false and fills `err` when the file exists but cannot be parsed.
// A missing file is not an error: `out` keeps its defaults.
bool load_config(const std::string &path, Config *out, std::string *err);
bool save_config(const Config &cfg, const std::string &path, std::string *err);

// Probe URLs actually used (config value, or the built-in default list).
std::vector<std::string> effective_probe_urls(const Config &cfg);
std::string effective_user_agent(const Config &cfg);

} // namespace sw
