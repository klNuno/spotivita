#include "PlaybackScreen.h"
#include "Font.h"
#include "Gui.h"
#include "GuiUtils.h"
#include "Input.h"
#include "Keyboard.h"
#include "Utils.h"
#include "Config.h"
#include <imgui_vita2d/imgui_internal.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <JSONObject.h>
#include <Logger.h>
#include <cfloat>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

const ImU32 COL_TRACK  = IM_COL32(83, 83, 83, 255);    // #535353
const ImU32 COL_WHITE  = IM_COL32(255, 255, 255, 255);
const ImU32 COL_GREENV = IM_COL32(30, 215, 96, 255);   // #1ED760
const ImU32 COL_GREY   = IM_COL32(179, 179, 179, 255);  // #B3B3B3
const ImU32 COL_DIM    = IM_COL32(110, 110, 110, 255);
const ImU32 COL_CLEAR  = IM_COL32(0, 0, 0, 0);
const ImU32 COL_ROW_HI = IM_COL32(40, 40, 40, 255);
const ImU32 COL_CARD   = IM_COL32(40, 40, 40, 255);     // #282828
const ImU32 COL_DARK   = IM_COL32(18, 18, 18, 255);
const ImU32 COL_LIKED  = IM_COL32(80, 56, 200, 255);    // Liked Songs art

const char *PLAYLIST_CACHE_PATH = "ux0:data/cspot/playlists.json";
const char *COVER_CACHE_DIR = "ux0:data/cspot/cache";
const int COVER_MAX_SIDE = 256;
const int COVER_CACHE_MAX_FILES = 400;
// Consecutive spclient failures before a name/metadata burst gives up: past
// that the network is down and grinding on only starves Mercury.
const int MAX_FAIL_STREAK = 5;
// Rows delivered to the GUI per batch while metadata streams in.
const int TRACK_BATCH = 6;
// Tracks handed to cspot per play: the whole queue goes into every Connect
// state frame, so a 500-track playlist plays from a window around the pick.
const size_t QUEUE_MAX = 100;
const size_t QUEUE_BEFORE = 10;
// Previous restarts the track past this point, like Spotify.
const int PREV_RESTART_MS = 3000;

const char *START_GROUP = "spotify:start-group:";
const char *END_GROUP = "spotify:end-group:";

const char *stateName(LoadState s) {
    switch (s) {
        case LoadState::NONE: return "none";
        case LoadState::LOADING: return "loading";
        case LoadState::LOADED: return "loaded";
        case LoadState::FAILED: return "failed";
    }
    return "?";
}

std::string describeStatus(long status) {
    switch (status) {
        case -1:  return "Spotify changed its search API. Update Spotivita.";
        case 0:   return "No connection to Spotify. Check the Wi-Fi.";
        case 401: return "Spotify session expired. Restart the app.";
        case 403: return "Spotify refused this action (Premium required).";
        case 404: return "Spotify could not find this item.";
        case 429: return "Spotify is rate limiting this app. Try again in a minute.";
        default:  return "Spotify error " + std::to_string(status) + ".";
    }
}

std::string idFromUri(const std::string &uri, const char *prefix) {
    size_t n = strlen(prefix);
    return uri.compare(0, n, prefix) == 0 ? uri.substr(n) : uri;
}

bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, strlen(prefix), prefix) == 0;
}

// Folder names in the rootlist are form-encoded: '+' for space, %XX bytes.
std::string formDecode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            char *end = nullptr;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) {
                out += static_cast<char>(v);
                i += 2;
            } else {
                out += '%';
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

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

// Square art at the left of a library row: an icon on a flat tile, standing in
// for the cover.
struct RowArt {
    const char *icon;     // glyph of the small icon font, or nullptr
    ImU32 bg;
    ImU32 fg;
};

// Full-width list row: optional art tile, title, optional grey subtitle,
// highlight while pressed. Long text ends in "...". Rows outside the view
// only take their space.
bool listRow(const char *id, const std::string &title, const std::string &subtitle,
             float width, ImU32 fg, ImFont *subFont, const RowArt *art = nullptr,
             ImFont *artFont = nullptr) {
    const float h = (subtitle.empty() && art == nullptr) ? 50.0f : 60.0f;
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
    if (art != nullptr) {
        const float side = 48.0f;
        ImVec2 a(p.x + 6.0f, p.y + (h - side) * 0.5f);
        dl->AddRectFilled(a, ImVec2(a.x + side, a.y + side), art->bg, 4.0f);
        if (art->icon != nullptr && artFont != nullptr) {
            ImVec2 isz = artFont->CalcTextSizeA(artFont->FontSize, FLT_MAX, 0.0f, art->icon);
            dl->AddText(artFont, artFont->FontSize,
                        ImVec2(a.x + (side - isz.x) * 0.5f, a.y + (side - isz.y) * 0.5f),
                        art->fg, art->icon);
        }
        tx = a.x + side + 12.0f;
    }
    float maxW = p.x + width - 8.0f - tx;
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

// Touch/mouse horizontal bar. Draws a track + fill (+ optional knob) and returns
// true while held, writing the live held fraction (0..1) to *outFrac. Commit is
// the caller's job, on release.
bool barControl(const char* id, float value, ImVec2 size,
                ImU32 fill, bool knob, float* outFrac) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("bar", size);
    bool held = ImGui::IsItemActive();
    float frac = value;
    if (held) {
        frac = (io.MousePos.x - p.x) / size.x;
        *outFrac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    }
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = p.y + size.y * 0.5f;
    float h = held ? 7.0f : 5.0f;
    dl->AddRectFilled(ImVec2(p.x, cy - h * 0.5f),
                      ImVec2(p.x + size.x, cy + h * 0.5f), COL_TRACK, h * 0.5f);
    dl->AddRectFilled(ImVec2(p.x, cy - h * 0.5f),
                      ImVec2(p.x + size.x * frac, cy + h * 0.5f), fill, h * 0.5f);
    if (knob || held) {
        dl->AddCircleFilled(ImVec2(p.x + size.x * frac, cy), held ? 11.0f : 8.0f, fill);
    }
    ImGui::PopID();
    return held;
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

// Builds a library in rootlist order: folders open and close around their
// playlists.
struct LibraryBuild {
    std::vector<Folder> folders;
    std::vector<Playlist> playlists;
    std::vector<LibraryEntry> order;
    int current = -1;

    void startFolder(const std::string &id, const std::string &name, int parent) {
        Folder f;
        f.id = id;
        f.name = name;
        f.parent = parent;
        folders.push_back(f);
        order.push_back({true, static_cast<int>(folders.size()) - 1});
    }
    void addPlaylist(const std::string &uri, const std::string &name, int folder) {
        Playlist p;
        p.uri = uri;
        p.name = name;
        p.folder = folder;
        playlists.push_back(std::move(p));
        order.push_back({false, static_cast<int>(playlists.size()) - 1});
    }
};

// The library is cached so a relaunch shows it at once; it is refreshed in
// the background as soon as a token exists. Format 2, in rootlist order:
//   {"v":2,"entries":[{"folder":id,"name":n,"parent":p} | {"uri":u,"name":n,"parent":p}]}
// with p the index of an earlier folder, -1 at the root. Format 1 was a flat
// array of {name, uri}.
LibraryBuild loadPlaylistCache() {
    LibraryBuild out;
    std::string buf;
    FILE* f = fopen(PLAYLIST_CACHE_PATH, "rb");
    if (!f) return out;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) buf.append(chunk, n);
    fclose(f);
    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) return out;
    cJSON* entries = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "entries");
    int c = cJSON_IsArray(entries) ? cJSON_GetArraySize(entries) : 0;
    for (int i = 0; i < c; i++) {
        cJSON* it = cJSON_GetArrayItem(entries, i);
        cJSON* nm = cJSON_GetObjectItem(it, "name");
        if (!cJSON_IsString(nm) || !nm->valuestring) continue;
        cJSON* pa = cJSON_GetObjectItem(it, "parent");
        int parent = cJSON_IsNumber(pa) ? pa->valueint : -1;
        if (parent < -1 || parent >= static_cast<int>(out.folders.size())) parent = -1;
        cJSON* ur = cJSON_GetObjectItem(it, "uri");
        cJSON* fo = cJSON_GetObjectItem(it, "folder");
        if (cJSON_IsString(ur) && ur->valuestring) {
            out.addPlaylist(ur->valuestring, nm->valuestring, parent);
        } else if (cJSON_IsString(fo) && fo->valuestring) {
            out.startFolder(fo->valuestring, nm->valuestring, parent);
        }
    }
    cJSON_Delete(root);
    return out;
}

void savePlaylistCache(const std::vector<Folder>& folders, const std::vector<Playlist>& pls,
                       const std::vector<LibraryEntry>& order) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 2);
    cJSON* entries = cJSON_CreateArray();
    for (const auto& e : order) {
        cJSON* o = cJSON_CreateObject();
        if (e.isFolder) {
            const Folder &fo = folders[e.index];
            cJSON_AddStringToObject(o, "folder", fo.id.c_str());
            cJSON_AddStringToObject(o, "name", fo.name.c_str());
            cJSON_AddNumberToObject(o, "parent", fo.parent);
        } else {
            const Playlist &p = pls[e.index];
            if (p.uri == LIKED_SONGS_URI) {
                cJSON_Delete(o);
                continue;
            }
            cJSON_AddStringToObject(o, "uri", p.uri.c_str());
            cJSON_AddStringToObject(o, "name", p.name.c_str());
            cJSON_AddNumberToObject(o, "parent", p.folder);
        }
        cJSON_AddItemToArray(entries, o);
    }
    cJSON_AddItemToObject(root, "entries", entries);
    char* txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;
    FILE* f = fopen(PLAYLIST_CACHE_PATH, "wb");
    if (f) {
        fwrite(txt, 1, strlen(txt), f);
        fclose(f);
    }
    free(txt);
}

bool readWholeFile(const std::string &path, std::string *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return !out->empty();
}

// Keeps the cover cache bounded: past the limit it is simply emptied (covers
// are re-downloaded on demand, and sorting by age would cost a stat per file).
void pruneCoverCache() {
    SceUID d = sceIoDopen(COVER_CACHE_DIR);
    if (d < 0) return;
    std::vector<std::string> names;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        if (!SCE_S_ISDIR(e.d_stat.st_mode)) names.push_back(e.d_name);
    }
    sceIoDclose(d);
    if (static_cast<int>(names.size()) <= COVER_CACHE_MAX_FILES) return;
    for (const auto &n : names) {
        sceIoRemove((std::string(COVER_CACHE_DIR) + "/" + n).c_str());
    }
    CSPOT_LOG(info, "cover cache pruned (%d files)", static_cast<int>(names.size()));
}

// Minimal protobuf wire reader: collect every length-delimited (wire type 2)
// field numbered `want` inside [p,end). Enough to walk playlist4 and metadata
// messages without generating nanopb code. Field numbers:
//   SelectedListContent: attributes=3 (ListAttributes.name=1), contents=5
//   ListItems.items=3, Item.uri=1
//   metadata Track: name=2, artist=4 (Artist.name=2)
// Reads one base-128 varint; false on truncation or overflow.
bool pbVarint(const uint8_t** p, const uint8_t* end, uint64_t* v) {
    *v = 0;
    for (int sh = 0; *p < end && sh < 64; sh += 7) {
        uint8_t b = *(*p)++;
        *v |= static_cast<uint64_t>(b & 0x7F) << sh;
        if (!(b & 0x80)) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<const uint8_t*, size_t>> pbLenFields(
        const uint8_t* p, const uint8_t* end, int want) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    uint64_t key = 0;
    while (p < end && pbVarint(&p, end, &key)) {
        int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        uint64_t len = 0;
        if (wt == 2) {
            if (!pbVarint(&p, end, &len) || len > static_cast<uint64_t>(end - p)) {
                break;
            }
            if (field == want) {
                out.push_back({p, static_cast<size_t>(len)});
            }
        } else if (wt == 0) {
            if (!pbVarint(&p, end, &len)) {
                break;
            }
            len = 0;
        } else if (wt == 5 || wt == 1) {
            len = wt == 5 ? 4 : 8;
            if (static_cast<uint64_t>(end - p) < len) {
                break;
            }
        } else {
            break;
        }
        p += len;
    }
    return out;
}

std::string pbString(const std::pair<const uint8_t*, size_t> &f) {
    return std::string(reinterpret_cast<const char*>(f.first), f.second);
}

// First varint field numbered `want` (0 if absent).
uint64_t pbVarintField(const uint8_t* p, const uint8_t* end, int want) {
    uint64_t key = 0;
    while (p < end && pbVarint(&p, end, &key)) {
        int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        uint64_t v = 0;
        if (wt == 0) {
            if (!pbVarint(&p, end, &v)) break;
            if (field == want) return v;
        } else if (wt == 2) {
            if (!pbVarint(&p, end, &v) || v > static_cast<uint64_t>(end - p)) break;
            p += v;
        } else if (wt == 5 || wt == 1) {
            size_t len = wt == 5 ? 4 : 8;
            if (static_cast<size_t>(end - p) < len) break;
            p += len;
        } else {
            break;
        }
    }
    return 0;
}

// Rootlist (SelectedListContent, decorated): contents=5 { items=3 { uri=1 },
// meta_items=4 { attributes=2 { name=1 } } }. Playlist names come from the
// meta items; a name stays "" when Spotify sent none.
LibraryBuild parseRootlist(const std::string &body) {
    LibraryBuild b;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    auto contents = pbLenFields(data, data + body.size(), 5);
    if (contents.empty()) return b;
    const uint8_t *cp = contents[0].first, *ce = cp + contents[0].second;
    auto items = pbLenFields(cp, ce, 3);
    auto metas = pbLenFields(cp, ce, 4);
    std::vector<std::string> uris;
    size_t playlistCount = 0;
    for (auto &it : items) {
        auto u = pbLenFields(it.first, it.first + it.second, 1);
        uris.push_back(u.empty() ? std::string() : pbString(u[0]));
        if (startsWith(uris.back(), SPOTIFY_PLAYLIST_HEADER)) playlistCount++;
    }
    // One meta item per item, or per playlist: accept either layout.
    bool perItem = metas.size() == items.size();
    bool perPlaylist = !perItem && metas.size() == playlistCount;
    CSPOT_LOG(info, "rootlist: %d items, %d playlists, %d meta items",
              static_cast<int>(items.size()), static_cast<int>(playlistCount),
              static_cast<int>(metas.size()));
    size_t seen = 0;
    for (size_t k = 0; k < uris.size() && k < SPOTIFY_ROOTLIST_LENGTH; k++) {
        const std::string &u = uris[k];
        if (startsWith(u, START_GROUP)) {
            std::string rest = u.substr(strlen(START_GROUP));
            size_t colon = rest.find(':');
            std::string id = rest.substr(0, colon);
            std::string name = colon == std::string::npos ? "" : formDecode(rest.substr(colon + 1));
            b.startFolder(id, name.empty() ? std::string("Folder") : name, b.current);
            b.current = static_cast<int>(b.folders.size()) - 1;
        } else if (startsWith(u, END_GROUP)) {
            if (b.current >= 0) b.current = b.folders[b.current].parent;
        } else if (startsWith(u, SPOTIFY_PLAYLIST_HEADER)) {
            std::string name;
            const std::pair<const uint8_t*, size_t> *m =
                perItem ? &metas[k] : (perPlaylist ? &metas[seen] : nullptr);
            if (m != nullptr) {
                auto attrs = pbLenFields(m->first, m->first + m->second, 2);
                if (!attrs.empty()) {
                    auto nm = pbLenFields(attrs[0].first, attrs[0].first + attrs[0].second, 1);
                    if (!nm.empty()) name = pbString(nm[0]);
                }
            }
            seen++;
            b.addPlaylist(u, name, b.current);
        }
    }
    return b;
}

// Liked Songs page (collection PageResponse): items=1 { uri=1, is_removed=3 },
// next_page_token=2. Appends track URIs, returns the next token ("" at the end).
std::string parseLikedPage(const std::string &body, std::vector<std::string> *uris) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    const uint8_t *end = data + body.size();
    for (auto &it : pbLenFields(data, end, 1)) {
        auto u = pbLenFields(it.first, it.first + it.second, 1);
        if (u.empty() || pbVarintField(it.first, it.first + it.second, 3) != 0) continue;
        std::string uri = pbString(u[0]);
        if (startsWith(uri, SPOTIFY_TRACK_HEADER)) uris->push_back(uri);
    }
    auto next = pbLenFields(data, end, 2);
    return next.empty() ? std::string() : pbString(next[0]);
}

// Item URIs of a SelectedListContent (rootlist or playlist), in order.
std::vector<std::string> parseListUris(const std::string &body, size_t limit) {
    std::vector<std::string> uris;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    auto contents = pbLenFields(data, data + body.size(), 5);
    if (contents.empty()) return uris;
    auto items = pbLenFields(contents[0].first, contents[0].first + contents[0].second, 3);
    for (auto &it : items) {
        if (uris.size() >= limit) break;
        auto u = pbLenFields(it.first, it.first + it.second, 1);
        uris.push_back(u.empty() ? std::string() : pbString(u[0]));
    }
    return uris;
}

// Display name of a SelectedListContent ("" if absent).
std::string parseListName(const std::string &body) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    auto attrs = pbLenFields(data, data + body.size(), 3);
    if (attrs.empty()) return "";
    auto names = pbLenFields(attrs[0].first, attrs[0].first + attrs[0].second, 1);
    return names.empty() ? "" : pbString(names[0]);
}

void parseTrackMeta(const std::string &body, std::string *name, std::string *artist) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    const uint8_t *end = data + body.size();
    auto n = pbLenFields(data, end, 2);
    if (!n.empty()) *name = pbString(n[0]);
    auto artists = pbLenFields(data, end, 4);
    for (auto &a : artists) {
        auto an = pbLenFields(a.first, a.first + a.second, 2);
        if (an.empty()) continue;
        if (!artist->empty()) *artist += ", ";
        *artist += pbString(an[0]);
    }
}

std::string jsonStr(cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItem(o, key);
    return (cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : std::string();
}

int findPlaylist(const std::vector<Playlist> &pls, const std::string &uri) {
    for (size_t i = 0; i < pls.size(); i++) {
        if (pls[i].uri == uri) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

PlaybackScreen::PlaybackScreen(GUI *gui) : Screen(gui) {
    int w = 0, h = 0;
    LoadTextureFromFile("app0:cover_art.png", &placeholder_tex, &w, &h);
    cover_art_tex = placeholder_tex;

    // Cached library: it shows at once, and tick() refreshes it once the token
    // exists (a playlist made on the phone since the last run shows up then).
    LibraryBuild cached = loadPlaylistCache();
    bool haveCache = !cached.order.empty();
    setLibrary(std::move(cached.folders), std::move(cached.playlists), std::move(cached.order));
    if (haveCache) {
        libraryState = LoadState::LOADED;
    }
    gui->net.post([] { pruneCoverCache(); });
}

PlaybackScreen::~PlaybackScreen() {
    if (cover_art_tex != placeholder_tex) {
        Render::free_texture(cover_art_tex);
    }
    Render::free_texture(placeholder_tex);
}

void PlaybackScreen::tick() {
    PlayerModel::Snapshot snap = gui->player.snapshot();
    if (!snap.imageUrl.empty() && snap.imageUrl != coverUrl) {
        fetchCover(snap.imageUrl);
    }
    // Once per run, as soon as a token exists: fetch the library, or refresh
    // the cached one.
    if (!libraryRefreshed && gui->api.has_token()) {
        libraryRefreshed = true;
        loadLibrary();
    }
}

void PlaybackScreen::setLibrary(std::vector<Folder> f, std::vector<Playlist> p,
                                std::vector<LibraryEntry> o) {
    // Liked Songs leads the library, like in Spotify.
    Playlist liked;
    liked.name = "Liked Songs";
    liked.uri = LIKED_SONGS_URI;
    p.insert(p.begin(), std::move(liked));
    for (auto &e : o) {
        if (!e.isFolder) e.index++;
    }
    o.insert(o.begin(), LibraryEntry{false, 0});

    // Keep the tracks already loaded (a running job finds its playlist by URI)
    // and the view the user is in.
    std::string openUri = openIndex >= 0 && openIndex < static_cast<int>(playlists.size())
                              ? playlists[openIndex].uri : "";
    std::string folderId = openFolder >= 0 && openFolder < static_cast<int>(folders.size())
                               ? folders[openFolder].id : "";
    for (auto &np : p) {
        int i = findPlaylist(playlists, np.uri);
        if (i >= 0) {
            np.tracks = std::move(playlists[i].tracks);
            np.tracksState = playlists[i].tracksState;
        }
    }
    folders = std::move(f);
    playlists = std::move(p);
    order = std::move(o);
    openIndex = openUri.empty() ? -1 : findPlaylist(playlists, openUri);
    openFolder = -1;
    for (size_t k = 0; k < folders.size() && !folderId.empty(); k++) {
        if (folders[k].id == folderId) openFolder = static_cast<int>(k);
    }
}

// ---------------------------------------------------------------- network

void PlaybackScreen::fetchCover(const std::string &url) {
    coverUrl = url;
    GUI *g = gui;
    gui->net.post([this, g, url] {
        std::string bytes;
        std::string path = cover_art_path(url);
        bool cached = readWholeFile(path, &bytes);
        if (!cached) {
            uint8_t *buf = NULL;
            int len = download(url.c_str(), &buf);
            if (len > 0 && buf != NULL) bytes.assign(reinterpret_cast<char*>(buf), len);
            free(buf);
        }
        int w = 0, h = 0;
        uint8_t *rgba = bytes.empty() ? NULL
            : decode_image(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                           COVER_MAX_SIDE, &w, &h);
        if (rgba != NULL && !cached) {
            cache_cover_art(url, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        }
        g->net.deliver([this, url, rgba, w, h] {
            if (rgba != NULL && url == coverUrl) {
                vita2d_texture *tex = Render::texture_from_rgba(rgba, w, h);
                if (tex != nullptr) {
                    if (cover_art_tex != placeholder_tex) {
                        Render::free_texture(cover_art_tex);
                    }
                    cover_art_tex = tex;
                }
            }
            free(rgba);
        });
    });
}

void PlaybackScreen::loadLibrary() {
    if (!gui->api.has_token()) {
        gui->toast("Not connected to Spotify yet.");
        return;
    }
    bool cached = libraryState == LoadState::LOADED;
    if (!cached) libraryState = LoadState::LOADING;
    libraryError.clear();
    int gen = ++libraryGen;
    namesLeft = 0;
    std::map<std::string, std::string> known;
    for (const auto &p : playlists) known[p.uri] = p.name;

    GUI *g = gui;
    gui->net.post([this, g, gen, known, cached] {
        ApiResult r = g->api.get_rootlist();
        if (gen != libraryGen) return;
        if (!r.ok()) {
            long status = r.status;
            g->net.deliver([this, gen, status, cached] {
                if (gen != libraryGen) return;
                libraryError = describeStatus(status);
                libraryState = cached ? LoadState::LOADED : LoadState::FAILED;
                if (cached) gui->toast("Could not refresh the library. " + libraryError);
            });
            return;
        }
        // Names the rootlist did not carry: one request per playlist below,
        // showing the cached name meanwhile.
        LibraryBuild b = parseRootlist(r.body);
        std::vector<std::string> uris;
        for (auto &p : b.playlists) {
            if (!p.name.empty()) continue;
            auto it = known.find(p.uri);
            p.name = it != known.end() ? it->second : std::string("Playlist");
            uris.push_back(p.uri);
        }
        auto shared = std::make_shared<LibraryBuild>(std::move(b));
        int missing = static_cast<int>(uris.size());
        g->net.deliver([this, gen, shared, missing] {
            if (gen != libraryGen) return;
            setLibrary(std::move(shared->folders), std::move(shared->playlists),
                       std::move(shared->order));
            libraryState = LoadState::LOADED;
            namesLeft = missing;
            if (missing == 0) savePlaylistCache(folders, playlists, order);
        });
        if (missing == 0) return;

        // Names (and the first track URIs, which come in the same response),
        // one playlist at a time, streamed into the list as they resolve.
        int failStreak = 0;
        for (const auto &uri : uris) {
            if (gen != libraryGen) return;
            ApiResult d = g->api.get_playlist(idFromUri(uri, SPOTIFY_PLAYLIST_HEADER));
            std::string name;
            std::vector<std::string> items;
            if (d.ok()) {
                failStreak = 0;
                name = parseListName(d.body);
                items = parseListUris(d.body, SPOTIFY_PLAYLIST_TRACK_LIMIT);
            } else if (++failStreak >= MAX_FAIL_STREAK) {
                CSPOT_LOG(error, "playlist names: %d failures in a row, stopping", failStreak);
                break;
            }
            g->net.deliver([this, gen, uri, name, items] {
                if (gen != libraryGen) return;
                if (namesLeft > 0) namesLeft--;
                int i = findPlaylist(playlists, uri);
                if (i < 0) return;
                if (!name.empty()) playlists[i].name = name;
                if (playlists[i].tracks.empty() && !items.empty()) {
                    for (size_t k = 0; k < items.size(); k++) {
                        if (items[k].compare(0, strlen(SPOTIFY_TRACK_HEADER), SPOTIFY_TRACK_HEADER) != 0) {
                            continue;   // local files, episodes: not playable here
                        }
                        playlists[i].tracks.push_back({"", "", items[k], static_cast<uint32_t>(k)});
                    }
                }
            });
        }
        g->net.deliver([this, gen] {
            if (gen != libraryGen) return;
            namesLeft = 0;
            savePlaylistCache(folders, playlists, order);
        });
    });
}

// Only one playlist streams its titles at a time. Bumping the generation stops
// the running job; its playlist goes back to NONE so reopening resumes it.
void PlaybackScreen::cancelTrackLoads() {
    tracksGen++;
    for (auto &p : playlists) {
        if (p.tracksState == LoadState::LOADING) p.tracksState = LoadState::NONE;
    }
}

void PlaybackScreen::openPlaylist(int index) {
    if (index < 0 || index >= static_cast<int>(playlists.size())) return;
    openIndex = index;
    Playlist &pl = playlists[index];
    if (pl.tracksState == LoadState::LOADED || pl.tracksState == LoadState::LOADING) return;
    cancelTrackLoads();
    pl.tracksState = LoadState::LOADING;
    int gen = tracksGen;
    std::string uri = pl.uri;
    std::vector<TrackRow> known = pl.tracks;

    GUI *g = gui;
    gui->net.post([this, g, gen, uri, known] {
        std::string plId = idFromUri(uri, SPOTIFY_PLAYLIST_HEADER);
        std::vector<TrackRow> rows = known;
        if (rows.empty()) {
            std::vector<std::string> items;
            long failed = 0;
            if (uri == LIKED_SONGS_URI) {
                std::string token;
                do {
                    ApiResult d = g->api.get_liked_page(token, SPOTIFY_LIKED_PAGE);
                    if (!d.ok()) {
                        if (items.empty()) failed = d.status != 0 ? d.status : -2;
                        break;
                    }
                    token = parseLikedPage(d.body, &items);
                } while (!token.empty() && items.size() < SPOTIFY_LIKED_LIMIT && gen == tracksGen);
            } else {
                ApiResult d = g->api.get_playlist(plId);
                if (d.ok()) {
                    items = parseListUris(d.body, SPOTIFY_PLAYLIST_TRACK_LIMIT);
                } else {
                    failed = d.status != 0 ? d.status : -2;
                }
            }
            if (failed != 0) {
                long status = failed == -2 ? 0 : failed;
                g->net.deliver([this, uri, status] {
                    int i = findPlaylist(playlists, uri);
                    if (i < 0) return;
                    playlists[i].tracksState = LoadState::FAILED;
                    gui->toast(describeStatus(status));
                });
                return;
            }
            for (size_t k = 0; k < items.size(); k++) {
                if (items[k].compare(0, strlen(SPOTIFY_TRACK_HEADER), SPOTIFY_TRACK_HEADER) == 0) {
                    rows.push_back({"", "", items[k], static_cast<uint32_t>(k)});
                }
            }
            g->net.deliver([this, uri, rows] {
                int i = findPlaylist(playlists, uri);
                if (i >= 0 && playlists[i].tracks.empty()) playlists[i].tracks = rows;
            });
        }

        // Titles via spclient metadata, one request per track, streamed in
        // batches.
        std::vector<std::pair<size_t, std::pair<std::string, std::string>>> batch;
        auto flush = [&]() {
            if (batch.empty()) return;
            auto done = batch;
            batch.clear();
            g->net.deliver([this, uri, done] {
                int i = findPlaylist(playlists, uri);
                if (i < 0) return;
                auto &tracks = playlists[i].tracks;
                for (auto &d : done) {
                    if (d.first < tracks.size()) {
                        tracks[d.first].name = d.second.first;
                        tracks[d.first].artist = d.second.second;
                    }
                }
            });
        };
        int failStreak = 0;
        for (size_t k = 0; k < rows.size(); k++) {
            if (gen != tracksGen) return;
            if (!rows[k].name.empty()) continue;   // resumed after a cancel
            ApiResult m = g->api.get_track_metadata(idFromUri(rows[k].uri, SPOTIFY_TRACK_HEADER));
            std::string name, artist;
            if (m.ok()) {
                failStreak = 0;
                parseTrackMeta(m.body, &name, &artist);
            } else if (++failStreak >= MAX_FAIL_STREAK) {
                long status = m.status;
                flush();
                g->net.deliver([this, status] { gui->toast(describeStatus(status)); });
                break;
            }
            batch.push_back({k, {name.empty() ? std::string("Unavailable") : name, artist}});
            if (static_cast<int>(batch.size()) >= TRACK_BATCH) flush();
        }
        flush();
        g->net.deliver([this, uri] {
            int i = findPlaylist(playlists, uri);
            if (i >= 0) playlists[i].tracksState = LoadState::LOADED;
        });
    });
}

void PlaybackScreen::startSearch(const std::string &query) {
    searchQuery = query;
    searchResults.clear();
    searchState = LoadState::LOADING;
    GUI *g = gui;
    gui->net.post([this, g, query] {
        ApiResult r = g->api.search(query, 20);
        std::vector<SearchTrack> found;
        long status = r.status;
        if (r.ok()) {
            // pathfinder searchTracks: data.searchV2.tracksV2.items[].item.data
            // { uri, name, artists.items[].profile.name }
            cJSON *root = cJSON_Parse(r.body.c_str());
            cJSON *data = root ? cJSON_GetObjectItem(root, "data") : NULL;
            cJSON *sv2 = data ? cJSON_GetObjectItem(data, "searchV2") : NULL;
            cJSON *tv2 = sv2 ? cJSON_GetObjectItem(sv2, "tracksV2") : NULL;
            cJSON *items = tv2 ? cJSON_GetObjectItem(tv2, "items") : NULL;
            if (!cJSON_IsArray(items)) status = -1;  // GraphQL errors come back as 200
            int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetObjectItem(cJSON_GetArrayItem(items, i), "item");
                cJSON *t = item ? cJSON_GetObjectItem(item, "data") : NULL;
                std::string name = jsonStr(t, "name"), uri = jsonStr(t, "uri");
                if (name.empty() || uri.empty()) continue;
                std::string artists;
                cJSON *ar = cJSON_GetObjectItem(t, "artists");
                cJSON *arItems = ar ? cJSON_GetObjectItem(ar, "items") : NULL;
                int na = cJSON_IsArray(arItems) ? cJSON_GetArraySize(arItems) : 0;
                for (int k = 0; k < na && k < 3; k++) {
                    cJSON *profile = cJSON_GetObjectItem(cJSON_GetArrayItem(arItems, k), "profile");
                    std::string a = jsonStr(profile, "name");
                    if (a.empty()) continue;
                    if (!artists.empty()) artists += ", ";
                    artists += a;
                }
                found.push_back({artists.empty() ? name : name + "\n" + artists, uri});
            }
            cJSON_Delete(root);
        }
        g->net.deliver([this, query, found, status] {
            if (query != searchQuery) return;   // a newer search replaced this one
            searchResults = found;
            searchState = status >= 200 && status < 300 ? LoadState::LOADED : LoadState::FAILED;
            if (searchState == LoadState::FAILED) gui->toast(describeStatus(status));
        });
    }, true);
}

void PlaybackScreen::reportPlayerError(long status) {
    if (status >= 200 && status < 300) return;
    gui->toast(describeStatus(status));
}

// Playback runs locally through cspot: the Web API player endpoints answer 429
// to tokens minted for this client, whatever the request rate.
void PlaybackScreen::playContext(const std::string &uri, uint32_t offset) {
    int i = findPlaylist(playlists, uri);
    if (i < 0) return;
    std::vector<std::string> uris;
    uint32_t index = 0;
    for (const TrackRow &row : playlists[i].tracks) {
        if (row.position == offset) index = uris.size();
        uris.push_back(row.uri);
    }
    if (uris.empty()) {
        gui->toast("Tracks are still loading");
        return;
    }
    if (uris.size() > QUEUE_MAX) {
        size_t from = index > QUEUE_BEFORE ? index - QUEUE_BEFORE : 0;
        if (from + QUEUE_MAX > uris.size()) from = uris.size() - QUEUE_MAX;
        uris = std::vector<std::string>(uris.begin() + from, uris.begin() + from + QUEUE_MAX);
        index -= from;
    }
    std::string context = uri == LIKED_SONGS_URI
                              ? "spotify:user:" + gui->api.user() + ":collection" : uri;
    gui->playTracksCallback(uris, context, index);
}

void PlaybackScreen::playTrack(const std::string &uri) {
    // Queue the whole result list so next/prev keep working.
    std::vector<std::string> uris;
    uint32_t index = 0;
    for (const SearchTrack &t : searchResults) {
        if (t.uri == uri) index = uris.size();
        uris.push_back(t.uri);
    }
    if (uris.empty()) uris.push_back(uri);
    gui->playTracksCallback(uris, "", index);
}

void PlaybackScreen::sendSeek(int ms) {
    gui->seekCallback(ms);
}

void PlaybackScreen::sendShuffle(bool on) {
    gui->shuffleCallback(on);
}

void PlaybackScreen::sendRepeat(int mode) {
    gui->repeatCallback(mode);
}

// ---------------------------------------------------------------- drawing

void PlaybackScreen::previous(const PlayerModel::Snapshot& snap) {
    if (snap.positionMs > PREV_RESTART_MS && snap.durationMs > 0) {
        gui->player.setPosition(0);
        sendSeek(0);
    } else {
        gui->prevCallback();
    }
}

bool PlaybackScreen::goBack() {
    if (tab == Tab::LOG) {
        tab = Tab::SETTINGS;
        return true;
    }
    if (tab != Tab::LIBRARY) return false;
    if (openIndex >= 0) {
        openIndex = -1;
        cancelTrackLoads();   // stop streaming titles for a list nobody looks at
        return true;
    }
    if (openFolder >= 0 && openFolder < static_cast<int>(folders.size())) {
        openFolder = folders[openFolder].parent;
        return true;
    }
    return false;
}

// Fixed layout for the 512 px the pane has: cover, title, artist, scrubber,
// transport, volume. Every block sits at a set height so nothing gets pushed
// off the bottom by a long title or a bigger font.
void PlaybackScreen::drawNowPlaying(const PlayerModel::Snapshot& snap) {
    float paneW = ImGui::GetContentRegionAvail().x;
    float coverSz = paneW - 120.0f;
    if (coverSz > 210.0f) coverSz = 210.0f;
    if (coverSz < 120.0f) coverSz = 120.0f;

    ImGui::SetCursorPos(ImVec2((paneW - coverSz) * 0.5f, 0.0f));
    ImGui::Image(Render::tex_id(cover_art_tex), ImVec2(coverSz, coverSz));
    float y = coverSz + 14.0f;

    bool idle = snap.durationMs == 0 && snap.artist.empty();
    float textW = paneW - 24.0f;
    std::string title = idle ? std::string("Nothing playing") : snap.name;
    std::string sub = idle ? std::string(gui->cspot_started ? "Pick a playlist, or play from your phone"
                                                            : "Connecting to Spotify...")
                           : (snap.artist.empty() ? snap.album : snap.artist);
    ImGui::SetCursorPosY(y);
    ImGui::PushFont(gui->font_bold);
    TextCentered(fitText(gui->font_bold, title, textW));
    ImGui::PopFont();
    ImGui::SetCursorPosY(y + 36.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    TextCentered(fitText(ImGui::GetFont(), sub, textW));
    ImGui::PopStyleColor();
    y += 76.0f;

    // Scrubber (position interpolated locally; seek committed on release).
    float barW = paneW - 32.0f;
    float frac = (snap.durationMs > 0) ? static_cast<float>(snap.positionMs) / snap.durationMs : 0.0f;
    if (scrubbing) frac = scrubFrac;
    float held = 0.0f;
    ImGui::SetCursorPos(ImVec2(16.0f, y));
    bool nowHeld = barControl("scrub", frac, ImVec2(barW, 24.0f), COL_WHITE, false, &held);
    if (nowHeld && snap.durationMs > 0) {
        scrubbing = true;
        scrubFrac = held;
    } else if (scrubbing) {
        scrubbing = false;
        int ms = static_cast<int>(scrubFrac * snap.durationMs);
        if (ms < 0) ms = 0;
        gui->player.setPosition(ms);   // instant local feedback
        sendSeek(ms);
    }

    int shownMs = scrubbing ? static_cast<int>(scrubFrac * snap.durationMs) : snap.positionMs;
    std::string left = fmtTime(shownMs);
    std::string right = fmtTime(snap.durationMs);
    ImGui::PushFont(gui->log_font);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    ImGui::SetCursorPos(ImVec2(16.0f, y + 26.0f));
    ImGui::TextUnformatted(left.c_str());
    float rw = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SetCursorPos(ImVec2(16.0f + barW - rw, y + 26.0f));
    ImGui::TextUnformatted(right.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    y += 50.0f;

    ImGui::SetCursorPosY(y);
    drawTransport(snap);
    y += 82.0f;

    // Volume (committed to cspot on release).
    float volFrac = snap.volume / 65535.0f;
    if (volSliding) volFrac = volSlideFrac;
    const char *volIcon = volFrac <= 0.01f ? ICON_FA_VOLUME_OFF
                        : (volFrac < 0.5f ? ICON_FA_VOLUME_DOWN : ICON_FA_VOLUME_UP);
    ImGui::SetCursorPos(ImVec2(16.0f, y - 1.0f));
    ImGui::PushFont(gui->small_icon_font);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    ImGui::TextUnformatted(volIcon);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    float volHeld = 0.0f;
    ImGui::SetCursorPos(ImVec2(52.0f, y));
    bool volNow = barControl("vol", volFrac, ImVec2(barW - 36.0f, 22.0f), COL_GREENV, false, &volHeld);
    if (volNow) {
        volSliding = true;
        volSlideFrac = volHeld;
    } else if (volSliding) {
        volSliding = false;
        int v = static_cast<int>(volSlideFrac * 65535.0f);
        gui->player.setVolume(v);   // instant local feedback
        gui->volumeCallback(v);
    }
}

// Spotify order: shuffle / prev / play / next / repeat. While a track loads the
// play button spins, so a tap never looks ignored.
void PlaybackScreen::drawTransport(const PlayerModel::Snapshot& snap) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 50.0f);
    AlignForWidth(52.0f + 64.0f + 72.0f + 64.0f + 52.0f + 8.0f * 4.0f);

    bool ready = gui->cspot_started;
    ImGui::PushFont(gui->icon_font);
    if (iconButton(ICON_FA_RANDOM "##shuffle", ImVec2(52.0f, 64.0f),
                   shuffleOn ? COL_GREENV : COL_GREY, COL_CLEAR) && ready) {
        shuffleOn = !shuffleOn;
        sendShuffle(shuffleOn);
    }
    ImGui::SameLine();
    if (iconButton(ICON_FA_STEP_BACKWARD "##prev", ImVec2(64.0f, 64.0f), COL_WHITE, COL_CLEAR) && ready) {
        previous(snap);
    }
    ImGui::PopFont();
    ImGui::SameLine();

    ImGui::PushFont(gui->playback_icon_font);
    const char* playIcon = snap.paused ? ICON_FA_PLAY_CIRCLE "###pp" : ICON_FA_PAUSE_CIRCLE "###pp";  // NOLINT
    if (iconButton(playIcon, ImVec2(72.0f, 64.0f), snap.loading ? COL_GREY : COL_WHITE, COL_CLEAR) && ready) {
        gui->playToggleCallback();
    }
    if (snap.loading) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->PathClear();
        dl->PathArcTo(c, 33.0f, t, t + 4.2f, 32);
        dl->PathStroke(COL_GREENV, false, 3.0f);
    }
    ImGui::PopFont();
    ImGui::SameLine();

    ImGui::PushFont(gui->icon_font);
    if (iconButton(ICON_FA_STEP_FORWARD "##next", ImVec2(64.0f, 64.0f), COL_WHITE, COL_CLEAR) && ready) {
        gui->nextCallback();
    }
    ImGui::SameLine();
    if (iconButton(ICON_FA_REDO "##repeat", ImVec2(52.0f, 64.0f),
                   repeatMode != 0 ? COL_GREENV : COL_GREY, COL_CLEAR) && ready) {
        repeatMode = (repeatMode + 1) % 3;
        sendRepeat(repeatMode);
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(2);

    // Repeat-one has no separate glyph in the bundled icon range: mark it.
    if (repeatMode == 2) {
        ImVec2 r = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(r.x - 8.0f, r.y - 14.0f), 4.0f, COL_GREENV);
    }
}

// Back arrow and a title on one line, for playlists, folders and the log.
bool PlaybackScreen::drawBackHeader(const std::string &title, float avail) {
    ImGui::PushFont(gui->icon_font);
    bool back = iconButton(ICON_FA_ARROW_LEFT "##back", ImVec2(56.0f, 48.0f), COL_WHITE, COL_CLEAR);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(gui->font_bold);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    ImGui::TextUnformatted(fitText(gui->font_bold, title, avail - 72.0f).c_str());
    ImGui::PopFont();
    return back;
}

void PlaybackScreen::drawLibrary(const PlayerModel::Snapshot&, float avail) {
    bool inFolder = openFolder >= 0 && openFolder < static_cast<int>(folders.size());
    if (inFolder) {
        if (drawBackHeader(folders[openFolder].name, avail)) {
            goBack();
            return;
        }
    } else {
        ImGui::PushFont(gui->font_bold);
        ImGui::TextUnformatted("Your Library");
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (namesLeft > 0) {
        Spinner("Updating playlists...");
    }
    static const RowArt kFolderArt = {ICON_FA_FOLDER, COL_CARD, COL_GREY};
    static const RowArt kLikedArt = {ICON_FA_HEART, COL_LIKED, COL_WHITE};
    static const RowArt kListArt = {ICON_FA_MUSIC, COL_CARD, COL_GREY};
    int shown = 0;
    for (size_t k = 0; k < order.size(); k++) {
        const LibraryEntry &e = order[k];
        std::string id = "##e" + std::to_string(k);
        if (e.isFolder) {
            const Folder &f = folders[e.index];
            if (f.parent != openFolder) continue;
            int count = 0;
            for (const auto &p : playlists) count += p.folder == e.index ? 1 : 0;
            for (const auto &c : folders) count += c.parent == e.index ? 1 : 0;
            std::string sub = std::to_string(count) + (count == 1 ? " item" : " items");
            if (listRow(id.c_str(), f.name, sub, avail, COL_WHITE, gui->log_font,
                        &kFolderArt, gui->small_icon_font)) {
                openFolder = e.index;
            }
        } else {
            const Playlist &p = playlists[e.index];
            if (p.folder != openFolder) continue;
            bool liked = p.uri == LIKED_SONGS_URI;
            std::string sub = p.tracksState == LoadState::LOADED
                                  ? "Playlist, " + std::to_string(p.tracks.size()) + " songs"
                                  : std::string("Playlist");
            if (listRow(id.c_str(), p.name, sub, avail, COL_WHITE, gui->log_font,
                        liked ? &kLikedArt : &kListArt, gui->small_icon_font)) {
                openPlaylist(e.index);
            }
        }
        shown++;
    }
    if (inFolder && shown == 0) {
        greyText("This folder is empty.");
    }
    if (inFolder) return;
    if (libraryState == LoadState::LOADING) {
        Spinner("Loading your playlists...");
    } else if (!gui->api.has_token()) {
        greyText(gui->cspot_started ? "Signing in to Spotify..." : "Connecting to Spotify...");
    } else if (libraryState == LoadState::FAILED) {
        greyText(libraryError.empty() ? "Could not load your playlists." : libraryError);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (pillButton("Try again", ImVec2(160.0f, 44.0f), COL_WHITE, COL_DARK)) {
            loadLibrary();
        }
    } else if (libraryState == LoadState::LOADED && order.size() <= 1) {
        greyText("No playlists yet.");
    }
}

void PlaybackScreen::drawPlaylist(const PlayerModel::Snapshot& snap, float avail) {
    Playlist& pl = playlists[openIndex];
    if (drawBackHeader(pl.name, avail)) {
        goBack();
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    if (pillButton("Play", ImVec2(120.0f, 44.0f), COL_GREENV, COL_DARK)) {
        playContext(pl.uri, pl.tracks.empty() ? 0 : pl.tracks[0].position);
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (pl.tracks.empty()) {
        if (pl.tracksState == LoadState::LOADING) {
            Spinner("Loading tracks...");
        } else if (pl.tracksState == LoadState::FAILED) {
            greyText("Could not load this playlist.");
            if (pillButton("Try again", ImVec2(160.0f, 44.0f), COL_WHITE, COL_DARK)) {
                pl.tracksState = LoadState::NONE;
                openPlaylist(openIndex);
            }
        } else {
            greyText("This playlist has no playable tracks.");
        }
        return;
    }
    for (size_t t = 0; t < pl.tracks.size(); t++) {
        const TrackRow &row = pl.tracks[t];
        std::string id = "##t" + std::to_string(t);
        bool current = !row.name.empty() && row.name == snap.name;
        ImU32 fg = row.name.empty() ? COL_DIM : (current ? COL_GREENV : COL_WHITE);
        if (listRow(id.c_str(), row.name.empty() ? std::string("...") : row.name, row.artist,
                    avail, fg, gui->log_font)) {
            playContext(pl.uri, row.position);
        }
    }
    if (pl.tracksState == LoadState::LOADING) {
        Spinner("");
    }
}

void PlaybackScreen::drawSearch(float avail) {
    ImGui::PushFont(gui->font_bold);
    ImGui::TextUnformatted("Search");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    std::string label = searchQuery.empty() ? std::string("Songs, artists...") : searchQuery;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.05f, 0.5f));
    bool open = pillButton((fitText(ImGui::GetFont(), label, avail - 40.0f) + "##q").c_str(),
                           ImVec2(avail, 46.0f), COL_CARD, searchQuery.empty() ? COL_GREY : COL_WHITE);
    ImGui::PopStyleVar();
    if (open) {
        Keyboard::Open("Search Spotify", searchQuery, [this](const std::string &q) {
            if (!q.empty()) startSearch(q);
        });
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (searchState == LoadState::LOADING) {
        Spinner("Searching...");
    } else if (searchState == LoadState::LOADED && searchResults.empty()) {
        greyText("No results.");
    } else if (searchState == LoadState::FAILED) {
        greyText("Search failed. Tap the field to try again.");
    }
    std::string playing = gui->player.snapshot().name;
    for (size_t i = 0; i < searchResults.size(); i++) {
        const std::string &l = searchResults[i].label;
        size_t nl = l.find('\n');
        std::string id = "##r" + std::to_string(i);
        bool current = l.substr(0, nl) == playing;
        if (listRow(id.c_str(), l.substr(0, nl), nl == std::string::npos ? "" : l.substr(nl + 1),
                    avail, current ? COL_GREENV : COL_WHITE, gui->log_font)) {
            playTrack(searchResults[i].uri);
        }
    }
}

void PlaybackScreen::drawLog(float avail) {
    if (drawBackHeader("Log", avail)) {
        goBack();
        return;
    }
    bool grew = log_snapshot(&logCopy, &logVersion);
    ImGui::PushFont(gui->log_font);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(logCopy.c_str(), logCopy.c_str() + logCopy.size());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    // Follow the tail unless the user scrolled up to read.
    if (grew && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
}

void PlaybackScreen::drawSettings(float avail) {
    ImGui::PushFont(gui->font_bold);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    // Audio quality, Spotify's names for the three Ogg Vorbis bitrates.
    ImGui::TextUnformatted("Audio quality");
    static const struct { const char *label; int kbps; } kQuality[] = {
        {"Low##q96", 96}, {"Normal##q160", 160}, {"Very high##q320", 320},
    };
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float pillW = (avail - spacing * 2.0f) / 3.0f;
    int current = gui->quality_kbps;
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine();
        bool on = current == kQuality[i].kbps;
        if (pillButton(kQuality[i].label, ImVec2(pillW, 44.0f), on ? COL_GREENV : COL_CARD,
                       on ? COL_DARK : COL_WHITE) && !on) {
            gui->qualityCallback(kQuality[i].kbps);
            gui->toast("Quality changes from the next track.");
        }
    }
    ImGui::PushFont(gui->log_font);
    greyText(std::to_string(current) + " kb/s. Higher quality uses more data and battery.");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    if (ImGui::Button("Refresh library", ImVec2(avail, 48.0f))) {
        loadLibrary();
    }
    if (ImGui::Button("Show log", ImVec2(avail, 48.0f))) {
        tab = Tab::LOG;
    }
    if (ImGui::Button("Unlink this Spotify account", ImVec2(avail, 48.0f))) {
        remove(CREDENTIALS_FILE_NAME);
        remove(PLAYLIST_CACHE_PATH);
        gui->isRunning = false;
    }
    if (ImGui::Button("Exit", ImVec2(avail, 48.0f))) {
        gui->isRunning = false;
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::PushFont(gui->log_font);
    greyText("START play/pause, L previous, R next, SELECT next tab, circle back, "
             "right stick scrolls.");
    ImGui::PopFont();
}

void PlaybackScreen::drawBrowse(const PlayerModel::Snapshot& snap) {
    float avail = ImGui::GetContentRegionAvail().x;
    switch (tab) {
        case Tab::LIBRARY:
            if (openIndex >= 0 && openIndex < static_cast<int>(playlists.size())) {
                drawPlaylist(snap, avail);
            } else {
                drawLibrary(snap, avail);
            }
            break;
        case Tab::SEARCH:
            drawSearch(avail);
            break;
        case Tab::LOG:
            drawLog(avail);
            break;
        case Tab::SETTINGS:
            drawSettings(avail);
            break;
    }
}

void PlaybackScreen::drawNav() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushFont(gui->icon_font);

    ImVec2 nb(80.0f, 60.0f);
    AlignForWidth(nb.x * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f);

    // The log hangs off Settings.
    static const struct { const char *icon; Tab tab; } kTabs[] = {
        {ICON_FA_MUSIC "##tl", Tab::LIBRARY}, {ICON_FA_SEARCH "##ts", Tab::SEARCH},
        {ICON_FA_COG "##tc", Tab::SETTINGS},
    };
    Tab shown = tab == Tab::LOG ? Tab::SETTINGS : tab;
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine();
        if (StyleButton(kTabs[i].icon, nb, shown == kTabs[i].tab)) {
            // Tapping Library again goes back to the top of the library.
            if (tab == Tab::LIBRARY && kTabs[i].tab == Tab::LIBRARY) {
                if (openIndex >= 0) cancelTrackLoads();
                openIndex = -1;
                openFolder = -1;
            }
            tab = kTabs[i].tab;
        }
    }

    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

void PlaybackScreen::draw() {
    PlayerModel::Snapshot snap = gui->player.snapshot();

    // Global shortcuts, independent of where the gamepad focus is.
    uint32_t pressed = Input::pressed();
    if (gui->cspot_started) {
        if (pressed & SCE_CTRL_START) gui->playToggleCallback();
        if (pressed & SCE_CTRL_LTRIGGER) previous(snap);
        if (pressed & SCE_CTRL_RTRIGGER) gui->nextCallback();
    }
    if (pressed & SCE_CTRL_SELECT) {
        tab = tab == Tab::LIBRARY ? Tab::SEARCH : (tab == Tab::SEARCH ? Tab::SETTINGS : Tab::LIBRARY);
    }
    if (Input::back_pressed()) {
        goBack();
    }

    float fullW = ImGui::GetContentRegionAvail().x;
    float leftW = fullW * 0.46f;

    // Now-playing is fixed-size: no scrolling, so a press near the bottom
    // can't auto-scroll the pane.
    const ImGuiWindowFlags kNoScroll = ImGuiWindowFlags_NavFlattened |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::BeginChild("nowplaying", ImVec2(leftW, 0.0f), false, kNoScroll);
    drawNowPlaying(snap);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0.0f, 0.0f), false, kNoScroll);
    {
        float navH = 76.0f;
        // One scroll position per view: the ID changes with the view, so
        // opening a playlist or a folder starts at its top.
        std::string browseId = "browse" + std::to_string(static_cast<int>(tab));
        if (tab == Tab::LIBRARY) {
            browseId += "_" + std::to_string(openIndex) + "_" + std::to_string(openFolder);
        }
        ImGui::BeginChild(browseId.c_str(), ImVec2(0.0f, ImGui::GetContentRegionAvail().y - navH),
                          false, ImGuiWindowFlags_NavFlattened);
        Input::scroll_area();
        drawBrowse(snap);
        ImGui::EndChild();

        ImGui::BeginChild("nav", ImVec2(0.0f, 0.0f), false, kNoScroll);
        drawNav();
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

std::string PlaybackScreen::debugState() {
    static const char *kTabs[] = {"library", "search", "log", "settings"};
    PlayerModel::Snapshot snap = gui->player.snapshot();
    std::string out = "{\"tab\":" + json_quote(kTabs[static_cast<int>(tab)]) +
        ",\"quality\":" + std::to_string(gui->quality_kbps.load()) +
        ",\"loading\":" + (snap.loading ? "true" : "false") +
        ",\"shuffle\":" + (shuffleOn ? "true" : "false") +
        ",\"repeat\":" + std::to_string(repeatMode) +
        ",\"library\":{\"state\":" + json_quote(stateName(libraryState)) +
        ",\"error\":" + json_quote(libraryError) +
        ",\"count\":" + std::to_string(playlists.size()) +
        ",\"folders\":" + std::to_string(folders.size()) +
        ",\"names_left\":" + std::to_string(namesLeft) +
        ",\"folder\":" + (openFolder >= 0 && openFolder < static_cast<int>(folders.size())
                              ? json_quote(folders[openFolder].name) : std::string("null")) +
        ",\"view\":[";
    // The rows of the folder shown, folders ending in '/', with the index that
    // "open" or "folder" takes.
    int n = 0;
    for (const auto &e : order) {
        int parent = e.isFolder ? folders[e.index].parent : playlists[e.index].folder;
        if (parent != openFolder) continue;
        if (n++ >= 30) break;
        if (n > 1) out += ",";
        out += e.isFolder ? json_quote(std::to_string(e.index) + ":" + folders[e.index].name + "/")
                          : json_quote(std::to_string(e.index) + ":" + playlists[e.index].name);
    }
    out += "]},\"open\":";
    if (openIndex >= 0 && openIndex < static_cast<int>(playlists.size())) {
        const Playlist &p = playlists[openIndex];
        out += "{\"index\":" + std::to_string(openIndex) + ",\"name\":" + json_quote(p.name) +
               ",\"state\":" + json_quote(stateName(p.tracksState)) +
               ",\"count\":" + std::to_string(p.tracks.size()) + ",\"tracks\":[";
        for (size_t t = 0; t < p.tracks.size() && t < 12; t++) {
            if (t) out += ",";
            out += json_quote(p.tracks[t].name);
        }
        out += "]}";
    } else {
        out += "null";
    }
    out += ",\"search\":{\"query\":" + json_quote(searchQuery) +
           ",\"state\":" + json_quote(stateName(searchState)) +
           ",\"results\":" + std::to_string(searchResults.size()) + "}}";
    return out;
}

bool PlaybackScreen::debugCommand(const std::string &cmd, const std::string &arg) {
    if (cmd == "tab") {
        static const char *kTabs[] = {"library", "search", "log", "settings"};
        for (int i = 0; i < 4; i++) {
            if (arg == kTabs[i]) {
                tab = static_cast<Tab>(i);
                return true;
            }
        }
        return false;
    }
    if (cmd == "search") {
        tab = Tab::SEARCH;
        startSearch(arg);
        return true;
    }
    if (cmd == "open") {
        tab = Tab::LIBRARY;
        openPlaylist(atoi(arg.c_str()));
        return true;
    }
    if (cmd == "folder") {
        int i = atoi(arg.c_str());
        if (i < 0 || i >= static_cast<int>(folders.size())) return false;
        tab = Tab::LIBRARY;
        openIndex = -1;
        openFolder = i;
        return true;
    }
    if (cmd == "back") {
        goBack();
        return true;
    }
    if (cmd == "play") {
        // play N: track N of the open playlist.
        if (openIndex < 0 || openIndex >= static_cast<int>(playlists.size())) return false;
        const Playlist &p = playlists[openIndex];
        size_t t = static_cast<size_t>(atoi(arg.c_str()));
        if (t >= p.tracks.size()) return false;
        playContext(p.uri, p.tracks[t].position);
        return true;
    }
    if (cmd == "quality") {
        int kbps = atoi(arg.c_str());
        if (kbps != 96 && kbps != 160 && kbps != 320) return false;
        gui->qualityCallback(kbps);
        return true;
    }
    if (cmd == "refresh") {
        loadLibrary();
        return true;
    }
    return false;
}
