// Self-contained test runner: no framework, no network.
//
// The portal-facing logic (HTML scanning, field identification, URL
// resolution) is the part most likely to break against a school's markup and
// the part that cannot be exercised without standing on campus. These tests
// pin it against HTML shaped like the portals actually deployed in Chinese
// universities.

#include <cstdio>
#include <string>
#include <vector>

#include "sw/byod.hpp"
#include "sw/config.hpp"
#include "sw/dns.hpp"
#include "sw/http.hpp"
#include "sw/html.hpp"
#include "sw/netenv.hpp"
#include "sw/portal.hpp"
#include "sw/srun.hpp"
#include "sw/util.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string &what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("  FAIL  %s\n", what.c_str());
  }
}

void check_eq(const std::string &got, const std::string &want, const std::string &what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL  %s\n        want: \"%s\"\n        got : \"%s\"\n", what.c_str(),
                want.c_str(), got.c_str());
  }
}

void section(const char *name) { std::printf("%s\n", name); }

std::string plan_value(const sw::portal::FormPlan &plan, const std::string &name) {
  for (const auto &kv : plan.fields) {
    if (kv.first == name) return kv.second;
  }
  return "<absent>";
}

sw::portal::LoginPage page_of(const std::string &url, const std::string &html) {
  sw::portal::LoginPage page;
  page.url = url;
  page.html = html;
  return page;
}

// ---------------------------------------------------------------------------

void test_url_resolution() {
  section("util::resolve_url");
  using sw::util::resolve_url;

  check_eq(resolve_url("http://a.cn/x/y.html", "http://b.cn/z"), "http://b.cn/z", "absolute wins");
  check_eq(resolve_url("https://a.cn/x/y.html", "//c.cn/z"), "https://c.cn/z", "protocol-relative");
  check_eq(resolve_url("http://a.cn/x/y.html", "/login.jsp"), "http://a.cn/login.jsp", "root-relative");
  check_eq(resolve_url("http://a.cn/x/y.html", "login.jsp"), "http://a.cn/x/login.jsp", "dir-relative");
  check_eq(resolve_url("http://a.cn/x/y.html?q=1", "?r=2"), "http://a.cn/x/y.html?r=2", "query-only");
  check_eq(resolve_url("http://a.cn:8080/portal", "do.php"), "http://a.cn:8080/do.php",
           "port preserved");
  check_eq(resolve_url("http://a.cn/x/y.html", ""), "http://a.cn/x/y.html", "empty ref");

  // A captive portal's relative URL carries whole URLs inside its query.
  // Treating the first "://" found anywhere as a scheme made this look
  // absolute, and libcurl then rejected it for having no host.
  check_eq(resolve_url("http://172.29.250.5:30004/byod/index.html?a=1",
                       "/byod/view/t.html?userurl=http://captive.apple.com/x&ssid=E"),
           "http://172.29.250.5:30004/byod/view/t.html?userurl=http://captive.apple.com/x&ssid=E",
           "relative path with an absolute URL in its query stays relative");
  check_eq(resolve_url("http://a.cn/p/q.html", "next.html?u=https://b.cn/z"),
           "http://a.cn/p/next.html?u=https://b.cn/z", "same for a dir-relative ref");
  check_eq(resolve_url("http://a.cn/x", "ftp://b.cn/f"), "ftp://b.cn/f",
           "a real scheme at the start is still absolute");
  check_eq(resolve_url("http://a.cn/x", "mailto:a@b.cn"), "http://a.cn/mailto:a@b.cn",
           "a schemeless colon is not a scheme");

  section("util::url_encode / expand_vars");
  check_eq(sw::util::url_encode("a b&c=d"), "a%20b%26c%3Dd", "reserved chars encoded");
  check_eq(sw::util::url_decode("a%20b%26c"), "a b&c", "decode round trip");
  check_eq(sw::util::expand_vars("u={username}&p={password}", {{"username", "s1"}, {"password", "p@ss"}}),
           "u=s1&p=p@ss", "plain substitution");
  check_eq(sw::util::expand_vars("p={password|url}", {{"password", "p@ss word"}}),
           "p=p%40ss%20word", "url-encoded substitution");
  check_eq(sw::util::expand_vars("{unknown}", {{"a", "b"}}), "{unknown}", "unknown left intact");
  check_eq(sw::util::html_unescape("a &amp; b &lt;c&gt; &quot;d&quot;"), "a & b <c> \"d\"",
           "entities decoded");
}

void test_form_scanning() {
  section("html::extract_forms - generic portal");
  const std::string html = R"(
    <html><head><title>Campus Network Login</title></head><body>
    <form id="loginForm" name="f" method="POST" action="/eportal/login.do">
      <input type="hidden" name="csrfToken" value="a1b2c3">
      <input type="text" name="userName" value="" placeholder="account">
      <input type="password" name="userPwd">
      <select name="domain">
        <option value="edu">edu</option>
        <option value="cmcc" selected>cmcc</option>
      </select>
      <input type="checkbox" name="remember" value="1">
      <input type="checkbox" name="agree" value="yes" checked>
      <input type="submit" name="submitBtn" value="Login">
    </form></body></html>)";

  std::vector<sw::html::Form> forms = sw::html::extract_forms(html);
  check(forms.size() == 1, "exactly one form found");
  if (forms.empty()) return;

  const sw::html::Form &f = forms[0];
  check_eq(f.action, "/eportal/login.do", "action attribute");
  check_eq(f.method, "post", "method lowercased");
  check_eq(f.id, "loginForm", "id attribute");
  check(f.has_password(), "password field detected");

  check(f.find("csrfToken") != nullptr, "hidden token captured");
  check_eq(f.find("csrfToken")->value, "a1b2c3", "hidden token value");
  check_eq(f.find("domain")->value, "cmcc", "selected <option> wins");
  check(f.find("agree") != nullptr, "checked checkbox included");
  check(f.find("remember") == nullptr, "unchecked checkbox omitted");
  check(f.find("submitBtn") == nullptr, "submit button omitted");

  check_eq(sw::html::title(html), "Campus Network Login", "page title");
}

void test_unquoted_attributes() {
  section("html::extract_forms - unquoted attributes (Dr.COM style)");
  // Portals of this vintage emit attributes without quotes at all.
  const std::string html =
      "<form method=post action=/0.htm name=f1>"
      "<input type=text name=DDDDD size=20>"
      "<input type=password name=upass>"
      "<input type=hidden name=R1 value=0>"
      "<input type=submit value=Login>"
      "</form>";

  std::vector<sw::html::Form> forms = sw::html::extract_forms(html);
  check(forms.size() == 1, "form parsed despite unquoted attributes");
  if (forms.empty()) return;

  check_eq(forms[0].action, "/0.htm", "unquoted action");
  check_eq(forms[0].method, "post", "unquoted method");
  check(forms[0].find("DDDDD") != nullptr, "unquoted input name");
  check(forms[0].has_password(), "unquoted password type");
  check_eq(forms[0].find("R1")->value, "0", "unquoted hidden value");
}

void test_redirect_hints() {
  section("html redirect hints");

  check_eq(sw::html::meta_refresh_url(
               R"(<meta http-equiv="refresh" content="0;url=http://10.0.0.1/portal/index.jsp">)"),
           "http://10.0.0.1/portal/index.jsp", "meta refresh, quoted");
  check_eq(sw::html::meta_refresh_url(R"(<meta http-equiv=refresh content="2; URL=/login.html">)"),
           "/login.html", "meta refresh, uppercase URL=");

  check_eq(sw::html::js_redirect_url(
               R"(<script>top.self.location.href='http://172.16.0.1:8080/portal?ip=1.2.3.4';</script>)"),
           "http://172.16.0.1:8080/portal?ip=1.2.3.4", "top.self.location.href");
  check_eq(sw::html::js_redirect_url(R"(<script>window.location.replace("/index.php");</script>)"),
           "/index.php", "location.replace");
  check_eq(sw::html::js_redirect_url(R"(<script>var x=1;</script>)"), "", "no false positive");

  check_eq(sw::html::iframe_src(R"(<iframe src="about:blank"></iframe><iframe src="/real.jsp">)"),
           "/real.jsp", "iframe skips about:blank");
  check_eq(sw::html::iframe_src(R"(<frameset><frame src="/login.asp"></frameset>)"), "/login.asp",
           "a plain <frame> counts too");

  section("html::submits_on_load");
  // The ISP bootstrap: nothing to fill in, the page posts the form itself.
  const std::string bootstrap =
      R"(<html><head><title>main</title></head>)"
      R"(<script>function getBasInfo(){document.getElementById("basPushUrl").value=)"
      R"(window.parent.location.href;document.forms[0].submit();}</script>)"
      R"HTML(<body onload="getBasInfo()"><form action="/index.do" method="post">)HTML"
      R"(<input name="basPushUrl" id="basPushUrl" type="hidden">)"
      R"(<input type="hidden" name="testmacauth" value="false"></form></body></html>)";
  check(sw::html::submits_on_load(bootstrap), "a self-submitting page is recognised");
  check(!sw::html::submits_on_load(
            R"(<html><body><form><input name=u><input type=password name=p></form></body></html>)"),
        "an ordinary login form is not auto-submitted");
  check(!sw::html::submits_on_load(
            R"HTML(<html><body onload="init()">no form here</body></html>)HTML"),
        "onload without a submit call is not enough");

  // Its fields are all hidden, so it can never be planned as a login form --
  // which is exactly why following it has to happen during discovery.
  sw::portal::LoginPage boot_page;
  boot_page.url = "http://portal.example.com/?wlanuserip=10.0.0.1";
  boot_page.html = bootstrap;
  sw::Config boot_cfg;
  check(!sw::portal::plan_form_login(boot_cfg, boot_page, "u", "p").ok,
        "the bootstrap form is not mistaken for a login form");

  section("html::script_srcs");
  // Shaped like the real BYOD portal: an empty body and a chain of scripts.
  const std::string byod = R"(<!doctype html><html><head><title>BYOD</title></head>
<body><div id="tip"></div></body>
<script type="text/javascript" src="/byod/resources/byod/common/js/customCommon.js" charset="utf-8"></script>
<script type="text/javascript" src="https://cdn.example.com/jquery.min.js"></script>
<script type="text/javascript" src="/byod/resources/byod/index.js?_=00001"></script>
<script>var inline = 1;</script>
</html>)";
  std::vector<std::string> scripts = sw::html::script_srcs(byod);
  check(scripts.size() == 3, "three external scripts found, inline one ignored");
  if (scripts.size() == 3) {
    check_eq(scripts[0], "/byod/resources/byod/common/js/customCommon.js", "first script, in order");
    check_eq(scripts[2], "/byod/resources/byod/index.js?_=00001", "query string preserved");
  }
  check(sw::html::script_srcs("<html><body>nothing</body></html>").empty(),
        "no scripts yields nothing");
}

void test_field_identification() {
  section("portal::plan_form_login - auto-detection");
  sw::Config cfg;

  const std::string html = R"(
    <form method="POST" action="/eportal/login.do">
      <input type="hidden" name="csrfToken" value="a1b2c3">
      <input type="text" name="userName">
      <input type="password" name="userPwd">
    </form>)";

  sw::portal::FormPlan plan = sw::portal::plan_form_login(
      cfg, page_of("http://10.1.1.1/portal/index.jsp", html), "20210001", "secret");

  check(plan.ok, "plan built");
  check_eq(plan.username_field, "userName", "username field detected");
  check_eq(plan.password_field, "userPwd", "password field detected");
  check_eq(plan.action_url, "http://10.1.1.1/eportal/login.do", "action resolved against page URL");
  check_eq(plan.method, "POST", "method preserved");
  check_eq(plan_value(plan, "userName"), "20210001", "username filled in");
  check_eq(plan_value(plan, "userPwd"), "secret", "password filled in");
  check_eq(plan_value(plan, "csrfToken"), "a1b2c3", "hidden token carried through");
}

void test_drcom_field_names() {
  section("portal::plan_form_login - DDDDD/upass naming");
  sw::Config cfg;
  const std::string html =
      "<form method=post action=/0.htm>"
      "<input type=text name=DDDDD><input type=password name=upass>"
      "<input type=hidden name=R1 value=0></form>";

  sw::portal::FormPlan plan =
      sw::portal::plan_form_login(cfg, page_of("http://1.1.1.2/", html), "u", "p");

  check(plan.ok, "plan built for Dr.COM-style form");
  check_eq(plan.username_field, "DDDDD", "DDDDD recognised as the account field");
  check_eq(plan.password_field, "upass", "upass recognised as the password field");
  check_eq(plan_value(plan, "R1"), "0", "hidden field preserved");
}

void test_hint_priority() {
  section("portal::plan_form_login - hint priority over field order");
  sw::Config cfg;

  // "validcode" contains the substring "id" and "pwdTip" contains "pwd", and
  // both appear before the fields that should actually win. A naive substring
  // matcher picks the wrong ones.
  const std::string html = R"(
    <form method="post" action="/login">
      <input type="text" name="validcode">
      <input type="text" name="pwdTip">
      <input type="text" name="username">
      <input type="text" name="password">
    </form>)";

  sw::portal::FormPlan plan =
      sw::portal::plan_form_login(cfg, page_of("http://p.cn/", html), "u", "p");

  check(plan.ok, "plan built");
  check_eq(plan.password_field, "password", "exact \"password\" beats \"pwdTip\"");
  check_eq(plan.username_field, "username", "exact \"username\" beats \"validcode\"");
  check_eq(plan_value(plan, "validcode"), "", "unrelated field left untouched");
  check_eq(plan_value(plan, "pwdTip"), "", "unrelated field left untouched");
}

void test_short_hints_do_not_overmatch() {
  section("portal::plan_form_login - short hints do not substring-match");
  sw::Config cfg;

  // Only weak candidates: "id" and "pw" must NOT be matched as substrings of
  // "validcode" / "pwdHint", so the plan must fail loudly instead of
  // submitting a password into the wrong input.
  const std::string html = R"(
    <form method="post" action="/login">
      <input type="text" name="validcode">
      <input type="text" name="pwdHint">
    </form>)";

  sw::portal::FormPlan plan =
      sw::portal::plan_form_login(cfg, page_of("http://p.cn/", html), "u", "p");
  check(!plan.ok, "refuses to match \"id\" inside \"validcode\"");
}

void test_config_overrides() {
  section("portal::plan_form_login - config overrides");
  sw::Config cfg;
  cfg.username_field = "acct";
  cfg.password_field = "secret";
  cfg.extra_fields["operator"] = "telecom";
  cfg.extra_fields["csrfToken"] = "forced";

  // Deliberately unhelpful names that no heuristic would guess.
  const std::string html = R"(
    <form method="get" action="do.php">
      <input type="text" name="acct">
      <input type="text" name="secret">
      <input type="hidden" name="csrfToken" value="original">
    </form>)";

  sw::portal::FormPlan plan =
      sw::portal::plan_form_login(cfg, page_of("http://p.cn/a/b.html", html), "u1", "p1");

  check(plan.ok, "plan built from overrides");
  check_eq(plan.username_field, "acct", "username_field override honoured");
  check_eq(plan.password_field, "secret", "password_field override honoured");
  check_eq(plan_value(plan, "acct"), "u1", "override field filled with username");
  check_eq(plan_value(plan, "secret"), "p1", "override field filled with password");
  check_eq(plan_value(plan, "operator"), "telecom", "extra field appended");
  check_eq(plan_value(plan, "csrfToken"), "forced", "extra field overrides the form default");
  check_eq(plan.method, "GET", "GET form stays a GET");
  check_eq(plan.action_url, "http://p.cn/a/do.php", "relative action resolved");
}

void test_plan_failure_is_reported() {
  section("portal::plan_form_login - unidentifiable form");
  sw::Config cfg;
  // Two opaque fields, neither typed as a password nor named like an account.
  const std::string html =
      R"(<form action="/go"><input type="text" name="q1"><input type="text" name="q2"></form>)";

  sw::portal::FormPlan plan =
      sw::portal::plan_form_login(cfg, page_of("http://p.cn/", html), "u", "p");
  check(!plan.ok, "refuses to guess when nothing matches");
  check(!plan.reason.empty(), "failure carries an explanation");

  // The explanation has to name the fields: the user may be on a network they
  // had to walk to, and "found 2 fields" costs them a second trip.
  check(sw::util::icontains(plan.reason, "q1"), "failure names the first field");
  check(sw::util::icontains(plan.reason, "q2"), "failure names the second field");
  check(sw::util::icontains(plan.reason, "username_field"), "failure says what to set");

  // When one side was recognised, say which, so only the other needs setting.
  const std::string half = R"(<form action="/go"><input type="password" name="pw1">
      <input type="text" name="zzz"><input type="text" name="qqq"></form>)";
  sw::portal::FormPlan partial =
      sw::portal::plan_form_login(cfg, page_of("http://p.cn/", half), "u", "p");
  if (!partial.ok) {
    check(sw::util::icontains(partial.reason, "pw1"), "names the side it did recognise");
  } else {
    check(true, "both sides recognised on this shape");
  }
}

void test_url_parsing() {
  section("util::parse_url");
  using sw::util::parse_url;

  sw::util::UrlParts p = parse_url("https://w.bnbu.edu.cn/index_1.html");
  check_eq(p.scheme, "https", "scheme");
  check_eq(p.host, "w.bnbu.edu.cn", "host");
  check_eq(p.port, "443", "https default port");

  p = parse_url("http://10.0.0.1:8080/portal?ip=1.2.3.4");
  check_eq(p.host, "10.0.0.1", "host with explicit port");
  check_eq(p.port, "8080", "explicit port");

  p = parse_url("http://portal.cn");
  check_eq(p.port, "80", "http default port");

  p = parse_url("http://user:pw@portal.cn/x");
  check_eq(p.host, "portal.cn", "userinfo stripped");

  p = parse_url("http://[2001:db8::1]:8080/x");
  check_eq(p.host, "2001:db8::1", "IPv6 literal unwrapped");
  check_eq(p.port, "8080", "IPv6 port");

  p = parse_url("portal.cn/login");
  check_eq(p.scheme, "http", "scheme defaults to http");
  check_eq(p.host, "portal.cn", "schemeless host");

  // Same trap as resolve_url: the embedded URL must not be read as the scheme.
  p = parse_url("/byod/view/t.html?userurl=http://captive.apple.com/x");
  check_eq(p.host, "", "a relative path has no host, embedded URL notwithstanding");
  check_eq(sw::util::url_origin("/byod/view/t.html?userurl=http://captive.apple.com/x"), "",
           "url_origin refuses a relative path too");
}

void test_dhcp_nameserver_parsing() {
  section("dns::parse_dhcp_nameservers");

  // Real `ipconfig getpacket en0` shape.
  const std::string multi = R"(
op = BOOTREPLY
yiaddr = 10.253.51.53
domain_name_server (ip_mult): {10.253.0.1, 10.253.0.2}
router (ip_mult): {10.253.51.1}
)";
  std::vector<std::string> servers = sw::dns::parse_dhcp_nameservers(multi);
  check(servers.size() == 2, "two nameservers parsed");
  if (servers.size() == 2) {
    check_eq(servers[0], "10.253.0.1", "first nameserver");
    check_eq(servers[1], "10.253.0.2", "second nameserver");
  }

  const std::string single = "domain_name_server (ip): 172.19.9.90\n";
  servers = sw::dns::parse_dhcp_nameservers(single);
  check(servers.size() == 1, "single-value form parsed");
  if (!servers.empty()) check_eq(servers[0], "172.19.9.90", "single nameserver");

  // domain_name is a different option and must not be mistaken for a server.
  servers = sw::dns::parse_dhcp_nameservers("domain_name (string): bnbu.edu.cn\n");
  check(servers.empty(), "domain_name is not a nameserver");

  servers = sw::dns::parse_dhcp_nameservers("domain_name_server (ip_mult): {not-an-ip}\n");
  check(servers.empty(), "non-IP values rejected");

  check(sw::dns::parse_dhcp_nameservers("").empty(), "empty packet yields nothing");
}

void test_resolve_failure_detection() {
  section("dns::is_resolve_failure");
  check(sw::dns::is_resolve_failure("Could not resolve host: w.bnbu.edu.cn"),
        "libcurl resolve error recognised");
  check(sw::dns::is_resolve_failure("could not resolve host"), "case-insensitive");
  check(!sw::dns::is_resolve_failure("Connection refused"), "connection error is not a resolve error");
  // The dorm-network case: libcurl reports a timeout that happened *during*
  // name resolution. Missing this left the DNS fallback dormant exactly where
  // it was needed most.
  check(sw::dns::is_resolve_failure("Resolving timed out after 6005 milliseconds"),
        "a timeout during resolution counts as a resolve failure");
  check(!sw::dns::is_resolve_failure("Connection timed out after 5005 milliseconds"),
        "a timeout during connect does NOT count as a resolve failure");
  check(!sw::dns::is_resolve_failure("SSL certificate problem"), "TLS error is not a resolve error");
  check(!sw::dns::is_resolve_failure(""), "empty error is not a resolve error");
}

void test_host_pinning() {
  section("http::Client host pinning (CURLOPT_RESOLVE)");
  sw::http::Client client;

  check(!client.has_resolve_for("portal.invalid"), "nothing pinned initially");
  client.add_resolve("portal.invalid", "1", "127.0.0.1");
  check(client.has_resolve_for("portal.invalid"), "pin recorded");
  client.add_resolve("portal.invalid", "1", "127.0.0.1");
  check(client.has_resolve_for("portal.invalid"), "duplicate pin is a no-op");

  // The pin must survive curl_easy_reset(), which send() calls on every
  // request. Port 1 is closed, so a working pin turns a resolve failure into a
  // connection failure -- a different error class, and the observable proof the
  // option took effect.
  sw::http::Client fresh;
  const char *url = "http://schoolwifi-pin-check.invalid:1/";

  // Informational only. .invalid cannot resolve per RFC 2606, but resolvers
  // that hijack NXDOMAIN will answer anyway, so this is reported rather than
  // asserted -- a test whose *number* of assertions depends on the network
  // hides real regressions behind a shifting total.
  sw::http::Response before = fresh.get(url, /*follow=*/false, 3);
  if (before.ok || !sw::dns::is_resolve_failure(before.error)) {
    std::printf("  note  this network answers for .invalid, so the unpinned "
                "baseline is weaker than usual\n");
  }

  fresh.add_resolve("schoolwifi-pin-check.invalid", "1", "127.0.0.1");
  sw::http::Response after = fresh.get(url, /*follow=*/false, 3);
  check(!sw::dns::is_resolve_failure(after.error),
        "pinned host gets past resolution (error was: " + after.error + ")");

  // And again, proving the pin is re-applied rather than consumed once.
  sw::http::Response again = fresh.get(url, /*follow=*/false, 3);
  check(!sw::dns::is_resolve_failure(again.error), "pin survives a second request");
}

void test_srun_primitives() {
  section("srun primitives (vectors from tests/srun_reference.py)");

  check_eq(sw::srun::base64("hello"), "OCubWC4=", "base64, no padding remainder");
  check_eq(sw::srun::base64("hi"), "OCY=", "base64, two-byte tail");
  check_eq(sw::srun::base64("a"), "Z+==", "base64, one-byte tail");
  check_eq(sw::srun::base64(""), "", "base64 of empty string");

  // x_encode output is binary; compare as hex.
  std::string encoded = sw::srun::x_encode("hello world", "key");
  std::string hex;
  static const char *digits = "0123456789abcdef";
  for (unsigned char c : encoded) {
    hex += digits[c >> 4];
    hex += digits[c & 0x0F];
  }
  check_eq(hex, "cfb3875b982e9a84704b25c10a02b24a", "x_encode matches the reference");
  check_eq(sw::srun::x_encode("", "key"), "", "x_encode of empty string");

  check_eq(sw::srun::sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d",
           "sha1 known vector");
  check_eq(sw::srun::hmac_md5_hex("key", "The quick brown fox jumps over the lazy dog"),
           "80070713463e7749b90c2dc24911e275", "hmac-md5 known vector");
}

void test_srun_login_payload() {
  section("srun login payload (vectors from tests/srun_reference.py)");

  const std::string token = "abcdef0123456789abcdef0123456789";
  const std::string username = "20210001";
  const std::string password = "s3cr3t p@ss";
  const std::string ip = "10.253.51.53";
  const std::string acid = "1";

  std::string info_json = sw::srun::build_info_json(username, password, ip, acid);
  check_eq(info_json,
           "{\"username\":\"20210001\",\"password\":\"s3cr3t p@ss\",\"ip\":\"10.253.51.53\""
           ",\"acid\":\"1\",\"enc_ver\":\"srun_bx1\"}",
           "info json matches JSON.stringify output");

  std::string info_param = sw::srun::build_info_param(info_json, token);
  check_eq(info_param,
           "{SRBX1}KE+cQm21RKCvcjkwbJmyVC0YyVm1VNMgQBH0hg7xWUIUiQQiukXxt74rwb8048xT/F5j8PCbSK00"
           "RqzcZRJNiARROXq5cgaA9YYeL+NDDwmBHAQqTByE+77vmyi5A8jJ2K7ci5XCQNH=",
           "encrypted info parameter matches the reference");

  std::string hmd5 = sw::srun::hmac_md5_hex(token, password);
  check_eq(hmd5, "c24de49827c7eee8669b2a835bada096", "hmd5 matches the reference");

  check_eq(sw::srun::build_chksum(token, username, hmd5, acid, ip, "200", "1", info_param),
           "78cf715df033dd66fe48b9e8ae70a998a9229550", "chksum matches the reference");
}

void test_srun_response_parsing() {
  section("json helpers / srun response parsing");

  const std::string jsonp =
      "jQuery_1758441234({\"challenge\":\"abc123\",\"client_ip\":\"10.253.51.53\","
      "\"online_ip\":\"10.253.51.53\",\"ecode\":0,\"error\":\"ok\"})";
  std::string body = sw::util::strip_jsonp(jsonp);
  check_eq(sw::util::json_field(body, "challenge"), "abc123", "challenge extracted");
  check_eq(sw::util::json_field(body, "online_ip"), "10.253.51.53", "online_ip extracted");
  check_eq(sw::util::json_field(body, "ecode"), "0", "numeric field extracted");
  check_eq(sw::util::json_field(body, "error"), "ok", "error extracted");
  check_eq(sw::util::json_field(body, "missing"), "", "absent field yields empty");

  // A bare JSON body (no callback wrapper) must survive strip_jsonp.
  check_eq(sw::util::json_field(sw::util::strip_jsonp("{\"error\":\"ok\"}"), "error"), "ok",
           "unwrapped JSON still parses");
}

void test_srun_portal_detection() {
  section("srun detection against the real BNBU portal page");

  // Trimmed from a live capture: the inline CONFIG block is how the portal
  // tells its own JavaScript which acid and client IP to use.
  const std::string html = R"HTML(<!DOCTYPE html><html><head>
<meta name="keywords" content="Srunsoft">
<title>Srunsoft</title>
<script src="./static/themes/pro/js/redirect.js?v=17bc0ca727628ae0"></script>
</head><body>
<div class="panel-row"><input type="text" id="username" class="input-box"></div>
<div class="panel-row"><input type="password" id="password" class="input-box"></div>
<script>
    var CONFIG = {
        page   : 'account',
        acid   : "1",
        ip     : "10.253.51.53",
        nas    : "",
        isIPV6 :  false ,
        portal : {"AuthIP":"","ServiceIP":"https://218.75.75.93:8800","MacAuth":true}
    };
</script>
</body></html>)HTML";

  const std::string url = "https://w.bnbu.edu.cn/srun_portal_pc?ac_id=1&theme=pro";

  check(sw::srun::looks_like_srun(url, html), "recognised as a Srun portal");
  check(!sw::srun::looks_like_srun("http://10.0.0.1/login.jsp", "<form><input name=u></form>"),
        "an ordinary form portal is not mistaken for Srun");

  sw::srun::PortalInfo info = sw::srun::parse_portal_info(url, html);
  check(info.ok, "portal info parsed");
  check_eq(info.origin, "https://w.bnbu.edu.cn", "origin derived from the page URL");
  check_eq(info.ac_id, "1", "acid read from CONFIG");
  check_eq(info.client_ip, "10.253.51.53", "client ip read from CONFIG (not AuthIP/isIPV6)");

  // The page has inputs but no <form> and no name attributes, which is exactly
  // why the generic form path cannot handle it.
  sw::portal::LoginPage page;
  page.url = url;
  page.html = html;
  sw::Config cfg;
  check(!sw::portal::plan_form_login(cfg, page, "u", "p").ok,
        "the generic form planner correctly gives up on this page");

  // ac_id must still be found when CONFIG is absent but the URL carries it.
  sw::srun::PortalInfo from_url = sw::srun::parse_portal_info(url, "<html>Srunsoft</html>");
  check_eq(from_url.ac_id, "1", "acid falls back to the URL query");
}

void test_query_helpers() {
  section("util query helpers");
  const std::string url = "http://p.cn:30004/byod/index.html?usermac=de03-2992-d0c7&userip=10.0.0.1&ssid=E";
  check_eq(sw::util::query_string(url), "usermac=de03-2992-d0c7&userip=10.0.0.1&ssid=E",
           "query string without the '?'");
  check_eq(sw::util::query_param(url, "usermac"), "de03-2992-d0c7", "parameter by name");
  check_eq(sw::util::query_param(url, "ssid"), "E", "last parameter");
  check_eq(sw::util::query_param(url, "wlannasid"), "", "absent parameter");
  check_eq(sw::util::query_string("http://p.cn/x"), "", "no query at all");
  check_eq(sw::util::url_without_query(url), "http://p.cn:30004/byod/index.html",
           "query stripped for loop comparison");
  check_eq(sw::util::url_without_query("http://p.cn/x#frag"), "http://p.cn/x", "fragment stripped");
  check_eq(sw::util::url_without_query("http://p.cn/x"), "http://p.cn/x", "nothing to strip");
  check_eq(sw::util::query_param("http://p.cn/x?a=b%20c", "a"), "b c", "value is percent-decoded");
}

void test_byod_init() {
  section("byod::interpret_init - reproducing index.js");

  // Shaped like the real dorm portal.
  const std::string page =
      "http://172.29.250.5:30004/byod/index.html?usermac=de03-2992-d0c7&ssid=E";
  const std::string encoded_page = sw::util::url_encode(page);

  check(sw::byod::looks_like_byod(page, "<title>BYOD</title><script src=\"/byod/resources/byod/index.js\">"),
        "the BYOD shell is recognised");
  check(!sw::byod::looks_like_byod("http://10.0.0.1/login.jsp", "<form><input name=u></form>"),
        "an ordinary portal is not mistaken for BYOD");

  // The page init sends us to also lives under /byod/ and also loads scripts
  // from /byod/resources/byod/. Treating it as a shell made the tool ask init
  // about it, get the same page back, and append the query again -- a loop
  // whose URL doubled on every pass.
  check(!sw::byod::looks_like_byod(
            "http://172.29.250.5:30004/byod/view/byod/template/templatePc.html?customId=19",
            "<html><body><div id=\"app\"></div>"
            "<script src=\"/byod/resources/byod/templatePc.js\"></script></body></html>"),
        "the page init points at is not itself treated as a shell");

  // index.js: url without '?' gets "?<query>&nasRedirectUrl=..."
  sw::byod::InitResult r = sw::byod::interpret_init(
      R"({"code":0,"msg":"","data":{"url":"http://172.29.250.5:30004/portal/login.html","userip":"172.29.26.219"}})",
      page);
  check(r.ok, "success response understood");
  check_eq(r.next_url,
           "http://172.29.250.5:30004/portal/login.html?usermac=de03-2992-d0c7&ssid=E"
           "&nasRedirectUrl=" + encoded_page,
           "query string and nasRedirectUrl appended after a '?'");
  check(!r.already_registered, "not flagged as already registered");

  // index.js: url that already has '?' gets "&<query>&nasRedirectUrl=..."
  r = sw::byod::interpret_init(
      R"({"code":0,"data":{"url":"http://1.2.3.4/login?tpl=a"}})", page);
  check_eq(r.next_url,
           "http://1.2.3.4/login?tpl=a&usermac=de03-2992-d0c7&ssid=E&nasRedirectUrl=" + encoded_page,
           "existing query preserved with '&'");

  // A page with no query of its own.
  r = sw::byod::interpret_init(R"({"code":0,"data":{"url":"http://1.2.3.4/login"}})",
                               "http://172.29.250.5:30004/byod/index.html");
  check_eq(r.next_url,
           "http://1.2.3.4/login?nasRedirectUrl=" +
               sw::util::url_encode("http://172.29.250.5:30004/byod/index.html"),
           "no source query means just nasRedirectUrl");

  // "Result" in the url means the device is already registered.
  r = sw::byod::interpret_init(R"({"code":0,"data":{"url":"http://1.2.3.4/byodResult.html"}})", page);
  check(r.ok, "result page is still a usable next hop");
  check(r.already_registered, "device already registered is flagged");

  // code -1 with a URL in data: index.js navigates there anyway.
  r = sw::byod::interpret_init(R"({"code":-1,"msg":"not allowed","data":"/byod/view/fail.html"})", page);
  check(r.ok, "failure code with a url is still followed");
  check_eq(r.next_url, "http://172.29.250.5:30004/byod/view/fail.html",
           "relative failure url resolved against the page");
  check_eq(r.message, "not allowed", "portal's own message kept");

  // Nothing usable.
  r = sw::byod::interpret_init(R"({"code":0,"msg":"","data":{}})", page);
  check(!r.ok, "a response with no url is a failure");
  check(!r.message.empty(), "failure carries an explanation");

  r = sw::byod::interpret_init("not json at all", page);
  check(!r.ok, "garbage is not mistaken for a hop");
  check_eq(r.raw, "not json at all", "raw response preserved for diagnostics");
}

void test_byod_login_payload() {
  section("byod login payload");

  check_eq(sw::util::base64_encode("s3cr3t p@ss"), "czNjcjN0IHBAc3M=", "standard base64");
  check_eq(sw::util::base64_encode("a"), "YQ==", "one-byte tail");
  check_eq(sw::util::base64_encode("ab"), "YWI=", "two-byte tail");
  check_eq(sw::util::base64_encode(""), "", "empty");

  bool ascii = false;
  check_eq(sw::byod::encode_password("s3cr3t p@ss", &ascii), "czNjcjN0IHBAc3M=",
           "password encoded the way the portal's JS does");
  check(ascii, "an ASCII password is reported as such");
  // The portal's escape doubles backslashes before encoding.
  check_eq(sw::byod::encode_password("a\\b", &ascii), sw::util::base64_encode("a\\\\b"),
           "backslash is doubled first");
  sw::byod::encode_password("\xe4\xb8\xad", &ascii);
  check(!ascii, "a non-ASCII password is flagged, since that escape is not reproduced");

  // Values are copied verbatim so a number stays a number.
  const std::string policy =
      R"({"code":0,"licenseCode":"LIC-123","userGroupId":42,"validationType":0,)"
      R"("guestManagerId":"gm-9","defaultServiceTypeId":-1})";
  check_eq(sw::util::json_raw_field(policy, "licenseCode"), "\"LIC-123\"",
           "string keeps its quotes");
  check_eq(sw::util::json_raw_field(policy, "userGroupId"), "42", "number stays unquoted");
  check_eq(sw::util::json_raw_field(policy, "validationType"), "0", "zero is not mistaken for absent");
  check_eq(sw::util::json_raw_field(policy, "missing"), "", "absent field yields empty");

  // The login page: three hidden inputs, no type=password anywhere.
  const std::string login_html =
      R"(<html><body><div id="app"><input type="text" id="id_userName"></div>)"
      R"(<form method="post"><input type="hidden" name="userName" value="">)"
      R"(<input type="hidden" name="userPwd" value="">)"
      R"(<input type="hidden" name="serviceType" value=""></form></body></html>)";
  check(sw::byod::looks_like_login_page(
            "http://10.0.0.1:30004/byod/view/byod/template/templatePc.html?customId=19",
            login_html),
        "the BYOD login page is recognised");
  check(!sw::byod::looks_like_login_page("http://10.0.0.1/login.jsp",
                                         "<form><input name=u><input type=password name=p></form>"),
        "an ordinary form portal is not");
  // Mentioning the names is not enough: the visible boxes carry ids like
  // "id_userName", and matching those claimed pages that have no such form.
  check(!sw::byod::looks_like_login_page(
            "http://10.0.0.1:30004/byod/view/byod/template/templatePc.html",
            R"(<html><body><input type="text" id="id_userName">)"
            R"(<input type="password" id="id_userPwd"></body></html>)"),
        "ids alone do not make it a BYOD login page");

  // E63018 means "unknown account OR wrong service", so the choices matter.
  const std::string policy_with_services =
      R"({"code":0,"defaultServiceTypeId":7,)"
      R"("serviceList":[{"value":7,"label":"校园网"},{"value":9,"label":"中国联通"}]})";
  std::vector<sw::byod::Service> services = sw::byod::parse_service_list(policy_with_services);
  check(services.size() == 2, "both services parsed");
  if (services.size() == 2) {
    check_eq(services[0].value, "7", "first service id");
    check_eq(services[0].label, "校园网", "first service label");
    check_eq(services[1].value, "9", "second service id");
  }
  check(sw::byod::parse_service_list(R"({"serviceList":[]})").empty(),
        "an empty list yields nothing");
  check(sw::byod::parse_service_list(R"({"code":0})").empty(), "a missing list yields nothing");

  // And the generic planner must still refuse it, naming the fields.
  sw::portal::LoginPage page;
  page.url = "http://10.0.0.1:30004/byod/view/byod/template/templatePc.html";
  page.html = login_html;
  sw::Config cfg;
  sw::portal::FormPlan plan = sw::portal::plan_form_login(cfg, page, "u", "p");
  check(!plan.ok, "the hidden triple cannot be planned as a normal form");
  check(sw::util::icontains(plan.reason, "userPwd"), "and the refusal names userPwd");
}

void test_netenv_parsing() {
  section("netenv parsing");

  const std::string routes =
      "Routing tables\n\nInternet:\n"
      "Destination        Gateway            Flags        Netif Expire\n"
      "default            172.19.9.90        UGScg          en0\n"
      "127                127.0.0.1          UCS            lo0\n";
  check_eq(sw::netenv::parse_default_route_interface(routes), "en0", "default route interface");

  const std::string tunnelled =
      "Destination        Gateway            Flags        Netif\n"
      "default            link#22            UCSg         utun4\n";
  check_eq(sw::netenv::parse_default_route_interface(tunnelled), "utun4",
           "tunnelled default route");
  check_eq(sw::netenv::parse_default_route_interface("no routes here"), "",
           "missing default route");

  check_eq(sw::netenv::parse_default_gateway(routes), "172.19.9.90", "default gateway");
  check_eq(sw::netenv::parse_default_gateway(tunnelled), "",
           "on-link default route has no gateway address");
  check_eq(sw::netenv::parse_default_gateway("no routes here"), "", "missing gateway");

  check(sw::netenv::is_tunnel_interface("utun3"), "utun is a tunnel");
  check(sw::netenv::is_tunnel_interface("ipsec0"), "ipsec is a tunnel");
  check(!sw::netenv::is_tunnel_interface("en0"), "en0 is not a tunnel");
  check(!sw::netenv::is_tunnel_interface("lo0"), "lo0 is not a tunnel");

  section("netenv::assess_link");
  // The dorm case: no lease at all. Every hostname then fails to resolve,
  // which used to be reported as a DNS problem worth configuring around.
  sw::netenv::LinkState link = sw::netenv::assess_link(true, "en0", "", "");
  check(!link.up, "no address and no route means the network was never joined");
  check(sw::util::icontains(link.reason, "not been joined"), "reason says so plainly");
  check(!sw::util::icontains(link.reason, "DNS"), "reason does not send the user to DNS");

  link = sw::netenv::assess_link(false, "en0", "", "");
  check(!link.up, "Wi-Fi off is also down");
  check(sw::util::icontains(link.reason, "switched off"), "reason names the real cause");

  check(sw::netenv::assess_link(true, "en0", "172.29.26.219", "172.29.26.1").up,
        "an address and a route means the link is up");
  check(sw::netenv::assess_link(true, "en0", "172.29.26.219", "").up,
        "an address alone is enough");
  check(sw::netenv::assess_link(false, "en0", "", "10.0.0.1").up,
        "a route alone is enough, Wi-Fi off notwithstanding");

  const std::string proxy =
      "<dictionary> {\n  HTTPEnable : 1\n  HTTPPort : 7897\n  HTTPProxy : 127.0.0.1\n"
      "  HTTPSEnable : 0\n  SOCKSEnable : 0\n}\n";
  check_eq(sw::netenv::parse_system_proxy(proxy), "HTTP 127.0.0.1:7897", "http proxy detected");
  check_eq(sw::netenv::parse_system_proxy("<dictionary> {\n  HTTPEnable : 0\n}\n"), "",
           "disabled proxy not reported");
}

void test_chained_portal_diagnosis() {
  section("portal::explain_failed_verification - chained portals");

  auto captive_at = [](const std::string &probe_url, const std::string &location) {
    sw::portal::Probe pr;
    pr.state = sw::portal::State::Captive;
    pr.probe_url = probe_url;
    pr.location = location;
    return pr;
  };

  // Same portal still in the way: the credentials are the suspect.
  sw::portal::Probe same = captive_at("http://captive.apple.com/hotspot-detect.html",
                                      "https://w.bnbu.edu.cn/index_1.html");
  std::string msg = sw::portal::explain_failed_verification(
      same, "https://w.bnbu.edu.cn/cgi-bin/srun_portal");
  check(sw::util::icontains(msg, "connectivity never came up"),
        "same portal -> plain verification failure");
  check(!sw::util::icontains(msg, "second authentication"),
        "same portal is not reported as a second stage");

  // A different portal took over: that is a second stage, not a bad password.
  sw::portal::Probe other = captive_at("http://captive.apple.com/hotspot-detect.html",
                                       "http://10.20.30.40/unicom/login");
  msg = sw::portal::explain_failed_verification(other, "https://w.bnbu.edu.cn/cgi-bin/srun_portal");
  check(sw::util::icontains(msg, "second authentication"), "different portal -> second stage");
  check(sw::util::icontains(msg, "w.bnbu.edu.cn"), "names the portal we logged in to");
  check(sw::util::icontains(msg, "10.20.30.40"), "names the portal now intercepting");

  // Same host, different port is still a different portal.
  sw::portal::Probe other_port =
      captive_at("http://captive.apple.com/hotspot-detect.html", "http://10.0.0.1:8080/second");
  msg = sw::portal::explain_failed_verification(other_port, "http://10.0.0.1/first");
  check(sw::util::icontains(msg, "second authentication"),
        "a different port counts as a different portal");

  // Interception without a Location header: the portal is whatever the
  // substituted body points at, never the probe URL itself.
  sw::portal::Probe injected;
  injected.state = sw::portal::State::Captive;
  injected.probe_url = "http://captive.apple.com/hotspot-detect.html";
  injected.body = "<html><body><script>top.self.location.href="
                  "'http://10.20.30.40:8080/unicom/login';</script></body></html>";
  msg = sw::portal::explain_failed_verification(injected, "http://172.29.250.5:30004/byod/x");
  check(sw::util::icontains(msg, "10.20.30.40:8080"), "names the portal the body points at");
  check(!sw::util::icontains(msg, "captive.apple.com"),
        "never reports the probe URL as the portal");

  // Nothing to go on: claim no second stage rather than guess.
  sw::portal::Probe opaque;
  opaque.state = sw::portal::State::Captive;
  opaque.probe_url = "http://captive.apple.com/hotspot-detect.html";
  opaque.body = "<html><body>blocked</body></html>";
  msg = sw::portal::explain_failed_verification(opaque, "http://172.29.250.5:30004/byod/x");
  check(!sw::util::icontains(msg, "second authentication"),
        "an unattributable interception is not called a second stage");

  // The link died outright rather than a portal appearing.
  // (portal_complaint is exercised through the e2e, which drives a portal that
  //  re-renders its form with the message in a hidden field.)
  sw::portal::Probe gone;
  gone.state = sw::portal::State::Offline;
  msg = sw::portal::explain_failed_verification(gone, "https://w.bnbu.edu.cn/cgi-bin/srun_portal");
  check(sw::util::icontains(msg, "unreachable"), "offline afterwards is reported as such");
  check(!sw::util::icontains(msg, "second authentication"),
        "offline is not mistaken for a second stage");
}

void test_form_encoding() {
  section("util::form_encode");
  sw::util::Pairs pairs = {{"user", "20210001"}, {"pwd", "p@ss word&x"}};
  check_eq(sw::util::form_encode(pairs), "user=20210001&pwd=p%40ss%20word%26x",
           "values percent-encoded");
}

void test_config_roundtrip() {
  section("config round trip");
  sw::Config cfg;
  cfg.ssid = "CAMPUS";
  cfg.username = "20210001";
  cfg.login_method = "raw";
  cfg.login_url = "http://1.1.1.1/login?u={username}";
  cfg.post_body = "u={username}&p={password|url}";
  cfg.extra_fields["operator"] = "telecom";
  cfg.captive_interval = 7;
  cfg.probe_timeout = 3;

  std::string path = "build/test-config.ini";
  std::string err;
  check(sw::save_config(cfg, path, &err), "config saved: " + err);

  sw::Config loaded;
  check(sw::load_config(path, &loaded, &err), "config loaded: " + err);
  check_eq(loaded.ssid, "CAMPUS", "ssid round trips");
  check_eq(loaded.username, "20210001", "username round trips");
  check_eq(loaded.login_method, "raw", "login_method round trips");
  check_eq(loaded.post_body, "u={username}&p={password|url}", "post_body round trips");
  check_eq(loaded.extra_fields["operator"], "telecom", "field.* round trips");
  check(loaded.captive_interval == 7, "int value round trips");
  check(loaded.probe_timeout == 3, "probe_timeout round trips");

  sw::Config missing;
  check(sw::load_config("build/definitely-not-here.ini", &missing, &err),
        "a missing config file is not an error");
  check_eq(missing.username, "", "missing config leaves defaults");
}

} // namespace

int main() {
  test_url_resolution();
  test_form_scanning();
  test_unquoted_attributes();
  test_redirect_hints();
  test_field_identification();
  test_drcom_field_names();
  test_hint_priority();
  test_short_hints_do_not_overmatch();
  test_config_overrides();
  test_plan_failure_is_reported();
  test_url_parsing();
  test_dhcp_nameserver_parsing();
  test_resolve_failure_detection();
  test_host_pinning();
  test_srun_primitives();
  test_srun_login_payload();
  test_srun_response_parsing();
  test_srun_portal_detection();
  test_query_helpers();
  test_byod_init();
  test_byod_login_payload();
  test_netenv_parsing();
  test_chained_portal_diagnosis();
  test_form_encoding();
  test_config_roundtrip();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
