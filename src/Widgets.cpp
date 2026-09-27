#include "Widgets.h"
#include <imgui_vita2d/imgui_internal.h>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>

// Drops what the font cannot draw (emoji, CJK, joiners) instead of showing
// '?' boxes, and the spaces left around them. A name made only of such
// characters stays "?".
std::string displayable(ImFont *font, const std::string &text) {
    std::string out;
    bool dropped = false;
    const char *p = text.c_str(), *end = p + text.size();
    while (p < end) {
        unsigned int c = 0;
        int len = ImTextCharFromUtf8(&c, p, end);
        if (len <= 0) break;
        bool keep = c < 0x80 || (c <= 0xFFFF &&
                    font->FindGlyphNoFallback(static_cast<ImWchar>(c)) != nullptr);
        if (keep && !(c == ' ' && (out.empty() || out.back() == ' '))) {
            out.append(p, len);
        }
        dropped |= !keep;
        p += len;
    }
    if (!dropped) return text;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string("?") : out;
}

// Cuts text to maxW pixels with a trailing "...", on UTF-8 boundaries.
std::string fitText(ImFont *font, const std::string &raw, float maxW) {
    std::string text = displayable(font, raw);
    float size = font->FontSize;
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= maxW) return text;
    const float dots = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
    size_t end = text.size();
    while (end > 0) {
        do {
            end--;
        } while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80);
        float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str(), text.c_str() + end).x;
        if (w + dots <= maxW) break;
    }
    while (end > 0 && text[end - 1] == ' ') end--;
    return text.substr(0, end) + "...";
}

// Flat icon button with explicit glyph/background colors.
bool iconButton(const char* label, ImVec2 size, ImU32 fg, ImU32 bg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool listRow(const char *id, const std::string &title, const std::string &subtitle,
             float width, ImU32 fg, ImFont *subFont, const RowArt *art,
             ImFont *artFont, const std::string &right, ImTextureID thumb, bool round) {
    const float h = (subtitle.empty() && art == nullptr && thumb == nullptr) ? 50.0f : LIST_ROW_H;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h));
    if (!ImGui::IsItemVisible()) return clicked;
    bool held = ImGui::IsItemActive();
    bool focused = ImGui::IsItemFocused();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (held || focused) {
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), COL_ROW_HI, 6.0f);
    }
    float tx = p.x + 8.0f;
    if (art != nullptr || thumb != nullptr) {
        const float side = 48.0f;
        ImVec2 a(p.x + 6.0f, p.y + (h - side) * 0.5f);
        ImVec2 b(a.x + side, a.y + side);
        float rounding = round ? side * 0.5f : 4.0f;
        if (thumb != nullptr) {
            dl->AddImageRounded(thumb, a, b, ImVec2(0, 0), ImVec2(1, 1), COL_WHITE, rounding);
        } else {
            dl->AddRectFilled(a, b, art->bg, rounding);
        }
        if (thumb == nullptr && art->icon != nullptr && artFont != nullptr) {
            ImVec2 isz = artFont->CalcTextSizeA(artFont->FontSize, FLT_MAX, 0.0f, art->icon);
            dl->AddText(artFont, artFont->FontSize,
                        ImVec2(a.x + (side - isz.x) * 0.5f, a.y + (side - isz.y) * 0.5f),
                        art->fg, art->icon);
        }
        tx = a.x + side + 12.0f;
    }
    float maxW = p.x + width - 8.0f - tx;
    if (!right.empty()) {
        float rw = subFont->CalcTextSizeA(subFont->FontSize, FLT_MAX, 0.0f, right.c_str()).x;
        dl->AddText(subFont, subFont->FontSize,
                    ImVec2(p.x + width - 10.0f - rw, p.y + (h - subFont->FontSize) * 0.5f),
                    COL_GREY, right.c_str());
        maxW -= rw + 14.0f;
    }
    ImFont *font = ImGui::GetFont();
    float ty = subtitle.empty() ? p.y + (h - font->FontSize) * 0.5f : p.y + 6.0f;
    dl->AddText(font, font->FontSize, ImVec2(tx, ty), fg, fitText(font, title, maxW).c_str());
    if (!subtitle.empty()) {
        dl->AddText(subFont, subFont->FontSize, ImVec2(tx, p.y + 8.0f + font->FontSize),
                    COL_GREY, fitText(subFont, subtitle, maxW).c_str());
    }
    return clicked;
}

bool pillButton(const char *label, ImVec2 size, ImU32 bg, ImU32 fg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y * 0.5f);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return r;
}

std::string fmtTime(int ms) {
    if (ms < 0) ms = 0;
    int s = ms / 1000;
    int m = s / 60;
    s %= 60;
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", m, s);
    return std::string(b);
}

// Spinning arc + label, drawn inline.
void Spinner(const char* label) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 9.0f;
    float cx = p.x + r, cy = p.y + r + 4.0f;
    float t = static_cast<float>(ImGui::GetTime());
    float a0 = t * 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathClear();
    dl->PathArcTo(ImVec2(cx, cy), r, a0, a0 + 4.2f, 24);
    dl->PathStroke(COL_GREENV, false, 2.5f);
    ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, r * 2.0f + 4.0f));
    if (label && *label) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    }
}

void greyText(const std::string &text) {
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

bool chipButton(const char *label, bool on, float height) {
    ImFont *font = ImGui::GetFont();
    const char *end = strstr(label, "##");
    if (end == nullptr) end = label + strlen(label);
    float w = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label, end).x + 32.0f;
    return pillButton(label, ImVec2(w, height), on ? COL_GREENV : COL_CARD, on ? COL_DARK : COL_WHITE);
}
