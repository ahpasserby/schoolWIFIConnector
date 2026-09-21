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

#include "sw/config.hpp"
#include "sw/html.hpp"
#include "sw/portal.hpp"
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
  test_form_encoding();
  test_config_roundtrip();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
