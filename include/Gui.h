#pragma once

#include <imgui_vita.h>
#include <atomic>
#include "Utils.h"
#include "API.h"
#include <functional>
#include <string>
#include <Logger.h>
#include "Screen.h"
#include "PlayerModel.h"

class GUI {
 public:
    ~GUI();
    void init();
    void start();
    std::atomic<bool> isRunning = true;
    // A Vita app is already foreground-active at cold launch and gets no initial
    // ON_ACTIVATE event, so paused MUST start false. Starting true deadlocked the
    // render loop before its first vglSwapBuffers, holding the GPU and wedging the
    // whole system (other apps crash, shutdown hangs). The watchdog flips this on
    // real background/foreground transitions afterwards.
    std::atomic<bool> paused = false;
    ImFont *font;
    ImFont *font_bold;
    ImFont *playback_icon_font;
    ImFont *icon_font;
    ImFont *log_font;
    Screen *login_screen = nullptr;
    Screen *playback_screen = nullptr;
    // screen is swapped from the cspot worker thread and read every render
    // frame, so it must be atomic (the Vita has a weak memory model).
    void set_screen(Screen *s) { screen = s; }

    bool cspot_started = false;

    // Spotify API
    API api;

    // Shared playback state (cspot worker writes, GUI thread reads).
    PlayerModel player;

    // CSpot control
    std::function<void()> nextCallback;
    std::function<void()> prevCallback;
    std::function<void()> playToggleCallback;
    std::function<void()> activateDevice;
    std::function<void(int)> volumeCallback;  // 0..65535

 private:
    std::atomic<Screen*> screen{nullptr};
};

int init_gui();
