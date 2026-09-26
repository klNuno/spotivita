#pragma once

#include <cstdint>

// Input layer between the Vita hardware and ImGui, replacing the imgui-vita
// touch emulation. That emulation only turned a touch into a click when the
// finger lifted within 250 ms and moved under 10 px, and had no drag-to-scroll,
// so slow taps were lost and long lists could only be scrolled by the thin
// scrollbar. Here a touch is a plain mouse press, a vertical drag inside a
// scroll area scrolls it (with inertia) and cancels the press.
//
// Every frame can also come from an injected script (debug server), which is
// how the app gets driven without a hand on the console.
namespace Input {

void init();

// GUI thread, once per rendered frame: samples the hardware (or the injected
// script), feeds ImGui and starts the ImGui frame. acceptInput=false while a
// system dialog owns the screen.
void new_frame(bool acceptInput);

// Call right after BeginChild of a scrollable child: applies finger drag,
// inertia and right-stick scrolling to it.
void scroll_area();

// Something still needs frames (inertia, injected script in progress).
bool animating();

// Buttons that went down this frame (SCE_CTRL_* mask), for global shortcuts.
uint32_t pressed();

// The system's cancel button (circle, or cross on Japanese consoles) went
// down this frame.
bool back_pressed();

// Debug server, any thread. Queues frames of synthetic input.
void inject_tap(int x, int y);
void inject_swipe(int x1, int y1, int x2, int y2, int frames);
void inject_buttons(uint32_t buttons);

}  // namespace Input
