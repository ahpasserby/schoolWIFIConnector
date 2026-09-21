#!/usr/bin/env bash
# End-to-end test: drives the real binary against tests/fake_portal.py.
#
# Covers the path that unit tests cannot: probe -> captive detection ->
# redirect hop -> meta refresh -> form discovery -> POST -> verification.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/schoolwifi"
PORT="${PORT:-8111}"
WORK="$(mktemp -d)"
CONFIG="$WORK/config.ini"

pass=0
fail=0

cleanup() {
  [[ -n "${PORTAL_PID:-}" ]] && kill "$PORTAL_PID" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { echo "  PASS  $1"; pass=$((pass + 1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail + 1)); }

[[ -x "$BIN" ]] || { echo "build first: make"; exit 1; }

python3 "$ROOT/tests/fake_portal.py" --port "$PORT" 2>"$WORK/portal.log" &
PORTAL_PID=$!

for _ in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/login" && break
  sleep 0.1
done

cat > "$CONFIG" <<INI
[network]
ssid =
interface =

[account]
username = 20210001

[portal]
probe_urls = http://127.0.0.1:$PORT/probe
logout_url = http://127.0.0.1:$PORT/logout
failure_contains = ERROR:

[watch]
online_interval = 1
captive_interval = 1
INI

export SCHOOLWIFI_PASSWORD='s3cr3t p@ss'

echo "e2e: captive portal login"

# 1. Starts out captive.
out="$("$BIN" --config "$CONFIG" status 2>&1)"
grep -q "Portal       captive" <<<"$out" && ok "detects the captive state" \
  || bad "detects the captive state -- got: $(grep Portal <<<"$out")"

# 2. diagnose walks the hops and finds the form.
out="$(cd "$WORK" && "$BIN" --config "$CONFIG" diagnose 2>&1)"
grep -q "meta refresh" <<<"$out" && ok "follows the meta-refresh hop" || bad "follows the meta-refresh hop"
grep -q "username_field = userName" <<<"$out" && ok "identifies userName" || bad "identifies userName"
grep -q "password_field = userPwd" <<<"$out" && ok "identifies userPwd" || bad "identifies userPwd"
grep -q "csrfToken" <<<"$out" && ok "sees the hidden CSRF token" || bad "sees the hidden CSRF token"
grep -q "\*\*\*\*\*\*\*\*" <<<"$out" && ok "masks the password in diagnostics" || bad "masks the password in diagnostics"
grep -q "<password>" <<<"$out" && bad "leaks a password placeholder into the plan" || ok "no password value in the plan"

# 3. login actually authenticates.
if "$BIN" --config "$CONFIG" login >"$WORK/login.log" 2>&1; then
  ok "login succeeds"
else
  bad "login succeeds -- $(cat "$WORK/login.log")"
fi
grep -q "bad or missing token\|missing domain\|missing hidden ip\|bad credentials" "$WORK/portal.log" \
  && bad "portal rejected the submission" || ok "hidden fields and select survived the round trip"

# 4. now reports online.
out="$("$BIN" --config "$CONFIG" status 2>&1)"
grep -q "Portal       online" <<<"$out" && ok "reports online after login" || bad "reports online after login"

# 5. login is idempotent.
out="$("$BIN" --config "$CONFIG" login 2>&1)"
grep -q "already online" <<<"$out" && ok "second login is a no-op" || bad "second login is a no-op"

# 6. logout flips it back.
"$BIN" --config "$CONFIG" logout >/dev/null 2>&1
out="$("$BIN" --config "$CONFIG" status 2>&1)"
grep -q "Portal       captive" <<<"$out" && ok "logout returns to captive" || bad "logout returns to captive"

# 7. a wrong password must fail rather than report success.
SCHOOLWIFI_PASSWORD='wrong-password' "$BIN" --config "$CONFIG" login >"$WORK/bad.log" 2>&1
if [[ $? -ne 0 ]] && grep -q "failure\|failed" "$WORK/bad.log"; then
  ok "wrong password is reported as a failure"
else
  bad "wrong password is reported as a failure -- $(cat "$WORK/bad.log")"
fi

kill "$PORTAL_PID" 2>/dev/null
wait "$PORTAL_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# Srun (深澜) portal: no HTML form at all. The server validates the challenge
# signature, so these checks only pass if the whole crypto chain is right.
# ---------------------------------------------------------------------------
echo ""
echo "e2e: srun portal login"

SRUN_PORT=$((PORT + 1))
python3 "$ROOT/tests/fake_portal.py" --port "$SRUN_PORT" --mode srun 2>"$WORK/srun.log" &
PORTAL_PID=$!
for _ in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$SRUN_PORT/srun_portal_pc" && break
  sleep 0.1
done

SRUN_CONFIG="$WORK/srun.ini"
cat > "$SRUN_CONFIG" <<INI
[network]
ssid =
interface =

[account]
username = 20210001

[portal]
probe_urls = http://127.0.0.1:$SRUN_PORT/probe

[watch]
online_interval = 1
captive_interval = 1
INI

rm -rf "$WORK"/schoolwifi-diagnose-*
out="$(cd "$WORK" && "$BIN" --config "$SRUN_CONFIG" diagnose 2>&1)"
grep -q "javascript redirect" <<<"$out" && ok "follows the injected JS redirect" || bad "follows the injected JS redirect"
grep -q "srun_portal_pc" <<<"$out" && ok "reaches the srun SPA" || bad "reaches the srun SPA"
grep -q "(none found)" <<<"$out" && ok "correctly finds no HTML form" || bad "correctly finds no HTML form"
grep -q "has no HTML form at all; that is expected" <<<"$out" \
  && ok "diagnose says srun needs no form rather than reporting a failure" \
  || bad "diagnose says srun needs no form rather than reporting a failure"

# A portal with no form keeps its logic in JS, so diagnose must save that JS.
dump=$(ls -dt "$WORK"/schoolwifi-diagnose-* 2>/dev/null | head -1)
if [[ -n "$dump" && -f "$dump/portal.html" ]]; then
  ok "diagnose saved the portal page"
else
  bad "diagnose saved the portal page"
fi
if [[ -n "$dump" ]] && ls "$dump"/*portal-logic.js >/dev/null 2>&1; then
  ok "diagnose fetched the portal's same-origin script"
else
  bad "diagnose fetched the portal's same-origin script"
fi
grep -q "not same-origin" <<<"$out" && ok "skips third-party scripts" || bad "skips third-party scripts"

if "$BIN" --config "$SRUN_CONFIG" login >"$WORK/srun-login.log" 2>&1; then
  ok "srun login succeeds"
else
  bad "srun login succeeds -- $(cat "$WORK/srun-login.log")"
fi
grep -q "detected a Srun portal" "$WORK/srun-login.log" && ok "srun portal auto-detected" \
  || bad "srun portal auto-detected"
grep -q "REJECT" "$WORK/srun.log" \
  && bad "server rejected a derived parameter: $(grep -o 'REJECT.*' "$WORK/srun.log" | head -1)" \
  || ok "server accepted chksum, info blob and hmd5"

out="$("$BIN" --config "$SRUN_CONFIG" status 2>&1)"
grep -q "Portal       online" <<<"$out" && ok "online after srun login" || bad "online after srun login"

curl -s -o /dev/null "http://127.0.0.1:$SRUN_PORT/cgi-bin/srun_portal?action=logout"
SCHOOLWIFI_PASSWORD='wrong-password' "$BIN" --config "$SRUN_CONFIG" login >"$WORK/srun-bad.log" 2>&1
if grep -q "password is incorrect" "$WORK/srun-bad.log" \
   && grep -q "REJECT bad-password" "$WORK/srun.log"; then
  ok "wrong password surfaces the portal's own error"
else
  bad "wrong password surfaces the portal's own error -- $(cat "$WORK/srun-bad.log")"
fi

kill "$PORTAL_PID" 2>/dev/null
wait "$PORTAL_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# Huawei-style BYOD: the page the gateway redirects to is an empty shell whose
# next hop is only obtainable by calling the API its JavaScript would call.
# ---------------------------------------------------------------------------
echo ""
echo "e2e: byod portal login"

BYOD_PORT=$((PORT + 2))
python3 "$ROOT/tests/fake_portal.py" --port "$BYOD_PORT" --mode byod 2>"$WORK/byod.log" &
PORTAL_PID=$!
for _ in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$BYOD_PORT/byod/index.html" && break
  sleep 0.1
done

BYOD_CONFIG="$WORK/byod.ini"
cat > "$BYOD_CONFIG" <<INI
[network]
ssid =
interface =

[account]
username = 20210001

[portal]
probe_urls = http://127.0.0.1:$BYOD_PORT/probe
failure_contains = ERROR:
service_suffix_id = 9
INI


if SCHOOLWIFI_PASSWORD='s3cr3t p@ss' "$BIN" --config "$BYOD_CONFIG" login >"$WORK/byod-login.log" 2>&1; then
  ok "byod login succeeds"
else
  bad "byod login succeeds -- $(cat "$WORK/byod-login.log")"
fi
grep -q "asking .* where the login page is" "$WORK/byod-login.log" \
  && ok "calls the byod init API instead of giving up on the empty shell" \
  || bad "calls the byod init API instead of giving up on the empty shell"
grep -q "portal hop (byod init)" "$WORK/byod-login.log" \
  && ok "follows the url the init API named" || bad "follows the url the init API named"
grep -q "nasRedirectUrl=" "$WORK/byod-login.log" \
  && ok "appends nasRedirectUrl the way index.js does" \
  || bad "appends nasRedirectUrl the way index.js does"
grep -q "No host part in the URL" "$WORK/byod-login.log" \
  && bad "relative init url was not resolved against the portal" \
  || ok "resolves the relative init url despite the URL inside its query"
grep -q "REJECT byod-init" "$WORK/byod.log" \
  && bad "gateway parameters were not forwarded to init" \
  || ok "forwards the gateway parameters to init"
grep -q "fetching login policy" "$WORK/byod-login.log" \
  && ok "fetches the login policy before submitting" \
  || bad "fetches the login policy before submitting"
grep -q "REJECT byod-" "$WORK/byod.log" \
  && bad "controller rejected a field: $(grep -o 'REJECT byod-[a-z]*' "$WORK/byod.log" | head -1)" \
  || ok "base64 password and echoed policy fields all accepted"

curl -s -o /dev/null "http://127.0.0.1:$BYOD_PORT/logout" 2>/dev/null
SCHOOLWIFI_PASSWORD='wrong-password' "$BIN" --config "$BYOD_CONFIG" login >"$WORK/byod-bad.log" 2>&1
if grep -q "password is incorrect" "$WORK/byod-bad.log" \
   && grep -q "REJECT byod-password" "$WORK/byod.log"; then
  ok "a wrong password surfaces the controller's own error code"
else
  bad "a wrong password surfaces the controller's own error code"
fi
# The portal's default service is the wrong one for this account, which is what
# E63018 reports. Without naming the alternatives that error is a dead end.
cat > "$WORK/byod-default-service.ini" <<INI
[network]
interface =
[account]
username = 20210001
[portal]
probe_urls = http://127.0.0.1:$BYOD_PORT/probe
INI
SCHOOLWIFI_PASSWORD='s3cr3t p@ss' "$BIN" --config "$WORK/byod-default-service.ini" \
  login >"$WORK/byod-service.log" 2>&1
grep -q "portal offers services: 7=" "$WORK/byod-service.log" \
  && ok "lists the services the portal offers" || bad "lists the services the portal offers"
grep -q "set service_suffix_id" "$WORK/byod-service.log" \
  && ok "E63018 says which config key to reach for" \
  || bad "E63018 says which config key to reach for"

# The captured init response must reach the diagnose dump: on a network nobody
# can reach twice, it is the only record of what the portal actually answered.
curl -s -o /dev/null "http://127.0.0.1:$BYOD_PORT/logout" 2>/dev/null
rm -rf "$WORK"/schoolwifi-diagnose-*
(cd "$WORK" && "$BIN" --config "$BYOD_CONFIG" diagnose) >"$WORK/byod-diag.log" 2>&1
dump=$(ls -dt "$WORK"/schoolwifi-diagnose-* 2>/dev/null | head -1)
if [[ -n "$dump" && -f "$dump/byod-init.json" ]] && grep -q '"url"' "$dump/byod-init.json"; then
  ok "diagnose captured the byod init response"
  grep -q "JSON, not the form" "$WORK/byod-diag.log" \
    && ok "diagnose reports the API path, not a form-planning failure" \
    || bad "diagnose reports the API path, not a form-planning failure"
else
  bad "diagnose captured the byod init response"
fi

kill "$PORTAL_PID" 2>/dev/null
wait "$PORTAL_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# The page the byod init call points at lives under /byod/ as well. Asking init
# about that page returns the same page, so the tool used to loop, re-appending
# the query on every pass until the URL was tens of kilobytes long.
# ---------------------------------------------------------------------------
echo ""
echo "e2e: byod must not loop on the page init points at"

LOOP_PORT=$((PORT + 3))
python3 "$ROOT/tests/fake_portal.py" --port "$LOOP_PORT" --mode byod-loop 2>"$WORK/loop.log" &
PORTAL_PID=$!
for _ in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$LOOP_PORT/byod/index.html" && break
  sleep 0.1
done

cat > "$WORK/loop.ini" <<INI
[network]
interface =
[account]
username = 20210001
[portal]
probe_urls = http://127.0.0.1:$LOOP_PORT/probe
INI

SCHOOLWIFI_PASSWORD='s3cr3t p@ss' "$BIN" --config "$WORK/loop.ini" login >"$WORK/loop-login.log" 2>&1
init_calls=$(grep -c "byodrs/init" "$WORK/loop.log")
if [[ "$init_calls" -eq 1 ]]; then
  ok "byod init is called once, not once per hop"
else
  bad "byod init is called once, not once per hop (called $init_calls times)"
fi
grep -q "customId=19&customId=19" "$WORK/loop-login.log" \
  && bad "the query string is being re-appended on every pass" \
  || ok "the query string is not re-appended"
grep -q "no <form> and no <input> fields" "$WORK/loop-login.log" \
  && ok "fails with the real reason instead of exhausting the hop budget" \
  || bad "fails with the real reason instead of exhausting the hop budget"

kill "$PORTAL_PID" 2>/dev/null
wait "$PORTAL_PID" 2>/dev/null

kill "$PORTAL_PID" 2>/dev/null
wait "$PORTAL_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# Networks that authenticate in stages: satisfying the first portal reveals a
# second one, on another host, wanting a different account.
# ---------------------------------------------------------------------------
echo ""
echo "e2e: chained two-stage login"

TS_PORT=$((PORT + 4))
python3 "$ROOT/tests/fake_portal.py" --port "$TS_PORT" --mode two-stage 2>"$WORK/ts.log" &
PORTAL_PID=$!
for _ in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$TS_PORT/byod/index.html" && break
  sleep 0.1
done

cat > "$WORK/ts2.ini" <<INI
[network]
interface =
[account]
username = isp-user
password = isp-pass
[portal]
probe_urls = http://127.0.0.1:$TS_PORT/probe
failure_contains = ERROR:
INI
cat > "$WORK/ts1.ini" <<INI
[network]
interface =
[account]
username = 20210001
[portal]
probe_urls = http://127.0.0.1:$TS_PORT/probe
service_suffix_id = 9
next_stage = $WORK/ts2.ini
INI

if SCHOOLWIFI_PASSWORD='s3cr3t p@ss' "$BIN" --config "$WORK/ts1.ini" login >"$WORK/ts-login.log" 2>&1; then
  ok "a two-stage network logs in end to end"
else
  bad "a two-stage network logs in end to end -- $(tail -2 "$WORK/ts-login.log")"
fi
grep -q "stage 1 done" "$WORK/ts-login.log" \
  && ok "reports the first stage as done rather than failed" \
  || bad "reports the first stage as done rather than failed"
grep -q "as isp-user" "$WORK/ts-login.log" \
  && ok "the second stage uses its own account" || bad "the second stage uses its own account"
grep -q "form the page submits itself" "$WORK/ts-login.log" \
  && ok "follows the form the ISP page submits on load" \
  || bad "follows the form the ISP page submits on load"
grep -qE "REJECT stage2-(baspushurl|testmacauth)" "$WORK/ts.log" \
  && bad "the auto-submitted form lost a hidden field" \
  || ok "stamps its own URL in and keeps the other hidden fields"

grep -q "REJECT stage2-credentials" "$WORK/ts.log" \
  && bad "SCHOOLWIFI_PASSWORD leaked into the second stage" \
  || ok "SCHOOLWIFI_PASSWORD does not leak into the second stage"
# A wrong password here is reported by re-rendering the form with the complaint
# in a hidden field, which is the only place the real reason appears.
curl -s -o /dev/null "http://127.0.0.1:$TS_PORT/logout" 2>/dev/null
cat > "$WORK/ts2-bad.ini" <<INI
[network]
interface =
[account]
username = isp-user
password = wrong-pass
[portal]
probe_urls = http://127.0.0.1:$TS_PORT/probe
INI
"$BIN" --config "$WORK/ts2-bad.ini" login >"$WORK/ts-bad.log" 2>&1
grep -q "The portal said:" "$WORK/ts-bad.log" \
  && ok "surfaces the portal's own complaint" || bad "surfaces the portal's own complaint"
grep -q "账号或密码错误" "$WORK/ts-bad.log" \
  && ok "and quotes it verbatim" || bad "and quotes it verbatim"

kill "$PORTAL_PID" 2>/dev/null
wait "$PORTAL_PID" 2>/dev/null

# ---------------------------------------------------------------------------
# A probe host the system resolver cannot resolve must still reach the DNS
# fallback and then the gateway fallback. Before those were wired into probe(),
# neither was attempted and the command simply reported "offline".
# ---------------------------------------------------------------------------
echo ""
echo "e2e: unresolvable probe host falls back"

cat > "$WORK/unresolvable.ini" <<INI
[network]
interface =
[account]
username = 20210001
[portal]
probe_urls = http://schoolwifi-e2e-nonexistent.invalid/check
probe_timeout = 3
INI

out="$("$BIN" --config "$WORK/unresolvable.ini" -v status 2>&1)"
grep -qE "DHCP nameserver|system DNS could not resolve" <<<"$out" \
  && ok "asks this network's DNS when the system resolver fails" \
  || bad "asks this network's DNS when the system resolver fails"
grep -q "trying the gateway" <<<"$out" \
  && ok "falls back to the gateway when nothing answers" \
  || bad "falls back to the gateway when nothing answers"
grep -q "Portal       offline" <<<"$out" \
  && ok "still reports offline once every fallback is exhausted" \
  || bad "still reports offline once every fallback is exhausted"

echo ""
echo "$((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]]
