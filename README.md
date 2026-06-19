![Linter](https://github.com/michal4132/cspot_vita/workflows/Linter/badge.svg)

# :musical_note: psvitify
A modern Spotify player for the PS VITA. Heavily reworked fork of cspot_vita:
Spotify-style UI, frame-gated rendering (near-zero idle power), TLS verification,
in-app search, a touch scrubber and volume.

Installs alongside the old CSpot (its own TITLEID), it does not replace it.

*Only to be used with premium spotify accounts!*

## How to install
1) Download the [VPK](https://github.com/michal4132/cspot_vita/releases/latest) and install it on your PS VITA
2) Install the libshacccg.suprx with [CrystalPSM](https://github.com/EliCrystal2001/CrystalPSM/releases/latest)
3) Open the app. It shows a "Waiting for Spotify Connect" screen
4) On your phone (same Wi-Fi as the Vita), open Spotify, tap the Connect/devices icon and pick **psvitify**
5) Enjoy! The Vita remembers the login, so next time it connects on its own

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

### Disclaimer
Using this code to connect to Spotify's API may be prohibited by their terms of service. Use at your own risk. The developers are not responsible for any negative consequences, including account closure.
