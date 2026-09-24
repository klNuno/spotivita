# Vita3K test harness

`vita3k.ps1` installs a VPK into the Vita3K emulator, boots it by TITLE ID, saves a 960x544 PNG of the emulated screen and stops the emulator. It runs without any window on the user's screen, so it can be used while someone works on the same PC.

## Usage

```powershell
pwsh -File tools/vita3k/vita3k.ps1 setup                      # once; safe to re-run
pwsh -File tools/vita3k/vita3k.ps1 install build/cspot_vita.vpk
pwsh -File tools/vita3k/vita3k.ps1 run VITA3KTST -Seconds 5
pwsh -File tools/vita3k/vita3k.ps1 shot out/screen.png
pwsh -File tools/vita3k/vita3k.ps1 stop

# or all four steps in one call (always stops the emulator at the end)
pwsh -File tools/vita3k/vita3k.ps1 smoke build/cspot_vita.vpk out/screen.png
```

`status` prints the running instance and its windows. Options: `-Renderer Vulkan|OpenGL` (default Vulkan), `-Root <dir>` (default `.vita3k` or `$env:VITA3K_ROOT`), `-BootTimeout <s>`, `-Update` (setup re-downloads Vita3K), `-PrintWindow` (shot, diagnosis only).

## How it works

- Install layout: `<Root>/bin` holds the Vita3K Windows build (continuous release). An empty `bin/portable` folder switches Vita3K to portable mode, so config, the emulated file system (`portable/fs/ux0`, `vs0`, `sa0`), logs and screenshots stay under it. `<Root>/firmware` keeps the PUP files, `<Root>/run/state.json` the running pid.
- `setup` downloads Vita3K and the firmware (`PSVUPDAT.PUP` 3.74 and the font package `PSP2UPDAT.PUP`, both from Sony's update servers), installs them with `Vita3K.exe --firmware`, then writes `portable/config.yml`: welcome dialog, update check, Discord and the missing-firmware prompt off, audio volume 0, v-sync off, screenshot format PNG, screenshot hotkey F9. It also sets `confirmExitApp=false` in `portable/gui-configs/CurrentSettings.ini` so closing never asks a question.
- Every Vita3K process is started with `CreateProcess` on a separate desktop created by `CreateDesktop` (`vita3k-harness`), suspended, then given BelowNormal priority and affinity `0xAAAA` before it resumes. The environment carries `SDL_AUDIO_DRIVER=dummy` (Vita3K has no null audio backend). Windows on that desktop are never shown and cannot take focus.
- `install` runs `Vita3K.exe <file.vpk>`. Vita3K installs the archive before its GUI starts, then boots the app; the harness waits for `ux0/app/<TITLEID>/eboot.bin` and the first window, then stops it. The TITLE ID comes from `sce_sys/param.sfo` in the VPK. The old app folder is deleted first (Vita3K replaces it anyway; save data is kept).
- `run` starts `Vita3K.exe -B <renderer> -r <TITLEID>` and waits for the game window, whose title contains `(<TITLEID>)`.
- `shot` posts F9 key down/up to the game window. Vita3K then reads the guest frame back from the renderer and writes a PNG under `portable/screenshots`, which the harness moves to the requested path and checks for 960x544.
- `stop` posts `WM_CLOSE` to the main window, which ends the app and exits. After 15 s it kills the process. The hidden desktop disappears when the last process on it exits.

## Limits

- Use Vulkan. With OpenGL the game runs at 60 FPS on the hidden desktop but the native screenshot comes back all black.
- `-PrintWindow` (`PrintWindow` with `PW_RENDERFULLCONTENT` from a thread attached to the hidden desktop) returns a blank white image for both backends. It is kept only for diagnosis.
- Window handles on the hidden desktop are invalid (error 1400) for threads on the user's desktop. Messages are posted from a worker thread that called `SetThreadDesktop`.
- Vita3K buffers its log file (`portable/vita3k.log`), so the log is complete only after the process exits. The harness waits on files and windows, not on log lines.
- One instance at a time. `run` refuses to start while `state.json` points at a live Vita3K.
- The first `--firmware` call in a fresh portable folder can fail in logging init (`boost::filesystem::create_directories: Invalid argument`). `setup` retries once.
- No input injection besides the screenshot key. Controller input would need more posted keys (see the `keyboard-button-*` entries in `config.yml`).

## Networking

Guest BSD sockets (`sceNetSocket`, `sceNetBind`, `sceNetListen`, `sceNetAccept`) map one to one onto host WinSock sockets (`vita3k/net/src/posixsocket.cpp`); addresses and ports are copied without translation. A guest server bound to `INADDR_ANY:2138` listens on the host's `0.0.0.0:2138`, so `127.0.0.1:2138` on the host reaches it. Not tested here. On Windows a first listen on a non-loopback address by a new executable can raise the Windows Defender Firewall prompt on the user's desktop; a firewall rule for `Vita3K.exe` created in advance avoids it.
