#include "Input.h"

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <imgui_vita.h>
#include <vitaGL.h>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>  // NOLINT

namespace Input {
namespace {

struct Frame {
    bool touch = false;
    float x = 0.0f, y = 0.0f;
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// A finger has to travel this far before a press becomes a scroll.
const float SCROLL_SLOP = 14.0f;
// Inertia decay per 60 Hz frame, and the speed below which it stops.
const float INERTIA_DECAY = 0.94f;
const float INERTIA_MIN = 0.4f;

std::mutex g_injectMutex;
std::deque<Frame> g_injected;

uint32_t g_confirm = SCE_CTRL_CROSS;
uint32_t g_cancel = SCE_CTRL_CIRCLE;
uint64_t g_lastTime = 0;

Frame g_prev;
uint32_t g_pressed = 0;
float g_startY = 0.0f;
float g_lastY = 0.0f;
bool g_scrolling = false;
bool g_touchStarted = false;   // first frame of a touch: scroll areas claim it
bool g_releasedAt = false;     // release frame: keep the pointer for the click
ImGuiID g_owner = 0;           // scroll area the finger went down in
float g_dragDy = 0.0f;         // scroll to apply this frame
float g_velocity = 0.0f;
ImGuiID g_inertiaOwner = 0;
float g_inertia = 0.0f;
float g_stickY = 0.0f;

Frame sampleHardware() {
    Frame f;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
        f.buttons = pad.buttons;
        f.lx = pad.lx; f.ly = pad.ly;
        f.rx = pad.rx; f.ry = pad.ry;
    }
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0) {
        // The front panel reports in 1920x1088 units: half of that is pixels.
        f.touch = true;
        f.x = touch.report[0].x * 0.5f;
        f.y = touch.report[0].y * 0.5f;
    }
    return f;
}

float stickAxis(uint8_t v) {
    float a = (static_cast<float>(v) - 128.0f) / 127.0f;
    if (fabsf(a) < 0.25f) return 0.0f;
    return a;
}

}  // namespace

void init() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    // Japanese consoles confirm with circle. AppUtil may already be initialised
    // by another module; a second init just returns an error we can ignore.
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter) >= 0 &&
        enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
        g_confirm = SCE_CTRL_CIRCLE;
        g_cancel = SCE_CTRL_CROSS;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.MouseDrawCursor = false;
}

void new_frame(bool acceptInput) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(960.0f, 544.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    uint64_t now = sceKernelGetProcessTimeWide();
    io.DeltaTime = g_lastTime > 0 ? static_cast<float>(now - g_lastTime) / 1000000.0f : 1.0f / 60.0f;
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
    g_lastTime = now;

    Frame f;
    bool injected = false;
    {
        std::lock_guard<std::mutex> g(g_injectMutex);
        if (!g_injected.empty()) {
            f = g_injected.front();
            g_injected.pop_front();
            injected = true;
        }
    }
    if (!injected) {
        f = acceptInput ? sampleHardware() : Frame();
    }

    g_pressed = f.buttons & ~g_prev.buttons;

    // Touch state machine: press, maybe turn into a scroll, release.
    g_touchStarted = f.touch && !g_prev.touch;
    g_releasedAt = !f.touch && g_prev.touch;
    g_dragDy = 0.0f;
    if (g_touchStarted) {
        g_startY = g_lastY = f.y;
        g_scrolling = false;
        g_owner = 0;
        g_velocity = 0.0f;
        g_inertia = 0.0f;       // a new touch catches a flung list
        g_inertiaOwner = 0;
    } else if (f.touch) {
        float dy = f.y - g_lastY;
        g_lastY = f.y;
        if (!g_scrolling && g_owner != 0 && fabsf(f.y - g_startY) > SCROLL_SLOP) {
            g_scrolling = true;
            dy = f.y - g_startY;   // no jump: the slop distance scrolls too
        }
        if (g_scrolling) {
            g_dragDy = dy;
            g_velocity = g_velocity * 0.6f + dy * 0.4f;
        }
    } else if (g_releasedAt && g_scrolling) {
        g_inertiaOwner = g_owner;
        g_inertia = g_velocity;
        g_scrolling = false;
    }

    // While scrolling, the pointer leaves the screen: the pressed item sees its
    // press end outside itself, which ImGui treats as "no click".
    if (g_scrolling) {
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MouseDown[0] = false;
    } else if (f.touch || (g_releasedAt && !g_scrolling)) {
        io.MousePos = f.touch ? ImVec2(f.x, f.y) : io.MousePos;
        io.MouseDown[0] = f.touch;
    } else {
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MouseDown[0] = false;
    }
    io.MouseDown[1] = io.MouseDown[2] = false;

    for (int i = 0; i < ImGuiNavInput_COUNT; i++) {
        io.NavInputs[i] = 0.0f;
    }
    io.NavInputs[ImGuiNavInput_Activate]  = (f.buttons & g_confirm) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_Cancel]    = (f.buttons & g_cancel) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_Input]     = (f.buttons & SCE_CTRL_TRIANGLE) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadLeft]  = (f.buttons & SCE_CTRL_LEFT) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadRight] = (f.buttons & SCE_CTRL_RIGHT) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadUp]    = (f.buttons & SCE_CTRL_UP) ? 1.0f : 0.0f;
    io.NavInputs[ImGuiNavInput_DpadDown]  = (f.buttons & SCE_CTRL_DOWN) ? 1.0f : 0.0f;
    float lx = stickAxis(f.lx), ly = stickAxis(f.ly);
    if (lx < 0) io.NavInputs[ImGuiNavInput_LStickLeft] = -lx;
    if (lx > 0) io.NavInputs[ImGuiNavInput_LStickRight] = lx;
    if (ly < 0) io.NavInputs[ImGuiNavInput_LStickUp] = -ly;
    if (ly > 0) io.NavInputs[ImGuiNavInput_LStickDown] = ly;
    g_stickY = stickAxis(f.ry);

    g_prev = f;

    ImGui::NewFrame();
    vglIndexPointerDefault();
}

void scroll_area() {
    ImGuiID id = ImGui::GetID("##touchscroll");
    if (g_touchStarted &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        g_owner = id;
    }
    float maxY = ImGui::GetScrollMaxY();
    float y = ImGui::GetScrollY();
    if (g_scrolling && g_owner == id && g_dragDy != 0.0f) {
        ImGui::SetScrollY(y - g_dragDy);
    } else if (g_inertiaOwner == id && fabsf(g_inertia) > INERTIA_MIN) {
        float next = y - g_inertia;
        if (next <= 0.0f || next >= maxY) {
            g_inertia = 0.0f;   // hit an end: stop instead of pushing forever
        }
        ImGui::SetScrollY(next < 0.0f ? 0.0f : (next > maxY ? maxY : next));
        g_inertia *= INERTIA_DECAY;
    }
    if (g_stickY != 0.0f) {
        // Right stick scrolls the list regardless of the pointer.
        float next = y + g_stickY * 14.0f;
        ImGui::SetScrollY(next < 0.0f ? 0.0f : (next > maxY ? maxY : next));
    }
}

bool animating() {
    if (fabsf(g_inertia) > INERTIA_MIN || g_stickY != 0.0f) {
        return true;
    }
    std::lock_guard<std::mutex> g(g_injectMutex);
    return !g_injected.empty();
}

uint32_t pressed() {
    return g_pressed;
}

void inject_tap(int x, int y) {
    Frame down;
    down.touch = true;
    down.x = static_cast<float>(x);
    down.y = static_cast<float>(y);
    std::lock_guard<std::mutex> g(g_injectMutex);
    g_injected.push_back(down);
    g_injected.push_back(down);
    g_injected.push_back(Frame());
    g_injected.push_back(Frame());
}

void inject_swipe(int x1, int y1, int x2, int y2, int frames) {
    if (frames < 2) frames = 2;
    std::lock_guard<std::mutex> g(g_injectMutex);
    for (int i = 0; i <= frames; i++) {
        Frame f;
        f.touch = true;
        float t = static_cast<float>(i) / frames;
        f.x = x1 + (x2 - x1) * t;
        f.y = y1 + (y2 - y1) * t;
        g_injected.push_back(f);
    }
    g_injected.push_back(Frame());
    g_injected.push_back(Frame());
}

void inject_buttons(uint32_t buttons) {
    Frame f;
    f.buttons = buttons;
    std::lock_guard<std::mutex> g(g_injectMutex);
    g_injected.push_back(f);
    g_injected.push_back(f);
    g_injected.push_back(Frame());
    g_injected.push_back(Frame());
}

}  // namespace Input
