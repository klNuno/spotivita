#!/usr/bin/env python3
"""One-time setup of a real Vita for unattended testing, over VitaShell's FTP.

On the Vita: open VitaShell and press SELECT (FTP server on port 1337). Then:

  python tools/vitasetup.py [--host IP] [--vpk build/dev/spotivita.vpk]

It finds the Vita on this PC's /24 (or VITA_HOST), then:
  1. copies vitacompanion 1.07 (user + kernel module, sha256-pinned) to ur0:tai/
     and adds both to the active taiHEN config.txt, after saving a copy of it
     next to it as config.txt.spotivita-bak;
  2. copies the dev VPK's files over ux0:app/SPOTIVITA/ when Spotivita is
     installed, or the VPK itself to ux0:data/spotivita/ for a VitaShell install.

Reboot the Vita afterwards. From then on vitacompanion keeps the console awake,
serves FTP on 1337 and takes launch/quit/reboot on 1338, so `vitactl deploy`
works even when the app is not running. Running this twice changes nothing.
"""
import argparse
import ftplib
import hashlib
import io
import os
import socket
import sys
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

FTP_PORT = 1337
TITLE_ID = "SPOTIVITA"
VC_URL = "https://github.com/devnoname120/vitacompanion/releases/download/1.07/"
VC_FILES = {
    "vitacompanion.suprx": "7ca14a00e853c26b836c78235e049d4386406e824284c22ce086b4b40e12c564",
    "vitacompanion_kernel.skprx": "6647230b99ea1f8562d960c295a559269775fa7ff7e96af119b954c68a0ed17e",
}
PLUGINS = {
    "*KERNEL": "ur0:tai/vitacompanion_kernel.skprx",
    "*main": "ur0:tai/vitacompanion.suprx",
}


def local_prefix():
    # Connecting a UDP socket sends nothing; it only picks the outgoing interface.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0].rsplit(".", 1)[0]
    finally:
        s.close()


def ftp_banner(host):
    try:
        with socket.create_connection((host, FTP_PORT), timeout=1.5) as s:
            s.settimeout(2.0)
            return s.recv(256).startswith(b"220")
    except OSError:
        return False


def find_vita():
    prefix = local_prefix()
    hosts = ["%s.%d" % (prefix, i) for i in range(1, 255)]
    with ThreadPoolExecutor(max_workers=64) as pool:
        found = [h for h, ok in zip(hosts, pool.map(ftp_banner, hosts)) if ok]
    if len(found) != 1:
        sys.exit("ERR %s FTP server on %s.0/24 port %d; pass --host"
                 % ("no" if not found else "more than one", prefix, FTP_PORT))
    return found[0]


def fetch_plugins():
    out = {}
    for name, digest in VC_FILES.items():
        with urllib.request.urlopen(VC_URL + name, timeout=30) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != digest:
            sys.exit("ERR %s does not match its pinned sha256" % name)
        out[name] = data
    return out


def is_dir(ftp, path):
    try:
        ftp.cwd(path)
        return True
    except ftplib.error_perm:
        return False
    finally:
        ftp.cwd("/")


def read_file(ftp, path):
    buf = io.BytesIO()
    try:
        ftp.retrbinary("RETR " + path, buf.write)
    except ftplib.error_perm:
        return None
    return buf.getvalue()


def write_file(ftp, path, data):
    ftp.storbinary("STOR " + path, io.BytesIO(data))


def make_dirs(ftp, path):
    parts = path.strip("/").split("/")
    for i in range(1, len(parts) + 1):
        sub = "/" + "/".join(parts[:i])
        if not is_dir(ftp, sub):
            ftp.mkd(sub)


def patch_config(text):
    """Add each plugin at the end of its section, after the plugins already
    there; append the section when missing."""
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    present = {ln.strip().lower() for ln in lines}
    for section, plugin in PLUGINS.items():
        if plugin.lower() in present:
            continue
        heads = [i for i, ln in enumerate(lines) if ln.strip() == section]
        if heads:
            end = heads[0] + 1
            while end < len(lines) and not lines[end].startswith("*"):
                end += 1
            while lines[end - 1].strip() == "":
                end -= 1
            lines.insert(end, plugin)
        else:
            while lines and lines[-1].strip() == "":
                lines.pop()
            lines += ["", section, plugin, ""]
    return nl.join(lines)


def install_plugins(ftp):
    for name, data in fetch_plugins().items():
        write_file(ftp, "/ur0:/tai/" + name, data)
        print("OK ur0:tai/" + name)
    # taiHEN reads ux0:tai/config.txt when it exists, ur0:tai/config.txt otherwise.
    for dev in ("ux0", "ur0"):
        path = "/%s:/tai/config.txt" % dev
        raw = read_file(ftp, path)
        if raw is not None:
            break
    else:
        sys.exit("ERR no taiHEN config.txt on ux0:tai or ur0:tai")
    text = raw.decode("utf-8", "replace")
    patched = patch_config(text)
    if patched == text:
        print("OK %s already loads vitacompanion" % path[1:].replace(":/", ":"))
        return
    if read_file(ftp, path + ".spotivita-bak") is None:
        write_file(ftp, path + ".spotivita-bak", raw)
    write_file(ftp, path, patched.encode("utf-8"))
    print("OK %s patched (copy in config.txt.spotivita-bak)" % path[1:].replace(":/", ":"))


def install_app(ftp, vpk):
    app = "/ux0:/app/" + TITLE_ID
    if not is_dir(ftp, app):
        make_dirs(ftp, "/ux0:/data/spotivita")
        with open(vpk, "rb") as f:
            write_file(ftp, "/ux0:/data/spotivita/spotivita-dev.vpk", f.read())
        print("OK ux0:data/spotivita/spotivita-dev.vpk: install it from VitaShell")
        return
    # sce_sys stays as installed: LiveArea data is cached by the system anyway.
    with zipfile.ZipFile(vpk) as z:
        for info in z.infolist():
            if info.is_dir() or info.filename.startswith("sce_sys/"):
                continue
            target = "%s/%s" % (app, info.filename)
            make_dirs(ftp, target.rsplit("/", 1)[0])
            write_file(ftp, target, z.read(info))
            print("OK ux0:app/%s/%s" % (TITLE_ID, info.filename))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("VITA_HOST"))
    ap.add_argument("--vpk", default="build/dev/spotivita.vpk")
    ap.add_argument("--no-plugins", action="store_true", help="skip vitacompanion")
    args = ap.parse_args()
    if not os.path.isfile(args.vpk):
        sys.exit("ERR %s not found: build the dev VPK first" % args.vpk)
    host = args.host or find_vita()
    print("OK Vita FTP at %s:%d" % (host, FTP_PORT))
    ftp = ftplib.FTP()
    ftp.connect(host, FTP_PORT, timeout=30)
    ftp.login()
    if not args.no_plugins:
        install_plugins(ftp)
    install_app(ftp, args.vpk)
    ftp.quit()
    print("OK done: reboot the Vita, then `python tools/vitactl.py find`")
    return 0


if __name__ == "__main__":
    sys.exit(main())
