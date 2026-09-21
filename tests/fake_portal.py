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
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

USERNAME = "20210001"
PASSWORD = "s3cr3t p@ss"
CSRF = "tok-abc-123"

state = {"online": False}


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
        path = urllib.parse.urlparse(self.path).path
        host = self.headers.get("Host", "127.0.0.1")

        if path == "/probe":
            if state["online"]:
                self._send(200, "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                                "<BODY>Success</BODY></HTML>")
            else:
                self._send(302, "", headers={"Location": f"http://{host}/splash"})
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
    args = ap.parse_args()
    server = HTTPServer(("127.0.0.1", args.port), Portal)
    sys.stderr.write(f"[portal] listening on 127.0.0.1:{args.port}\n")
    server.serve_forever()


if __name__ == "__main__":
    main()
