#pragma once

#include <imgui_vita2d/imgui.h>
#include <string>

#define PLAY_BUTTON_BACKGROUND        IM_COL32(255, 255, 255, 255)
#define SPOTIFY_GREEN                 IM_COL32(30, 215, 96, 255)   // #1ED760
#define BACKGROUND_COLOR              ImVec4(0.07f, 0.07f, 0.07f, 1.00f)  // #121212
#define WINDOW_FLAGS                  (ImGuiWindowFlags_NoTitleBar      \
                                     | ImGuiWindowFlags_NoMove          \
                                     | ImGuiWindowFlags_NoResize        \
                                     | ImGuiWindowFlags_NoCollapse)

// ImGui helper functions
ImFont* AddDefaultFont(float pixel_size);
bool StyleButton(const char* label, ImVec2 btn_size, bool active = false);
void AlignForWidth(float width, float alignment = 0.5f);
void TextCentered(const std::string& text);

// Logger. Written from every thread (cspot, net, GUI), read by the GUI thread.
// The on-screen copy keeps the last LOG_KEEP_BYTES; the file keeps everything.
void init_logger();
void flush_logger();
// Copies the log into *out if it changed since *version; returns true if so.
bool log_snapshot(std::string *out, unsigned *version);
// Last `bytes` of the in-memory log (debug server).
std::string log_tail(size_t bytes);

// JSON string literal (quotes included) for the debug server's state dumps.
std::string json_quote(const std::string &s);
