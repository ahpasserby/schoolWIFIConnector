# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A macOS CLI (`schoolwifi`) that authenticates against campus Wi-Fi captive
portals. It exists because macOS's Captive Network Assistant frequently fails
to appear, leaving the machine associated but unable to reach the internet.
Written in C++17 with an Objective-C++ bridge, distributed as a single binary.

## Commands

```bash
make              # build -> build/schoolwifi
make test         # unit tests (tests/test_main.cpp), no network needed
make e2e          # end-to-end against tests/fake_portal.py, no network needed
make check        # test + e2e
make install      # -> $(PREFIX)/bin, PREFIX defaults to /usr/local
make clean
```

`CXXFLAGS` carries `-MMD -MP` and the Makefile `-include`s the generated `.d`
files. Do not remove this: without header dependency tracking, editing a struct
in `include/sw/` leaves stale objects linked against the old layout, and the
resulting ABI mismatch surfaces as unrelated nonsense (a field reading empty,
for instance) rather than as a build error.

There is **no CMake and no package manager** — this is deliberate. Everything
linked (`libcurl`, CoreWLAN, Security, Foundation) ships with macOS and the
Command Line Tools, so `git clone && make` works on a bare machine. Do not
introduce a third-party dependency without a strong reason.

Running a single unit test: the suite is a plain `main()` calling `test_*()`
functions in sequence. To isolate one, comment out the others in
`tests/test_main.cpp:main` — there is no filter flag. The e2e script takes
`PORT=nnnn ./tests/e2e.sh` if 8111 is busy.

Manual smoke test against the fake portal:
```bash
python3 tests/fake_portal.py --port 8111 &
SCHOOLWIFI_PASSWORD='s3cr3t p@ss' ./build/schoolwifi -c /tmp/c.ini -v login
```

## Architecture

Strict layering, `src/` mirrors `include/sw/`. Lower layers know nothing about
portals:

```
main.cpp        subcommand dispatch, password resolution, LaunchAgent plist
  portal.cpp    THE STATE MACHINE: probe -> resolve page -> plan -> submit -> verify
    srun.cpp    Srun (深澜) portals: challenge/response login, no HTML form
    byod.cpp    Huawei BYOD shells: the login URL comes from an API, not a link
    html.cpp    forgiving <form>/<input> scanner + redirect-hint extraction
    netenv.cpp  proxy / VPN-tunnel detection for diagnostics
    http.cpp    libcurl session (cookies persist across requests in a Client)
    config.cpp  INI parsing; every field has a working default
    wifi.mm     CoreWLAN bridge (the only Objective-C++ file)
    keychain.cpp  Security.framework generic passwords
    util.cpp / log.cpp
```

`portal.cpp` is where the real logic lives; read it first. The login flow is
four separable steps, each independently testable:

1. `probe()` — GETs a connectivity-check URL **with redirects disabled**. This
   is essential: the injected `Location` header *is* the portal's address. A
   302 means captive; a 200 whose body isn't the expected payload means a
   transparent proxy swapped the page, which is also captive.
2. `resolve_login_page()` — walks up to `kMaxHops` from the intercept point,
   following `<meta refresh>`, then JS (`location.href=`, `top.self.location`),
   then `<iframe src>`, stopping at the first page containing a `type=password`
   input. Real portals take 2–3 hops. Loop-guarded.
3. `plan_form_login()` — pure function (no I/O), which is why it carries most
   of the test coverage. Builds the submission from the form's *own* fields so
   hidden CSRF/session/IP values survive; config `username_field`/
   `password_field` override detection; `field.*` entries override or append.
4. `judge()` — decides success. Prefers `success_contains`/`failure_contains`
   when configured, otherwise **re-probes** rather than trusting the portal's
   own "login successful" page.

`login_method = raw` bypasses steps 2–3 entirely and replays a templated
request, for API-style portals that have no HTML form.

## Platform constraints that are not obvious

These were each discovered against a real macOS 26 machine; don't "simplify"
them away.

- **CoreWLAN's `-ssid` returns nil** on macOS 14+ without a Location Services
  grant, which a plain CLI cannot hold. `wifi.mm` therefore takes the interface
  name and power state from CoreWLAN but parses SSID/BSSID out of
  `ipconfig getsummary <iface>`. An empty SSID must always degrade gracefully —
  it disables the `ssid` guard, it never blocks a login.
- **The proxy environment must be bypassed** (`CURLOPT_PROXY ""` +
  `NOPROXY "*"` in `http.cpp`). With `http_proxy` set, probes either fail
  outright or — far worse — make a captive network look online.
- **TLS verification is off by default.** Campus gateways almost universally
  present self-signed certificates; refusing them makes the tool useless. The
  only secret sent is the credential the browser would post to the same host.
- **`curl_easy_reset()` clears the cookie engine**, so `Client::send` re-sets
  `CURLOPT_COOKIEFILE ""` after every reset. Portals set a session cookie on
  the login page and require it on the POST — losing it breaks login silently.
- **A campus portal's hostname often resolves only in the campus DNS**, and a
  user who pinned a public resolver in System Settings keeps that setting on
  every network — so the name simply does not exist to the system resolver,
  while public names still resolve fine. `portal.cpp`'s `fetch()` therefore
  retries any resolve failure by querying the DHCP-offered nameserver directly
  (`dns.cpp`, libresolv with an explicit `nsaddr_list`) and pinning the answer
  via `CURLOPT_RESOLVE`. That option, like the cookie engine, is dropped by
  `curl_easy_reset()` and must be re-applied per request.
- **Every portal-facing request, `probe()` included, goes through `fetch()`**
  so that fallback applies. A network can block the pinned public resolver
  outright, in which case *nothing* resolves and even the connectivity-check
  hostnames fail — the tool then has to resolve them the way every other
  device on that network does. Note the two distinct libcurl strings:
  "Resolving timed out" is a DNS failure, "Connection timed out" is not, and
  `dns::is_resolve_failure` must keep telling them apart.
- **A network that drops traffic is not the same as one that intercepts it.**
  Dorm and campus networks sometimes blackhole the connectivity-check hosts
  entirely, so `probe()` sees only timeouts and would report Offline with no
  portal to find. After every check URL fails at the transport level it tries
  `http://<default gateway>/`, and escalates to Captive only when that page
  actually looks like a portal (password field or redirect hint) — a plain
  router admin page must not send `login` off to submit credentials to it.
- Each failed probe costs a full `probe_timeout`, so those failures log at
  **info**, not debug. A command that prints nothing for 15 seconds reads as a
  hang, and that is exactly how it was first reported.
- Some networks chain two portals (campus, then an ISP). `judge()` therefore
  compares the authority (`host`, plus `:port` when non-default) of the portal
  it submitted to against the one intercepting afterwards, and says so when
  they differ — otherwise a successful first-stage login is reported as a
  generic failure and looks like a wrong password. Chaining them automatically
  is not implemented; the documented workaround is a second config file.
- A 200 response does not mean online. Portals frequently intercept without
  redirecting, answering 200 with a splash page — this is what BNBU does. For
  probe URLs with a known success payload the marker settles it; for any other
  URL, `probe()` treats a redirect hint in the body as proof of substitution,
  because a genuine connectivity check never carries one.
- Only `wifi.mm` compiles as Objective-C++ (`-fobjc-arc`, set per-suffix rule
  in the Makefile). Keep ObjC out of the `.cpp` files.

## Conventions

- Field-name heuristics in `portal.cpp` (`kUsernameHints`, `kPasswordHints`)
  are consulted **in hint priority order, not field order**, and only hints of
  4+ characters are allowed to substring-match. Short hints like `id` and `pw`
  would otherwise claim fields named `validcode` or `pwdTip`. There are
  regression tests for exactly this; keep them passing when editing the lists.
- When detection is ambiguous, `plan_form_login` returns `ok = false` with a
  `reason` instead of guessing. Submitting a password into the wrong input is
  worse than failing with a message pointing at `schoolwifi diagnose`.
- Passwords are masked (`kMaskedPassword`) before being recorded in
  `LoginResult::sent_fields`, because `diagnose` and failure paths print them.
  Anything new that echoes submitted fields must preserve this.
- Unknown config keys are ignored rather than rejected — a daemon should keep
  running across config-format changes.
- `watch` is the only command that writes to the log file by default, and it
  polls with 1-second wakeups so launchd's SIGTERM is honoured promptly.
- User-facing docs (README, `docs/`, `config/config.example.ini`) and the
  `setup` wizard's prompts are in Chinese — that is who runs them. Code,
  comments, commit messages and diagnostic output (`status`, `login`, log
  lines) stay in English.
- `setup` is the only place a user types values they have never seen named
  before, so each prompt names the config key, says what the field is, and
  states what Enter alone does. `interface` is validated against
  `wifi::interfaces()`: a bad value there fails much later with an error that
  looks nothing like the typo that caused it.

Portals whose page carries no form and no redirect hint keep their logic in
JavaScript, which this tool cannot execute. `diagnose` therefore saves
`schoolwifi-diagnose-<ts>/` containing the page plus every **same-origin**
`<script src>`, numbered in load order — third-party hosts are skipped so the
dump stays the portal's own code and no request leaks elsewhere. That folder is
the starting point for adding support for a new portal family, the way
`srun.cpp` came about.

## Testing without a campus network

`tests/fake_portal.py` reproduces the full gateway behaviour — 302 → splash →
meta-refresh → form with a CSRF token — and **rejects submissions that drop the
hidden fields**, so the e2e test actually catches form-handling regressions.
This is the only way to exercise the login path off-campus; use it rather than
adding mocks inside the C++.
