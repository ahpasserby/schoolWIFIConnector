#include "sw/config.hpp"

#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include "sw/util.hpp"

namespace sw {
namespace {

int to_int(const std::string &s, int fallback) {
  try {
    return std::stoi(util::trim(s));
  } catch (...) {
    return fallback;
  }
}

} // namespace

std::string default_config_path() {
  std::string home = util::home_dir();
  if (home.empty()) return "schoolwifi.ini";
  return home + "/.config/schoolwifi/config.ini";
}

std::vector<std::string> effective_probe_urls(const Config &cfg) {
  if (!cfg.probe_urls.empty()) return cfg.probe_urls;
  return {
      "http://captive.apple.com/hotspot-detect.html",
      "http://connectivitycheck.platform.hicloud.com/generate_204",
      "http://www.msftconnecttest.com/connecttest.txt",
  };
}

std::string effective_user_agent(const Config &cfg) {
  if (!cfg.user_agent.empty()) return cfg.user_agent;
  // Portals routinely serve a different (often broken) page to unknown agents,
  // so impersonate the Safari the user would otherwise have used.
  return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
         "(KHTML, like Gecko) Version/17.0 Safari/605.1.15";
}

bool load_config(const std::string &path, Config *out, std::string *err) {
  std::string resolved = util::expand_tilde(path);
  std::ifstream is(resolved);
  if (!is) return true;  // no file yet: defaults stand

  out->source_path = resolved;
  std::string line;
  std::string section;
  int lineno = 0;

  while (std::getline(is, line)) {
    ++lineno;
    std::string trimmed = util::trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
      section = util::lower(util::trim(trimmed.substr(1, trimmed.size() - 2)));
      continue;
    }

    std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      if (err) *err = resolved + ":" + std::to_string(lineno) + ": expected key = value";
      return false;
    }

    std::string key = util::lower(util::trim(trimmed.substr(0, eq)));
    std::string value = util::trim(trimmed.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }

    // `field.<name>` under [portal] becomes an extra POST field, preserving the
    // original case of <name> because portals are case-sensitive about them.
    std::string raw_key = util::trim(trimmed.substr(0, eq));
    if (util::starts_with(util::lower(raw_key), "field.")) {
      out->extra_fields[raw_key.substr(6)] = value;
      continue;
    }

    if (key == "ssid") out->ssid = value;
    else if (key == "interface") out->interface = value;
    else if (key == "username") out->username = value;
    else if (key == "keychain_service") out->keychain_service = value;
    else if (key == "password") out->password = value;
    else if (key == "login_method") out->login_method = util::lower(value);
    else if (key == "login_url") out->login_url = value;
    else if (key == "logout_url") out->logout_url = value;
    else if (key == "http_method") out->http_method = value;
    else if (key == "post_body") out->post_body = value;
    else if (key == "username_field") out->username_field = value;
    else if (key == "password_field") out->password_field = value;
    else if (key == "success_contains") out->success_contains = value;
    else if (key == "failure_contains") out->failure_contains = value;
    else if (key == "user_agent") out->user_agent = value;
    else if (key == "probe_urls") {
      out->probe_urls.clear();
      for (const std::string &u : util::split(value, ',')) {
        std::string t = util::trim(u);
        if (!t.empty()) out->probe_urls.push_back(t);
      }
    }
    else if (key == "online_interval") out->online_interval = to_int(value, out->online_interval);
    else if (key == "captive_interval") out->captive_interval = to_int(value, out->captive_interval);
    else if (key == "max_retries") out->max_retries = to_int(value, out->max_retries);
    else if (key == "retry_backoff") out->retry_backoff = to_int(value, out->retry_backoff);
    else if (key == "log_file") out->log_file = value;
    // Unknown keys are ignored on purpose: forward compatibility beats a hard
    // failure in a daemon that has to keep running.
  }

  return true;
}

bool save_config(const Config &cfg, const std::string &path, std::string *err) {
  std::string resolved = util::expand_tilde(path);
  std::ostringstream os;

  os << "# schoolwifi configuration\n"
     << "# Generated " << util::now_iso() << "\n"
     << "# Docs: https://github.com/ahpasserby/schoolWIFIConnector\n\n";

  os << "[network]\n";
  os << "# Only log in when associated with this SSID. Leave empty to act on any network.\n";
  os << "ssid = " << cfg.ssid << "\n";
  os << "interface = " << cfg.interface << "\n\n";

  os << "[account]\n";
  os << "username = " << cfg.username << "\n";
  os << "keychain_service = " << cfg.keychain_service << "\n";
  os << "# The password lives in the macOS Keychain (see `schoolwifi setup`).\n";
  os << "# Setting `password = ...` here works but stores it in plaintext.\n\n";

  os << "[portal]\n";
  os << "login_method = " << cfg.login_method << "\n";
  os << "login_url = " << cfg.login_url << "\n";
  os << "logout_url = " << cfg.logout_url << "\n";
  if (cfg.login_method == "raw") {
    os << "http_method = " << cfg.http_method << "\n";
    os << "post_body = " << cfg.post_body << "\n";
  }
  os << "username_field = " << cfg.username_field << "\n";
  os << "password_field = " << cfg.password_field << "\n";
  os << "success_contains = " << cfg.success_contains << "\n";
  os << "failure_contains = " << cfg.failure_contains << "\n";
  for (const auto &kv : cfg.extra_fields) {
    os << "field." << kv.first << " = " << kv.second << "\n";
  }
  os << "\n";

  os << "[watch]\n";
  os << "online_interval = " << cfg.online_interval << "\n";
  os << "captive_interval = " << cfg.captive_interval << "\n";
  os << "max_retries = " << cfg.max_retries << "\n";
  os << "retry_backoff = " << cfg.retry_backoff << "\n";
  os << "log_file = " << cfg.log_file << "\n";

  if (!util::write_file(resolved, os.str())) {
    if (err) *err = "could not write " + resolved;
    return false;
  }
  ::chmod(resolved.c_str(), 0600);
  return true;
}

} // namespace sw
