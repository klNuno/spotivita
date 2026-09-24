#include "Render.h"

#include <psp2/gxm.h>
#include <Logger.h>
#include <cstring>

// Shader blobs linked from libimgui_vita2d.a (only these two objects are pulled
// in; its backend object is never referenced).
extern "C" {
extern const SceGxmProgram _binary_assets_imgui_v_cg_gxp_start;
extern const SceGxmProgram _binary_assets_imgui_f_cg_gxp_start;
}

namespace Render {
namespace {

const int SCREEN_W = 960;
const int SCREEN_H = 544;
// vita2d's per-frame scratch memory: ImGui vertices and indices are copied
// here every frame. A full library list is ~150 KB; the default 1 MB pool left
// no margin for the log view.
const unsigned int POOL_SIZE = 4 * 1024 * 1024;

SceGxmShaderPatcherId g_vertexId;
SceGxmShaderPatcherId g_fragmentId;
SceGxmVertexProgram *g_vertex = nullptr;
SceGxmFragmentProgram *g_fragment = nullptr;
const SceGxmProgramParameter *g_wvp = nullptr;
float g_ortho[16];
vita2d_texture *g_font = nullptr;
bool g_poolWarned = false;

const SceGxmProgram *vertexProgram() { return &_binary_assets_imgui_v_cg_gxp_start; }
const SceGxmProgram *fragmentProgram() { return &_binary_assets_imgui_f_cg_gxp_start; }

// Same matrix as the stock imgui_vita2d backend (near 0, far 1), which these
// shaders were written against.
void orthographic(float *m, float l, float r, float b, float t) {
    memset(m, 0, 16 * sizeof(float));
    m[0x0] = 2.0f / (r - l);
    m[0xC] = -(r + l) / (r - l);
    m[0x5] = 2.0f / (t - b);
    m[0xD] = -(t + b) / (t - b);
    m[0xA] = -2.0f;
    m[0xE] = 1.0f;
    m[0xF] = 1.0f;
}

bool createPrograms() {
    SceGxmShaderPatcher *patcher = vita2d_get_shader_patcher();
    if (sceGxmProgramCheck(vertexProgram()) < 0 || sceGxmProgramCheck(fragmentProgram()) < 0 ||
        sceGxmShaderPatcherRegisterProgram(patcher, vertexProgram(), &g_vertexId) < 0 ||
        sceGxmShaderPatcherRegisterProgram(patcher, fragmentProgram(), &g_fragmentId) < 0) {
        return false;
    }
    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(vertexProgram(), "aPosition");
    const SceGxmProgramParameter *uv = sceGxmProgramFindParameterByName(vertexProgram(), "aTexcoord");
    const SceGxmProgramParameter *col = sceGxmProgramFindParameterByName(vertexProgram(), "aColor");
    g_wvp = sceGxmProgramFindParameterByName(vertexProgram(), "wvp");
    if (pos == nullptr || uv == nullptr || col == nullptr || g_wvp == nullptr) {
        return false;
    }

    // ImDrawVert: float pos[2], float uv[2], uint32 col (RGBA bytes).
    static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert layout");
    static_assert(sizeof(ImDrawIdx) == 2, "16-bit indices");
    SceGxmVertexAttribute attrs[3];
    memset(attrs, 0, sizeof(attrs));
    attrs[0].offset = offsetof(ImDrawVert, pos);
    attrs[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[0].componentCount = 2;
    attrs[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);
    attrs[1].offset = offsetof(ImDrawVert, uv);
    attrs[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[1].componentCount = 2;
    attrs[1].regIndex = sceGxmProgramParameterGetResourceIndex(uv);
    attrs[2].offset = offsetof(ImDrawVert, col);
    attrs[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    attrs[2].componentCount = 4;
    attrs[2].regIndex = sceGxmProgramParameterGetResourceIndex(col);
    SceGxmVertexStream stream;
    stream.stride = sizeof(ImDrawVert);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, g_vertexId, attrs, 3, &stream, 1, &g_vertex) < 0) {
        return false;
    }

    SceGxmBlendInfo blend;
    memset(&blend, 0, sizeof(blend));
    blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    // Multisample mode must match the one vita2d_init_advanced picked (NONE).
    return sceGxmShaderPatcherCreateFragmentProgram(patcher, g_fragmentId,
                                                    SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                    SCE_GXM_MULTISAMPLE_NONE, &blend,
                                                    vertexProgram(), &g_fragment) >= 0;
}

}  // namespace

void init() {
    vita2d_init_advanced(POOL_SIZE);
    vita2d_set_clear_color(RGBA8(0x12, 0x12, 0x12, 0xFF));
    if (!createPrograms()) {
        CSPOT_LOG(error, "render: ImGui shader setup failed");
    }
    orthographic(g_ortho, 0.0f, SCREEN_W, SCREEN_H, 0.0f);
}

void upload_fonts() {
    ImGuiIO &io = ImGui::GetIO();
    unsigned char *pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (g_font != nullptr) {
        free_texture(g_font);
    }
    g_font = texture_from_rgba(pixels, w, h);
    io.Fonts->TexID = tex_id(g_font);
    // The GPU copy is all ImGui needs from here on; drop the CPU one.
    io.Fonts->ClearTexData();
}

void begin_frame() {
    // The pool is single-buffered: the previous frame must be off the GPU
    // before its vertices are overwritten.
    vita2d_wait_rendering_done();
    vita2d_pool_reset();
    vita2d_start_drawing();
    vita2d_clear_screen();
}

void draw(ImDrawData *data) {
    if (data == nullptr || g_vertex == nullptr || g_fragment == nullptr) {
        return;
    }
    SceGxmContext *ctx = vita2d_get_context();
    const ImVec2 off = data->DisplayPos;
    int clip[4] = {-1, -1, -1, -1};

    vita2d_enable_clipping();
    for (int n = 0; n < data->CmdListsCount; n++) {
        const ImDrawList *list = data->CmdLists[n];
        size_t vtxBytes = static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        size_t idxBytes = static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (vtxBytes == 0 || idxBytes == 0) {
            continue;
        }
        void *vertices = vita2d_pool_memalign(vtxBytes, sizeof(ImDrawVert));
        uint16_t *indices = static_cast<uint16_t *>(vita2d_pool_memalign(idxBytes, sizeof(void *)));
        if (vertices == nullptr || indices == nullptr) {
            if (!g_poolWarned) {
                CSPOT_LOG(error, "render: frame pool exhausted, dropping draw lists");
                g_poolWarned = true;
            }
            break;
        }
        memcpy(vertices, list->VtxBuffer.Data, vtxBytes);
        memcpy(indices, list->IdxBuffer.Data, idxBytes);

        for (int i = 0; i < list->CmdBuffer.Size; i++) {
            const ImDrawCmd &cmd = list->CmdBuffer[i];
            if (cmd.UserCallback != nullptr) {
                cmd.UserCallback(list, &cmd);
                continue;
            }
            int x0 = static_cast<int>(cmd.ClipRect.x - off.x);
            int y0 = static_cast<int>(cmd.ClipRect.y - off.y);
            int x1 = static_cast<int>(cmd.ClipRect.z - off.x + 0.5f);
            int y1 = static_cast<int>(cmd.ClipRect.w - off.y + 0.5f);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > SCREEN_W) x1 = SCREEN_W;
            if (y1 > SCREEN_H) y1 = SCREEN_H;
            if (x1 <= x0 || y1 <= y0 || cmd.ElemCount == 0 || cmd.TextureId == nullptr) {
                continue;
            }
            // vita2d clips through the stencil buffer and each change costs two
            // full draws, so only touch it when the rectangle really moves.
            if (x0 != clip[0] || y0 != clip[1] || x1 != clip[2] || y1 != clip[3]) {
                vita2d_set_clip_rectangle(x0, y0, x1, y1);
                clip[0] = x0; clip[1] = y0; clip[2] = x1; clip[3] = y1;
            }
            // The clip draws switch programs, so set ours for every command.
            sceGxmSetVertexProgram(ctx, g_vertex);
            sceGxmSetFragmentProgram(ctx, g_fragment);
            void *uniforms = nullptr;
            sceGxmReserveVertexDefaultUniformBuffer(ctx, &uniforms);
            sceGxmSetUniformDataF(uniforms, g_wvp, 0, 16, g_ortho);
            sceGxmSetVertexStream(ctx, 0, vertices);
            vita2d_texture *tex = static_cast<vita2d_texture *>(cmd.TextureId);
            sceGxmSetFragmentTexture(ctx, 0, &tex->gxm_tex);
            sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                       indices + cmd.IdxOffset, cmd.ElemCount);
        }
    }
    vita2d_disable_clipping();
}

void end_frame(bool dialog) {
    vita2d_end_drawing();
    if (dialog) {
        vita2d_common_dialog_update();
    }
    vita2d_swap_buffers();
}

vita2d_texture *texture_from_rgba(const uint8_t *rgba, int w, int h) {
    if (rgba == nullptr || w <= 0 || h <= 0) {
        return nullptr;
    }
    // Default format is U8U8U8U8_ABGR: bytes R, G, B, A in memory, as decoded.
    vita2d_texture *tex = vita2d_create_empty_texture(static_cast<unsigned>(w), static_cast<unsigned>(h));
    if (tex == nullptr) {
        return nullptr;
    }
    uint8_t *dst = static_cast<uint8_t *>(vita2d_texture_get_datap(tex));
    const unsigned stride = vita2d_texture_get_stride(tex);
    const size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; y++) {
        memcpy(dst + static_cast<size_t>(y) * stride, rgba + static_cast<size_t>(y) * row, row);
    }
    vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    // Clamp, or linear filtering blends the opposite edge into the border.
    sceGxmTextureSetUAddrMode(&tex->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&tex->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    return tex;
}

void free_texture(vita2d_texture *tex) {
    if (tex == nullptr) {
        return;
    }
    vita2d_wait_rendering_done();
    vita2d_free_texture(tex);
}

}  // namespace Render
