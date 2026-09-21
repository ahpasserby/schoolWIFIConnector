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
rm -f "$ROOT"/schoolwifi-portal-*.html

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

echo ""
echo "$((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]]
