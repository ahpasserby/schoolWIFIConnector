#!/usr/bin/env python3
"""A captive portal that behaves like the real thing, for end-to-end testing.

Flow, matching what a campus gateway actually does:

    GET /probe   -> 302 to /splash     (while unauthenticated)
    GET /splash  -> meta-refresh to /login
    GET /login   -> form with a hidden CSRF token
    POST /auth   -> validates credentials AND the token, flips state
    GET /probe   -> 200 "Success"      (once authenticated)

Run standalone:  python3 tests/fake_portal.py --port 8111
"""

import argparse
import hashlib
import hmac
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from srun_reference import build_info_param_for_test, srun_hmd5, srun_chksum

USERNAME = "20210001"
PASSWORD = "s3cr3t p@ss"
CSRF = "tok-abc-123"
ACID = "1"
CLIENT_IP = "10.253.51.53"

state = {"online": False, "token": None, "mode": "form"}

# The SPA a Srun portal actually serves: inputs carry only `id`, there is no
# <form>, and the parameters the client must compute come from CONFIG.
SRUN_PAGE = """<!DOCTYPE html><html><head>
<meta name="keywords" content="Srunsoft">
<title>Srunsoft</title>
</head><body>
<div id="app" class="main">
  <div class="panel-row"><input type="text" id="username" class="input-box"></div>
  <div class="panel-row"><input type="password" id="password" class="input-box"></div>
  <button type="button" class="btn-login" id="login-account">Login</button>
</div>
<script>
    var CONFIG = {
        page   : 'account',
        acid   : "%s",
        ip     : "%s",
        nas    : "",
        isIPV6 :  false ,
        portal : {"AuthIP":"","ServiceIP":"https://218.75.75.93:8800","MacAuth":true}
    };
</script>
</body></html>"""


class Portal(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[portal] " + (fmt % args) + "\n")

    def _send(self, code, body, ctype="text/html; charset=utf-8", headers=None):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = {k: v[0] for k, v in urllib.parse.parse_qs(parsed.query).items()}
        host = self.headers.get("Host", "127.0.0.1")

        if path == "/probe":
            if state["online"]:
                self._send(200, "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                                "<BODY>Success</BODY></HTML>")
            elif state["mode"] == "srun":
                # Mirrors the real BNBU capture: HTTP 200 with an injected body
                # carrying a JS redirect, rather than a clean 302.
                self._send(200, "<html><body><script>top.self.location.href="
                                f"'http://{host}/index_1.html';</script></body></html>")
            else:
                self._send(302, "", headers={"Location": f"http://{host}/splash"})
            return

        if path == "/index_1.html":
            self._send(200, '<html><head><meta http-equiv="refresh" '
                            'content="0;url=/srun_portal_pc?ac_id=1&theme=pro">'
                            '</head><body>redirecting</body></html>')
            return

        if path == "/srun_portal_pc":
            self._send(200, SRUN_PAGE % (ACID, CLIENT_IP))
            return

        if path == "/cgi-bin/get_challenge":
            token = "abcdef0123456789abcdef0123456789"
            state["token"] = token
            cb = query.get("callback", "jQuery")
            body = ('{"challenge":"%s","client_ip":"%s","online_ip":"%s",'
                    '"ecode":0,"error":"ok","res":"ok"}' % (token, CLIENT_IP, CLIENT_IP))
            self._send(200, f"{cb}({body})", ctype="application/json")
            return

        if path == "/cgi-bin/srun_portal":
            self._srun_portal(query)
            return

        if path == "/splash":
            # No form here at all -- the connector has to follow the hop.
            self._send(200, '<html><head>'
                            '<meta http-equiv="refresh" content="0;url=/login">'
                            '</head><body>Redirecting...</body></html>')
            return

        if path == "/login":
            self._send(200, f'''<html><head><title>Campus Network</title></head><body>
              <form id="loginForm" method="POST" action="/auth">
                <input type="hidden" name="csrfToken" value="{CSRF}">
                <input type="hidden" name="ip" value="10.1.2.3">
                <input type="text" name="userName" value="">
                <input type="password" name="userPwd">
                <select name="domain">
                  <option value="edu">edu</option>
                  <option value="cmcc" selected>cmcc</option>
                </select>
                <input type="submit" value="Login">
              </form></body></html>''')
            return

        if path == "/logout":
            state["online"] = False
            self._send(200, "<html><body>logged out</body></html>")
            return

        self._send(404, "not found")

    def _srun_portal(self, query):
        """Validates a login the way a real Srun gateway does: by recomputing
        every derived parameter and comparing."""
        cb = query.get("callback", "jQuery")

        def reply(obj, rejected=None):
            if rejected:
                self.log_message("REJECT %s", rejected)
            self._send(200, f"{cb}({obj})", ctype="application/json")

        if query.get("action") == "logout":
            state["online"] = False
            reply('{"error":"ok","suc_msg":"logout_ok"}')
            return

        token = state.get("token")
        if not token:
            reply('{"error":"login_error","error_msg":"no challenge was issued"}', rejected="no-challenge")
            return

        username = query.get("username", "")
        ip = query.get("ip", "")
        acid = query.get("ac_id", "")
        info = query.get("info", "")
        chksum = query.get("chksum", "")
        n = query.get("n", "")
        type_ = query.get("type", "")

        if username != USERNAME:
            reply('{"error":"login_error","error_msg":"E2553: user not found"}', rejected="unknown-user")
            return

        expected_hmd5 = srun_hmd5(PASSWORD, token)
        if query.get("password", "") != "{MD5}" + expected_hmd5:
            reply('{"error":"login_error","error_msg":"E2531: password is incorrect"}', rejected="bad-password")
            return

        # Re-encrypt the info blob from what the server already knows; any
        # difference means the client got the encryption wrong.
        expected_info = build_info_param_for_test(username, PASSWORD, ip, acid, token)
        if info != expected_info:
            reply('{"error":"login_error","error_msg":"E0001: info parameter mismatch"}', rejected="info-mismatch")
            return

        expected_chksum = srun_chksum(token, username, expected_hmd5, acid, ip, n, type_, info)
        if chksum != expected_chksum:
            reply('{"error":"login_error","error_msg":"E0002: chksum mismatch"}', rejected="chksum-mismatch")
            return

        state["online"] = True
        reply('{"error":"ok","suc_msg":"login_ok","username":"%s","online_ip":"%s"}'
              % (username, ip))

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        fields = urllib.parse.parse_qs(self.rfile.read(length).decode("utf-8"))
        flat = {k: v[0] for k, v in fields.items()}

        if path != "/auth":
            self._send(404, "not found")
            return

        # A real portal rejects a submission that dropped the hidden token or
        # the select, which is exactly what this test is here to catch.
        if flat.get("csrfToken") != CSRF:
            self._send(200, "<html><body>ERROR: bad or missing token</body></html>")
            return
        if flat.get("domain") != "cmcc":
            self._send(200, "<html><body>ERROR: missing domain</body></html>")
            return
        if flat.get("ip") != "10.1.2.3":
            self._send(200, "<html><body>ERROR: missing hidden ip</body></html>")
            return
        if flat.get("userName") != USERNAME or flat.get("userPwd") != PASSWORD:
            self._send(200, "<html><body>ERROR: bad credentials</body></html>")
            return

        state["online"] = True
        self._send(200, "<html><body>Login succeeded</body></html>")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8111)
    ap.add_argument("--mode", choices=["form", "srun"], default="form",
                    help="form = classic HTML form portal; srun = Srun/深澜 SPA portal")
    args = ap.parse_args()
    state["mode"] = args.mode
    server = HTTPServer(("127.0.0.1", args.port), Portal)
    sys.stderr.write(f"[portal] listening on 127.0.0.1:{args.port} (mode={args.mode})\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
