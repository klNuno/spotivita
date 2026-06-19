#include "Gui.h"
#include "GuiUtils.h"
#include "Utils.h"
#include <Logger.h>
#include <cstdio>
#include "Font.h"
#include "PlaybackScreen.h"
#include "LoginScreen.h"

void GUI::init() {
    // NOTE: the original working app called vglInitExtended directly. A stray
    // vglUseExtraMem(GL_TRUE) was added during the build-restore and is removed
    // here: it changes vitaGL's internal memory pools and is not needed.
    dbg_mark("G0-vglInit-pre");
    vglInitExtended(0, 960, 544, 0x800000, SCE_GXM_MULTISAMPLE_4X);
    dbg_mark("G1-vglInit-done");

    // ROOT-CAUSE FIX: imgui-vita's NewFrame derives io.DisplaySize from the live
    // GL viewport, and its RenderDrawData early-returns when DisplaySize is 0 --
    // so nothing ImGui-drawn ever shows. Older vitaGL seeded a 960x544 viewport at
    // init; the current one leaves it at 0x0, deadlocking that path forever (the
    // code that would set the viewport never runs). Seed it once, explicitly.
    glViewport(0, 0, 960, 544);
    glScissor(0, 0, 960, 544);
    dbg_mark("G1b-viewport");

    // Setup ImGui binding
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplVitaGL_Init();
    dbg_mark("G2-imgui-init");
    io.MouseDrawCursor = false;

    font = AddDefaultFont(26);
    log_font = AddDefaultFont(12);

    // Add icon font
    ImFontConfig icons_config;
    icons_config.OversampleH = icons_config.OversampleV = 1;
    icons_config.PixelSnapH = true;

    ImWchar playback_ranges[] = {
        0xf144, 0xf144,  // play icon
        0xf28b, 0xf28b,  // pause icon
        0,
    };

    ImWchar ranges[] = {
        0xf048, 0xf048,  // backward icon
        0xf051, 0xf051,  // forward icon
        0xf013, 0xf013,  // cog (settings) icon
        0xf02d, 0xf02d,  // book (log) icon
        0xf002, 0xf002,  // search icon
        0xf001, 0xf001,  // music icon
        0,
    };

    icon_font = io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, 48.0f, NULL, ranges);
    playback_icon_font = io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, 96.0f, NULL, playback_ranges);
    io.Fonts->Build();
    dbg_mark("G3-fonts-built");

    // Setup style
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = BACKGROUND_COLOR;
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;

    ImGui_ImplVitaGL_TouchUsage(true);
    ImGui_ImplVitaGL_UseIndirectFrontTouch(false);
    ImGui_ImplVitaGL_UseRearTouch(false);
    ImGui_ImplVitaGL_GamepadUsage(true);
    ImGui_ImplVitaGL_MouseStickUsage(false);

    login_screen = new LoginScreen(this);
    playback_screen = new PlaybackScreen(this);
    dbg_mark("G4-screens-ready");
}

void GUI::start() {
    bool first = true;
    unsigned long fcount = 0;
    while (isRunning) {
        // When the system backgrounds us (PS button), it owns the display.
        // Idle WITHOUT an open ImGui frame so we never hold the GPU mid-frame:
        // blocking inside ImGui::Begin (as the old code did) starves SceGxm and
        // wedges the device. Skipping the whole frame keeps ImGui state balanced.
        if (paused) {
            sceKernelDelayThread(100000);
            continue;
        }

        if (first) dbg_mark("S1-first-frame-pre");
        // imgui-vita reads DisplaySize from the live GL viewport inside NewFrame
        // and skips ALL rendering when it is 0. The current vitaGL does not keep a
        // viewport seeded across frames, so set it every frame, right before.
        glViewport(0, 0, 960, 544);
        glScissor(0, 0, 960, 544);
        ImGui_ImplVitaGL_NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(960.0f, 544.0f), ImGuiCond_Once);

        if (ImGui::Begin("CSpot", nullptr, WINDOW_FLAGS)) {
            Screen *current = screen.load();
            if (current) {
                current->draw();
            }

            // ImGui::SetNextWindowPos(ImVec2(650, 20), ImGuiCond_FirstUseEver);
            // bool show = true;
            // ImGui::ShowDemoWindow(&show);

            ImGui::End();
        }

        ImGui::Render();
        if (first) {
            ImGuiIO& dio = ImGui::GetIO();
            ImDrawData* dd = ImGui::GetDrawData();
            char b[96];
            snprintf(b, sizeof b, "DBG disp=%.0fx%.0f vtx=%d cmds=%d",
                     dio.DisplaySize.x, dio.DisplaySize.y,
                     dd ? dd->TotalVtxCount : -1, dd ? dd->CmdListsCount : -1);
            dbg_mark(b);
        }
        // DIAGNOSTIC + likely fix: the loop never cleared the framebuffer. A
        // distinctive clear color tells us whether the display surface is live
        // (blue shows) or whether swaps never reach the screen (still spinner).
        glClearColor(0.0f, 0.0f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
        if (first) dbg_mark("S3-pre-swap");
        vglSwapBuffers(GL_FALSE);
        if (first) { dbg_mark("S4-first-swap-done"); first = false; }

        // Render-loop heartbeat: confirms whether frames keep flowing past the
        // first swap (loop alive = display/presentation issue) or stall (a later
        // frame wedges the GPU). Frequent early, then every 60 frames.
        fcount++;
        if (fcount <= 6 || (fcount % 60) == 0) {
            char b[24];
            snprintf(b, sizeof b, "F%lu", fcount);
            dbg_mark(b);
        }
    }

    dbg_mark("S5-loop-exit");
    // ImGui_ImplVitaGL_Shutdown();
    ImGui::DestroyContext();
    // vglEnd() was removed from vitaGL; the process exits right after anyway.
}

GUI::~GUI() {
    delete login_screen;
    delete playback_screen;
}
