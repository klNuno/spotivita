#!/usr/bin/env python3
"""Write a Spotify login into the Vita3K ux0 so Spotivita starts logged in.

Zeroconf needs a phone on the same LAN as the Vita. This does the login on
the PC instead:

  1. Prints a Spotify authorize URL (PKCE, same client as librespot) and
     waits for its redirect on http://127.0.0.1:5588/login. Open the URL in
     a browser on this PC. From another device the redirect lands on a dead
     127.0.0.1 page: copy that page's address into redirect.txt next to this
     script instead.
  2. Trades the code for an access token, hands it to librespot, which logs
     in to the AP and caches reusable stored credentials.
  3. Converts librespot's credentials.json into Spotivita's authBlob.json
     (authType 1, the type a Zeroconf login stores).

No secret is printed. Needs librespot (cargo install librespot) and curl.

  python tools/vita3k/login.py [--out <authBlob.json>]

Env: VITA3K_ROOT (same default as vita3k.ps1), LIBRESPOT (librespot binary).
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse

CLIENT_ID = "65b708073fc0480ea92a077233ca87bd"
REDIRECT = "http://127.0.0.1:5588/login"
SCOPES = ("streaming user-read-private user-read-email user-read-playback-state "
          "user-modify-playback-state playlist-read-private playlist-read-collaborative "
          "user-library-read")
HERE = os.path.dirname(os.path.abspath(__file__))


def default_out():
    root = os.environ.get("VITA3K_ROOT") or os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), ".vita3k")
    return os.path.join(root, "bin", "portable", "fs", "ux0", "data", "cspot", "authBlob.json")


def wait_for_code(url, state):
    got = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            got.update({k: v[0] for k, v in q.items()})
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"Spotivita login received, you can close this tab")

        def log_message(self, *args):
            pass

    print("Open this URL and log in to Spotify:\n\n" + url + "\n", flush=True)
    pasted = os.path.join(HERE, "redirect.txt")
    srv = http.server.HTTPServer(("127.0.0.1", 5588), Handler)
    srv.timeout = 1
    deadline = time.time() + 3600
    while "code" not in got and "error" not in got and time.time() < deadline:
        srv.handle_request()
        if os.path.exists(pasted):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(open(pasted).read().strip()).query)
            got.update({k: v[0] for k, v in q.items()})
            os.remove(pasted)
    srv.server_close()
    if "code" not in got or got.get("state") != state:
        sys.exit("login failed: %s" % got.get("error", "no code or state mismatch"))
    return got["code"]


def exchange(code, verifier):
    # curl, not urllib: some Python builds on Windows ship without a CA bundle.
    # The body goes through stdin so the code never shows on a command line.
    body = urllib.parse.urlencode({
        "grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT,
        "client_id": CLIENT_ID, "code_verifier": verifier})
    r = subprocess.run(["curl", "-s", "--data-binary", "@-",
                        "-H", "Content-Type: application/x-www-form-urlencoded",
                        "https://accounts.spotify.com/api/token"],
                       input=body.encode(), capture_output=True)
    resp = json.loads(r.stdout or b"{}")
    if "access_token" not in resp:
        sys.exit("token exchange failed: %s %s" % (resp.get("error"), resp.get("error_description")))
    return resp["access_token"]


def stored_credentials(token, librespot):
    cache = tempfile.mkdtemp(prefix="spotivita-login-")
    cred = os.path.join(cache, "credentials.json")
    env = dict(os.environ, LIBRESPOT_ACCESS_TOKEN=token)
    p = subprocess.Popen([librespot, "--cache", cache, "--disable-audio-cache", "--backend", "pipe",
                          "--disable-discovery", "--name", "spotivita-login"],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(60):
            if os.path.exists(cred) and os.path.getsize(cred) > 0 or p.poll() is not None:
                break
            time.sleep(1)
        time.sleep(1)   # let librespot finish writing the file
    finally:
        p.kill()
        p.wait()
    if not os.path.exists(cred):
        shutil.rmtree(cache, ignore_errors=True)
        sys.exit("librespot wrote no credentials")
    c = json.load(open(cred))
    shutil.rmtree(cache, ignore_errors=True)
    return c


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=default_out(), help="authBlob.json to write")
    args = ap.parse_args()
    librespot = os.environ.get("LIBRESPOT") or shutil.which("librespot")
    if not librespot:
        sys.exit("librespot not found: cargo install librespot, or set LIBRESPOT")

    verifier = base64.urlsafe_b64encode(secrets.token_bytes(64)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": CLIENT_ID, "response_type": "code", "redirect_uri": REDIRECT,
        "code_challenge_method": "S256", "code_challenge": challenge,
        "state": state, "scope": SCOPES})

    token = exchange(wait_for_code(url, state), verifier)
    c = stored_credentials(token, librespot)
    blob = {"authData": c["auth_data"], "authType": c["auth_type"], "username": c["username"]}
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(blob, f)
    print("wrote %s (authType %d)" % (args.out, c["auth_type"]))


if __name__ == "__main__":
    main()
