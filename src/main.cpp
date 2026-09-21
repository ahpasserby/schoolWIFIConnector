#include <mach-o/dyld.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "sw/config.hpp"
#include "sw/html.hpp"
#include "sw/http.hpp"
#include "sw/keychain.hpp"
#include "sw/log.hpp"
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

  if (!page.html.empty()) {
    std::string out = "schoolwifi-portal-" + sw::util::now_compact() + ".html";
    if (sw::util::write_file(out, page.html)) {
      std::printf("\nSaved the raw portal page to %s\n", out.c_str());
    }
  }
  return 0;
}

int cmd_setup(sw::Config cfg, const std::string &path) {
  std::printf("schoolwifi setup - writing %s\n\n", path.c_str());

  sw::wifi::Info info = sw::wifi::current(cfg.interface);
  std::string ssid_default = cfg.ssid.empty() ? info.ssid : cfg.ssid;

  if (!info.ssid.empty()) std::printf("Currently associated with: %s\n", info.ssid.c_str());
  cfg.ssid = sw::util::read_line(
      "SSID to auto-login on [" + (ssid_default.empty() ? "any" : ssid_default) + "]: ", ssid_default);
  if (sw::util::iequals(cfg.ssid, "any")) cfg.ssid.clear();

  cfg.interface = sw::util::read_line("Wi-Fi interface [" + info.interface + "]: ", info.interface);
  cfg.username = sw::util::read_line(
      "Campus username" + (cfg.username.empty() ? "" : " [" + cfg.username + "]") + ": ",
      cfg.username);
  if (cfg.username.empty()) {
    std::fprintf(stderr, "error: a username is required\n");
    return 2;
  }

  std::string password = sw::util::read_password("Campus password (not echoed): ");
  if (password.empty()) {
    std::fprintf(stderr, "error: a password is required\n");
    return 2;
  }

  std::string err;
  if (!sw::keychain::set_password(cfg.keychain_service, cfg.username, password, &err)) {
    std::fprintf(stderr, "error: could not store the password in the Keychain: %s\n", err.c_str());
    return 1;
  }
  std::printf("Stored the password in the login Keychain (%s/%s).\n", cfg.keychain_service.c_str(),
              cfg.username.c_str());

  if (cfg.log_file.empty()) cfg.log_file = "~/Library/Logs/schoolwifi.log";

  if (!sw::save_config(cfg, path, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  std::printf("\nWrote %s\n", path.c_str());
  std::printf("Next: connect to the campus Wi-Fi, then run `schoolwifi login`.\n");
  std::printf("If it fails, run `schoolwifi diagnose` and adjust the [portal] section.\n");
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
