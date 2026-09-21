#include <mach-o/dyld.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "sw/config.hpp"
#include "sw/dns.hpp"
#include "sw/html.hpp"
#include "sw/http.hpp"
#include "sw/keychain.hpp"
#include "sw/log.hpp"
#include "sw/netenv.hpp"
#include "sw/portal.hpp"
#include "sw/util.hpp"
#include "sw/wifi.hpp"

namespace {

constexpr const char *kVersion = "0.1.0";
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
  setup            Interactive first-run configuration
  install-agent    Install the LaunchAgent so watch runs at login
  uninstall-agent  Remove the LaunchAgent
  agent-status     Show whether the LaunchAgent is loaded
  version          Print the version

OPTIONS
  -c, --config PATH  Config file (default: ~/.config/schoolwifi/config.ini)
  -v, --verbose      Log every HTTP request and redirect
  -q, --quiet        Only log warnings and errors
  -h, --help         This message

ENVIRONMENT
  SCHOOLWIFI_PASSWORD  Password to use instead of the Keychain entry
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

// Password sources, in order of precedence.
bool resolve_password(const sw::Config &cfg, std::string *out, std::string *err) {
  if (const char *env = std::getenv("SCHOOLWIFI_PASSWORD"); env && *env) {
    *out = env;
    return true;
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

// Returns false when the config pins an SSID and we are demonstrably on a
// different one. An unreadable SSID never blocks a login attempt.
bool ssid_allows_action(const sw::Config &cfg, const sw::wifi::Info &info) {
  if (cfg.ssid.empty()) return true;
  if (info.ssid.empty()) {
    sw::log::debug("SSID unreadable; proceeding despite the ssid guard");
    return true;
  }
  if (sw::util::iequals(cfg.ssid, info.ssid)) return true;
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
  return pr.state == sw::portal::State::Online ? 0 : 1;
}

int cmd_login(const sw::Config &cfg) {
  if (cfg.username.empty()) {
    sw::log::error("no username configured; run `schoolwifi setup`");
    return 2;
  }

  sw::wifi::Info info = sw::wifi::current(cfg.interface);
  if (!ssid_allows_action(cfg, info)) return 0;

  std::string password;
  std::string err;
  if (!resolve_password(cfg, &password, &err)) {
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
  std::string cmd = "open '" + url + "'";
  return std::system(cmd.c_str()) == 0 ? 0 : 1;
}

int cmd_watch(const sw::Config &cfg) {
  if (cfg.username.empty()) {
    sw::log::error("no username configured; run `schoolwifi setup`");
    return 2;
  }

  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);

  sw::log::info("watching (online every " + std::to_string(cfg.online_interval) + "s, captive every " +
                std::to_string(cfg.captive_interval) + "s)");

  int consecutive_failures = 0;
  auto last_state = sw::portal::State::Offline;
  bool first = true;

  while (!g_stop) {
    sw::Config live = cfg;  // re-read nothing: config changes need a restart
    sw::http::Client client;
    make_client(&client, live);

    sw::wifi::Info info = sw::wifi::current(live.interface);
    sw::portal::Probe pr = sw::portal::probe(client, live);

    if (first || pr.state != last_state) {
      sw::log::info(std::string("state: ") + sw::portal::state_name(pr.state) +
                    (info.ssid.empty() ? "" : " on " + info.ssid));
      last_state = pr.state;
      first = false;
    }

    int sleep_for = live.online_interval;

    if (pr.state == sw::portal::State::Captive && ssid_allows_action(live, info)) {
      std::string password;
      std::string err;
      if (!resolve_password(live, &password, &err)) {
        sw::log::error(err);
        return 2;  // unrecoverable: no point spinning
      }

      sw::portal::LoginResult res = sw::portal::login(client, live, password);
      if (res.success) {
        sw::log::info("connected: " + res.message);
        consecutive_failures = 0;
        last_state = sw::portal::State::Online;
      } else {
        ++consecutive_failures;
        sw::log::warn("login failed (" + std::to_string(consecutive_failures) + "/" +
                      std::to_string(live.max_retries) + "): " + res.message);
        if (consecutive_failures >= live.max_retries) {
          sw::log::warn("backing off for " + std::to_string(live.retry_backoff) + "s");
          sleep_for = live.retry_backoff;
          consecutive_failures = 0;
        } else {
          sleep_for = live.captive_interval;
        }
      }
    } else if (pr.state != sw::portal::State::Online) {
      sleep_for = live.captive_interval;
    }

    // Wake once a second so SIGTERM from launchd is honoured promptly.
    for (int i = 0; i < sleep_for && !g_stop; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  sw::log::info("stopped");
  return 0;
}

int cmd_diagnose(const sw::Config &cfg) {
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

  std::printf("\n== what login would submit ==\n");
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

  for (const auto &capture : page.captures) {
    std::string path = dir + "/" + capture.first;
    if (sw::util::write_file(path, capture.second)) {
      std::printf("  %s  (%zu bytes, captured during discovery)\n", path.c_str(),
                  capture.second.size());
    }
  }

  std::string origin = sw::util::url_origin(page.url);
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
  std::printf("\n下一步：连上校园网后运行  schoolwifi login\n");
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

  std::string config_path =
      opts.config_path.empty() ? sw::default_config_path() : opts.config_path;

  sw::Config cfg;
  std::string err;
  if (!sw::load_config(config_path, &cfg, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 2;
  }

  const std::string &cmd = opts.args[0];

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
  if (cmd == "login") return cmd_login(cfg);
  if (cmd == "logout") return cmd_logout(cfg);
  if (cmd == "open") return cmd_open(cfg);
  if (cmd == "watch") return cmd_watch(cfg);
  if (cmd == "diagnose") return cmd_diagnose(cfg);
  if (cmd == "setup") return cmd_setup(cfg, config_path);
  if (cmd == "install-agent") return cmd_install_agent(cfg);
  if (cmd == "uninstall-agent") return cmd_uninstall_agent();
  if (cmd == "agent-status") return cmd_agent_status();

  std::fprintf(stderr, "error: unknown command \"%s\"\n\n", cmd.c_str());
  print_usage();
  return 2;
}
