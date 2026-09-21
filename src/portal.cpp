#include "sw/portal.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "sw/byod.hpp"
#include "sw/dns.hpp"
#include "sw/html.hpp"
#include "sw/log.hpp"
#include "sw/netenv.hpp"
#include "sw/srun.hpp"
#include "sw/util.hpp"

namespace sw::portal {
namespace {

constexpr int kMaxHops = 6;
constexpr int kVerifyAttempts = 4;
constexpr int kVerifyDelayMs = 1200;

const char *kMaskedPassword = "********";

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// What a given connectivity-check endpoint returns when nothing intercepts it.
struct Expectation {
  long status;
  const char *marker;  // substring that must appear in the body ("" = any)
};

Expectation expectation_for(const std::string &url) {
  if (util::icontains(url, "generate_204") || util::icontains(url, "gen_204")) return {204, ""};
  if (util::icontains(url, "msftconnecttest")) return {200, "Microsoft Connect Test"};
  if (util::icontains(url, "captive.apple.com") || util::icontains(url, "hotspot-detect")) {
    return {200, "Success"};
  }
  return {200, ""};
}

// Names campus portals use for the account field, most specific first.
const char *kUsernameHints[] = {
    "username", "user_name", "loginname", "login_name", "userid", "user_id", "account",
    "stuid",    "studentid", "ddddd",     "uname",      "user",   "login",   "email",
    "phone",    "mobile",    "name",      "id",
};

const char *kPasswordHints[] = {
    "password", "passwd", "upass", "userpass", "pwd", "pass", "pw", "secret",
};

// Picks the field best matching a hint list. Hints are consulted in priority
// order (not field order) so "username" beats a field that merely happens to
// contain a weaker hint.
std::string pick_field(const std::vector<html::Field> &fields, const char *const *hints,
                       std::size_t count, const std::string &exclude) {
  for (std::size_t h = 0; h < count; ++h) {
    for (const html::Field &f : fields) {
      if (f.name == exclude) continue;
      if (util::iequals(f.name, hints[h])) return f.name;
    }
  }

  // Substring fallback, restricted to hints long enough to be meaningful:
  // "id" would otherwise claim a field named "validcode", and "pw" would
  // claim "pwdTip".
  for (std::size_t h = 0; h < count; ++h) {
    if (std::char_traits<char>::length(hints[h]) < 4) continue;
    for (const html::Field &f : fields) {
      if (f.name == exclude) continue;
      if (util::icontains(f.name, hints[h])) return f.name;
    }
  }
  return "";
}

template <std::size_t N>
std::string pick_field(const std::vector<html::Field> &fields, const char *const (&hints)[N],
                       const std::string &exclude = "") {
  return pick_field(fields, hints, N, exclude);
}

// A campus portal's hostname often exists only in the campus DNS. If the user
// pinned a public resolver on the interface (System Settings > Network > DNS),
// the system resolver will never see that zone and the name simply does not
// exist. Ask the DNS server this network handed out over DHCP instead, and pin
// the answer for the rest of the session.
bool pin_host_via_network_dns(http::Client &client, const Config &cfg, const std::string &url) {
  util::UrlParts parts = util::parse_url(url);
  if (parts.host.empty()) return false;
  if (client.has_resolve_for(parts.host)) return false;  // already pinned or already failed

  std::vector<std::string> servers;
  if (!cfg.dns_server.empty()) servers.push_back(cfg.dns_server);
  for (const std::string &s : dns::dhcp_nameservers(cfg.interface)) servers.push_back(s);
  if (servers.empty()) {
    log::debug("no DHCP nameserver to fall back to");
    return false;
  }

  for (const std::string &server : servers) {
    std::vector<std::string> ips = dns::resolve_a(server, parts.host);
    if (ips.empty()) continue;

    log::info("system DNS could not resolve " + parts.host + "; " + server + " says " + ips[0]);
    // Pin every port the portal might bounce between, not just this URL's.
    client.add_resolve(parts.host, parts.port, ips[0]);
    client.add_resolve(parts.host, "80", ips[0]);
    client.add_resolve(parts.host, "443", ips[0]);
    return true;
  }

  log::debug("no DHCP nameserver could resolve " + parts.host);
  return false;
}

// Every portal-facing fetch goes through here so the DNS fallback applies
// uniformly.
http::Response fetch(http::Client &client, const Config &cfg, const http::Request &req) {
  http::Response resp = client.send(req);
  if (resp.ok || !dns::is_resolve_failure(resp.error)) return resp;
  if (!pin_host_via_network_dns(client, cfg, req.url)) return resp;
  return client.send(req);
}

http::Request get_request(const std::string &url, int timeout_sec = 10, bool follow = true) {
  http::Request req;
  req.url = url;
  req.method = "GET";
  req.follow = follow;
  req.timeout_sec = timeout_sec;
  return req;
}

// "host" normally, "host:port" when the port is not the scheme's default --
// a portal on another port of the same machine is still another portal.
std::string authority_of(const std::string &url) {
  util::UrlParts parts = util::parse_url(url);
  if (parts.host.empty()) return "";
  bool default_port = (parts.scheme == "https" && parts.port == "443") ||
                      (parts.scheme == "http" && parts.port == "80");
  return default_port ? parts.host : parts.host + ":" + parts.port;
}

// Where a probe says the interception is happening.
std::string intercepting_authority(const Probe &pr) {
  if (pr.state != State::Captive) return "";
  std::string where =
      pr.location.empty() ? pr.probe_url : util::resolve_url(pr.probe_url, pr.location);
  return authority_of(where);
}

} // namespace

// Distinguishes "the credentials did not take" from "a different portal is now
// in the way", which is what a network with chained logins looks like from
// here. Reporting the second as a plain failure sends people hunting for a
// wrong-password problem they do not have.
std::string explain_failed_verification(const Probe &last, const std::string &submitted_to) {
  std::string now_at = intercepting_authority(last);
  std::string was_at = authority_of(submitted_to);

  if (!now_at.empty() && !was_at.empty() && now_at != was_at) {
    return "logged in to " + was_at + ", but the network is still intercepted - now by " + now_at +
           ". That is a second authentication stage; schoolwifi handles one portal per run, so "
           "this one needs its own credentials (see the chained-portal notes in the README)";
  }
  if (last.state == State::Offline) {
    return "submitted, but the network went unreachable afterwards";
  }
  return "submitted, but connectivity never came up";
}

namespace {

std::string first_line(const std::string &s, std::size_t limit = 200) {
  std::string cleaned;
  for (char c : s) {
    if (c == '\n' || c == '\r' || c == '\t') {
      if (!cleaned.empty() && cleaned.back() != ' ') cleaned += ' ';
    } else {
      cleaned += c;
    }
    if (cleaned.size() >= limit) break;
  }
  return util::trim(cleaned);
}

} // namespace

const char *state_name(State s) {
  switch (s) {
    case State::Online: return "online";
    case State::Captive: return "captive";
    case State::Offline: return "offline";
  }
  return "unknown";
}

namespace {

// Last resort when nothing answers a connectivity check: the default gateway.
// On a dorm or campus network the gateway usually *is* the portal, and a
// network that silently drops traffic to the check endpoints (rather than
// intercepting it) leaves no other trace to follow.
bool gateway_looks_like_portal(http::Client &client, const Config &cfg, Probe *pr) {
  std::string gateway = netenv::default_gateway();
  if (gateway.empty()) return false;

  std::string url = "http://" + gateway + "/";
  log::info("no check endpoint answered; trying the gateway at " + url);

  http::Response resp = fetch(client, cfg, get_request(url, cfg.probe_timeout, /*follow=*/false));
  if (!resp.ok) {
    log::info("gateway did not answer either (" + resp.error + ")");
    return false;
  }

  std::string location = resp.header("location");
  bool redirects = resp.status >= 300 && resp.status < 400 && !location.empty();
  bool portal_shaped = redirects || !html::meta_refresh_url(resp.body).empty() ||
                       !html::js_redirect_url(resp.body).empty();
  for (const html::Form &f : html::extract_forms(resp.body)) {
    if (f.has_password()) portal_shaped = true;
  }

  if (!portal_shaped) {
    // A plain router admin page is not a captive portal; saying so would send
    // login off to submit credentials to someone's home router.
    log::info("the gateway answered (HTTP " + std::to_string(resp.status) +
              ") but the page does not look like a login portal");
    return false;
  }

  log::info("the gateway is serving what looks like a login portal");
  pr->state = State::Captive;
  pr->probe_url = url;
  pr->status = resp.status;
  pr->body = resp.body;
  pr->location = location;
  pr->error.clear();
  return true;
}

} // namespace

Probe probe(http::Client &client, const Config &cfg) {
  Probe last;
  int timeout = cfg.probe_timeout > 0 ? cfg.probe_timeout : 5;

  for (const std::string &url : effective_probe_urls(cfg)) {
    Probe pr;
    pr.probe_url = url;

    // follow = false: the redirect the portal injects IS the information we want.
    // Through fetch() so that a network which blocks the system resolver still
    // gets a probe: the check hostnames are then resolved via this network's
    // own DNS, which is exactly what every other device on it does.
    http::Response resp = fetch(client, cfg, get_request(url, timeout, /*follow=*/false));
    pr.status = resp.status;
    pr.body = resp.body;
    pr.redirects = resp.redirects;

    if (!resp.ok) {
      pr.state = State::Offline;
      pr.error = resp.error;
      last = pr;
      // Visible at the default log level on purpose: each of these costs a
      // full timeout, and without it the command looks frozen.
      log::info("probe " + url + " failed (" + resp.error + "); trying the next one");
      continue;
    }

    pr.location = resp.header("location");
    Expectation exp = expectation_for(url);

    if (resp.status >= 300 && resp.status < 400 && !pr.location.empty()) {
      pr.state = State::Captive;
    } else if (resp.status == exp.status &&
               (exp.marker[0] == '\0' || util::icontains(resp.body, exp.marker))) {
      // For an endpoint with no known success payload, a 200 alone proves
      // nothing: portals commonly intercept without redirecting, answering 200
      // with a splash page. A genuine connectivity check never carries a
      // redirect hint, so one here means the response was substituted.
      bool substituted = exp.marker[0] == '\0' && (!html::meta_refresh_url(resp.body).empty() ||
                                                   !html::js_redirect_url(resp.body).empty());
      pr.state = substituted ? State::Captive : State::Online;
    } else {
      // A 200 that is not the expected payload means a transparent proxy
      // swapped the response for a splash page.
      pr.state = State::Captive;
    }

    log::debug(std::string("probe ") + url + " -> " + state_name(pr.state) + " (status " +
               std::to_string(pr.status) + ")");
    return pr;
  }

  if (last.state == State::Offline && gateway_looks_like_portal(client, cfg, &last)) {
    return last;
  }
  return last;
}

LoginPage resolve_login_page(http::Client &client, const Config &cfg, const Probe &pr) {
  LoginPage page;

  // An explicit login_url short-circuits discovery entirely.
  if (!cfg.login_url.empty()) {
    page.url = cfg.login_url;
    page.trail.push_back("config login_url: " + cfg.login_url);
    http::Response resp = fetch(client, cfg, get_request(cfg.login_url));
    page.url = resp.final_url.empty() ? cfg.login_url : resp.final_url;
    page.html = resp.body;
    if (!resp.ok) page.note = "fetch failed: " + resp.error;
    return page;
  }

  std::string url;
  std::string html;

  if (!pr.location.empty()) {
    url = util::resolve_url(pr.probe_url, pr.location);
    page.trail.push_back("redirect: " + url);
  } else {
    url = pr.probe_url;
    html = pr.body;  // the intercepted body is already the splash page
    page.trail.push_back("intercepted body from " + url);
  }

  std::vector<std::string> visited;  // origin+path of every page actually loaded
  bool byod_tried = false;

  for (int hop = 0; hop < kMaxHops; ++hop) {
    if (html.empty()) {
      http::Response resp = fetch(client, cfg, get_request(url));
      if (!resp.ok) {
        page.note = "fetch failed: " + resp.error;
        break;
      }
      if (!resp.final_url.empty() && resp.final_url != url) {
        url = resp.final_url;
        page.trail.push_back("followed to: " + url);
      }
      html = resp.body;
    }

    visited.push_back(util::url_without_query(url));

    for (const html::Form &f : html::extract_forms(html)) {
      if (f.has_password()) {
        page.url = url;
        page.html = html;
        page.trail.push_back("login form found at: " + url);
        log::info("login form found at " + url);
        return page;
      }
    }

    // No password field here: look for the next hop the browser would take.
    std::string next = html::meta_refresh_url(html);
    std::string kind = "meta refresh";
    if (next.empty()) {
      next = html::js_redirect_url(html);
      kind = "javascript redirect";
    }
    if (next.empty()) {
      next = html::iframe_src(html);
      kind = "iframe";
    }
    if (next.empty() && !byod_tried && byod::looks_like_byod(url, html)) {
      byod_tried = true;
      // A BYOD shell page hides its next hop behind an API call rather than a
      // link, so ask the portal the same question its JavaScript would.
      byod::InitResult init = byod::init(client, cfg, url);
      if (!init.raw.empty()) page.captures.emplace_back("byod-init.json", init.raw);
      if (init.ok) {
        next = init.next_url;
        kind = init.already_registered ? "byod init (device already registered)" : "byod init";
      } else {
        page.note = "byod: " + init.message;
      }
    }
    if (next.empty()) break;

    std::string resolved = util::resolve_url(url, next);
    // Compare paths, not whole URLs: a portal that re-appends its parameters
    // hands back a longer URL each time while pointing at the same page.
    std::string resolved_path = util::url_without_query(resolved);
    bool loop = false;
    for (const std::string &been : visited) {
      if (been == resolved_path) loop = true;
    }
    if (loop) {
      page.note = "redirect loop: " + resolved_path + " was already visited";
      break;
    }

    page.trail.push_back(kind + ": " + resolved);
    log::info("portal hop (" + kind + "): " + resolved);
    url = resolved;
    html.clear();
  }

  // Running out of hops leaves the last URL fetched but not loaded, and a page
  // that was never loaded cannot be inspected. Fetch it so `diagnose` has
  // something to dump.
  if (html.empty() && !url.empty()) {
    http::Response resp = fetch(client, cfg, get_request(url));
    if (resp.ok) {
      html = resp.body;
      if (!resp.final_url.empty()) url = resp.final_url;
    } else if (page.note.empty()) {
      page.note = "fetch failed: " + resp.error;
    }
  }

  page.url = url;
  page.html = html;
  return page;
}

FormPlan plan_form_login(const Config &cfg, const LoginPage &page, const std::string &username,
                         const std::string &password) {
  FormPlan plan;

  std::vector<html::Form> forms = html::extract_forms(page.html);
  if (forms.empty()) {
    plan.reason = "no <form> and no <input> fields on " + page.url;
    return plan;
  }

  const html::Form *chosen = nullptr;
  for (const html::Form &f : forms) {
    if (f.has_password()) {
      chosen = &f;
      break;
    }
  }
  if (!chosen) {
    // Fall back to the largest form; some portals mark the password input as
    // type="text" and hide it with CSS.
    const html::Form *biggest = &forms[0];
    for (const html::Form &f : forms) {
      if (f.fields.size() > biggest->fields.size()) biggest = &f;
    }
    chosen = biggest;
  }

  for (const html::Field &f : chosen->fields) {
    if (!cfg.password_field.empty()) {
      if (util::iequals(f.name, cfg.password_field)) plan.password_field = f.name;
    } else if (util::iequals(f.type, "password") && plan.password_field.empty()) {
      plan.password_field = f.name;
    }

    if (!cfg.username_field.empty()) {
      if (util::iequals(f.name, cfg.username_field)) plan.username_field = f.name;
    }
  }

  if (plan.password_field.empty()) {
    plan.password_field = pick_field(chosen->fields, kPasswordHints);
  }

  if (plan.username_field.empty()) {
    plan.username_field = pick_field(chosen->fields, kUsernameHints, plan.password_field);
  }
  if (plan.username_field.empty()) {
    // Last resort: the first visible text-ish input that is not the password.
    for (const html::Field &f : chosen->fields) {
      if (f.name == plan.password_field) continue;
      if (f.type.empty() || f.type == "text" || f.type == "email" || f.type == "tel") {
        plan.username_field = f.name;
        break;
      }
    }
  }

  if (plan.username_field.empty() || plan.password_field.empty()) {
    // Name the fields. Saying only how many there were forces a second
    // command on a network the user may have to walk back to.
    std::string listing;
    for (const html::Field &f : chosen->fields) {
      if (!listing.empty()) listing += ", ";
      listing += f.name;
      if (!f.type.empty()) listing += "[" + f.type + "]";
    }
    if (listing.empty()) listing = "(none)";

    plan.reason = "could not tell which input is the username and which is the password. "
                  "The form has: " +
                  listing + ". Set username_field / password_field in [portal] to two of these";
    if (!plan.username_field.empty()) {
      plan.reason += " (username_field = " + plan.username_field + " was recognised)";
    } else if (!plan.password_field.empty()) {
      plan.reason += " (password_field = " + plan.password_field + " was recognised)";
    }
    return plan;
  }

  // Start from the form's own values so hidden CSRF/session tokens survive.
  for (const html::Field &f : chosen->fields) {
    std::string value = f.value;
    if (f.name == plan.username_field) value = username;
    if (f.name == plan.password_field) value = password;
    plan.fields.emplace_back(f.name, value);
  }

  for (const auto &kv : cfg.extra_fields) {
    bool replaced = false;
    for (auto &field : plan.fields) {
      if (field.first == kv.first) {
        field.second = kv.second;
        replaced = true;
      }
    }
    if (!replaced) plan.fields.emplace_back(kv.first, kv.second);
  }

  plan.action_url =
      chosen->action.empty() ? page.url : util::resolve_url(page.url, chosen->action);
  plan.method = util::iequals(chosen->method, "get") ? "GET" : "POST";
  plan.ok = true;
  return plan;
}

namespace {

// Decides whether a submission worked, preferring the config's explicit
// markers and otherwise re-probing until connectivity actually appears.
void judge(http::Client &client, const Config &cfg, const http::Response &resp, LoginResult *out) {
  out->status = resp.status;
  out->response_body = resp.body;

  if (!cfg.failure_contains.empty() && util::icontains(resp.body, cfg.failure_contains)) {
    out->success = false;
    out->message = "portal reported failure: " + first_line(resp.body);
    return;
  }
  if (!cfg.success_contains.empty() && util::icontains(resp.body, cfg.success_contains)) {
    out->success = true;
    out->message = "portal reported success";
    return;
  }

  Probe last;
  for (int attempt = 1; attempt <= kVerifyAttempts; ++attempt) {
    sleep_ms(kVerifyDelayMs);
    last = probe(client, cfg);
    if (last.state == State::Online) {
      out->success = true;
      out->message = "verified online";
      return;
    }
    log::info("waiting for connectivity (" + std::to_string(attempt) + "/" +
              std::to_string(kVerifyAttempts) + "): still " + state_name(last.state));
  }

  out->success = false;
  out->message = explain_failed_verification(last, out->posted_to);
  if (!resp.body.empty()) {
    std::string t = html::title(resp.body);
    if (!t.empty()) out->message += " (portal page title: " + t + ")";
  }
}

} // namespace

LoginResult login(http::Client &client, const Config &cfg, const std::string &password) {
  LoginResult result;

  Probe pr = probe(client, cfg);
  if (pr.state == State::Online) {
    result.success = true;
    result.message = "already online, nothing to do";
    return result;
  }
  if (pr.state == State::Offline) {
    result.success = false;
    result.message = "no network path at all" + (pr.error.empty() ? "" : " (" + pr.error + ")");
    return result;
  }

  std::map<std::string, std::string> vars = {
      {"username", cfg.username},
      {"password", password},
  };

  if (cfg.login_method == "raw") {
    if (cfg.login_url.empty()) {
      result.message = "login_method = raw requires login_url in the config";
      return result;
    }
    http::Request req;
    req.url = util::expand_vars(cfg.login_url, vars);
    req.method = util::iequals(cfg.http_method, "GET") ? "GET" : "POST";
    req.body = util::expand_vars(cfg.post_body, vars);
    req.follow = true;
    req.timeout_sec = 12;

    result.posted_to = req.url;
    result.method = req.method;
    result.sent_fields.emplace_back("(raw body)",
                                    util::expand_vars(cfg.post_body,
                                                      {{"username", cfg.username},
                                                       {"password", kMaskedPassword}}));

    http::Response resp = fetch(client, cfg, req);
    if (!resp.ok) {
      result.message = "request failed: " + resp.error;
      return result;
    }
    judge(client, cfg, resp, &result);
    return result;
  }

  LoginPage page = resolve_login_page(client, cfg, pr);
  if (page.html.empty()) {
    result.message = "could not load the portal page";
    if (!page.note.empty()) result.message += ": " + page.note;
    return result;
  }

  // Srun portals ship no <form> at all - the request is assembled in JS from a
  // server-issued challenge - so they need their own path entirely.
  if (cfg.login_method == "srun" || srun::looks_like_srun(page.url, page.html)) {
    srun::PortalInfo portal_info = srun::parse_portal_info(page.url, page.html);
    if (!portal_info.ok) {
      result.message = "looks like a Srun portal, but " + portal_info.note;
      return result;
    }
    log::info("detected a Srun portal at " + portal_info.origin);
    if (!portal_info.note.empty()) log::warn("srun: " + portal_info.note);

    LoginResult srun_result = srun::login(client, cfg, portal_info, password);
    if (!srun_result.success) return srun_result;

    // The portal saying "ok" is not proof the network came up; hold it to the
    // same standard as every other login path.
    Probe last;
    for (int attempt = 1; attempt <= kVerifyAttempts; ++attempt) {
      sleep_ms(kVerifyDelayMs);
      last = probe(client, cfg);
      if (last.state == State::Online) {
        srun_result.message += "; verified online";
        return srun_result;
      }
      log::info("waiting for connectivity (" + std::to_string(attempt) + "/" +
                std::to_string(kVerifyAttempts) + "): not online yet");
    }
    srun_result.success = false;
    srun_result.message = "srun accepted the login (" + srun_result.message + ") but " +
                          explain_failed_verification(last, srun_result.posted_to);
    return srun_result;
  }

  FormPlan plan = plan_form_login(cfg, page, cfg.username, password);
  if (!plan.ok) {
    result.message = plan.reason;
    return result;
  }

  std::string encoded = util::form_encode(plan.fields);
  result.posted_to = plan.action_url;
  result.method = plan.method;
  for (const auto &kv : plan.fields) {
    result.sent_fields.emplace_back(kv.first,
                                    kv.first == plan.password_field ? kMaskedPassword : kv.second);
  }

  http::Request req;
  req.method = plan.method;
  req.referer = page.url;
  req.follow = true;
  req.timeout_sec = 12;
  if (plan.method == "GET") {
    req.url = plan.action_url + (plan.action_url.find('?') == std::string::npos ? "?" : "&") + encoded;
  } else {
    req.url = plan.action_url;
    req.body = encoded;
  }

  log::info("submitting login to " + plan.action_url + " as " + cfg.username);
  http::Response resp = fetch(client, cfg, req);
  if (!resp.ok) {
    result.message = "submit failed: " + resp.error;
    return result;
  }

  judge(client, cfg, resp, &result);
  return result;
}

LoginResult logout(http::Client &client, const Config &cfg) {
  LoginResult result;

  // A Srun logout is a signed API call, not a URL to visit. It needs the
  // portal's origin, which is only knowable from the config once we are
  // online and the portal no longer intercepts anything.
  if (cfg.logout_url.empty() && cfg.login_method == "srun" && !cfg.login_url.empty()) {
    srun::PortalInfo portal_info;
    portal_info.origin = util::url_origin(cfg.login_url);
    portal_info.ac_id = "1";
    portal_info.ok = !portal_info.origin.empty();
    if (portal_info.ok) return srun::logout(client, cfg, portal_info);
  }

  if (cfg.logout_url.empty()) {
    result.message = "no logout_url configured";
    return result;
  }

  std::map<std::string, std::string> vars = {{"username", cfg.username}};
  http::Request req;
  req.url = util::expand_vars(cfg.logout_url, vars);
  req.method = "GET";
  req.follow = true;
  req.timeout_sec = 10;

  result.posted_to = req.url;
  result.method = req.method;

  http::Response resp = fetch(client, cfg, req);
  if (!resp.ok) {
    result.message = "request failed: " + resp.error;
    return result;
  }

  result.status = resp.status;
  result.response_body = resp.body;
  result.success = resp.status >= 200 && resp.status < 400;
  result.message = result.success ? "logout request sent" : "portal returned " + std::to_string(resp.status);
  return result;
}

} // namespace sw::portal
