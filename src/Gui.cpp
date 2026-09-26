#include "Gui.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <cstdint>
#include <cfloat>
#include <cstdio>
#include <string>
#include <Logger.h>
#include "DevKit.h"
#include "GuiUtils.h"
#include "Input.h"
#include "Keyboard.h"
#include "Render.h"
#include "Utils.h"
#include "Font.h"
#include "PlaybackScreen.h"
#include "LoginScreen.h"

namespace {

// Dear ImGui 1.79 has no idle-frame helper, so frame-gate by hand. After the last input event, keep rendering a few
// frames (popups/combos take ~4 to settle), then stop building until something
// changes. This -- not a lighter toolkit or a slower clock -- is what makes the
// app sip power: a static UI does ~no CPU build and ~no GPU work.
const int WAKE_FRAMES = 4;
const uint64_t IDLE_FRAME_US = 100000;    // ~10 fps full rebuild when static
const uint64_t MAX_NAP_US = 33000;        // re-probe input at >= 30 Hz

struct InputSnapshot {
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    uint16_t touchNum = 0, tx = 0, ty = 0;
    bool operator!=(const InputSnapshot& o) const {
        return buttons != o.buttons || lx != o.lx || ly != o.ly || rx != o.rx ||
               ry != o.ry || touchNum != o.touchNum || tx != o.tx || ty != o.ty;
    }
};

InputSnapshot sampleInput() {
    InputSnapshot s;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
        s.buttons = pad.buttons;
        // Mask the low bits so resting analog jitter isn't read as movement.
        s.lx = pad.lx & 0xF0; s.ly = pad.ly & 0xF0;
        s.rx = pad.rx & 0xF0; s.ry = pad.ry & 0xF0;
    }
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0) {
        s.touchNum = touch.reportNum;
        if (touch.reportNum > 0) {
            s.tx = touch.report[0].x & 0xFFF0;
            s.ty = touch.report[0].y & 0xFFF0;
        }
    }
    return s;
}

// True while the user is actively holding/touching: keep rendering so scroll and
// drag stay smooth instead of settling after WAKE_FRAMES.
bool inputActive(const InputSnapshot& s) {
    bool stick = s.lx < 0x60 || s.lx > 0xA0 || s.ly < 0x60 || s.ly > 0xA0 ||
                 s.rx < 0x60 || s.rx > 0xA0 || s.ry < 0x60 || s.ry > 0xA0;
    return s.buttons != 0 || stick || s.touchNum > 0;
}

void applySpotifyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.ScrollbarSize = 10.0f;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);

    ImVec4 base  = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);     // black: OLED pixels off
    ImVec4 elev  = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);  // #181818
    ImVec4 card  = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);  // #282828
    ImVec4 green = ImVec4(0.12f, 0.84f, 0.38f, 1.00f);  // #1ED760
    ImVec4 grey  = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);  // #B3B3B3

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]         = base;
    c[ImGuiCol_ChildBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]          = elev;
    c[ImGuiCol_Text]             = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_TextDisabled]     = grey;
    c[ImGuiCol_Button]           = card;
    c[ImGuiCol_ButtonHovered]    = card;
    c[ImGuiCol_ButtonActive]     = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBg]          = card;
    c[ImGuiCol_FrameBgHovered]   = elev;
    c[ImGuiCol_FrameBgActive]    = card;
    c[ImGuiCol_Header]           = card;
    c[ImGuiCol_HeaderHovered]    = elev;
    c[ImGuiCol_HeaderActive]     = card;
    c[ImGuiCol_SliderGrab]       = green;
    c[ImGuiCol_SliderGrabActive] = green;
    c[ImGuiCol_CheckMark]        = green;
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]    = card;
    c[ImGuiCol_NavHighlight]     = green;
}

}  // namespace

void GUI::init() {
    // vita2d, no MSAA: ImGui already anti-aliases its shapes through the atlas.
    Render::init();

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;   // no imgui.ini written next to the app
    io.LogFilename = NULL;

    font = AddDefaultFont(26);
    log_font = AddDefaultFont(14);

    font_bold = AddTextFont("app0:PlusJakartaSans-Bold.ttf", "app0:Roboto-Bold.ttf", 30.0f);

    static const ImWchar playback_ranges[] = { 0xf144, 0xf144, 0xf28b, 0xf28b, 0 };
    static const ImWchar ranges[] = {
        0xf048, 0xf048,  // backward
        0xf051, 0xf051,  // forward
        0xf013, 0xf013,  // cog
        0xf02d, 0xf02d,  // book
        0xf002, 0xf002,  // search
        0xf001, 0xf001,  // music
        0xf074, 0xf074,  // random (shuffle)
        0xf01e, 0xf01e,  // redo (repeat)
        0xf060, 0xf060,  // arrow-left (back)
        0,
    };
    icon_font = io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, 40.0f, NULL, ranges);
    playback_icon_font = io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, 58.0f, NULL, playback_ranges);
    static const ImWchar small_ranges[] = {
        0xf001, 0xf001,  // music (playlist art)
        0xf004, 0xf004,  // heart (Liked Songs)
        0xf07b, 0xf07b,  // folder
        0xf026, 0xf028,  // volume off/down/up
        0,
    };
    small_icon_font = io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, 24.0f, NULL, small_ranges);
    io.Fonts->Build();
    Render::upload_fonts();

    applySpotifyTheme();
    Input::init();

    net.start();

    login_screen = new LoginScreen(this);
    playback_screen = new PlaybackScreen(this);
}

void GUI::toast(const std::string &msg) {
    toastText = msg;
    toastUntilUs = sceKernelGetProcessTimeWide() + 3500000;
}

void GUI::drawToast() {
    if (toastText.empty() || sceKernelGetProcessTimeWide() >= toastUntilUs) {
        return;
    }
    const float wrap = 760.0f;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, wrap, toastText.c_str());
    ImVec2 pad(20.0f, 12.0f);
    ImVec2 size(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
    // Over the right pane, above the tab row, so the scrubber and its times
    // stay visible. Wide toasts slide left as far as the screen edge.
    float x = 692.0f - size.x * 0.5f;
    if (x + size.x > 944.0f) x = 944.0f - size.x;
    if (x < 16.0f) x = 16.0f;
    ImVec2 p0(x, 544.0f - size.y - 96.0f);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(48, 48, 48, 245), 10.0f);
    dl->AddText(font, font->FontSize, ImVec2(p0.x + pad.x, p0.y + pad.y),
                IM_COL32(255, 255, 255, 255), toastText.c_str(), NULL, wrap);
}

std::string GUI::debugState() {
    Screen *current = screen.load();
    const char *name = current == nullptr ? "none"
                     : current == login_screen ? "login"
                     : current == playback_screen ? "playback" : "other";
    PlayerModel::Snapshot snap = player.snapshot();
    std::string toastNow = sceKernelGetProcessTimeWide() < toastUntilUs ? toastText : "";
    std::string out = "{\"screen\":" + json_quote(name) +
        ",\"app_paused\":" + (paused ? "true" : "false") +
        ",\"cspot\":" + (cspot_started ? "true" : "false") +
        ",\"token\":" + (api.has_token() ? "true" : "false") +
        ",\"net_busy\":" + (net.busy() ? "true" : "false") +
        ",\"keyboard\":" + (Keyboard::Active() ? "true" : "false") +
        ",\"toast\":" + json_quote(toastNow) +
        ",\"track\":{\"name\":" + json_quote(snap.name) +
        ",\"artist\":" + json_quote(snap.artist) +
        ",\"album\":" + json_quote(snap.album) +
        ",\"position_ms\":" + std::to_string(snap.positionMs) +
        ",\"duration_ms\":" + std::to_string(snap.durationMs) +
        ",\"paused\":" + (snap.paused ? "true" : "false") +
        ",\"volume\":" + std::to_string(snap.volume) + "}" +
        ",\"view\":" + (current ? current->debugState() : std::string("{}")) + "}";
    return out;
}

bool GUI::debugCommand(const std::string &cmd, const std::string &arg) {
    if (cmd == "toast") {
        toast(arg);
        return true;
    }
    // Lets the playback UI be exercised without a Spotify login (Vita3K).
    if (cmd == "screen") {
        Screen *target = arg == "login" ? login_screen : arg == "playback" ? playback_screen : nullptr;
        if (target != nullptr) set_screen(target);
        return target != nullptr;
    }
    Screen *current = screen.load();
    return current != nullptr && current->debugCommand(cmd, arg);
}

void GUI::start() {
    InputSnapshot prev = sampleInput();
    int wake = WAKE_FRAMES;
    uint64_t last_present = 0;

    while (isRunning) {
        // Debug server requests are answered even while backgrounded.
        bool devkit = DevKit::pump(this);

        // Backgrounded: the system owns the display. Idle WITHOUT an open ImGui
        // frame so we never hold the GPU mid-frame (that wedges SceGxm).
        if (paused) {
            sceKernelDelayThread(100000);
            continue;
        }

        // Finished network jobs apply their results here, between frames.
        bool changed = net.drainResults() || devkit;
        static_cast<PlaybackScreen*>(playback_screen)->tick();

        uint64_t now = sceKernelGetProcessTimeWide();
        InputSnapshot cur = sampleInput();
        bool dialog = Keyboard::Active();
        if (cur != prev || inputActive(cur) || changed || dialog || Input::animating()) {
            wake = WAKE_FRAMES;
        }
        prev = cur;

        // Interaction: render every vsync'd frame (60 fps). Static UI: drop to
        // ~10 fps full rebuilds (spinners and the scrubber still move).
        if (wake > 0 || (now - last_present) >= IDLE_FRAME_US) {
            // While the IME is up it owns the touch screen and the buttons.
            Input::new_frame(!dialog);
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(960.0f, 544.0f), ImGuiCond_Always);
            if (ImGui::Begin("Spotivita", nullptr, WINDOW_FLAGS | ImGuiWindowFlags_NoScrollbar |
                                                  ImGuiWindowFlags_NoScrollWithMouse |
                                                  ImGuiWindowFlags_NoBringToFrontOnFocus)) {
                Screen *current = screen.load();
                if (current) {
                    current->draw();
                }
            }
            ImGui::End();
            drawToast();
            ImGui::Render();
            Render::begin_frame();
            Render::draw(ImGui::GetDrawData());
            Render::end_frame(dialog);
            if (dialog) {
                Keyboard::Poll();
            }
            last_present = now;
            if (wake > 0) wake--;
        } else {
            uint64_t nap = MAX_NAP_US;
            uint64_t until_frame = last_present + IDLE_FRAME_US - now;
            if (until_frame < nap) nap = until_frame;
            sceKernelDelayThread(static_cast<SceUInt32>(nap));
        }
    }

    ImGui::DestroyContext();
}

GUI::~GUI() {
    delete login_screen;
    delete playback_screen;
}
