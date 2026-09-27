#pragma once

#include <imgui_vita2d/imgui.h>
#include <string>

// Spotify's dark palette.
const ImU32 COL_TRACK  = IM_COL32(83, 83, 83, 255);     // #535353
const ImU32 COL_WHITE  = IM_COL32(255, 255, 255, 255);
const ImU32 COL_GREENV = IM_COL32(30, 215, 96, 255);    // #1ED760
const ImU32 COL_GREY   = IM_COL32(179, 179, 179, 255);  // #B3B3B3
const ImU32 COL_DIM    = IM_COL32(110, 110, 110, 255);
const ImU32 COL_CLEAR  = IM_COL32(0, 0, 0, 0);
const ImU32 COL_ROW_HI = IM_COL32(40, 40, 40, 255);
const ImU32 COL_CARD   = IM_COL32(40, 40, 40, 255);     // #282828
const ImU32 COL_DARK   = IM_COL32(18, 18, 18, 255);
const ImU32 COL_LIKED  = IM_COL32(80, 56, 200, 255);    // Liked Songs art

// Height of a list row with a subtitle or art.
const float LIST_ROW_H = 60.0f;

// Drops what the font cannot draw (emoji, CJK, joiners) instead of showing
// '?' boxes. A name made only of such characters stays "?".
std::string displayable(ImFont *font, const std::string &text);
// Cuts text to maxW pixels with a trailing "...", on UTF-8 boundaries.
std::string fitText(ImFont *font, const std::string &raw, float maxW);

// Flat icon button with explicit glyph/background colors.
bool iconButton(const char *label, ImVec2 size, ImU32 fg, ImU32 bg);
bool pillButton(const char *label, ImVec2 size, ImU32 bg, ImU32 fg);
// Filter chip: green when on, sized to its text.
bool chipButton(const char *label, bool on, float height);

// Square art at the left of a list row: an icon on a flat tile, standing in
// for the cover.
struct RowArt {
    const char *icon;     // glyph of the small icon font, or nullptr
    ImU32 bg;
    ImU32 fg;
};

// Full-width list row: optional art (a cover once thumb is loaded, else the
// icon tile), title, optional grey subtitle, optional grey text at the right
// end (a duration), highlight while pressed. Long text ends in "...". Rows
// outside the view only take their space.
bool listRow(const char *id, const std::string &title, const std::string &subtitle,
             float width, ImU32 fg, ImFont *subFont, const RowArt *art = nullptr,
             ImFont *artFont = nullptr, const std::string &right = std::string(),
             ImTextureID thumb = nullptr, bool round = false);

std::string fmtTime(int ms);
// Spinning arc + label, drawn inline.
void Spinner(const char *label);
void greyText(const std::string &text);
