#pragma once

// Renderer: vita2d + Dear ImGui 1.79 (imgui_vita2d's precompiled GXP shaders).
//
// It replaced vitaGL, whose runtime shader compiler (libshacccg.suprx) had to be
// extracted onto every console by hand and does not exist under Vita3K at all.
// vita2d only needs SceGxm, so the app now boots on a stock Vita and in the
// emulator. The stock imgui_vita2d backend is not used: it loads a Japanese
// system font into our atlas and owns input; this one only draws.

#include <imgui_vita2d/imgui.h>
#include <vita2d.h>
#include <cstdint>

namespace Render {

// vita2d, the ImGui shader programs and the projection. Call before ImGui
// fonts are built and before any texture is created.
void init();
// Uploads the built font atlas; call once after io.Fonts->Build().
void upload_fonts();

// One presented frame: begin_frame, ImGui::Render, draw, end_frame.
void begin_frame();
void draw(ImDrawData *data);
// dialog: let the system common dialog (IME) draw over this frame.
void end_frame(bool dialog);

// GUI thread. RGBA8 pixels to a new texture (nullptr on failure).
vita2d_texture *texture_from_rgba(const uint8_t *rgba, int w, int h);
// GUI thread. Waits for the GPU first: a frame in flight may still sample it.
void free_texture(vita2d_texture *tex);

inline ImTextureID tex_id(vita2d_texture *tex) { return reinterpret_cast<ImTextureID>(tex); }

}  // namespace Render
