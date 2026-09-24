![Linter](https://github.com/michal4132/cspot_vita/workflows/Linter/badge.svg)

# :musical_note: psvitify
A modern Spotify player for the PS VITA. Heavily reworked fork of cspot_vita:
Spotify-style UI, frame-gated rendering (near-zero idle power), TLS verification,
in-app search, a touch scrubber and volume.

Installs alongside the old CSpot (its own TITLEID), it does not replace it.

Music keeps playing in the background: press the PS button and the app keeps
streaming (BGM port + background-app attributes), including with the screen
off. The UI stops rendering entirely while backgrounded.

*Only to be used with premium spotify accounts!*

## How to install
1) Download the [VPK](https://github.com/michal4132/cspot_vita/releases/latest) and install it on your PS VITA
2) Open the app. It shows a "Waiting for Spotify Connect" screen
3) On your phone (same Wi-Fi as the Vita), open Spotify, tap the Connect/devices icon and pick **psvitify**
4) Enjoy! The Vita remembers the login, so next time it connects on its own

The app draws with vita2d and precompiled shaders, so `libshacccg.suprx` is no
longer needed.

> Spotify removed username/password login in 2024, so the app can no longer log
> in with typed credentials. It uses the Spotify Connect (Zeroconf) flow instead:
> your phone hands the Vita an authentication blob over the local network.

## Building

### Prerequisites
- [vitasdk](https://github.com/vitasdk)
- `protoc` (protobuf-compiler) on the host PATH, required at configure time by bell's nanopb codegen

This repo uses git submodules (cspot -> bell -> ...). Clone with them, or run:

```shell
git submodule update --init --recursive
mkdir build && cd build
cmake .. && make
```

### Testing without touching the console

A devkit build adds a small TCP server on port 2138 (state as JSON, taps,
swipes, buttons, screenshots, logs, file transfer, eboot hot-swap):

```shell
cmake -B build/dev -DPSVITIFY_DEVKIT=ON && cmake --build build/dev
python tools/vitactl.py find                      # locate the Vita on the LAN
VITA_HOST=192.168.1.42 python tools/vitactl.py deploy build/dev/eboot.bin
VITA_HOST=192.168.1.42 python tools/vitactl.py "state; tap 660 492; shot s.png"
```

The server can read and write files on the console, so release builds leave it
out. On a PC, `tools/vita3k` boots the same build in the Vita3K emulator on a
hidden desktop (see its README).

### Disclaimer
Using this code to connect to Spotify's API may be prohibited by their terms of service. Use at your own risk. The developers are not responsible for any negative consequences, including account closure.
