#pragma once

class GUI;

// Remote-control server for testing without touching the console (port 2138,
// plain TCP, one text command per line). Built only with -DSPOTIVITA_DEVKIT=ON:
// it can read and write files, so it never ships in a release VPK.
//
//   ping                      -> OK pong
//   state                     -> OK <json>
//   tap X Y                   -> OK            (960x544 screen pixels)
//   swipe X1 Y1 X2 Y2 [F]     -> OK            (F frames, default 12)
//   press BUTTON[+BUTTON]     -> OK            (cross circle square triangle up
//                                              down left right l r start select)
//   ui CMD [ARG]              -> OK | ERR      (screen hook, e.g. "ui search daft punk")
//   shot                      -> OK <len>\n<BMP bytes>
//   log [BYTES]               -> OK <len>\n<text>
//   get PATH                  -> OK <len>\n<bytes>
//   put PATH LEN\n<bytes>     -> OK
//   relaunch                  -> OK, then the app restarts from app0:eboot.bin
//   quit                      -> OK, then the app exits
namespace DevKit {

void start(GUI *gui);

// GUI thread, every loop iteration (also while paused). Runs requests that need
// GUI-owned state. Returns true when it did something that should be drawn.
bool pump(GUI *gui);

}  // namespace DevKit
