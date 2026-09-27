#!/usr/bin/env python3
"""Drive a Spotivita devkit build (SPOTIVITA_DEVKIT=ON) over TCP port 2138.

Works against a real Vita on the LAN or Vita3K on this PC (127.0.0.1, with
ux0:data/cspot/loopback present). One command per call, or several with ';':

  vitactl.py [--host H] state
  vitactl.py tap 700 200
  vitactl.py swipe 700 450 700 150 [frames]
  vitactl.py press start          (cross circle square triangle up down left right l r start select, '+' joins)
  vitactl.py ui search daft punk  (tab NAME, search Q, open N, folder N, back, play N, quality 96|160|320, refresh, toast TEXT, screen login|playback)
  vitactl.py shot out.png         (BMP from the device, converted to PNG when Pillow is present;
                                   under Vita3K the guest framebuffer stays black: use vita3k.ps1 shot)
  vitactl.py log [bytes]
  vitactl.py get ux0:data/cspot/log.txt local.txt
  vitactl.py put local_eboot.bin ux0:app/SPOTIVITA/eboot.bin
  vitactl.py deploy build/dev/eboot.bin   (FTP on port 1337 when it answers: vitacompanion quits,
                                   uploads and relaunches, VitaShell's FTP only uploads; otherwise
                                   devkit put + relaunch, which works under Vita3K only)
  vitactl.py wait [seconds]       (poll until the app answers ping)
  vitactl.py find [a.b.c]         (scan a.b.c.1-254, default this PC's /24, for a devkit build
                                   or vitacompanion)
  vitactl.py vc launch SPOTIVITA  (raw vitacompanion command on port 1338: launch, quit, reboot,
                                   screen on|off, press, release; one per call, no ';')

vitacompanion is installed once with tools/vitasetup.py.
"""
import argparse
import ftplib
import io
import json
import os
import socket
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor

PORT = 2138
FTP_PORT = 1337
VC_PORT = 1338
TITLE_ID = "SPOTIVITA"


class Link:
    def __init__(self, host, timeout=10.0):
        self.sock = socket.create_connection((host, PORT), timeout=timeout)
        self.buf = b""

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace")

    def _exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(max(65536, n - len(self.buf)))
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def text(self, cmd):
        self.sock.sendall((cmd + "\n").encode("utf-8"))
        return self._line()

    def blob(self, cmd, payload=None):
        self.sock.sendall((cmd + "\n").encode("utf-8"))
        if payload is not None:
            self.sock.sendall(payload)
            return self._line(), None
        head = self._line()
        if not head.startswith("OK "):
            return head, None
        return "OK", self._exact(int(head[3:]))


def save_image(data, path):
    if path.lower().endswith(".png"):
        try:
            from io import BytesIO
            from PIL import Image
            Image.open(BytesIO(data)).save(path)
            return path
        except ImportError:
            path = path[:-4] + ".bmp"
    with open(path, "wb") as f:
        f.write(data)
    return path


def wait_ready(host, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            if Link(host, timeout=2.0).text("ping") == "OK pong":
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def local_prefix():
    # Connecting a UDP socket sends nothing; it only picks the outgoing interface.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0].rsplit(".", 1)[0]
    finally:
        s.close()


def answers(host):
    try:
        return Link(host, timeout=1.5).text("ping") == "OK pong"
    except OSError:
        return False


def listens(host, port=VC_PORT):
    try:
        socket.create_connection((host, port), timeout=1.5).close()
        return True
    except OSError:
        return False


def probe(host):
    return [name for name, ok in (("devkit", answers(host)),
                                  ("vitacompanion", listens(host))) if ok]


def find(prefix):
    hosts = ["%s.%d" % (prefix, i) for i in range(1, 255)]
    with ThreadPoolExecutor(max_workers=64) as pool:
        found = [(h, what) for h, what in zip(hosts, pool.map(probe, hosts)) if what]
    for h, what in found:
        print("OK", h, " ".join(what))
    if not found:
        print("ERR no devkit build or vitacompanion on %s.0/24" % prefix)
    return 0 if found else 1


def vc(host, line):
    """One vitacompanion command; returns what it answered before closing."""
    with socket.create_connection((host, VC_PORT), timeout=5.0) as s:
        s.sendall((line + "\n").encode("utf-8"))
        s.shutdown(socket.SHUT_WR)
        out = b""
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                out += chunk
        except socket.timeout:
            pass
    return out.decode("utf-8", "replace").strip()


def sync_assets(ftp, build_dir):
    """Upload the VPK's top-level files (fonts, certificates) the console lacks or holds
    at another size. A new asset only reaches the console through the VPK, and an eboot
    that expects it crashes at boot without it."""
    # The newest one: a build dir can keep a VPK from before a rename.
    vpks = sorted((n for n in os.listdir(build_dir) if n.endswith(".vpk")),
                  key=lambda n: os.path.getmtime(os.path.join(build_dir, n)), reverse=True)
    if not vpks:
        return
    remote_dir = "/ux0:/app/%s/" % TITLE_ID
    have = {}
    lines = []
    ftp.retrlines("LIST " + remote_dir, lines.append)
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) == 9 and parts[4].isdigit():
            have[parts[8]] = int(parts[4])
    with zipfile.ZipFile(os.path.join(build_dir, vpks[0])) as z:
        for info in z.infolist():
            name = info.filename
            if "/" in name or name == "eboot.bin" or have.get(name) == info.file_size:
                continue
            ftp.storbinary("STOR " + remote_dir + name, io.BytesIO(z.read(info)))
            print("OK ftp", name, info.file_size)


def deploy_ftp(host, local):
    """FTP the eboot: vitacompanion (quit, upload, launch) or VitaShell's FTP (upload only).

    On hardware a running app cannot write its own ux0:app folder, so the devkit
    `put` of an eboot only works under Vita3K."""
    remote = "/ux0:/app/%s/eboot.bin" % TITLE_ID
    companion = listens(host, VC_PORT)
    if companion:
        print(vc(host, "quit " + TITLE_ID) or "OK quit")
        # Right after a quit the app folder still refuses writes ("550 File not found").
        time.sleep(2.0)
    with open(local, "rb") as f:
        data = f.read()
    ftp = ftplib.FTP()
    ftp.connect(host, FTP_PORT, timeout=30)
    ftp.login()
    sync_assets(ftp, os.path.dirname(os.path.abspath(local)))
    ftp.storbinary("STOR " + remote, io.BytesIO(data))
    ftp.quit()
    print("OK ftp", remote[1:], len(data))
    if not companion:
        print("OK uploaded through VitaShell: open Spotivita on the Vita")
        return 0
    print(vc(host, "launch " + TITLE_ID) or "OK launch")
    ok = wait_ready(host, 40)
    print("OK relaunched" if ok else "ERR app did not come back")
    return 0 if ok else 1


def run(host, argv):
    cmd = argv[0]
    if cmd == "find":
        return find(argv[1] if len(argv) > 1 else local_prefix())
    if cmd == "wait":
        ok = wait_ready(host, float(argv[1]) if len(argv) > 1 else 30)
        print("OK ready" if ok else "ERR no answer")
        return 0 if ok else 1
    if cmd == "vc":
        r = vc(host, " ".join(argv[1:]))
        print(r or "OK")
        return 1 if r.lower().startswith(("err", "unknown", "invalid")) else 0
    if cmd == "deploy" and listens(host, FTP_PORT):
        return deploy_ftp(host, argv[1])
    link = Link(host)
    if cmd == "state":
        r = link.text("state")
        if r.startswith("OK "):
            print(json.dumps(json.loads(r[3:]), indent=2, ensure_ascii=False))
            return 0
        print(r)
        return 1
    if cmd == "shot":
        r, data = link.blob("shot")
        if data is None:
            print(r)
            return 1
        print("OK", save_image(data, argv[1] if len(argv) > 1 else "shot.png"))
        return 0
    if cmd == "log":
        r, data = link.blob("log " + (argv[1] if len(argv) > 1 else "8192"))
        sys.stdout.write(data.decode("utf-8", "replace") if data else r + "\n")
        return 0 if data is not None else 1
    if cmd == "get":
        r, data = link.blob("get " + argv[1])
        if data is None:
            print(r)
            return 1
        out = argv[2] if len(argv) > 2 else os.path.basename(argv[1].split(":")[-1])
        with open(out, "wb") as f:
            f.write(data)
        print("OK", out, len(data))
        return 0
    if cmd in ("put", "deploy"):
        local = argv[1]
        remote = argv[2] if cmd == "put" else "ux0:app/%s/eboot.bin" % TITLE_ID
        with open(local, "rb") as f:
            data = f.read()
        r, _ = link.blob("put %s %d" % (remote, len(data)), data)
        print(r)
        if cmd == "deploy" and not r.startswith("OK"):
            print("A running app cannot write its own ux0:app folder on hardware. Reboot the Vita "
                  "so vitacompanion loads, or open VitaShell and press SELECT (FTP), then retry.")
        if cmd == "put" or not r.startswith("OK"):
            return 0 if r.startswith("OK") else 1
        print(link.text("relaunch"))
        time.sleep(2.0)
        ok = wait_ready(host, 40)
        print("OK relaunched" if ok else "ERR app did not come back")
        return 0 if ok else 1
    r = link.text(" ".join(argv))
    print(r)
    return 0 if r.startswith("OK") else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("VITA_HOST", "127.0.0.1"))
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    if not args.command:
        ap.print_help()
        return 2
    rc = 0
    for part in " ".join(args.command).split(";"):
        part = part.strip()
        if part:
            try:
                rc = run(args.host, part.split()) or rc
            except OSError as e:
                print(f"ERR {args.host}:{PORT} unreachable ({e}). App closed, console asleep, "
                      "or a release build without the devkit?")
                return 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
