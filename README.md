[![CI](https://github.com/klNuno/spotivita/actions/workflows/ci.yml/badge.svg)](https://github.com/klNuno/spotivita/actions/workflows/ci.yml)

# Spotivita

A Spotify player for the PS Vita. Open your playlists, search for tracks and
play them on the console, or pick the Vita as a speaker from the Spotify app on
your phone.

![Spotivita playing a search result](docs/player.png)

Spotify Premium is required. Spotivita started as a fork of
[cspot_vita](https://github.com/michal4132/cspot_vita) and still streams
through [cspot](https://github.com/feelfreelinux/cspot). Most of the rest is
new: the interface, the library, search, local playback control and the
network layer.

## Features

- Your playlists, and the tracks inside them.
- Track search.
- Play, pause, next, previous, seek on the progress bar, shuffle, repeat and
  volume, all handled on the Vita.
- Spotify Connect: the Vita shows up in the device list of the Spotify app, so
  your phone can drive it too.
- Background playback: press the PS button and the music keeps going, screen
  off included. The interface stops drawing while it is in the background.
- No `libshacccg.suprx` needed. It draws with vita2d and precompiled shaders.

## Status

Everything above runs in the Vita3K emulator, which has no sound, so playback
there is checked by watching the position move. The build has not run on a
real console since the renderer moved from vitaGL to vita2d. Background
playback and the phone login were last checked on hardware before that change.
If something breaks on your Vita, please open an issue.

## Install

1. Get `spotivita.vpk`. Each CI run on `master` attaches it as the
   `spotivita-vpk` artifact (Actions tab, GitHub login needed). A release will
   follow the first hardware test.
2. Install it with VitaShell. Its TITLEID is `SPOTIVITA`, so it sits next to an
   existing CSpot install instead of replacing it.
3. Open Spotivita. It waits for Spotify Connect.

   ![Login screen](docs/login.png)

4. On your phone, on the same Wi-Fi, open Spotify, tap the devices icon and
   pick Spotivita. The Vita keeps that login, so later launches connect on
   their own.

Spotify turned off username and password logins in 2024. The Vita cannot log
in by itself anymore, so your phone hands it a login over the local network
(Zeroconf).

## How it talks to Spotify

- cspot keeps the session with Spotify's access point, streams and decodes the
  audio (Ogg Vorbis), and answers Spotify Connect.
- Playlists and track names come from spclient, search from the same GraphQL
  endpoint the web player uses.
- The public Web API answers 429 to every request from this client, so nothing
  depends on it. When you play a track, the Vita builds the queue itself and
  hands it to cspot.
- Search uses a query hash from the web player. When Spotify rotates it, search
  shows "Spotify changed its search API" until the app is updated.

## Building

You need [vitasdk](https://vitasdk.org) with the `vita2d`, `imgui-vita2d`,
`curl` and `openssl` packages, plus `protoc` and Python's `protobuf` and
`setuptools` on the host for nanopb.

```shell
git clone --recursive https://github.com/klNuno/spotivita.git
cd spotivita
cmake -B build && cmake --build build
```

The VPK lands in `build/spotivita.vpk`. `.github/workflows/ci.yml` builds it in
the `vitasdk/vitasdk` Docker image. That image installs OpenSSL 1.1.1 while its
libcurl expects 1.0.2, so the workflow swaps the package back first.

## Testing without the console in your hands

A devkit build adds a TCP server on port 2138. It returns the app state as
JSON and takes taps, swipes, buttons, screenshots, logs, file transfers and
eboot hot swaps. It can read and write files on the console, so release builds
leave it out.

```shell
cmake -B build/dev -DSPOTIVITA_DEVKIT=ON && cmake --build build/dev
python tools/vitactl.py find                 # finds the Vita on your /24
VITA_HOST=<vita-ip> python tools/vitactl.py deploy build/dev/eboot.bin
VITA_HOST=<vita-ip> python tools/vitactl.py "state; tap 660 492; shot s.png"
```

- `tools/vitasetup.py` prepares a console once. Open VitaShell, press SELECT
  to start its FTP server, run the script, then reboot. It installs
  [vitacompanion](https://github.com/devnoname120/vitacompanion), which serves
  FTP, takes launch, quit and reboot commands, and keeps the Vita awake. It also
  copies the dev build over the installed app, or leaves the VPK in
  `ux0:data/spotivita/` for VitaShell when the app is not installed yet. After
  that, `vitactl deploy` works even when the app has crashed.
- `tools/vita3k` runs the same build in Vita3K on a hidden Windows desktop,
  with no window on your screen. `tools/vita3k/login.py` logs the emulator in
  without a phone. Details are in [tools/vita3k/README.md](tools/vita3k/README.md).

## Credits

- [michal4132](https://github.com/michal4132) for cspot_vita, the base of this
  project.
- [feelfreelinux](https://github.com/feelfreelinux) and contributors for cspot
  and bell.
- The vitasdk, vita2d, Dear ImGui and vitacompanion projects.

## Disclaimer

Using this code to connect to Spotify's API may be prohibited by their terms of
service. Use at your own risk. The developers are not responsible for any
negative consequences, including account closure.
