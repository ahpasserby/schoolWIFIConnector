#include <mach-o/dyld.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "sw/byod.hpp"
#include "sw/config.hpp"
#include "sw/dns.hpp"
#include "sw/html.hpp"
#include "sw/http.hpp"
#include "sw/keychain.hpp"
#include "sw/log.hpp"
#include "sw/netenv.hpp"
#include "sw/portal.hpp"
#include "sw/srun.hpp"
#include "sw/util.hpp"
#include "sw/wifi.hpp"

namespace {

constexpr const char *kVersion = "0.2.0";
constexpr const char *kAgentLabel = "com.ahpasserby.schoolwifi";

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
  std::string config_path;
  bool verbose = false;
  bool quiet = false;
  std::vector<std::string> args;  // command + positional arguments
};

void print_usage() {
  std::printf(R"(schoolwifi %s - campus captive-portal connector for macOS

USAGE
  schoolwifi <command> [options]

COMMANDS
  status           Show Wi-Fi and portal state (online / captive / offline)
  login            Authenticate against the portal now
  logout           Send the configured logout request
  open             Open the real portal page in your browser (manual fallback)
  watch            Stay resident and log in whenever the portal reappears
  diagnose         Dump portal discovery details to help write a config
  setup            Register or update the currently visible authentication step
  install-agent    Install the LaunchAgent so watch runs at login
  uninstall-agent  Remove the LaunchAgent
  agent-status     Show whether the LaunchAgent is loaded
  profiles         List configured networks and show which one applies here
  version          Print the version

OPTIONS
  -c, --config PATH  Config file. Without it, the profile whose ssid matches
                     the current network is used; see `schoolwifi profiles`
  -v, --verbose      Log every HTTP request and redirect
  -q, --quiet        Only log warnings and errors
  -h, --help         This message

ENVIRONMENT
  SCHOOLWIFI_PASSWORD  Password override for explicit legacy configs only
)",
              kVersion);
}

std::string executable_path() {
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return "schoolwifi";
  char resolved[4096];
  if (::realpath(buf, resolved)) return resolved;
  return buf;
}

std::string agent_plist_path() {
  return sw::util::home_dir() + "/Library/LaunchAgents/" + kAgentLabel + ".plist";
}

bool parse_options(int argc, char **argv, Options *opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage();
      std::exit(0);
    } else if (arg == "-v" || arg == "--verbose") {
      opts->verbose = true;
    } else if (arg == "-q" || arg == "--quiet") {
      opts->quiet = true;
    } else if (arg == "-c" || arg == "--config") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a path\n", arg.c_str());
        return false;
      }
      opts->config_path = argv[++i];
    } else if (sw::util::starts_with(arg, "--config=")) {
      opts->config_path = arg.substr(9);
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "error: unknown option %s\n", arg.c_str());
      return false;
    } else {
      opts->args.push_back(arg);
    }
  }
  return true;
}

// Password sources, in order of precedence. SCHOOLWIFI_PASSWORD is a manual
// override for the login being invoked, so it does not apply to a chained
// later stage: that stage authenticates somewhere else, with its own account.
bool resolve_password(const sw::Config &cfg, std::string *out, std::string *err,
                      bool allow_env = true) {
  if (allow_env) {
    if (const char *env = std::getenv("SCHOOLWIFI_PASSWORD"); env && *env) {
      *out = env;
      return true;
    }
  }
  if (!cfg.username.empty()) {
    std::string kc_err;
    if (sw::keychain::get_password(cfg.keychain_service, cfg.username, out, &kc_err)) return true;
    sw::log::debug("keychain: " + kc_err);
  }
  if (!cfg.password.empty()) {
    sw::log::warn("using the plaintext password from the config file");
    *out = cfg.password;
    return true;
  }
  *err = "no password available; run `schoolwifi setup` or set SCHOOLWIFI_PASSWORD";
  return false;
}

// Explicit legacy configs may opt out of SSID pinning; pinned configs fail closed.
bool ssid_allows_action(const sw::Config &cfg, const sw::wifi::Info &info) {
  if (cfg.ssid.empty()) return true;
  if (info.ssid.empty()) {
    sw::log::error("SSID unreadable; refusing to send saved credentials");
    return false;
  }
  if (sw::matches_registered_network(cfg.ssid, info.ssid)) return true;
  sw::log::info("on \"" + info.ssid + "\", config targets \"" + cfg.ssid + "\" - skipping");
  return false;
}

void make_client(sw::http::Client *client, const sw::Config &cfg) {
  client->set_user_agent(sw::effective_user_agent(cfg));
  client->set_interface(cfg.interface);
}

int cmd_status(const sw::Config &cfg) {
  sw::wifi::Info info = sw::wifi::current(cfg.interface);
  sw::http::Client client;
  make_client(&client, cfg);
  sw::portal::Probe pr = sw::portal::probe(client, cfg);

  std::printf("Interface    %s%s\n", info.interface.c_str(), info.power_on ? "" : " (Wi-Fi off)");
  std::printf("SSID         %s\n", info.ssid.empty() ? "(unavailable)" : info.ssid.c_str());
  if (!info.bssid.empty()) std::printf("BSSID        %s\n", info.bssid.c_str());
  std::printf("IPv4         %s\n", info.ipv4.empty() ? "(none)" : info.ipv4.c_str());
  std::printf("Portal       %s\n", sw::portal::state_name(pr.state));

  if (pr.state == sw::portal::State::Captive) {
    std::string where = pr.location.empty() ? pr.probe_url
                                            : sw::util::resolve_url(pr.probe_url, pr.location);
    std::printf("Intercept    %s (HTTP %ld)\n", where.c_str(), pr.status);
    std::printf("\nRun `schoolwifi login` to authenticate, or `schoolwifi open` to do it by hand.\n");
  } else if (pr.state == sw::portal::State::Offline) {
    std::printf("Detail       %s\n", pr.error.empty() ? "no route to the probe hosts" : pr.error.c_str());
  }

  sw::netenv::LinkState link = sw::netenv::assess_link(
      info.power_on, info.interface, info.ipv4, sw::netenv::default_gateway());
  if (!link.up) std::printf("Link         %s\n", link.reason.c_str());

  std::printf("Config       %s\n", cfg.source_path.empty() ? "(defaults, no file)" : cfg.source_path.c_str());
  if (cfg.network_profile) std::printf("Stages       %zu\n", cfg.stages.size());
  if (!cfg.next_stage.empty()) {
    std::printf("Next stage   %s\n", sw::util::expand_tilde(cfg.next_stage).c_str());
  }
  return pr.state == sw::portal::State::Online ? 0 : 1;
}

// Networks that authenticate in stages need a second login, with different
// credentials, against a portal that only appears once the first one is
// satisfied. Each stage is an ordinary config file; `next_stage` chains them.
constexpr int kMaxLoginStages = 4;

int cmd_login(const sw::Config &cfg, int stage = 1) {
  if (cfg.username.empty()) {
    sw::log::error("no username configured; run `schoolwifi setup`");
    return 2;
  }

  sw::wifi::Info info = sw::wifi::current(cfg.interface);
  if (!ssid_allows_action(cfg, info)) return 0;

  std::string password;
  std::string err;
  if (!resolve_password(cfg, &password, &err, /*allow_env=*/stage == 1)) {
    sw::log::error(err);
    return 2;
  }

  sw::http::Client client;
  make_client(&client, cfg);
  sw::portal::LoginResult res = sw::portal::login(client, cfg, password);

  if (res.success) {
    sw::log::info("connected: " + res.message);
    return 0;
  }

  // This login was accepted; another portal simply took over. Carry on with
  // the next stage's config rather than reporting a failure.
  if (!res.next_portal.empty() && !cfg.next_stage.empty()) {
    if (stage >= kMaxLoginStages) {
      sw::log::error("stopped after " + std::to_string(stage) +
                     " authentication stages; next_stage is probably looping");
      return 1;
    }

    std::string next_path = sw::util::expand_tilde(cfg.next_stage);
    sw::log::info("stage " + std::to_string(stage) + " done; " + res.next_portal +
                  " now wants authentication too -- continuing with " + next_path);

    sw::Config next;
    std::string load_err;
    if (!sw::load_config(next_path, &next, &load_err)) {
      sw::log::error("could not read " + next_path + ": " + load_err);
      return 2;
    }
    if (next.source_path.empty()) {
      sw::log::error("next_stage points at " + next_path + ", which does not exist");
      return 2;
    }
    return cmd_login(next, stage + 1);
  }

  sw::log::error("login failed: " + res.message);

  // Check this before blaming DNS: with no lease every name fails to resolve,
  // and pointing at dns_server sends people to debug the wrong thing.
  sw::netenv::LinkState link = sw::netenv::assess_link(
      info.power_on, info.interface, info.ipv4, sw::netenv::default_gateway());
  if (!link.up) {
    sw::log::info(link.reason);
    sw::log::info("  - nothing here is a portal problem yet; wait for an IP address");
    sw::log::info("  - `schoolwifi status` shows one as soon as the network is joined");
    return 1;
  }

  if (sw::dns::is_resolve_failure(res.message)) {
    sw::log::info("the portal's hostname did not resolve, even via this network's DNS");
    sw::log::info("  - check `schoolwifi diagnose` for the dns section");
    sw::log::info("  - or set dns_server = <campus DNS> under [network] in the config");
  }
  if (!res.posted_to.empty()) {
    sw::log::info("submitted " + res.method + " to " + res.posted_to);
    for (const auto &kv : res.sent_fields) {
      sw::log::info("  " + kv.first + " = " + kv.second);
    }
  }
  sw::log::info("run `schoolwifi diagnose` to inspect the portal page");
  return 1;
}

// Network profiles bind credentials to the currently observed portal, rather
// than assuming that the first authentication stage always needs to run.

int network_login(sw::Config cfg, bool interactive, bool edit = false) {
  sw::http::Client client;
  make_client(&client, cfg);
  std::vector<std::string> attempted;
  for (int step = 0; step < 16; ++step) {
    if (!cfg.ssid.empty()) {
      const auto here = sw::wifi::current(cfg.interface);
      if (!sw::matches_registered_network(cfg.ssid, here.ssid)) {
        sw::log::error("Wi-Fi changed or its name is unavailable; no credentials sent"); return 2;
      }
    }
    auto pr = sw::portal::probe(client, cfg);
    if (pr.state == sw::portal::State::Online) {
      sw::log::info("connected: verified online");
      if (edit) std::printf("当前已经联网，无需登记；下次出现认证页面时运行 schoolwifi login。\n");
      return 0;
    }
    if (pr.state != sw::portal::State::Captive) {
      sw::log::error("no reachable portal; check Wi-Fi and run schoolwifi diagnose"); return 1;
    }
    auto page = sw::portal::resolve_login_page(client, cfg, pr);
    const auto identity = sw::portal::stage_identity(page);
    if (identity.empty()) {
      sw::log::error("unrecognized portal; run schoolwifi diagnose or use schoolwifi open"); return 1;
    }
    if (std::find(attempted.begin(), attempted.end(), identity) != attempted.end()) {
      sw::log::error("portal repeated after authentication; stopping instead of retrying credentials");
      return 1;
    }
    int selected = -1;
    for (std::size_t i = 0; i < cfg.stages.size(); ++i) {
      if (cfg.stages[i].portal_match == identity) {
        if (selected >= 0) { sw::log::error("multiple stages match this portal; fix the profile"); return 2; }
        selected = static_cast<int>(i);
      }
    }
    bool enrolled = false;
    if (selected < 0 || edit) {
      if (!interactive) {
        sw::log::error("this portal is not enrolled; run schoolwifi login in an interactive terminal");
        return 2;
      }
      std::printf("\n网络：%s\n认证页面：%s\n地址：%s\n", cfg.ssid.c_str(),
                  sw::html::title(page.html).c_str(), sw::util::url_origin(page.url).c_str());
      if (selected < 0) {
        std::printf("需要登记这一道认证。只填写这个登录页面所需的账号。\n");
        std::vector<int> choices;
        for (std::size_t i = 0; i < cfg.stages.size(); ++i) {
          if (cfg.stages[i].portal_match.empty()) {
            choices.push_back(static_cast<int>(i));
            std::printf("  %zu. 使用已有账号 %s\n", choices.size(), cfg.stages[i].username.c_str());
          }
        }
        if (!choices.empty()) {
          auto answer = sw::util::read_line("选择已有账号的编号，或输入 new 登记新账号（回车取消）： ");
          if (answer.empty()) return 2;
          if (answer != "new") {
            if (answer.find_first_not_of("0123456789") != std::string::npos || answer.size() > 2 ||
                std::stoi(answer) < 1 || std::stoi(answer) > static_cast<int>(choices.size())) {
              sw::log::error("invalid choice; nothing saved"); return 2;
            }
            selected = choices[std::stoi(answer) - 1];
          }
        }
      }
      if (selected < 0 || edit) {
        if (selected < 0) {
          if (cfg.stages.size() >= 16) { sw::log::error("too many authentication stages"); return 2; }
          cfg.stages.emplace_back();
          selected = static_cast<int>(cfg.stages.size() - 1);
        }
        auto &stage = cfg.stages[selected];
        auto username = sw::util::read_line("账号（回车取消）： ");
        if (username.empty() || username.find_first_of("\r\n") != std::string::npos) return 2;
        auto password = sw::util::read_password("密码（不显示，回车取消）： ");
        if (password.empty()) return 2;
        stage.username = username;
        stage.keychain_service = "schoolwifi/network/" + sw::util::url_encode(cfg.ssid) +
                                 "/stage/" + std::to_string(selected + 1);
        std::string err;
        if (!sw::keychain::set_password(stage.keychain_service, username, password, &err)) {
          sw::log::error(err); return 2;
        }
      }
      auto &stage = cfg.stages[selected];
      stage.portal_match = identity;
      // Keep legacy custom protocol settings; discovery reuses the observed page.
      if (stage.login_method == "raw") {
        sw::log::error("legacy raw requests require explicit -c; cannot bind automatically"); return 2;
      }
      stage.login_method = identity.rfind("srun ", 0) == 0 ? "srun" : "form";
      stage.password.clear(); stage.next_stage.clear();
      std::string err;
      if (!sw::save_config(cfg, cfg.source_path, &err)) { sw::log::error(err); return 2; }
      std::printf("已保存到 %s；密码保存在钥匙串。\n", cfg.source_path.c_str());
      edit = false;
      enrolled = true;
    }
    if (enrolled) {
      pr = sw::portal::probe(client, cfg);
      if (pr.state == sw::portal::State::Online) { sw::log::info("connected: verified online"); return 0; }
      if (pr.state != sw::portal::State::Captive) return 1;
      page = sw::portal::resolve_login_page(client, cfg, pr);
      if (sw::portal::stage_identity(page) != identity) {
        sw::log::error("portal changed during enrollment; run login again"); return 1;
      }
    }
    sw::Config stage = cfg.stages[selected];
    stage.ssid = cfg.ssid; stage.interface = cfg.interface;
    stage.dns_server = cfg.dns_server; stage.probe_urls = cfg.probe_urls;
    stage.probe_timeout = cfg.probe_timeout;
    if (stage.login_method == "raw") { sw::log::error("raw stages require explicit legacy config"); return 2; }
    std::string password, err;
    if (!resolve_password(stage, &password, &err, /*allow_env=*/false)) {
      sw::log::error("no password for this stage; run schoolwifi setup to update this portal's account"); return 2;
    }
    // Recheck association after an interactive prompt; never submit on a changed SSID.
    if (!cfg.ssid.empty() && !sw::matches_registered_network(cfg.ssid, sw::wifi::current(cfg.interface).ssid)) {
      sw::log::error("Wi-Fi changed while registering; please retry on the intended network"); return 2;
    }
    attempted.push_back(identity);
    sw::log::info("using authentication stage " + std::to_string(selected + 1));
    client.set_user_agent(sw::effective_user_agent(stage));
    auto result = sw::portal::login(client, stage, password, &pr, &page);
    if (result.success) { sw::log::info("connected: " + result.message); return 0; }
    // Re-discover even for same-host stage transitions; no credential guessing.
    auto after = sw::portal::probe(client, cfg);
    if (after.state == sw::portal::State::Online) { sw::log::info("connected: verified online"); return 0; }
    if (after.state == sw::portal::State::Captive) {
      auto next = sw::portal::resolve_login_page(client, cfg, after);
      auto next_id = sw::portal::stage_identity(next);
      if (!next_id.empty() && next_id != identity) continue;
    }
    sw::log::error("login failed: " + result.message + "; use schoolwifi setup to update this account");
    return 1;
  }
  sw::log::error("too many portal transitions"); return 1;
}

int automatic_login(bool interactive, bool edit = false) {
  auto info = sw::wifi::current("");
  if (!info.power_on || info.ssid.empty()) {
    sw::log::error("connect to Wi-Fi first; its name must be readable before automatic login"); return 2;
  }
  if (info.ssid.find_first_of("\r\n") != std::string::npos) {
    sw::log::error("Wi-Fi name contains unsupported line breaks"); return 2;
  }
  auto profiles = sw::discover_profiles();
  std::string why, err;
  auto path = sw::select_profile(profiles, info.ssid, &why);
  if (path.empty()) {
    std::vector<std::string> choices;
    for (const auto &profile : profiles) {
      if (profile.is_entry && profile.ssid == info.ssid) choices.push_back(profile.path);
    }
    if (!choices.empty()) {
      if (!interactive) {
        sw::log::error(why + "; run schoolwifi login in a terminal to choose a profile");
        return 2;
      }
      std::printf("这个 WiFi 有多份已保存配置，请选择这次使用哪一份。原文件都会保留。\n");
      for (std::size_t i = 0; i < choices.size(); ++i) {
        sw::Config candidate;
        std::string load_error;
        sw::load_config(choices[i], &candidate, &load_error);
        std::printf("  %zu. %s%s\n", i + 1, choices[i].c_str(),
                    candidate.network_profile ? "（新网络配置）" :
                    candidate.next_stage.empty() ? "（旧单道配置）" : "（旧配置，包含后续认证）");
      }
      const auto answer = sw::util::read_line("输入编号（回车取消）： ");
      if (answer.empty()) return 2;
      if (answer.size() > 6 || answer.find_first_not_of("0123456789") != std::string::npos ||
          std::stoul(answer) < 1 || std::stoul(answer) > choices.size()) {
        sw::log::error("invalid choice; nothing changed"); return 2;
      }
      path = choices[std::stoul(answer) - 1];
    }
  }
  sw::Config cfg;
  if (!path.empty() && !sw::load_config(path, &cfg, &err)) { sw::log::error(err); return 2; }
  cfg.ssid = info.ssid; cfg.interface = info.interface;
  if (!cfg.network_profile && !sw::consolidate_network_config(&cfg, &err)) { sw::log::error(err); return 2; }
  return network_login(cfg, interactive, edit);
}

int automatic_watch() {
  ::signal(SIGINT, on_signal); ::signal(SIGTERM, on_signal);
  while (!g_stop) {
    automatic_login(false);
    for (int i = 0; i < 30 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return 0;
}

int cmd_logout(const sw::Config &cfg) {
  sw::http::Client client;
  make_client(&client, cfg);
  sw::portal::LoginResult res = sw::portal::logout(client, cfg);
  if (res.success) {
    sw::log::info(res.message);
    return 0;
  }
  sw::log::error(res.message);
  return 1;
}

int cmd_open(const sw::Config &cfg) {
  sw::http::Client client;
  make_client(&client, cfg);

  sw::portal::Probe pr = sw::portal::probe(client, cfg);
  if (pr.state == sw::portal::State::Online) {
    sw::log::info("already online; no portal to open");
    return 0;
  }
  if (pr.state == sw::portal::State::Offline) {
    sw::log::error("offline: nothing responded to the connectivity probe");
    return 1;
  }

  sw::portal::LoginPage page = sw::portal::resolve_login_page(client, cfg, pr);
  std::string url = page.url;
  if (url.empty()) {
    url = pr.location.empty() ? pr.probe_url : sw::util::resolve_url(pr.probe_url, pr.location);
  }

  sw::log::info("opening " + url);
  // Hand it to the default browser rather than the Captive Network Assistant,
  // which is exactly the window that fails to appear.
  const auto args = sw::util::browser_open_args(url);
  if (args.empty()) { sw::log::error("refusing to open a non-HTTP(S) portal URL"); return 1; }
  return sw::util::run_process(args) == 0 ? 0 : 1;
}

int cmd_watch(const sw::Config &cfg) {
  // Old profiles have no trusted portal binding. Never silently run them in
  // the background, even when installed by a previous version's LaunchAgent.
  if (!cfg.network_profile || cfg.ssid.empty()) {
    sw::log::error("background authentication requires a named network profile with portal bindings; "
                   "run schoolwifi login interactively, then reinstall the agent without -c");
    return 2;
  }
  ::signal(SIGINT, on_signal); ::signal(SIGTERM, on_signal);
  while (!g_stop) {
    sw::Config live; std::string err;
    if (!sw::load_config(cfg.source_path, &live, &err) || live.source_path.empty() ||
        !live.network_profile || live.ssid.empty()) {
      sw::log::error("could not reload a named network profile"); return 2;
    }
    network_login(live, false);
    for (int i = 0; i < std::max(1, live.online_interval) && !g_stop; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return 0;
}

int cmd_diagnose(sw::Config cfg) {
  sw::wifi::Info info = sw::wifi::current(cfg.interface);
  sw::http::Client client;
  make_client(&client, cfg);

  std::printf("== environment ==\n");
  std::printf("interface : %s (power %s)\n", info.interface.c_str(), info.power_on ? "on" : "off");
  std::printf("ssid      : %s\n", info.ssid.empty() ? "(unavailable)" : info.ssid.c_str());
  std::printf("bssid     : %s\n", info.bssid.empty() ? "(unavailable)" : info.bssid.c_str());
  std::printf("ipv4      : %s\n", info.ipv4.empty() ? "(none)" : info.ipv4.c_str());
  std::printf("config    : %s\n", cfg.source_path.empty() ? "(defaults)" : cfg.source_path.c_str());

  // A system resolver pinned to a public DNS cannot see a campus-only zone,
  // which shows up much later as an inscrutable "could not resolve host".
  // Surfacing it here turns that into a one-line explanation.
  std::vector<std::string> sys_dns = sw::dns::system_nameservers();
  std::vector<std::string> dhcp_dns = sw::dns::dhcp_nameservers(cfg.interface);
  std::printf("\n== dns ==\n");
  std::printf("system    : %s\n",
              sys_dns.empty() ? "(unknown)" : sw::util::join(sys_dns, ", ").c_str());
  std::printf("dhcp      : %s\n",
              dhcp_dns.empty() ? "(none offered)" : sw::util::join(dhcp_dns, ", ").c_str());
  if (!cfg.dns_server.empty()) std::printf("configured: %s\n", cfg.dns_server.c_str());

  bool shares_server = false;
  for (const std::string &a : sys_dns) {
    for (const std::string &b : dhcp_dns) {
      if (a == b) shares_server = true;
    }
  }
  if (!sys_dns.empty() && !dhcp_dns.empty() && !shares_server) {
    std::printf("note      : the system resolver ignores this network's DNS (manually pinned\n");
    std::printf("            in System Settings). A portal hostname that exists only in the\n");
    std::printf("            campus zone will not resolve; schoolwifi falls back to the DHCP\n");
    std::printf("            server above automatically.\n");
  }

  // A proxy or tunnel is the other classic reason authentication "just fails":
  // schoolwifi bypasses them for its own requests, but the browser behind
  // `schoolwifi open` does not, and a tunnel owning the default route defeats
  // everything.
  sw::netenv::Interference net = sw::netenv::detect();
  std::printf("\n== interference ==\n");
  std::printf("default route : %s%s\n",
              net.default_route_interface.empty() ? "(unknown)"
                                                  : net.default_route_interface.c_str(),
              net.tunnelled_default_route ? "   <- a tunnel owns the default route" : "");
  std::printf("system proxy  : %s\n",
              net.system_proxy ? net.system_proxy_detail.c_str() : "(none)");
  std::printf("env proxy     : %s\n",
              net.env_proxy ? net.env_proxy_detail.c_str() : "(none)");
  if (!net.tunnel_interfaces.empty()) {
    std::printf("tunnel ifaces : %s\n", sw::util::join(net.tunnel_interfaces, ", ").c_str());
  }
  if (net.tunnelled_default_route) {
    std::printf("note          : all traffic is going through a tunnel. Captive-portal\n");
    std::printf("                authentication cannot work this way -- turn the VPN or the\n");
    std::printf("                proxy's TUN mode off until you are logged in.\n");
  } else if (net.system_proxy || net.env_proxy) {
    std::printf("note          : schoolwifi bypasses these for its own requests, but your\n");
    std::printf("                browser does not -- `schoolwifi open` may fail to load the\n");
    std::printf("                portal while the proxy is on.\n");
  }

  std::printf("\n== probe ==\n");
  sw::portal::Probe pr = sw::portal::probe(client, cfg);
  std::printf("url       : %s\n", pr.probe_url.c_str());
  std::printf("state     : %s\n", sw::portal::state_name(pr.state));
  std::printf("status    : %ld\n", pr.status);
  if (!pr.location.empty()) std::printf("location  : %s\n", pr.location.c_str());
  if (!pr.error.empty()) std::printf("error     : %s\n", pr.error.c_str());

  if (pr.state == sw::portal::State::Online) {
    std::printf("\nAlready online - there is no portal to inspect right now.\n");
    return 0;
  }
  if (pr.state == sw::portal::State::Offline) {
    std::printf("\nNothing reachable. Check that Wi-Fi is associated first.\n");
    return 1;
  }

  std::printf("\n== portal discovery ==\n");
  sw::portal::LoginPage page = sw::portal::resolve_login_page(client, cfg, pr);
  for (const std::string &hop : page.trail) std::printf("  %s\n", hop.c_str());
  if (!page.note.empty()) std::printf("  note: %s\n", page.note.c_str());
  std::printf("login page: %s\n", page.url.c_str());
  std::printf("page title: %s\n", sw::html::title(page.html).c_str());

  bool submission_known = true;
  if (cfg.network_profile) {
    const auto identity = sw::portal::stage_identity(page);
    int selected = -1;
    for (std::size_t i = 0; i < cfg.stages.size(); ++i) {
      if (!identity.empty() && cfg.stages[i].portal_match == identity) {
        if (selected >= 0) { selected = -2; break; }
        selected = static_cast<int>(i);
      }
    }
    std::printf("stage identity: %s\n", identity.empty() ? "(unrecognized)" : identity.c_str());
    submission_known = selected >= 0;
    if (submission_known) {
      auto stage = cfg.stages[selected];
      stage.interface = cfg.interface; stage.dns_server = cfg.dns_server;
      stage.probe_urls = cfg.probe_urls; stage.probe_timeout = cfg.probe_timeout;
      cfg = stage;
      std::printf("matched stage: %d\n", selected + 1);
    }
  }
  std::printf("\n== forms ==\n");
  std::vector<sw::html::Form> forms = sw::html::extract_forms(page.html);
  if (forms.empty()) std::printf("  (none found)\n");
  for (std::size_t i = 0; i < forms.size(); ++i) {
    const sw::html::Form &f = forms[i];
    std::printf("  form #%zu  action=%s method=%s%s%s\n", i,
                f.action.empty() ? "(self)" : f.action.c_str(), f.method.c_str(),
                f.id.empty() ? "" : (" id=" + f.id).c_str(),
                f.has_password() ? "  <- has password field" : "");
    for (const sw::html::Field &field : f.fields) {
      std::printf("      %-24s type=%-10s value=%s\n", field.name.c_str(),
                  field.type.empty() ? "(none)" : field.type.c_str(),
                  sw::util::iequals(field.type, "password") ? "(hidden)" : field.value.c_str());
    }
  }

  // A BYOD login page decides nothing in HTML: what matters is the policy its
  // JavaScript fetches, and which service the account has to be filed under.
  std::string byod_policy_raw;
  if (sw::byod::looks_like_login_page(page.url, page.html)) {
    std::printf("\n== byod login policy ==\n");
    sw::byod::Policy policy = sw::byod::fetch_policy(client, cfg, page.url);
    if (!policy.ok) {
      std::printf("  could not fetch it: %s\n", policy.message.c_str());
    } else {
      byod_policy_raw = policy.raw;
      std::printf("  defaultServiceTypeId : %s\n",
                  policy.default_service_id.empty() ? "(absent)" : policy.default_service_id.c_str());
      if (policy.services.empty()) {
        std::printf("  serviceList          : (empty -- the portal offers no choice)\n");
      } else {
        std::printf("  serviceList          :\n");
        for (const sw::byod::Service &svc : policy.services) {
          std::printf("      service_suffix_id = %-6s  %s\n", svc.value.c_str(), svc.label.c_str());
        }
        std::printf("  Set service_suffix_id under [portal] to pick one.\n");
      }
    }
  }

  std::printf("\n== what login would submit ==\n");

  // Mirror the decision `login` makes. Running the form planner here regardless
  // reported a failure for pages that login handles perfectly well through an
  // API, which reads as a problem that is not there.
  std::string origin = sw::util::url_origin(page.url);
  if (!submission_known) {
    std::printf("  No unique registered stage matches; login will request enrollment, not submit credentials.\n");
  } else if (sw::byod::looks_like_login_page(page.url, page.html)) {
    std::printf("  POST %s/byod/byodrs/login/defaultLogin   (JSON, not the form)\n",
                origin.c_str());
    std::printf("      userName        = %s\n",
                cfg.username.empty() ? "<username>" : cfg.username.c_str());
    std::printf("      userPassword    = (the password, base64-encoded)\n");
    std::printf("      serviceSuffixId = %s\n",
                cfg.service_suffix_id.empty() ? "(the portal's default, see above)"
                                              : cfg.service_suffix_id.c_str());
    std::printf("      licenseCode, userGroupId, validationType, guestManagerId\n");
    std::printf("                      = copied verbatim from byodrs/login/init\n");
    std::printf("      shopIdE, wlannasid\n");
    std::printf("                      = copied from byodrs/init\n");
    std::printf("  The page's <form> is a decoy: its inputs are hidden and never submitted.\n");
  } else if (sw::srun::looks_like_srun(page.url, page.html)) {
    sw::srun::PortalInfo srun_info = sw::srun::parse_portal_info(page.url, page.html);
    std::printf("  GET %s/cgi-bin/get_challenge   then\n", srun_info.origin.c_str());
    std::printf("  GET %s/cgi-bin/srun_portal?action=login\n", srun_info.origin.c_str());
    std::printf("      username = %s\n",
                cfg.username.empty() ? "<username>" : cfg.username.c_str());
    std::printf("      ac_id    = %s\n", srun_info.ac_id.c_str());
    std::printf("      password / info / chksum are derived from the challenge.\n");
    std::printf("  This portal has no HTML form at all; that is expected.\n");
  } else {
  sw::portal::FormPlan plan =
      sw::portal::plan_form_login(cfg, page, cfg.username.empty() ? "<username>" : cfg.username,
                                  "<password>");
  if (!plan.ok) {
    std::printf("  cannot build a submission: %s\n", plan.reason.c_str());
  } else {
    std::printf("  %s %s\n", plan.method.c_str(), plan.action_url.c_str());
    std::printf("  username_field = %s\n", plan.username_field.c_str());
    std::printf("  password_field = %s\n", plan.password_field.c_str());
    for (const auto &kv : plan.fields) {
      std::printf("      %-24s = %s\n", kv.first.c_str(),
                  kv.first == plan.password_field ? "********" : kv.second.c_str());
    }
  }
  }

  if (page.html.empty()) return 0;

  // A portal whose page carries no form and no redirect keeps its logic in
  // JavaScript. The page alone is then useless for working out what to submit,
  // so save the scripts it loads next to it.
  std::string dir = "schoolwifi-diagnose-" + sw::util::now_compact();
  bool saved_page = sw::util::write_file(dir + "/portal.html", page.html);
  if (!saved_page) {
    std::fprintf(stderr, "\nCould not write %s/\n", dir.c_str());
    return 0;
  }

  std::printf("\n== saved ==\n");
  std::printf("  %s/portal.html\n", dir.c_str());

  if (!byod_policy_raw.empty() &&
      sw::util::write_file(dir + "/byod-login-init.json", byod_policy_raw)) {
    std::printf("  %s/byod-login-init.json  (%zu bytes)\n", dir.c_str(), byod_policy_raw.size());
  }

  for (const auto &capture : page.captures) {
    std::string path = dir + "/" + capture.first;
    if (sw::util::write_file(path, capture.second)) {
      std::printf("  %s  (%zu bytes, captured during discovery)\n", path.c_str(),
                  capture.second.size());
    }
  }

  int index = 0;
  for (const std::string &src : sw::html::script_srcs(page.html)) {
    std::string url = sw::util::resolve_url(page.url, src);
    // Same-origin only: the point is the portal's own code, and fetching from
    // third-party hosts would both leak the request and pull in noise.
    if (sw::util::url_origin(url) != origin) {
      std::printf("  (skipped, not same-origin) %s\n", url.c_str());
      continue;
    }

    sw::http::Response resp = client.get(url, /*follow=*/true, 10);
    if (!resp.ok || resp.body.empty()) {
      std::printf("  (failed) %s -- %s\n", url.c_str(),
                  resp.error.empty() ? "empty response" : resp.error.c_str());
      continue;
    }

    // Name files by load order so the bootstrap chain stays readable.
    std::string base = url;
    std::size_t cut = base.find_first_of("?#");
    if (cut != std::string::npos) base = base.substr(0, cut);
    std::size_t slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.empty()) base = "script.js";

    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%02d-", ++index);
    std::string path = dir + "/" + prefix + base;
    if (sw::util::write_file(path, resp.body)) {
      std::printf("  %s  (%zu bytes)\n", path.c_str(), resp.body.size());
    }
  }

  std::printf("\nThis folder contains the portal's own code, and the page URL\n");
  std::printf("carries your IP and MAC. Redact before sharing it anywhere public.\n");
  return 0;
}

// The setup wizard is the one place a first-time user is asked to type
// values they have never seen named before, so every prompt states what the
// field means, what the config key is called, and what Enter alone will do.
// It speaks Chinese because that is who runs it; diagnostic output elsewhere
// stays English.
int cmd_setup(sw::Config cfg, const std::string &path) {
  std::printf("schoolwifi 配置向导\n");
  std::printf("配置文件将写入：%s\n", path.c_str());
  std::printf("（每一项直接按回车都会使用方括号里的默认值）\n\n");

  sw::wifi::Info info = sw::wifi::current(cfg.interface);
  std::vector<std::string> ifaces = sw::wifi::interfaces();

  // A second network needs a second set of credentials, so a config written
  // anywhere but the default path gets its own keychain entry derived from the
  // filename. Two profiles sharing the default service name would otherwise
  // overwrite each other's password whenever the accounts happened to match.
  if (path != sw::default_config_path() && cfg.keychain_service == "schoolwifi") {
    std::string base = path;
    std::size_t slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);

    std::string suffix;
    for (char c : base) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') suffix += c;
    }
    if (!suffix.empty() && suffix != "config") cfg.keychain_service = "schoolwifi-" + suffix;
  }

  // --- 1. SSID ------------------------------------------------------------
  std::string ssid_default = cfg.ssid.empty() ? info.ssid : cfg.ssid;
  std::printf("[1/4] 校园网 WiFi 名称  (配置项 ssid)\n");
  std::printf("      只有连到这个 WiFi 时才会自动登录，避免在别的网络上发送校园网密码。\n");
  if (info.ssid.empty()) {
    std::printf("      当前读不到已连接的 WiFi 名称，请手动输入。\n");
  } else {
    std::printf("      当前已连接：%s\n", info.ssid.c_str());
  }
  std::printf("      输入 any 表示不限制网络。\n");
  cfg.ssid = sw::util::read_line("    > [" + (ssid_default.empty() ? "any" : ssid_default) + "] ",
                                 ssid_default);
  if (sw::util::iequals(cfg.ssid, "any")) cfg.ssid.clear();

  // --- 2. Interface -------------------------------------------------------
  std::printf("\n[2/4] 无线网卡名  (配置项 interface)\n");
  std::printf("      这里要填的是网卡名，不是 WiFi 名称。Mac 上几乎总是 en0，直接回车即可。\n");
  if (!ifaces.empty()) {
    std::printf("      检测到的无线网卡：%s\n", sw::util::join(ifaces, ", ").c_str());
  }

  for (;;) {
    std::string chosen = sw::util::read_line("    > [" + info.interface + "] ", info.interface);
    if (ifaces.empty()) {  // nothing to validate against; accept it
      cfg.interface = chosen;
      break;
    }
    bool known = false;
    for (const std::string &name : ifaces) {
      if (sw::util::iequals(name, chosen)) known = true;
    }
    if (known) {
      cfg.interface = chosen;
      break;
    }
    // Rejecting this is worth the extra prompt: a bogus interface makes every
    // later request fail to bind, with an error that looks nothing like the
    // typo that caused it.
    std::printf("      \"%s\" 不是这台机器上的无线网卡。可选：%s\n", chosen.c_str(),
                sw::util::join(ifaces, ", ").c_str());
    std::printf("      （如果你想填的是 WiFi 名称，那是上一项 ssid，不是这一项。）\n");
  }

  // --- 3. Username --------------------------------------------------------
  std::printf("\n[3/4] 校园网账号  (配置项 username)\n");
  std::printf("      通常是学号，就是你在网页认证页面里填的那个账号。\n");
  cfg.username = sw::util::read_line(
      cfg.username.empty() ? "    > " : "    > [" + cfg.username + "] ", cfg.username);
  if (cfg.username.empty()) {
    std::fprintf(stderr, "\n错误：账号不能为空。\n");
    return 2;
  }

  // --- 4. Password --------------------------------------------------------
  std::printf("\n[4/4] 校园网密码\n");
  std::printf("      输入时不会回显。密码存进 macOS 登录钥匙串，不会写进配置文件。\n");
  std::string password = sw::util::read_password("    > ");
  if (password.empty()) {
    std::fprintf(stderr, "\n错误：密码不能为空。\n");
    return 2;
  }

  std::string err;
  if (!sw::keychain::set_password(cfg.keychain_service, cfg.username, password, &err)) {
    std::fprintf(stderr, "错误：无法写入钥匙串：%s\n", err.c_str());
    return 1;
  }

  // --- 5. An optional second authentication stage ------------------------
  // Dorm networks commonly put a campus portal in front of an ISP one, with a
  // different account for each. Asking here costs one keystroke and saves
  // discovering the second layer the hard way.
  std::string stage2_path;
  std::string stage2_username;
  std::printf("\n[5/5] 这个网络需要过两道认证吗？\n");
  std::printf("      宿舍宽带常见：先过校园网，再过运营商（联通/电信/移动），两道账号不一样。\n");
  std::printf("      校园网一般只有一道，直接回车即可。\n");
  std::string wants_stage2 = sw::util::read_line("    > 需要第二道吗 [y/N] ", "n");

  if (wants_stage2 == "y" || wants_stage2 == "Y" || wants_stage2 == "yes") {
    // Derive the second file from the first, so the pair stays recognisable.
    std::string base = path;
    std::size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > base.rfind('/')) base = base.substr(0, dot);
    stage2_path = base + "-stage2.ini";

    std::printf("\n[第二道] 账号\n");
    std::printf("      运营商宽带账号，和上面那个不是同一个。\n");
    stage2_username = sw::util::read_line("    > ", "");
    if (stage2_username.empty()) {
      std::fprintf(stderr, "\n错误：第二道的账号不能为空。\n");
      return 2;
    }

    std::printf("\n[第二道] 密码\n");
    std::printf("      同样存进钥匙串，用的是另一个独立条目。\n");
    std::string stage2_password = sw::util::read_password("    > ");
    if (stage2_password.empty()) {
      std::fprintf(stderr, "\n错误：第二道的密码不能为空。\n");
      return 2;
    }

    sw::Config stage2;
    stage2.ssid = cfg.ssid;
    stage2.interface = cfg.interface;
    stage2.username = stage2_username;
    stage2.keychain_service = cfg.keychain_service + "-stage2";
    stage2.log_file = "~/Library/Logs/schoolwifi.log";

    if (!sw::keychain::set_password(stage2.keychain_service, stage2.username, stage2_password,
                                    &err)) {
      std::fprintf(stderr, "错误：无法写入钥匙串：%s\n", err.c_str());
      return 1;
    }
    if (!sw::save_config(stage2, stage2_path, &err)) {
      std::fprintf(stderr, "错误：%s\n", err.c_str());
      return 1;
    }
    cfg.next_stage = stage2_path;
  }

  if (cfg.log_file.empty()) cfg.log_file = "~/Library/Logs/schoolwifi.log";
  if (!sw::save_config(cfg, path, &err)) {
    std::fprintf(stderr, "错误：%s\n", err.c_str());
    return 1;
  }

  std::printf("\n配置完成\n");
  std::printf("  WiFi 名称 (ssid)       %s\n", cfg.ssid.empty() ? "(不限制)" : cfg.ssid.c_str());
  std::printf("  无线网卡  (interface)  %s\n", cfg.interface.c_str());
  std::printf("  账号      (username)   %s\n", cfg.username.c_str());
  std::printf("  密码                   已存入钥匙串 (%s/%s)\n", cfg.keychain_service.c_str(),
              cfg.username.c_str());
  if (cfg.keychain_service != "schoolwifi") {
    std::printf("  （这份配置用的是独立的钥匙串条目，不会和默认配置冲突）\n");
  }
  std::printf("  配置文件               %s\n", path.c_str());
  if (!stage2_path.empty()) {
    std::printf("\n  第二道认证\n");
    std::printf("    账号                 %s\n", stage2_username.c_str());
    std::printf("    密码                 已存入钥匙串 (%s-stage2/%s)\n",
                cfg.keychain_service.c_str(), stage2_username.c_str());
    std::printf("    配置文件             %s\n", stage2_path.c_str());
    std::printf("    （login 会在第一道过了之后自动接着跑第二道）\n");
  }

  std::printf("\n下一步：连上这个网络后直接运行  schoolwifi login\n");
  std::printf("不用加 -c —— 它会按 SSID 自动选择这份配置。`schoolwifi profiles` 可以查看。\n");
  std::printf("如果失败，运行  schoolwifi diagnose  查看门户结构，\n");
  std::printf("并参考 docs/adapting-to-your-campus.md 填写 [portal] 段。\n");
  return 0;
}

int cmd_install_agent(const sw::Config &cfg) {
  std::string binary = executable_path();
  std::string plist_path = agent_plist_path();
  std::string log_file = cfg.log_file.empty() ? "~/Library/Logs/schoolwifi.log" : cfg.log_file;
  std::string resolved_log = sw::util::expand_tilde(log_file);

  std::string plist =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n"
      "<dict>\n"
      "  <key>Label</key>\n"
      "  <string>" + std::string(kAgentLabel) + "</string>\n"
      "  <key>ProgramArguments</key>\n"
      "  <array>\n"
      "    <string>" + binary + "</string>\n"
      "    <string>watch</string>\n";
  if (!cfg.source_path.empty()) {
    plist += "    <string>--config</string>\n    <string>" + cfg.source_path + "</string>\n";
  }
  plist +=
      "  </array>\n"
      "  <key>RunAtLoad</key>\n"
      "  <true/>\n"
      "  <key>KeepAlive</key>\n"
      "  <true/>\n"
      "  <key>ProcessType</key>\n"
      "  <string>Background</string>\n"
      "  <key>StandardOutPath</key>\n"
      "  <string>" + resolved_log + "</string>\n"
      "  <key>StandardErrorPath</key>\n"
      "  <string>" + resolved_log + "</string>\n"
      "</dict>\n"
      "</plist>\n";

  if (!sw::util::write_file(plist_path, plist)) {
    std::fprintf(stderr, "error: could not write %s\n", plist_path.c_str());
    return 1;
  }
  sw::util::mkdir_p(sw::util::dirname(resolved_log));

  std::string uid = std::to_string(static_cast<int>(::getuid()));
  // bootout first so a reinstall picks up the new plist.
  std::system(("launchctl bootout gui/" + uid + "/" + kAgentLabel + " 2>/dev/null").c_str());
  int rc = std::system(("launchctl bootstrap gui/" + uid + " '" + plist_path + "'").c_str());
  if (rc != 0) {
    std::fprintf(stderr, "error: launchctl bootstrap failed (exit %d)\n", rc);
    return 1;
  }

  std::printf("Installed %s\n", plist_path.c_str());
  std::printf("Binary    %s\n", binary.c_str());
  std::printf("Log       %s\n", resolved_log.c_str());
  std::printf("\nThe agent now runs at login. Tail it with:\n  tail -f %s\n", resolved_log.c_str());
  return 0;
}

int cmd_uninstall_agent() {
  std::string uid = std::to_string(static_cast<int>(::getuid()));
  std::string plist_path = agent_plist_path();

  std::system(("launchctl bootout gui/" + uid + "/" + kAgentLabel + " 2>/dev/null").c_str());
  if (sw::util::file_exists(plist_path)) {
    ::unlink(plist_path.c_str());
    std::printf("Removed %s\n", plist_path.c_str());
  } else {
    std::printf("No LaunchAgent installed.\n");
  }
  return 0;
}

int cmd_profiles() {
  std::vector<sw::Profile> profiles = sw::discover_profiles();
  if (profiles.empty()) {
    std::printf("No profiles in %s\n", sw::config_dir().c_str());
    std::printf("Run `schoolwifi setup` to make one.\n");
    return 1;
  }

  sw::wifi::Info here = sw::wifi::current("");
  std::string why;
  std::string chosen = sw::select_profile(profiles, here.ssid, &why);

  std::printf("Profiles in %s\n\n", sw::config_dir().c_str());
  for (const sw::Profile &profile : profiles) {
    std::string name = profile.path;
    std::size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    sw::Config detail; std::string err;
    sw::load_config(profile.path, &detail, &err);
    if (detail.network_profile) {
      std::printf("  %s%s  [%s]  %zu authentication stages\n", profile.path == chosen ? "* " : "  ",
                  profile.path.c_str(), profile.ssid.c_str(), detail.stages.size());
      continue;
    }
    std::printf("  %s%-24s %-22s %s%s\n", profile.path == chosen ? "* " : "  ", name.c_str(),
                profile.ssid.empty() ? "(any network)" : profile.ssid.c_str(),
                profile.username.empty() ? "(no account)" : profile.username.c_str(),
                profile.is_entry ? "   [legacy]" : "   [later stage / superseded legacy]");
  }

  std::printf("\nCurrent SSID: %s\n", here.ssid.empty() ? "(unavailable)" : here.ssid.c_str());
  if (chosen.empty()) {
    std::printf("No profile selected automatically: %s\n", why.c_str());
    std::printf("Run schoolwifi login in a terminal to register this network when a portal appears.\n");
  } else {
    std::printf("`schoolwifi login` here would use the one marked *\n");
  }
  return 0;
}

int cmd_agent_status() {
  std::string uid = std::to_string(static_cast<int>(::getuid()));
  std::string plist_path = agent_plist_path();
  std::printf("plist: %s\n", sw::util::file_exists(plist_path) ? plist_path.c_str() : "(not installed)");
  std::string out = sw::util::exec_capture("launchctl print gui/" + uid + "/" + kAgentLabel +
                                           " 2>/dev/null | head -20");
  if (out.empty()) {
    std::printf("state: not loaded\n");
    return 1;
  }
  std::printf("state: loaded\n%s", out.c_str());
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Options opts;
  if (!parse_options(argc, argv, &opts)) return 2;

  if (opts.args.empty()) {
    print_usage();
    return 2;
  }

  if (opts.verbose) sw::log::set_level(sw::log::Level::Debug);
  if (opts.quiet) sw::log::set_level(sw::log::Level::Warn);

  const auto &requested = opts.args[0];
  if (opts.config_path.empty()) {
    if (requested == "login" || requested == "setup")
      return automatic_login(::isatty(STDIN_FILENO), requested == "setup");
    if (requested == "watch") return automatic_watch();
  }

  // With no -c, pick the profile whose ssid matches the network we are on, so
  // one command works in the dorm and on campus without the user remembering
  // which file belongs where.
  std::string config_path = opts.config_path;
  bool auto_selected = false;
  if (config_path.empty()) {
    const std::string &cmd_name = opts.args[0];
    bool wants_a_network = cmd_name == "login" || cmd_name == "status" ||
                           cmd_name == "diagnose" || cmd_name == "watch" || cmd_name == "open" ||
                           cmd_name == "logout";
    if (wants_a_network) {
      sw::wifi::Info here = sw::wifi::current("");
      std::string why;
      config_path = sw::select_profile(sw::discover_profiles(), here.ssid, &why);
      if (config_path.empty()) {
        sw::log::debug("profile auto-selection: " + why);
      } else {
        auto_selected = true;
      }
    }
  }
  if (config_path.empty()) config_path = sw::default_config_path();

  sw::Config cfg;
  std::string err;
  if (!sw::load_config(config_path, &cfg, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }

  const std::string &cmd = opts.args[0];
  if (auto_selected) sw::log::info("using profile " + config_path + " for this network");

  // `watch` is the only command that runs unattended, so it is the only one
  // that writes to the log file by default.
  if (cmd == "watch" && !cfg.log_file.empty()) sw::log::set_file(cfg.log_file);

  if (cmd == "version" || cmd == "--version") {
    std::printf("schoolwifi %s\n", kVersion);
    return 0;
  }
  if (cmd == "help") {
    print_usage();
    return 0;
  }
  if (cmd == "status") return cmd_status(cfg);
  if (cmd == "login") return cfg.network_profile ? network_login(cfg, ::isatty(STDIN_FILENO)) : cmd_login(cfg);
  if (cmd == "logout") return cmd_logout(cfg);
  if (cmd == "open") return cmd_open(cfg);
  if (cmd == "watch") return cmd_watch(cfg);
  if (cmd == "diagnose") return cmd_diagnose(cfg);
  if (cmd == "setup") return cfg.network_profile ? network_login(cfg, ::isatty(STDIN_FILENO), true) : cmd_setup(cfg, config_path);
  if (cmd == "install-agent") {
    if (opts.config_path.empty()) cfg.source_path.clear();
    return cmd_install_agent(cfg);
  }
  if (cmd == "uninstall-agent") return cmd_uninstall_agent();
  if (cmd == "agent-status") return cmd_agent_status();
  if (cmd == "profiles") return cmd_profiles();

  std::fprintf(stderr, "error: unknown command \"%s\"\n\n", cmd.c_str());
  print_usage();
  return 2;
}
