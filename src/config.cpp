#include "sw/config.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <set>

#include <algorithm>
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

std::string config_dir() { return util::dirname(default_config_path()); }

bool matches_registered_network(const std::string &expected, const std::string &observed) {
  return !expected.empty() && !observed.empty() && expected == observed;
}

std::string network_config_path(const std::string &ssid) {
  // Encode unsafe bytes injectively; preserve Unicode and spaces for readability.
  std::string name;
  const char *hex = "0123456789ABCDEF";
  for (unsigned char c : ssid) {
    if (c < 32 || c == 127 || c == '/' || c == ':' || c == '%' || c == '\\' || c == '.') {
      name += '%'; name += hex[c >> 4]; name += hex[c & 15];
    } else name += static_cast<char>(c);
  }
  return config_dir() + "/networks/" + name + ".ini";
}

std::vector<Profile> discover_profiles() {
  std::vector<Profile> profiles;
  std::string dir = config_dir();

  std::vector<std::string> paths;
  for (const auto &folder : {dir, dir + "/networks"}) {
    DIR *handle = ::opendir(folder.c_str());
    if (handle == nullptr) continue;
    while (dirent *entry = ::readdir(handle)) {
      std::string name = entry->d_name;
      if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ini") != 0) continue;
      paths.push_back(folder + "/" + name);
    }
    ::closedir(handle);
  }
  std::sort(paths.begin(), paths.end());

  std::vector<std::string> later_stages;
  for (const std::string &path : paths) {
    Config cfg;
    std::string err;
    if (!load_config(path, &cfg, &err) || cfg.source_path.empty()) continue;

    Profile profile;
    profile.path = path;
    profile.ssid = cfg.ssid;
    profile.username = cfg.username;
    profiles.push_back(profile);

    if (!cfg.next_stage.empty()) later_stages.push_back(util::expand_tilde(cfg.next_stage));
  }

  // A consolidated profile supersedes legacy entry files for the same SSID.
  std::set<std::string> consolidated;
  for (const auto &path : paths) {
    Config cfg; std::string err;
    if (load_config(path, &cfg, &err) && cfg.network_profile) consolidated.insert(cfg.ssid);
  }
  for (Profile &profile : profiles) {
    Config cfg; std::string err;
    if (load_config(profile.path, &cfg, &err) && !cfg.network_profile && consolidated.count(cfg.ssid))
      profile.is_entry = false;
    for (const std::string &stage : later_stages) {
      if (stage == profile.path) profile.is_entry = false;
    }
  }
  return profiles;
}

std::string select_profile(const std::vector<Profile> &profiles, const std::string &ssid,
                           std::string *why) {
  if (ssid.empty()) {
    if (why) *why = "the current SSID could not be read";
    return "";
  }

  std::vector<const Profile *> matches;
  for (const Profile &profile : profiles) {
    if (!profile.is_entry) continue;
    if (profile.ssid == ssid) matches.push_back(&profile);
  }

  if (matches.size() == 1) return matches[0]->path;
  if (matches.empty()) {
    if (why) *why = "no profile names the SSID \"" + ssid + "\"";
    return "";
  }

  std::string listing;
  for (const Profile *profile : matches) {
    if (!listing.empty()) listing += ", ";
    listing += profile->path;
  }
  if (why) *why = "several profiles claim \"" + ssid + "\" (" + listing + "); pick one with -c";
  return "";
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
  Config *target = out;

  while (std::getline(is, line)) {
    ++lineno;
    std::string trimmed = util::trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
      section = util::lower(util::trim(trimmed.substr(1, trimmed.size() - 2)));
      target = out;
      if (util::starts_with(section, "stage.")) {
        const auto dot = section.find('.', 6);
        const auto number = section.substr(6, dot == std::string::npos ? dot : dot - 6);
        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos ||
            number.size() > 2 || std::stoi(number) < 1 || std::stoi(number) > 16) {
          if (err) *err = "invalid stage section";
          return false;
        }
        const auto index = static_cast<std::size_t>(std::stoi(number));
        if (out->stages.size() < index) out->stages.resize(index);
        target = &out->stages[index - 1];
      }
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
      target->extra_fields[raw_key.substr(6)] = value;
      continue;
    }

    if (key == "format") target->network_profile = value == "2";
    else if (key == "portal_match") target->portal_match = value;
    else if (key == "ssid") target->ssid = value;
    else if (key == "interface") target->interface = value;
    else if (key == "dns_server") target->dns_server = value;
    else if (key == "username") target->username = value;
    else if (key == "keychain_service") target->keychain_service = value;
    else if (key == "password") target->password = value;
    else if (key == "login_method") target->login_method = util::lower(value);
    else if (key == "login_url") target->login_url = value;
    else if (key == "logout_url") target->logout_url = value;
    else if (key == "http_method") target->http_method = value;
    else if (key == "post_body") target->post_body = value;
    else if (key == "username_field") target->username_field = value;
    else if (key == "password_field") target->password_field = value;
    else if (key == "success_contains") target->success_contains = value;
    else if (key == "failure_contains") target->failure_contains = value;
    else if (key == "user_agent") target->user_agent = value;
    else if (key == "service_suffix_id") target->service_suffix_id = value;
    else if (key == "next_stage") target->next_stage = value;
    else if (key == "probe_urls") {
      target->probe_urls.clear();
      for (const std::string &u : util::split(value, ',')) {
        std::string t = util::trim(u);
        if (!t.empty()) target->probe_urls.push_back(t);
      }
    }
    else if (key == "probe_timeout") target->probe_timeout = to_int(value, target->probe_timeout);
    else if (key == "online_interval") target->online_interval = to_int(value, target->online_interval);
    else if (key == "captive_interval") target->captive_interval = to_int(value, target->captive_interval);
    else if (key == "max_retries") target->max_retries = to_int(value, target->max_retries);
    else if (key == "retry_backoff") target->retry_backoff = to_int(value, target->retry_backoff);
    else if (key == "log_file") target->log_file = value;
    // Unknown keys are ignored on purpose: forward compatibility beats a hard
    // failure in a daemon that has to keep running.
  }

  return true;
}

static std::string config_text(const Config &cfg) {
  std::ostringstream os;

  os << "# schoolwifi configuration\n"
     << "# Generated " << util::now_iso() << "\n"
     << "# Docs: https://github.com/ahpasserby/schoolWIFIConnector\n\n";

  os << "[network]\n";
  if (cfg.network_profile) os << "format = 2\n";
  os << "# Only log in when associated with this SSID. Leave empty to act on any network.\n";
  os << "ssid = \"" << cfg.ssid << "\"\n";
  os << "interface = " << cfg.interface << "\n";
  os << "# Nameserver for the portal's hostname when the system DNS cannot resolve it.\n";
  os << "# Empty = use whatever this network hands out over DHCP.\n";
  os << "dns_server = " << cfg.dns_server << "\n\n";

  os << "[account]\n";
  os << "username = " << cfg.username << "\n";
  os << "keychain_service = " << cfg.keychain_service << "\n";
  os << "# The password lives in the macOS Keychain (see `schoolwifi setup`).\n";
  os << "# Setting `password = ...` here works but stores it in plaintext.\n\n";

  os << "[portal]\n";
  os << "portal_match = " << cfg.portal_match << "\n";
  os << "probe_urls = " << util::join(cfg.probe_urls, ",") << "\n";
  os << "user_agent = " << cfg.user_agent << "\n";
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
  os << "probe_timeout = " << cfg.probe_timeout << "\n";
  os << "service_suffix_id = " << cfg.service_suffix_id << "\n";
  os << "next_stage = " << cfg.next_stage << "\n";
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

  return os.str();
}

bool save_config(const Config &cfg, const std::string &path, std::string *err) {
  const std::string resolved = util::expand_tilde(path);
  if (cfg.network_profile) {
    for (const auto &stage : cfg.stages) {
      if (!stage.password.empty()) {
        if (err) *err = "move plaintext stage passwords to Keychain before updating the profile";
        return false;
      }
    }
  }
  std::string text = config_text(cfg);
  for (std::size_t i = 0; i < cfg.stages.size(); ++i) {
    std::istringstream stage(config_text(cfg.stages[i]));
    std::string line;
    while (std::getline(stage, line)) {
      if (!line.empty() && line.front() == '[')
        line.insert(1, "stage." + std::to_string(i + 1) + ".");
      text += line + "\n";
    }
  }
  if (!util::mkdir_p(util::dirname(resolved))) {
    if (err) *err = "could not create config directory";
    return false;
  }
  std::string temp = resolved + ".tmp.XXXXXX";
  int fd = ::mkstemp(&temp[0]);
  if (fd < 0) { if (err) *err = "could not create config file"; return false; }
  FILE *file = ::fdopen(fd, "w");
  if (!file) { ::close(fd); ::unlink(temp.c_str()); return false; }
  bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  if (std::fclose(file) != 0) ok = false;
  if (ok) ok = ::rename(temp.c_str(), resolved.c_str()) == 0;
  if (!ok) { ::unlink(temp.c_str()); if (err) *err = "could not save " + resolved; }
  return ok;
}

bool consolidate_network_config(sw::Config *cfg, std::string *err) {
  if (cfg->network_profile) return true;
  const sw::Config legacy = *cfg;
  cfg->network_profile = true;
  cfg->username.clear(); cfg->password.clear(); cfg->next_stage.clear();
  cfg->login_url.clear(); cfg->login_method = "form";
  cfg->stages.clear();
  std::vector<std::string> visited;
  sw::Config stage = legacy;
  while (!stage.username.empty()) {
    if (cfg->stages.size() >= 16 ||
        std::find(visited.begin(), visited.end(), stage.source_path) != visited.end()) {
      *err = "legacy stage chain is cyclic or too long"; return false;
    }
    visited.push_back(stage.source_path);
    if (!stage.ssid.empty() && stage.ssid != legacy.ssid) {
      *err = "legacy stages name different networks"; return false;
    }
    if (stage.login_method == "raw") {
      *err = "legacy raw requests require explicit -c; automatic migration is unsupported"; return false;
    }
    if (!stage.password.empty()) {
      *err = "legacy config contains a plaintext password; move it to Keychain before migration";
      return false;
    }
    const auto next = stage.next_stage;
    stage.next_stage.clear(); stage.stages.clear();
    cfg->stages.push_back(stage);
    if (next.empty()) break;
    stage = sw::Config{};
    if (!sw::load_config(next, &stage, err) || stage.source_path.empty()) {
      *err = "could not read legacy next_stage: " + next; return false;
    }
  }
  cfg->source_path = sw::network_config_path(cfg->ssid);
  if (sw::util::file_exists(cfg->source_path)) {
    *err = "network profile path already exists; refusing to overwrite " + cfg->source_path;
    return false;
  }
  return true;
}

} // namespace sw
