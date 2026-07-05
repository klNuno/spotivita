#include "Gui.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <cstdint>
#include <cstdio>
#include <Logger.h>
#include "GuiUtils.h"
#include "Utils.h"
#include "Font.h"
#include "PlaybackScreen.h"
#include "LoginScreen.h"

namespace {

// imgui-vita ships Dear ImGui ~1.61 (2018): none of the modern idle helpers
// exist, so frame-gate by hand. After the last input event, keep rendering a few
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
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);

    ImVec4 base  = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);  // #121212
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
    c[ImGuiCol_ButtonHovered]    = elev;
    c[ImGuiCol_ButtonActive]     = green;
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
}

}  // namespace

void GUI::init() {
    // 4X MSAA: wasteful for a flat UI, but it is the exact init the app has
    // years of on-device proof with (incl. the patched vitaGL toolchain build).
    // Switch to NONE only after verifying it boots on hardware -- the init path
    // is too fragile to change blind (see the black-screen history above).
    vglInitExtended(0, 960, 544, 0x800000, SCE_GXM_MULTISAMPLE_4X);

    // imgui-vita derives io.DisplaySize from the live GL viewport inside NewFrame
    // and skips all rendering when it is 0; current vitaGL doesn't seed one, so
    // set it here and again every rendered frame.
    glViewport(0, 0, 960, 544);
    glScissor(0, 0, 960, 544);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplVitaGL_Init();
    io.MouseDrawCursor = false;

    font = AddDefaultFont(26);
    log_font = AddDefaultFont(12);

    static const ImWchar latin[] = { 0x0020, 0x017F, 0 };
    font_bold = io.Fonts->AddFontFromFileTTF("app0:PlusJakartaSans-Bold.ttf", 30.0f, NULL, latin);

    ImWchar playback_ranges[] = { 0xf144, 0xf144, 0xf28b, 0xf28b, 0 };
    ImWchar ranges[] = {
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
    io.Fonts->Build();

    applySpotifyTheme();

    ImGui_ImplVitaGL_TouchUsage(true);
    ImGui_ImplVitaGL_UseIndirectFrontTouch(false);
    ImGui_ImplVitaGL_UseRearTouch(false);
    ImGui_ImplVitaGL_GamepadUsage(true);
    ImGui_ImplVitaGL_MouseStickUsage(false);

    login_screen = new LoginScreen(this);
    playback_screen = new PlaybackScreen(this);
}

void GUI::start() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    InputSnapshot prev = sampleInput();
    int wake = WAKE_FRAMES;
    uint64_t last_present = 0;

    while (isRunning) {
        // Backgrounded: the system owns the display. Idle WITHOUT an open ImGui
        // frame so we never hold the GPU mid-frame (that wedges SceGxm).
        if (paused) {
            sceKernelDelayThread(100000);
            continue;
        }

        uint64_t now = sceKernelGetProcessTimeWide();
        InputSnapshot cur = sampleInput();
        if (cur != prev || inputActive(cur)) {
            wake = WAKE_FRAMES;
        }
        prev = cur;

        // Interaction: render every vsync'd frame (60 fps). Static UI: drop to
        // ~10 fps full rebuilds. A full (cheap) rebuild instead of re-presenting
        // cached ImDrawData keeps us correct with any imgui backend, and still
        // kills the 60 fps busy loop that burned the battery.
        if (wake > 0 || (now - last_present) >= IDLE_FRAME_US) {
            glViewport(0, 0, 960, 544);
            glScissor(0, 0, 960, 544);
            ImGui_ImplVitaGL_NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(960.0f, 544.0f), ImGuiCond_Once);
            if (ImGui::Begin("psvitify", nullptr, WINDOW_FLAGS)) {
                Screen *current = screen.load();
                if (current) {
                    current->draw();
                }
                ImGui::End();
            }
            ImGui::Render();
            glClearColor(0.07f, 0.07f, 0.07f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
            vglSwapBuffers(GL_FALSE);
            last_present = now;
            if (wake > 0) wake--;
            // Frame is closed: safe point for ALL blocking network the UI queued
            // (cover art, playlists, tracks, search, play/seek/shuffle/volume).
            // Running it here (post-swap, no open frame) means a slow request
            // pauses the loop but never holds the GPU mid-frame -> no SceGxm wedge.
            ((PlaybackScreen*) playback_screen)->runDeferred();
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
