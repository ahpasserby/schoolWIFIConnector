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
out="$("$BIN" --config "$CONFIG" diagnose 2>&1)"
grep -q "meta refresh" <<<"$out" && ok "follows the meta-refresh hop" || bad "follows the meta-refresh hop"
grep -q "username_field = userName" <<<"$out" && ok "identifies userName" || bad "identifies userName"
grep -q "password_field = userPwd" <<<"$out" && ok "identifies userPwd" || bad "identifies userPwd"
grep -q "csrfToken" <<<"$out" && ok "sees the hidden CSRF token" || bad "sees the hidden CSRF token"
grep -q "\*\*\*\*\*\*\*\*" <<<"$out" && ok "masks the password in diagnostics" || bad "masks the password in diagnostics"
grep -q "<password>" <<<"$out" && bad "leaks a password placeholder into the plan" || ok "no password value in the plan"
rm -rf "$ROOT"/schoolwifi-diagnose-*

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

out="$("$BIN" --config "$SRUN_CONFIG" diagnose 2>&1)"
grep -q "javascript redirect" <<<"$out" && ok "follows the injected JS redirect" || bad "follows the injected JS redirect"
grep -q "srun_portal_pc" <<<"$out" && ok "reaches the srun SPA" || bad "reaches the srun SPA"
grep -q "(none found)" <<<"$out" && ok "correctly finds no HTML form" || bad "correctly finds no HTML form"

# A portal with no form keeps its logic in JS, so diagnose must save that JS.
dump=$(ls -d "$ROOT"/schoolwifi-diagnose-* 2>/dev/null | head -1)
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
rm -rf "$ROOT"/schoolwifi-diagnose-*

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
