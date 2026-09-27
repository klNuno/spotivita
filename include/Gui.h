#pragma once

#include <imgui_vita2d/imgui.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "API.h"
#include "NetWorker.h"
#include "PlayerModel.h"
#include "Screen.h"
#include "Utils.h"

class GUI {
 public:
    ~GUI();
    void init();
    void start();
    std::atomic<bool> isRunning{true};
    // A Vita app is already foreground-active at cold launch and gets no initial
    // ON_ACTIVATE event, so paused MUST start false. Starting true deadlocked the
    // render loop before its first buffer swap, holding the GPU and wedging the
    // whole system (other apps crash, shutdown hangs). The watchdog flips this on
    // real background/foreground transitions afterwards.
    std::atomic<bool> paused{false};
    ImFont *font = nullptr;
    ImFont *font_bold = nullptr;
    ImFont *playback_icon_font = nullptr;
    ImFont *icon_font = nullptr;
    ImFont *small_icon_font = nullptr;   // 24 px: row art, volume
    ImFont *log_font = nullptr;
    Screen *login_screen = nullptr;
    Screen *playback_screen = nullptr;
    // screen is swapped from the cspot worker thread and read every render
    // frame, so it must be atomic (the Vita has a weak memory model).
    void set_screen(Screen *s) { screen = s; }

    // Set by the cspot thread once the player controls below are wired.
    std::atomic<bool> cspot_started{false};

    // Spotify HTTP API (used from the net worker only) and the worker itself.
    API api;
    NetWorker net;

    // Shared playback state (cspot worker writes, GUI thread reads).
    PlayerModel player;

    // Player controls. They queue a command for the cspot thread, which owns
    // SpircController; calling cspot from the GUI thread raced its state.
    // No-op defaults until the cspot thread wires them.
    std::function<void()> nextCallback = []() {};
    std::function<void()> prevCallback = []() {};
    std::function<void()> playToggleCallback = []() {};
    std::function<void()> pauseCallback = []() {};
    std::function<void(int)> volumeCallback = [](int) {};  // 0..65535
    // Local playback: track URIs, context URI, start index, and whether the
    // list goes on past these tracks (cspot then sets queueEnded at their end).
    std::function<void(const std::vector<std::string>&, const std::string&, uint32_t, bool)>
        playTracksCallback = [](const std::vector<std::string>&, const std::string&, uint32_t, bool) {};
    std::atomic<bool> queueEnded{false};
    std::function<void(int)> seekCallback = [](int) {};       // ms
    std::function<void(bool)> shuffleCallback = [](bool) {};
    std::function<void(int)> repeatCallback = [](int) {};    // 0 off, 1 all, 2 one

    // Audio quality in kb/s (96, 160 or 320), saved in the cspot config.
    std::atomic<int> quality_kbps{320};
    std::function<void(int)> qualityCallback = [](int) {};

    // Short message at the bottom of the screen (GUI thread only).
    void toast(const std::string &msg);

    // Debug server hooks (GUI thread).
    std::string debugState();
    bool debugCommand(const std::string &cmd, const std::string &arg);

 private:
    void drawToast();

    std::atomic<Screen*> screen{nullptr};
    std::string toastText;
    uint64_t toastUntilUs = 0;
};
