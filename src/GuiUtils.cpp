#include <psp2/io/stat.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>  // NOLINT
#include <string>
#include "GuiUtils.h"

// Logger. The old version appended to an ImGuiTextBuffer from the cspot thread
// while the GUI thread rendered straight out of it, and cleared it (freeing the
// storage) mid-render: a use-after-free that only the no-op mutex hid. Now all
// access goes through log_mutex and the GUI renders its own copy.
static const size_t LOG_KEEP_BYTES = 16 * 1024;
static std::string s_log;  // NOLINT(runtime/string): guarded by log_mutex
static unsigned s_logVersion = 0;
static FILE *logger_fp = NULL;
static std::mutex log_mutex;

void init_logger() {
    logger_fp = fopen("ux0:data/cspot/log.txt", "w");
}

void flush_logger() {
    std::lock_guard<std::mutex> guard(log_mutex);
    // Guard the NULL case: fflush(NULL) flushes every open stream, not intended.
    if (logger_fp != NULL) {
        fflush(logger_fp);
    }
}

static void appendLocked(const char *fmt, va_list args) {
    char line[1024];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n < 0) {
        return;
    }
    size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
    s_log.append(line, len);
    if (s_log.size() > LOG_KEEP_BYTES) {
        // Drop the oldest half at a line boundary so the view stays readable.
        size_t cut = s_log.find('\n', s_log.size() - LOG_KEEP_BYTES / 2);
        s_log.erase(0, cut == std::string::npos ? s_log.size() - LOG_KEEP_BYTES / 2 : cut + 1);
    }
    s_logVersion++;
    if (logger_fp != NULL) {
        fwrite(line, 1, len, logger_fp);
    }
}

// printf-style sink for cspot/bell logs (see MenuLogger in main.cpp).
int print_to_menu(const char* fmt, ...) {
    std::lock_guard<std::mutex> guard(log_mutex);
    va_list args;
    va_start(args, fmt);
    appendLocked(fmt, args);
    va_end(args);
    return 0;
}

int vprint_to_menu(const char* fmt, va_list args) {
    std::lock_guard<std::mutex> guard(log_mutex);
    appendLocked(fmt, args);
    return 0;
}

bool log_snapshot(std::string *out, unsigned *version) {
    std::lock_guard<std::mutex> guard(log_mutex);
    if (*version == s_logVersion) {
        return false;
    }
    *out = s_log;
    *version = s_logVersion;
    return true;
}

std::string log_tail(size_t bytes) {
    std::lock_guard<std::mutex> guard(log_mutex);
    if (s_log.size() <= bytes) {
        return s_log;
    }
    return s_log.substr(s_log.size() - bytes);
}

ImFont* AddTextFont(const char *path, const char *fallbackPath, float pixel_size) {
    ImGuiIO &io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x017F,  // Basic Latin + Latin-1 Supplement + Latin Extended-A
        0x2010, 0x2027,  // dashes, curly quotes, bullet, ellipsis
        0x2030, 0x203A,
        0x20AC, 0x20AC,  // euro
        0,
    };
    // Plus Jakarta Sans has no Greek or Cyrillic: Roboto fills them in, and
    // whatever Jakarta lacks in the ranges above. The first font wins a glyph.
    static const ImWchar fallback[] = {
        0x0370, 0x03FF,  // Greek
        0x0400, 0x04FF,  // Cyrillic
        0x2010, 0x2027,
        0x2030, 0x203A,
        0x20AC, 0x20AC,
        0,
    };
    // ImGui asserts on a missing file, which killed the app at boot when only
    // eboot.bin was copied over an install without the Roboto files.
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        print_to_menu("font missing: %s\n", path);
        return io.Fonts->AddFontDefault();
    }
    ImFont *font = io.Fonts->AddFontFromFileTTF(path, pixel_size, NULL, ranges);
    if (sceIoGetstat(fallbackPath, &st) < 0) {
        print_to_menu("font missing: %s\n", fallbackPath);
        return font;
    }
    ImFontConfig merge;
    merge.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(fallbackPath, pixel_size, &merge, fallback);
    return font;
}

ImFont* AddDefaultFont(float pixel_size) {
    return AddTextFont("app0:PlusJakartaSans-Regular.ttf", "app0:Roboto-Regular.ttf", pixel_size);
}

void AlignForWidth(float width, float alignment) {
    float avail = ImGui::GetContentRegionAvail().x;
    float off = (avail - width) * alignment;
    if (off > 0.0f)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
}

void TextCentered(const std::string& text) {
    float avail = ImGui::GetContentRegionAvail().x;
    float textWidth = ImGui::CalcTextSize(text.c_str()).x;
    // Long titles would start left of the pane and get clipped on both sides;
    // pin them to the left edge instead so at least the start is readable.
    float x = (avail - textWidth) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (x > 0.0f ? x : 0.0f));
    // TextUnformatted, not Text: track/artist names come from Spotify and can
    // contain '%', which ImGui::Text would interpret as a printf format.
    ImGui::TextUnformatted(text.c_str());
}

bool StyleButton(const char* label, ImVec2 btn_size, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, PLAY_BUTTON_BACKGROUND);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PLAY_BUTTON_BACKGROUND);
        ImGui::PushStyleColor(ImGuiCol_Text, BACKGROUND_COLOR);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, BACKGROUND_COLOR);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BACKGROUND_COLOR);
        ImGui::PushStyleColor(ImGuiCol_Text, PLAY_BUTTON_BACKGROUND);
    }
    bool ret = ImGui::Button(label, btn_size);
    ImGui::PopStyleColor(3);
    return ret;
}

std::string json_quote(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}
