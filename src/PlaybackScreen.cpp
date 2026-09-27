#include "PlaybackScreen.h"
#include "Font.h"
#include "Gui.h"
#include "GuiUtils.h"
#include "Input.h"
#include "Keyboard.h"
#include "Utils.h"
#include "Config.h"
#include "Catalog.h"
#include "Proto.h"
#include "ScreenUtil.h"
#include "Thumbs.h"
#include "Widgets.h"
#include <imgui_vita2d/imgui_internal.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <JSONObject.h>
#include <Logger.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
        case -1:  return "Spotify changed its API. Update Spotivita.";
        case 0:   return "No connection to Spotify. Check the Wi-Fi.";
        case 401: return "Spotify session expired. Restart the app.";
        case 403: return "Spotify refused this action (Premium required).";
        case 404: return "Spotify could not find this item.";
        case 429: return "Spotify is rate limiting this app. Try again in a minute.";
        default:  return "Spotify error " + std::to_string(status) + ".";
    }
}

bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, strlen(prefix), prefix) == 0;
}

int findPlaylist(const std::vector<Playlist> &pls, const std::string &uri) {
    for (size_t i = 0; i < pls.size(); i++) {
        if (pls[i].uri == uri) return static_cast<int>(i);
    }
    return -1;
}

namespace {

const char *PLAYLIST_CACHE_PATH = "ux0:data/cspot/playlists.json";
const char *COVER_CACHE_DIR = "ux0:data/cspot/cache";
// One file per opened playlist: its rows with titles, so a reopen (even of a
// 4000-track list) shows at once and only new tracks need metadata.
const char *TRACK_CACHE_DIR = "ux0:data/cspot/tracks";
const char *SORT_PREFS_PATH = "ux0:data/cspot/sorts.json";
// Tracks per extended-metadata request (about 900 bytes of answer each).
const size_t META_BATCH = 100;
// Track rows have a fixed height so only the visible ones are laid out.
const float TRACK_ROW_H = 60.0f;
// Lists longer than this get the fast-scroll thumb.
const size_t FAST_SCROLL_MIN = 40;
const char *const kSortNames[SORT_COUNT] = {
    "Custom order", "Title", "Artist", "Album", "Recently added", "Duration",
};
const char *const kSortKeys[SORT_COUNT] = {
    "custom", "title", "artist", "album", "added", "duration",
};
// On the sort button, where the full name does not fit.
const char *const kSortShort[SORT_COUNT] = {
    "Custom", "Title", "Artist", "Album", "Added", "Duration",
};
const int COVER_MAX_SIDE = 256;
const int COVER_CACHE_MAX_FILES = 400;
// Consecutive spclient failures before a name/metadata burst gives up: past
// that the network is down and grinding on only starves Mercury.
const int MAX_FAIL_STREAK = 5;
// Tracks handed to cspot per play: the whole queue goes into every Connect
// state frame, so a 500-track playlist plays from a window around the pick.
const size_t QUEUE_MAX = 100;
const size_t QUEUE_BEFORE = 10;
// Previous restarts the track past this point, like Spotify.
const int PREV_RESTART_MS = 3000;
// This Vita started playing last, yet the other device still plays after this
// long: Spotify did not stop it, so the app asks it to pause.
const uint64_t TAKEOVER_GRACE_US = 4000000ULL;

const char *START_GROUP = "spotify:start-group:";
const char *END_GROUP = "spotify:end-group:";
const char *ALBUM_HEADER = "spotify:album:";

std::string idFromUri(const std::string &uri, const char *prefix) {
    size_t n = strlen(prefix);
    return uri.compare(0, n, prefix) == 0 ? uri.substr(n) : uri;
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

// Font Awesome glyph for a Spotify Connect device type.
const char *deviceIcon(const std::string &type) {
    if (type == "SMARTPHONE") return ICON_FA_MOBILE_ALT;
    if (type == "TABLET") return ICON_FA_TABLET_ALT;
    if (type == "COMPUTER" || type == "CHROMEBOOK") return ICON_FA_LAPTOP;
    if (type == "TV" || type == "STB" || type == "CAST_VIDEO") return ICON_FA_TV;
    if (type == "GAME_CONSOLE") return ICON_FA_GAMEPAD;
    if (type == "AUTOMOBILE") return ICON_FA_CAR;
    if (type == "SPEAKER" || type == "CAST_AUDIO" || type == "AUDIO_DONGLE") return ICON_FA_VOLUME_UP;
    return ICON_FA_BROADCAST_TOWER;
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
    void addPlaylist(const std::string &uri, const std::string &name, int folder, int length = 0) {
        Playlist p;
        p.uri = uri;
        p.name = name;
        p.folder = folder;
        p.length = length;
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
            cJSON* len = cJSON_GetObjectItem(it, "n");
            out.addPlaylist(ur->valuestring, nm->valuestring, parent,
                            cJSON_IsNumber(len) ? len->valueint : 0);
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
            if (p.length > 0) cJSON_AddNumberToObject(o, "n", p.length);
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

// Rootlist (SelectedListContent, decorated): contents=5 { items=3 { uri=1 },
// meta_items=4 { attributes=2 { name=1 }, length=3 } }. Playlist names and
// track counts come from the meta items; a name stays "" when Spotify sent none.
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
            int length = 0;
            const std::pair<const uint8_t*, size_t> *m =
                perItem ? &metas[k] : (perPlaylist ? &metas[seen] : nullptr);
            if (m != nullptr) {
                auto attrs = pbLenFields(m->first, m->first + m->second, 2);
                if (!attrs.empty()) {
                    auto nm = pbLenFields(attrs[0].first, attrs[0].first + attrs[0].second, 1);
                    if (!nm.empty()) name = pbString(nm[0]);
                }
                length = static_cast<int>(pbVarintField(m->first, m->first + m->second, 3));
            }
            seen++;
            b.addPlaylist(u, name, b.current, length);
        }
    }
    return b;
}

// One entry of a track list, before its metadata.
struct ListItem {
    std::string uri;
    int64_t addedMs;
};

// Liked Songs page (collection PageResponse): items=1 { uri=1, added_at=2
// (seconds), is_removed=3 }, next_page_token=2. Appends the tracks, returns
// the next token ("" at the end).
std::string parseLikedPage(const std::string &body, std::vector<ListItem> *items) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    const uint8_t *end = data + body.size();
    for (auto &it : pbLenFields(data, end, 1)) {
        const uint8_t *ip = it.first, *ie = it.first + it.second;
        auto u = pbLenFields(ip, ie, 1);
        if (u.empty() || pbVarintField(ip, ie, 3) != 0) continue;
        std::string uri = pbString(u[0]);
        if (!startsWith(uri, SPOTIFY_TRACK_HEADER)) continue;
        items->push_back({uri, static_cast<int64_t>(pbVarintField(ip, ie, 2)) * 1000});
    }
    auto next = pbLenFields(data, end, 2);
    return next.empty() ? std::string() : pbString(next[0]);
}

// Items of a SelectedListContent (rootlist or playlist), in order: contents=5
// { items=3 { uri=1, attributes=2 { timestamp=2 (ms) } } }. The whole list
// comes in one answer (a 3800-track playlist is about 330 KB).
std::vector<ListItem> parseListItems(const std::string &body, size_t limit) {
    std::vector<ListItem> out;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    auto contents = pbLenFields(data, data + body.size(), 5);
    if (contents.empty()) return out;
    auto items = pbLenFields(contents[0].first, contents[0].first + contents[0].second, 3);
    for (auto &it : items) {
        if (out.size() >= limit) break;
        const uint8_t *ip = it.first, *ie = it.first + it.second;
        auto u = pbLenFields(ip, ie, 1);
        auto attrs = pbLenFields(ip, ie, 2);
        int64_t added = attrs.empty() ? 0 : static_cast<int64_t>(
            pbVarintField(attrs[0].first, attrs[0].first + attrs[0].second, 2));
        out.push_back({u.empty() ? std::string() : pbString(u[0]), added});
    }
    return out;
}

// Playable rows of a list (local files and episodes are left out), with the
// titles already known for their URIs.
std::vector<TrackRow> rowsFromItems(const std::vector<ListItem> &items,
                                    const std::vector<TrackRow> &known) {
    std::map<std::string, const TrackRow*> byUri;
    for (const auto &r : known) byUri[r.uri] = &r;
    std::vector<TrackRow> rows;
    rows.reserve(items.size());
    for (size_t k = 0; k < items.size(); k++) {
        if (!startsWith(items[k].uri, SPOTIFY_TRACK_HEADER)) continue;
        TrackRow row;
        auto it = byUri.find(items[k].uri);
        if (it != byUri.end()) row = *it->second;
        row.uri = items[k].uri;
        row.position = static_cast<uint32_t>(k);
        row.addedMs = items[k].addedMs;
        rows.push_back(std::move(row));
    }
    return rows;
}

bool sameRows(const std::vector<TrackRow> &a, const std::vector<TrackRow> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].uri != b[i].uri || a[i].addedMs != b[i].addedMs) return false;
    }
    return true;
}

// Display name of a SelectedListContent ("" if absent).
std::string parseListName(const std::string &body) {
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    auto attrs = pbLenFields(data, data + body.size(), 3);
    if (attrs.empty()) return "";
    auto names = pbLenFields(attrs[0].first, attrs[0].first + attrs[0].second, 1);
    return names.empty() ? "" : pbString(names[0]);
}

// Header of a playlist opened from the catalog: attributes=3 { name=1,
// picture=13 { size=1, url=2 } }, owner_username=16.
struct ListHeader {
    std::string name, picture, owner;
};

ListHeader parseListHeader(const std::string &body) {
    ListHeader h;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    const uint8_t *end = data + body.size();
    h.name = parseListName(body);
    auto attrs = pbLenFields(data, end, 3);
    if (!attrs.empty()) {
        for (const auto &pic : pbLenFields(attrs[0], 13)) {
            auto url = pbLenFields(pic, 2);
            if (url.empty()) continue;
            h.picture = pbString(url[0]);
            break;
        }
    }
    auto owner = pbLenFields(data, end, 16);
    if (!owner.empty()) h.owner = pbString(owner[0]);
    return h;
}

// Track cache: a first line "spotivita-tracks 2", then one row per line,
// tab-separated: uri, title, artist, album, duration ms, added ms, position.
std::string trackCachePath(const std::string &uri) {
    std::string id = uri == LIKED_SONGS_URI ? std::string("liked")
                                            : idFromUri(uri, SPOTIFY_PLAYLIST_HEADER);
    return std::string(TRACK_CACHE_DIR) + "/" + id + ".tsv";
}

std::string cacheField(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

void saveTrackCache(const std::string &uri, const std::vector<TrackRow> &rows) {
    sceIoMkdir(TRACK_CACHE_DIR, 0777);
    std::string out = "spotivita-tracks 2\n";
    for (const auto &r : rows) {
        out += r.uri + "\t" + cacheField(r.name) + "\t" + cacheField(r.artist) + "\t" +
               cacheField(r.album) + "\t" + std::to_string(r.durationMs) + "\t" +
               std::to_string(r.addedMs) + "\t" + std::to_string(r.position) + "\n";
    }
    std::string path = trackCachePath(uri), tmp = path + ".part";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    sceIoRemove(path.c_str());
    if (ok) sceIoRename(tmp.c_str(), path.c_str());
}

std::vector<TrackRow> loadTrackCache(const std::string &uri) {
    std::vector<TrackRow> rows;
    std::string buf;
    if (!readWholeFile(trackCachePath(uri), &buf)) return rows;
    size_t nl = buf.find('\n');
    if (nl == std::string::npos || buf.compare(0, nl, "spotivita-tracks 2") != 0) return rows;
    size_t p = nl + 1;
    while (p < buf.size()) {
        size_t e = buf.find('\n', p);
        if (e == std::string::npos) e = buf.size();
        std::vector<std::string> f;
        size_t s = p;
        while (s <= e && f.size() < 7) {
            size_t t = buf.find('\t', s);
            if (t == std::string::npos || t > e) t = e;
            f.push_back(buf.substr(s, t - s));
            s = t + 1;
        }
        p = e + 1;
        if (f.size() < 7 || !startsWith(f[0], SPOTIFY_TRACK_HEADER)) continue;
        TrackRow r;
        r.uri = f[0];
        r.name = f[1];
        r.artist = f[2];
        r.album = f[3];
        r.durationMs = atoi(f[4].c_str());
        r.addedMs = strtoll(f[5].c_str(), nullptr, 10);
        r.position = static_cast<uint32_t>(strtoul(f[6].c_str(), nullptr, 10));
        rows.push_back(std::move(r));
    }
    return rows;
}

// Unlinking the account drops the lists of the old one.
void clearTrackCache() {
    SceUID d = sceIoDopen(TRACK_CACHE_DIR);
    if (d < 0) return;
    std::vector<std::string> names;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        if (!SCE_S_ISDIR(e.d_stat.st_mode)) names.push_back(e.d_name);
    }
    sceIoDclose(d);
    for (const auto &n : names) {
        sceIoRemove((std::string(TRACK_CACHE_DIR) + "/" + n).c_str());
    }
}

// Sort choices: {"<playlist uri>": [mode, descending]}.
std::map<std::string, std::pair<int, bool>> loadSortPrefs() {
    std::map<std::string, std::pair<int, bool>> out;
    std::string buf;
    if (!readWholeFile(SORT_PREFS_PATH, &buf)) return out;
    cJSON *root = cJSON_Parse(buf.c_str());
    for (cJSON *it = root ? root->child : nullptr; it != nullptr; it = it->next) {
        if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) < 2 || it->string == nullptr) continue;
        int mode = cJSON_GetArrayItem(it, 0)->valueint;
        if (mode < 0 || mode >= SORT_COUNT) continue;
        out[it->string] = {mode, cJSON_IsTrue(cJSON_GetArrayItem(it, 1))};
    }
    cJSON_Delete(root);
    return out;
}

void saveSortPrefs(const std::map<std::string, std::pair<int, bool>> &prefs) {
    cJSON *root = cJSON_CreateObject();
    for (const auto &p : prefs) {
        cJSON *a = cJSON_CreateArray();
        cJSON_AddItemToArray(a, cJSON_CreateNumber(p.second.first));
        cJSON_AddItemToArray(a, cJSON_CreateBool(p.second.second));
        cJSON_AddItemToObject(root, p.first.c_str(), a);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;
    FILE *f = fopen(SORT_PREFS_PATH, "wb");
    if (f) {
        fwrite(txt, 1, strlen(txt), f);
        fclose(f);
    }
    free(txt);
}

// ASCII case-insensitive; other bytes compare as they are.
int compareNoCase(const std::string &a, const std::string &b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
        int ca = tolower(static_cast<unsigned char>(a[i]));
        int cb = tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca - cb;
    }
    return static_cast<int>(a.size()) - static_cast<int>(b.size());
}

std::string lowerAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

// First character of s, upper-cased when ASCII ("#" for digits and signs).
std::string initial(const std::string &s) {
    if (s.empty()) return "?";
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) {
        return isalpha(c) ? std::string(1, static_cast<char>(toupper(c))) : std::string("#");
    }
    size_t n = 1;
    while (n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n++;
    return s.substr(0, n);
}

// Bubble next to the fast-scroll thumb: where in the sort order the thumb is.
std::string scrollLabel(const TrackRow &row, int sort, size_t viewRow) {
    switch (sort) {
        case SORT_TITLE: return initial(row.name);
        case SORT_ARTIST: return initial(row.artist);
        case SORT_ALBUM: return initial(row.album);
        case SORT_ADDED: {
            if (row.addedMs <= 0) return "?";
            time_t t = static_cast<time_t>(row.addedMs / 1000);
            struct tm tmv;
            gmtime_r(&t, &tmv);
            char b[16];
            snprintf(b, sizeof(b), "%04d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
            return b;
        }
        case SORT_DURATION: {
            int s = row.durationMs / 1000;
            char b[16];
            snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
            return b;
        }
        default: return "#" + std::to_string(viewRow + 1);
    }
}

// "3797 songs, 11 h 20 min"
std::string listSummary(size_t count, int64_t totalMs) {
    std::string out = std::to_string(count) + (count == 1 ? " song" : " songs");
    int64_t min = totalMs / 60000;
    if (min >= 60) {
        out += ", " + std::to_string(min / 60) + " h " + std::to_string(min % 60) + " min";
    } else if (min > 0) {
        out += ", " + std::to_string(min) + " min";
    }
    return out;
}

}  // namespace

PlaybackScreen::PlaybackScreen(GUI *gui) : Screen(gui), thumbs(&gui->images) {
    int w = 0, h = 0;
    LoadTextureFromFile("app0:cover_art.png", &placeholder_tex, &w, &h);
    cover_art_tex = placeholder_tex;

    // Cached library: it shows at once, and tick() refreshes it once the token
    // exists (a playlist made on the phone since the last run shows up then).
    sortPrefs = loadSortPrefs();
    shuffleSeed = static_cast<unsigned>(sceKernelGetProcessTimeWide());
    LibraryBuild cached = loadPlaylistCache();
    bool haveCache = !cached.order.empty();
    setLibrary(std::move(cached.folders), std::move(cached.playlists), std::move(cached.order));
    if (haveCache) {
        libraryState = LoadState::LOADED;
    }
    gui->net.post([] { pruneCoverCache(); });
    gui->images.post([] { prune_thumb_cache(); });
}

PlaybackScreen::~PlaybackScreen() {
    if (cover_art_tex != placeholder_tex) {
        Render::free_texture(cover_art_tex);
    }
    Render::free_texture(placeholder_tex);
}

void PlaybackScreen::tick() {
    PlayerModel::Snapshot snap = gui->player.snapshot();
    bool remoteNow = remoteShown(snap);
    // Album and artists of the track shown, for the cover and its buttons.
    std::string nowUri = remoteNow ? remote.trackUri : snap.uri;
    if (startsWith(nowUri, SPOTIFY_TRACK_HEADER) && nowUri != nowLinks.uri) {
        nowLinks = TrackLinks();
        nowLinks.uri = nowUri;
        nowLinks.state = LoadState::LOADING;
        nowPending = 0;
        fetchLinks(nowUri);
    }
    thumbs.trim();
    if (remoteNow) snap = remoteSnapshot();
    if (!snap.imageUrl.empty() && snap.imageUrl != coverUrl) {
        fetchCover(snap.imageUrl);
    }
    // Once per run, as soon as a token exists: fetch the library, or refresh
    // the cached one.
    if (!libraryRefreshed && gui->api.has_token()) {
        libraryRefreshed = true;
        loadLibrary();
    }
    tickPlayback();
}

void PlaybackScreen::tickPlayback() {
    if (gui->queueEnded.exchange(false)) {
        continueQueue();
    }
    PlayerModel::Snapshot snap = gui->player.snapshot();
    tickConnect(snap);
    if (sleepAtUs == 0 && !sleepEndOfTrack) return;
    bool due = sleepAtUs != 0 && sceKernelGetProcessTimeWide() >= sleepAtUs;
    // End of track: just before it ends, or once another one took its place
    // (a skip, or a tick that came too late).
    if (sleepEndOfTrack) {
        due |= snap.name != sleepTrack;
        due |= snap.durationMs > 0 && !snap.paused && snap.positionMs >= snap.durationMs - 400;
    }
    if (!due) return;
    sleepAtUs = 0;
    sleepEndOfTrack = false;
    gui->pauseCallback();
    gui->toast("Sleep timer: paused.");
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
            np.fresh = playlists[i].fresh;
            np.filter = playlists[i].filter;
        }
        auto pref = sortPrefs.find(np.uri);
        if (pref != sortPrefs.end()) {
            np.sort = pref->second.first;
            np.sortDesc = pref->second.second;
        }
    }
    // Albums and playlists opened from the catalog stay after the library.
    for (auto &old : playlists) {
        if (old.external && findPlaylist(p, old.uri) < 0) p.push_back(std::move(old));
    }
    viewDirty = true;
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
            std::vector<TrackRow> rows;
            if (d.ok()) {
                failStreak = 0;
                name = parseListName(d.body);
                rows = rowsFromItems(parseListItems(d.body, SPOTIFY_PLAYLIST_TRACK_LIMIT), {});
            } else if (++failStreak >= MAX_FAIL_STREAK) {
                CSPOT_LOG(error, "playlist names: %d failures in a row, stopping", failStreak);
                break;
            }
            g->net.deliver([this, gen, uri, name, rows] {
                if (gen != libraryGen) return;
                if (namesLeft > 0) namesLeft--;
                int i = findPlaylist(playlists, uri);
                if (i < 0) return;
                if (!name.empty()) playlists[i].name = name;
                if (playlists[i].tracks.empty() && !rows.empty()) {
                    playlists[i].tracks = rows;
                    playlists[i].fresh = true;
                    if (i == openIndex) viewDirty = true;
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
    openList(playlists[index].uri);
}

// A playlist opens from its cache file at once. Then, once per run, the whole
// track list is fetched (one request, or Liked Songs page by page) and merged
// with the titles already known, and the missing titles come in batches of
// META_BATCH through extended-metadata. An album's track list comes from its
// metadata. Lists from the catalog keep no cache file.
void PlaybackScreen::ensureTracks(int index) {
    if (index < 0 || index >= static_cast<int>(playlists.size())) return;
    Playlist &pl = playlists[index];
    if (pl.tracksState == LoadState::LOADED || pl.tracksState == LoadState::LOADING) return;
    cancelTrackLoads();
    pl.tracksState = LoadState::LOADING;
    int gen = tracksGen;
    std::string uri = pl.uri;
    std::vector<TrackRow> known = pl.tracks;
    bool fresh = pl.fresh;
    bool external = pl.external;

    GUI *g = gui;
    gui->net.post([this, g, gen, uri, known, fresh, external] {
        // Replaces the GUI copy when it still holds the same list, or when
        // rows is the fresh list from Spotify.
        auto deliverRows = [this, g, uri](const std::vector<TrackRow> &rows, bool isFresh) {
            g->net.deliver([this, uri, rows, isFresh] {
                int i = findPlaylist(playlists, uri);
                if (i < 0) return;
                Playlist &p = playlists[i];
                if (isFresh || p.tracks.empty() || sameRows(p.tracks, rows)) {
                    p.tracks = rows;
                    if (isFresh) p.fresh = true;
                    if (i == openIndex) viewDirty = true;
                }
            });
        };
        std::vector<TrackRow> rows = known;
        bool dirty = false;

        // Titles saved by an earlier run.
        bool missing = rows.empty();
        for (const auto &r : rows) missing |= r.name.empty();
        if (missing && !external) {
            std::vector<TrackRow> cached = loadTrackCache(uri);
            if (rows.empty()) {
                rows = std::move(cached);
            } else if (!cached.empty()) {
                std::map<std::string, const TrackRow*> byUri;
                for (const auto &c : cached) byUri[c.uri] = &c;
                for (auto &r : rows) {
                    auto it = byUri.find(r.uri);
                    if (!r.name.empty() || it == byUri.end()) continue;
                    r.name = it->second->name;
                    r.artist = it->second->artist;
                    r.album = it->second->album;
                    r.durationMs = it->second->durationMs;
                }
            }
            if (!rows.empty()) deliverRows(rows, false);
        }

        if (!fresh) {
            std::vector<ListItem> items;
            long failed = 0;
            if (uri == LIKED_SONGS_URI) {
                std::string token;
                do {
                    ApiResult d = g->api.get_liked_page(token, SPOTIFY_LIKED_PAGE);
                    if (!d.ok()) {
                        failed = d.status != 0 ? d.status : -2;
                        break;
                    }
                    token = parseLikedPage(d.body, &items);
                } while (!token.empty() && items.size() < SPOTIFY_LIKED_LIMIT && gen == tracksGen);
            } else if (startsWith(uri, ALBUM_HEADER)) {
                ApiResult d = g->api.get_extended({uri}, EXT_ALBUM_V4);
                AlbumMeta album;
                bool ok = false;
                if (d.ok()) {
                    auto entities = parseExtendedEntities(d.body);
                    auto it = entities.find(uri);
                    ok = it != entities.end() && parseAlbumMeta(it->second, &album);
                }
                if (ok) {
                    for (const auto &t : album.trackUris) items.push_back({t, 0});
                    std::string sub = album.type + (album.year > 0 ? ", " + std::to_string(album.year) : "");
                    g->net.deliver([this, uri, album, sub] {
                        int i = findPlaylist(playlists, uri);
                        if (i < 0) return;
                        Playlist &p = playlists[i];
                        if (!album.name.empty()) p.name = album.name;
                        p.subtitle = sub;
                        if (!album.coverUrl.empty()) p.imageUrl = album.coverUrl;
                        if (!album.artists.empty()) p.artists = album.artists;
                    });
                } else {
                    failed = d.ok() ? 404 : (d.status != 0 ? d.status : -2);
                }
            } else {
                ApiResult d = g->api.get_playlist(idFromUri(uri, SPOTIFY_PLAYLIST_HEADER));
                if (d.ok()) {
                    items = parseListItems(d.body, SPOTIFY_PLAYLIST_TRACK_LIMIT);
                    if (external) {
                        ListHeader h = parseListHeader(d.body);
                        g->net.deliver([this, uri, h] {
                            int i = findPlaylist(playlists, uri);
                            if (i < 0) return;
                            Playlist &p = playlists[i];
                            if (!h.name.empty()) p.name = h.name;
                            if (!h.picture.empty()) p.imageUrl = h.picture;
                            if (p.subtitle.empty() && !h.owner.empty()) p.subtitle = "By " + h.owner;
                        });
                    }
                } else {
                    failed = d.status != 0 ? d.status : -2;
                }
            }
            if (gen != tracksGen) return;
            if (failed != 0) {
                long status = failed == -2 ? 0 : failed;
                bool haveRows = !rows.empty();
                g->net.deliver([this, uri, status, haveRows] {
                    int i = findPlaylist(playlists, uri);
                    if (i < 0) return;
                    if (!haveRows) playlists[i].tracksState = LoadState::FAILED;
                    gui->toast((haveRows ? "Showing the saved list. " : "") + describeStatus(status));
                });
                if (!haveRows) return;
            } else {
                std::vector<TrackRow> merged = rowsFromItems(items, rows);
                dirty = !sameRows(merged, rows);
                rows = std::move(merged);
                deliverRows(rows, true);
            }
        }

        std::vector<size_t> todo;
        for (size_t k = 0; k < rows.size(); k++) {
            if (rows[k].name.empty()) todo.push_back(k);
        }
        int failStreak = 0;
        for (size_t b = 0; b < todo.size(); b += META_BATCH) {
            if (gen != tracksGen) {
                if (dirty && !external) saveTrackCache(uri, rows);
                return;
            }
            size_t e = std::min(todo.size(), b + META_BATCH);
            std::vector<std::string> uris;
            for (size_t j = b; j < e; j++) uris.push_back(rows[todo[j]].uri);
            ApiResult m = g->api.get_tracks_metadata(uris);
            if (!m.ok()) {
                CSPOT_LOG(error, "extended-metadata -> %ld", m.status);
                if (++failStreak >= MAX_FAIL_STREAK) {
                    long status = m.status;
                    g->net.deliver([this, status] { gui->toast(describeStatus(status)); });
                    break;
                }
                b -= META_BATCH;   // same batch again
                continue;
            }
            failStreak = 0;
            std::map<std::string, TrackMeta> metas;
            parseExtendedMetadata(m.body, &metas);
            std::vector<std::pair<size_t, TrackRow>> done;
            for (size_t j = b; j < e; j++) {
                TrackRow &r = rows[todo[j]];
                auto it = metas.find(r.uri);
                if (it != metas.end() && !it->second.name.empty()) {
                    r.name = it->second.name;
                    r.artist = it->second.artist;
                    r.album = it->second.album;
                    r.durationMs = it->second.durationMs;
                } else {
                    r.name = "Unavailable";   // removed from Spotify, or not in this country
                }
                done.push_back({todo[j], r});
            }
            dirty = true;
            g->net.deliver([this, uri, done] {
                int i = findPlaylist(playlists, uri);
                if (i < 0) return;
                auto &tracks = playlists[i].tracks;
                for (const auto &d : done) {
                    if (d.first < tracks.size() && tracks[d.first].uri == d.second.uri) {
                        tracks[d.first] = d.second;
                    }
                }
                if (i == openIndex) viewDirty = true;
            });
        }
        if (dirty && !external) saveTrackCache(uri, rows);
        g->net.deliver([this, uri, gen] {
            int i = findPlaylist(playlists, uri);
            if (i >= 0 && gen == tracksGen) playlists[i].tracksState = LoadState::LOADED;
        });
    });
}

void PlaybackScreen::reportPlayerError(long status) {
    if (status >= 200 && status < 300) return;
    gui->toast(describeStatus(status));
}

// ---------------------------------------------------------------- open list

void PlaybackScreen::buildView() {
    viewOf = openIndex;
    viewDirty = false;
    view.clear();
    viewMissing = 0;
    viewTotalMs = 0;
    if (openIndex < 0 || openIndex >= static_cast<int>(playlists.size())) return;
    const Playlist &pl = playlists[openIndex];
    const std::vector<TrackRow> &t = pl.tracks;
    std::string needle = lowerAscii(pl.filter);
    view.reserve(t.size());
    for (size_t i = 0; i < t.size(); i++) {
        const TrackRow &r = t[i];
        if (r.name.empty()) viewMissing++;
        if (!needle.empty() && (r.name.empty() ||
                                (lowerAscii(r.name).find(needle) == std::string::npos &&
                                 lowerAscii(r.artist).find(needle) == std::string::npos &&
                                 lowerAscii(r.album).find(needle) == std::string::npos))) {
            continue;
        }
        view.push_back(static_cast<uint32_t>(i));
        viewTotalMs += r.durationMs;
    }
    const int mode = pl.sort;
    const bool desc = pl.sortDesc;
    auto primary = [mode](const TrackRow &a, const TrackRow &b) -> int {
        switch (mode) {
            case SORT_TITLE: return compareNoCase(a.name, b.name);
            case SORT_ARTIST: {
                int c = compareNoCase(a.artist, b.artist);
                return c != 0 ? c : compareNoCase(a.album, b.album);
            }
            case SORT_ALBUM: return compareNoCase(a.album, b.album);
            case SORT_ADDED: return a.addedMs < b.addedMs ? -1 : (a.addedMs > b.addedMs ? 1 : 0);
            case SORT_DURATION: return a.durationMs - b.durationMs;
            default: return 0;
        }
    };
    std::stable_sort(view.begin(), view.end(), [&](uint32_t x, uint32_t y) {
        const TrackRow &a = t[x], &b = t[y];
        // Rows without metadata (not loaded yet, or unavailable) stay at the
        // end of a sorted list.
        bool ma = a.durationMs == 0, mb = b.durationMs == 0;
        if (mode != SORT_CUSTOM && mode != SORT_ADDED && ma != mb) return mb;
        int c = primary(a, b);
        if (c == 0) c = a.position < b.position ? -1 : (a.position > b.position ? 1 : 0);
        return desc ? c > 0 : c < 0;
    });
}

// The same order again flips its direction, like Spotify.
void PlaybackScreen::setSort(int mode) {
    if (openIndex < 0 || openIndex >= static_cast<int>(playlists.size())) return;
    if (mode < 0 || mode >= SORT_COUNT) return;
    Playlist &pl = playlists[openIndex];
    if (pl.sort == mode) {
        pl.sortDesc = !pl.sortDesc;
    } else {
        pl.sort = mode;
        pl.sortDesc = mode == SORT_ADDED;   // newest first
    }
    if (pl.sort == SORT_CUSTOM && !pl.sortDesc) {
        sortPrefs.erase(pl.uri);
    } else {
        sortPrefs[pl.uri] = {pl.sort, pl.sortDesc};
    }
    saveSortPrefs(sortPrefs);
    viewDirty = true;
    scrollToRow = 0;
}

// Playback runs locally through cspot: the Web API player endpoints answer 429
// to tokens minted for this client, whatever the request rate. The app keeps
// the whole list (queueUris, in play order, already shuffled when shuffling)
// and hands cspot a window of it at a time.
void PlaybackScreen::playView(size_t row, bool shuffle) {
    if (openIndex < 0 || openIndex >= static_cast<int>(playlists.size())) return;
    if (viewDirty || viewOf != openIndex) buildView();
    const Playlist &pl = playlists[openIndex];
    if (view.empty()) {
        gui->toast(pl.tracks.empty() ? "Tracks are still loading." : "No song matches this filter.");
        return;
    }
    std::vector<std::string> uris;
    uris.reserve(view.size());
    for (uint32_t i : view) uris.push_back(pl.tracks[i].uri);
    playUris(std::move(uris), row, shuffle,
             pl.uri == LIKED_SONGS_URI ? "spotify:user:" + gui->api.user() + ":collection" : pl.uri);
}

void PlaybackScreen::playUris(std::vector<std::string> uris, size_t row, bool shuffle,
                              const std::string &context) {
    if (uris.empty()) return;
    size_t start = row < uris.size() ? row : 0;
    if (shuffle) {
        size_t first = row < uris.size() ? row : static_cast<size_t>(rand_r(&shuffleSeed)) % uris.size();
        std::swap(uris[0], uris[first]);
        for (size_t k = uris.size() - 1; k > 1; k--) {
            size_t j = 1 + static_cast<size_t>(rand_r(&shuffleSeed)) % k;
            std::swap(uris[k], uris[j]);
        }
        start = 0;
    }
    queueContext = context;
    queueUris = std::move(uris);
    sendWindow(start);
    if (shuffleOn != shuffle) {
        shuffleOn = shuffle;
        sendShuffle(shuffle);
    }
}

void PlaybackScreen::sendWindow(size_t start) {
    if (start >= queueUris.size()) return;
    size_t from = start > QUEUE_BEFORE ? start - QUEUE_BEFORE : 0;
    if (queueUris.size() > QUEUE_MAX && from + QUEUE_MAX > queueUris.size()) {
        from = queueUris.size() - QUEUE_MAX;
    }
    size_t to = std::min(queueUris.size(), from + QUEUE_MAX);
    std::vector<std::string> window(queueUris.begin() + from, queueUris.begin() + to);
    queueNext = to;
    // A list longer than a window asks cspot to call back at the end of each
    // one; a shorter one repeats through cspot itself.
    gui->playTracksCallback(window, queueContext, static_cast<uint32_t>(start - from),
                            queueUris.size() > QUEUE_MAX);
}

// cspot reached the end of its window (or next was pressed on its last track).
void PlaybackScreen::continueQueue() {
    if (queueNext < queueUris.size()) {
        sendWindow(queueNext);
    } else if (repeatMode == 1 && !queueUris.empty()) {
        sendWindow(0);
    }
}

void PlaybackScreen::locateCurrent(const PlayerModel::Snapshot& snap) {
    if (openIndex < 0 || openIndex >= static_cast<int>(playlists.size())) return;
    if (viewDirty || viewOf != openIndex) buildView();
    const Playlist &pl = playlists[openIndex];
    for (size_t r = 0; r < view.size() && !snap.name.empty(); r++) {
        const TrackRow &t = pl.tracks[view[r]];
        if (snap.uri.empty() ? t.name == snap.name : t.uri == snap.uri) {
            scrollToRow = static_cast<int>(r);
            return;
        }
    }
    gui->toast(snap.name.empty() ? "Nothing is playing." : "The song playing is not in this list.");
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

// ---------------------------------------------------------------- connect

bool PlaybackScreen::otherActive() const {
    return remote.valid && !remote.activeId.empty() && remote.activeId != DEVICE_ID;
}

bool PlaybackScreen::remoteShown(const PlayerModel::Snapshot& local) const {
    return otherActive() && local.paused && !local.loading;
}

void PlaybackScreen::tickConnect(const PlayerModel::Snapshot& local) {
    uint64_t now = sceKernelGetProcessTimeWide();
    bool localPlaying = !local.paused && !local.loading && local.durationMs > 0;
    if (localPlaying && !localWasPlaying) localStartUs = now;
    localWasPlaying = localPlaying;

    ConnectState s = gui->connect.snapshot();
    if (s.version != connectVersion) {
        connectVersion = s.version;
        remote = s;
        // A device starts playing when a cluster first names it active and
        // playing; later clusters about the same playback change nothing.
        std::string playing = s.playing && otherActive() ? s.activeId : "";
        if (!playing.empty() && playing != otherPlayingId) remoteStartUs = now;
        otherPlayingId = playing;
        if (otherActive() && startsWith(s.trackUri, "spotify:track:") && s.trackUri != remoteTrack.uri) {
            fetchRemoteTrack(s.trackUri);
        }
    }
    if (!localPlaying || !otherActive() || !remote.playing) return;
    if (remoteStartUs > localStartUs) {
        // The other device started last: this one steps aside, like Spotify.
        if (handledRemoteStart == remoteStartUs) return;
        handledRemoteStart = remoteStartUs;
        CSPOT_LOG(info, "connect: %s started, pausing here", remote.deviceName.c_str());
        gui->yieldCallback();
        gui->toast("Playing on " + remote.deviceName + ", paused here.");
    } else if (now - localStartUs > TAKEOVER_GRACE_US && handledLocalStart != localStartUs) {
        handledLocalStart = localStartUs;
        CSPOT_LOG(info, "connect: %s still plays, pausing it", remote.deviceName.c_str());
        remoteCommand("pause");
        gui->toast("Paused " + remote.deviceName + ".");
    }
}

// The pane for the other device: the cluster's metadata when it has some,
// else what fetchRemoteTrack found for its URI.
PlayerModel::Snapshot PlaybackScreen::remoteSnapshot() const {
    PlayerModel::Snapshot s;
    bool meta = remoteTrack.uri == remote.trackUri;
    s.name = !remote.title.empty() ? remote.title : (meta ? remoteTrack.name : "");
    s.artist = !remote.artist.empty() ? remote.artist : (meta ? remoteTrack.artist : "");
    s.album = meta ? remoteTrack.album : "";
    s.imageUrl = !remote.imageUrl.empty() ? remote.imageUrl : (meta ? remoteTrack.imageUrl : "");
    s.durationMs = remote.durationMs > 0 ? remote.durationMs : (meta ? remoteTrack.durationMs : 0);
    int64_t pos = remote.positionMs;
    if (remote.playing) pos += static_cast<int64_t>((sceKernelGetProcessTimeWide() - remote.receivedAtUs) / 1000);
    if (s.durationMs > 0 && pos > s.durationMs) pos = s.durationMs;
    s.positionMs = pos < 0 ? 0 : static_cast<int>(pos);
    s.paused = !remote.playing;
    s.volume = remote.volume >= 0 ? remote.volume : gui->player.snapshot().volume;
    return s;
}

void PlaybackScreen::fetchRemoteTrack(const std::string &uri) {
    remoteTrack = RemoteTrack();
    remoteTrack.uri = uri;
    GUI *g = gui;
    gui->net.post([this, g, uri] {
        ApiResult m = g->api.get_tracks_metadata({uri});
        std::map<std::string, TrackMeta> metas;
        if (m.ok()) parseExtendedMetadata(m.body, &metas);
        auto it = metas.find(uri);
        if (it == metas.end()) return;
        TrackMeta t = it->second;
        g->net.deliver([this, uri, t] {
            if (remoteTrack.uri != uri) return;
            remoteTrack.name = t.name;
            remoteTrack.artist = t.artist;
            remoteTrack.album = t.album;
            remoteTrack.imageUrl = t.coverUrl;
            remoteTrack.durationMs = t.durationMs;
        });
    }, true);
}

// Sent to the device the pane shows. The local copy of the cluster moves at
// once, so the button and the clock answer before Spotify does.
void PlaybackScreen::remoteCommand(const std::string &endpoint, int64_t valueMs) {
    if (!otherActive()) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (endpoint == "pause" || endpoint == "resume" || endpoint == "seek_to") {
        int64_t pos = remote.positionMs;
        if (remote.playing) pos += static_cast<int64_t>((now - remote.receivedAtUs) / 1000);
        remote.positionMs = endpoint == "seek_to" ? valueMs : pos;
        remote.receivedAtUs = now;
        if (endpoint != "seek_to") remote.playing = endpoint == "resume";
        if (endpoint == "resume") remoteStartUs = now;
    }
    std::string target = remote.activeId, name = remote.deviceName;
    GUI *g = gui;
    gui->net.post([g, target, name, endpoint, valueMs] {
        if (g->connect.command(target, endpoint, valueMs)) return;
        g->net.deliver([g, name] { g->toast("Could not reach " + name + "."); });
    }, true);
}

// Moves the other device's playback to this Vita (Spotify sends cspot a load
// frame with its queue and position).
void PlaybackScreen::playHere() {
    if (!otherActive()) return;
    std::string name = remote.deviceName;
    GUI *g = gui;
    gui->net.post([g, name] {
        if (g->connect.transfer(DEVICE_ID)) return;
        g->net.deliver([g, name] { g->toast("Could not take playback from " + name + "."); });
    }, true);
    gui->toast("Moving playback here.");
}

// "Playing on <device>" at the bottom of the pane, with a button that brings
// the playback here.
void PlaybackScreen::drawRemoteBar(float paneW) {
    float h = 40.0f;
    float y = ImGui::GetWindowHeight() - h - 8.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRectFilled(ImVec2(wp.x + 8.0f, wp.y + y), ImVec2(wp.x + paneW - 8.0f, wp.y + y + h),
                      COL_CARD, 8.0f);
    ImFont *icon = gui->small_icon_font;
    dl->AddText(icon, icon->FontSize, ImVec2(wp.x + 18.0f, wp.y + y + (h - icon->FontSize) * 0.5f),
                COL_GREENV, deviceIcon(remote.deviceType));
    float pillW = 112.0f;
    std::string label = (remote.playing ? "Playing on " : "Paused on ") + remote.deviceName;
    ImFont *f = gui->log_font;
    label = fitText(f, label, paneW - pillW - 72.0f);
    dl->AddText(f, f->FontSize, ImVec2(wp.x + 52.0f, wp.y + y + (h - f->FontSize) * 0.5f),
                COL_GREENV, label.c_str());
    ImGui::SetCursorPos(ImVec2(paneW - pillW - 14.0f, y + 5.0f));
    ImGui::PushFont(f);
    if (pillButton("Play here##connect", ImVec2(pillW, h - 10.0f), COL_GREENV, COL_DARK)) playHere();
    ImGui::PopFont();
}

// ---------------------------------------------------------------- drawing

void PlaybackScreen::previous(const PlayerModel::Snapshot& snap) {
    bool restart = snap.positionMs > PREV_RESTART_MS && snap.durationMs > 0;
    if (remoteMode) {
        if (restart) {
            remoteCommand("seek_to", 0);
        } else {
            remoteCommand("skip_prev");
        }
    } else if (restart) {
        gui->player.setPosition(0);
        sendSeek(0);
    } else {
        gui->prevCallback();
    }
}

void PlaybackScreen::togglePlay(const PlayerModel::Snapshot& snap) {
    if (remoteMode) {
        remoteCommand(snap.paused ? "resume" : "pause");
    } else {
        gui->playToggleCallback();
    }
}

void PlaybackScreen::skipNext() {
    if (remoteMode) {
        remoteCommand("skip_next");
    } else {
        gui->nextCallback();
    }
}

bool PlaybackScreen::goBack() {
    if (tab == Tab::LOG) {
        tab = Tab::SETTINGS;
        return true;
    }
    std::vector<Page> *stack = pageStack();
    if (stack != nullptr && !stack->empty()) {
        Page popped = stack->back();
        stack->pop_back();
        const Page *top = topPage();
        // Stop streaming titles for a list nobody looks at.
        if (popped.kind == Page::LIST && (top == nullptr || top->kind != Page::LIST || top->uri != popped.uri)) {
            cancelTrackLoads();
        }
        syncOpen();
        return true;
    }
    if (tab != Tab::LIBRARY) return false;
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

    // The cover opens the album; the buttons at its left open the album and
    // the artist.
    bool idle = snap.durationMs == 0 && snap.artist.empty();
    bool links = !idle && !nowLinks.uri.empty();
    float coverX = (paneW - coverSz) * 0.5f;
    ImGui::SetCursorPos(ImVec2(coverX, 0.0f));
    ImVec2 cp = ImGui::GetCursorScreenPos();
    bool coverTap = ImGui::InvisibleButton("##cover", ImVec2(coverSz, coverSz));
    bool coverFocus = ImGui::IsItemFocused();
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    cdl->AddImage(Render::tex_id(cover_art_tex), cp, ImVec2(cp.x + coverSz, cp.y + coverSz));
    if (coverFocus) {
        cdl->AddRect(ImVec2(cp.x - 2.0f, cp.y - 2.0f), ImVec2(cp.x + coverSz + 2.0f, cp.y + coverSz + 2.0f),
                     COL_GREENV, 4.0f, ImDrawCornerFlags_All, 3.0f);
    }
    if (coverTap && links) goNowAlbum();
    if (links) {
        ImGui::PushFont(gui->small_icon_font);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 24.0f);
        ImGui::SetCursorPos(ImVec2(coverX - 58.0f, coverSz * 0.5f - 52.0f));
        if (iconButton(ICON_FA_COMPACT_DISC "##nowalbum", ImVec2(48.0f, 48.0f), COL_WHITE, COL_CARD)) {
            goNowAlbum();
        }
        ImGui::SetCursorPos(ImVec2(coverX - 58.0f, coverSz * 0.5f + 4.0f));
        if (iconButton(ICON_FA_USER "##nowartist", ImVec2(48.0f, 48.0f), COL_WHITE, COL_CARD)) {
            goNowArtist();
        }
        ImGui::PopStyleVar();
        ImGui::PopFont();
    }
    float y = coverSz + 14.0f;

    // Sleep timer armed: a moon and the time left, in the corner by the cover.
    if (sleepAtUs != 0 || sleepEndOfTrack) {
        std::string left = "end";
        if (sleepAtUs != 0) {
            uint64_t now = sceKernelGetProcessTimeWide();
            uint64_t min = sleepAtUs > now ? (sleepAtUs - now + 59999999ULL) / 60000000ULL : 0;
            left = std::to_string(min) + " min";
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImFont *icon = gui->small_icon_font;
        dl->AddText(icon, icon->FontSize, ImVec2(wp.x + paneW - 76.0f, wp.y + 4.0f), COL_GREENV,
                    ICON_FA_MOON);
        dl->AddText(gui->log_font, gui->log_font->FontSize, ImVec2(wp.x + paneW - 76.0f, wp.y + 32.0f),
                    COL_GREY, left.c_str());
    }

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
    // Title and artist lines are links too (touch only: the gamepad has the
    // buttons by the cover).
    if (links) {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorPos(ImVec2(12.0f, y));
        if (ImGui::InvisibleButton("##titlelink", ImVec2(textW, 34.0f))) goNowAlbum();
        ImGui::SetCursorPos(ImVec2(12.0f, y + 36.0f));
        if (ImGui::InvisibleButton("##artistlink", ImVec2(textW, 34.0f))) goNowArtist();
        ImGui::PopItemFlag();
    }
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
        if (remoteMode) {
            remoteCommand("seek_to", ms);
        } else {
            gui->player.setPosition(ms);   // instant local feedback
            sendSeek(ms);
        }
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
        if (remoteMode) {
            remote.volume = v;
            std::string target = remote.activeId;
            GUI *g = gui;
            gui->net.post([g, target, v] { g->connect.setVolume(target, v); }, true);
        } else {
            gui->player.setVolume(v);   // instant local feedback
            gui->volumeCallback(v);
        }
    }

    if (remoteMode) drawRemoteBar(paneW);
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
                   remoteMode ? COL_DIM : (shuffleOn ? COL_GREENV : COL_GREY), COL_CLEAR) &&
        ready && !remoteMode) {
        shuffleOn = !shuffleOn;
        sendShuffle(shuffleOn);
        // cspot shuffles the window it holds; the rest of a long list here.
        if (shuffleOn && queueNext + 1 < queueUris.size()) {
            for (size_t k = queueUris.size() - 1; k > queueNext; k--) {
                size_t j = queueNext + static_cast<size_t>(rand_r(&shuffleSeed)) % (k - queueNext + 1);
                std::swap(queueUris[k], queueUris[j]);
            }
        }
    }
    ImGui::SameLine();
    if (iconButton(ICON_FA_STEP_BACKWARD "##prev", ImVec2(64.0f, 64.0f), COL_WHITE, COL_CLEAR) &&
        (ready || remoteMode)) {
        previous(snap);
    }
    ImGui::PopFont();
    ImGui::SameLine();

    ImGui::PushFont(gui->playback_icon_font);
    const char* playIcon = snap.paused ? ICON_FA_PLAY_CIRCLE "###pp" : ICON_FA_PAUSE_CIRCLE "###pp";  // NOLINT
    if (iconButton(playIcon, ImVec2(72.0f, 64.0f), snap.loading ? COL_GREY : COL_WHITE, COL_CLEAR) &&
        (ready || remoteMode)) {
        togglePlay(snap);
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
    if (iconButton(ICON_FA_STEP_FORWARD "##next", ImVec2(64.0f, 64.0f), COL_WHITE, COL_CLEAR) &&
        (ready || remoteMode)) {
        skipNext();
    }
    ImGui::SameLine();
    if (iconButton(ICON_FA_REDO "##repeat", ImVec2(52.0f, 64.0f),
                   remoteMode ? COL_DIM : (repeatMode != 0 ? COL_GREENV : COL_GREY), COL_CLEAR) &&
        ready && !remoteMode) {
        repeatMode = (repeatMode + 1) % 3;
        sendRepeat(repeatMode);
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(2);

    // Repeat-one has no separate glyph in the bundled icon range: mark it.
    if (repeatMode == 2 && !remoteMode) {
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
            size_t count = p.tracks.empty() ? static_cast<size_t>(p.length) : p.tracks.size();
            std::string sub = count > 0 ? "Playlist, " + std::to_string(count) +
                                              (count == 1 ? " song" : " songs")
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

// Play, shuffle play, sort menu, filter, and a button that scrolls to the song
// playing.
void PlaybackScreen::drawPlaylistActions(Playlist &pl, const PlayerModel::Snapshot& snap, float) {
    const float h = 44.0f;
    if (pillButton("Play##playall", ImVec2(96.0f, h), COL_GREENV, COL_DARK)) {
        playView(SIZE_MAX, false);
    }
    ImGui::SameLine();
    ImGui::PushFont(gui->small_icon_font);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
    bool shuffle = iconButton(ICON_FA_RANDOM "##shuffleplay", ImVec2(48.0f, h), COL_WHITE, COL_CARD);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (shuffle) playView(SIZE_MAX, true);
    ImGui::SameLine();

    // Sort button: the order's short name, and an arrow for its direction.
    ImFont *font = ImGui::GetFont();
    const char *shortName = kSortShort[pl.sort];
    float textW = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, shortName).x;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f, 4.0f));
    bool openSort = pillButton((std::string(shortName) + "##sortbtn").c_str(),
                               ImVec2(textW + 56.0f, h), COL_CARD, COL_WHITE);
    ImGui::PopStyleVar(2);
    ImVec2 sortMin = ImGui::GetItemRectMin(), sortMax = ImGui::GetItemRectMax();
    ImFont *icon = gui->small_icon_font;
    ImGui::GetWindowDrawList()->AddText(
        icon, icon->FontSize, ImVec2(sortMax.x - 34.0f, sortMin.y + (h - icon->FontSize) * 0.5f),
        pl.sort == SORT_CUSTOM ? COL_GREY : COL_GREENV,
        pl.sortDesc ? ICON_FA_SORT_AMOUNT_DOWN : ICON_FA_SORT_AMOUNT_UP);
    if (openSort) ImGui::OpenPopup("##sort");
    ImGui::SameLine();

    ImGui::PushFont(gui->small_icon_font);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
    bool find = iconButton(ICON_FA_SEARCH "##filter", ImVec2(48.0f, h),
                           pl.filter.empty() ? COL_WHITE : COL_GREENV, COL_CARD);
    ImGui::SameLine();
    bool locate = iconButton(ICON_FA_CROSSHAIRS "##locate", ImVec2(48.0f, h), COL_WHITE, COL_CARD);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (find) {
        std::string uri = pl.uri;
        Keyboard::Open("Filter this list", pl.filter, [this, uri](const std::string &q) {
            int i = findPlaylist(playlists, uri);
            if (i < 0) return;
            playlists[i].filter = q;
            viewDirty = true;
            scrollToRow = 0;
        });
    }
    if (locate) locateCurrent(snap);

    ImGui::SetNextWindowPos(ImVec2(sortMin.x, sortMax.y + 4.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::BeginPopup("##sort")) {
        popupOpen = true;
        for (int m = 0; m < SORT_COUNT; m++) {
            bool on = pl.sort == m;
            ImGui::PushStyleColor(ImGuiCol_Text, on ? COL_GREENV : COL_WHITE);
            bool pick = ImGui::Selectable((std::string(kSortNames[m]) + "##s" + std::to_string(m)).c_str(),
                                          false, 0, ImVec2(250.0f, 40.0f));
            ImGui::PopStyleColor();
            if (on) {
                ImVec2 r = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddText(
                    icon, icon->FontSize, ImVec2(r.x - 28.0f, r.y - 40.0f + (40.0f - icon->FontSize) * 0.5f),
                    COL_GREENV, pl.sortDesc ? ICON_FA_ARROW_DOWN : ICON_FA_ARROW_UP);
            }
            if (pick) {
                setSort(m);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// Thumb on the right edge of a long list: drag it (or tap the rail) to jump
// anywhere; a bubble shows where in the sort order it is.
void PlaybackScreen::drawFastScroll(const Playlist &pl, float y0, float step) {
    float maxY = ImGui::GetScrollMaxY();
    if (maxY <= 0.0f || view.empty()) return;
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float railW = 30.0f, thumbH = 56.0f, pad = 4.0f;
    float top = wp.y + pad, travel = ws.y - pad * 2.0f - thumbH;
    float frac = ImGui::GetScrollY() / maxY;
    float thumbY = top + frac * travel;

    ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - railW, wp.y));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InvisibleButton("##fastscroll", ImVec2(railW, ws.y));
    ImGui::PopItemFlag();
    bool held = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) {
        bool onThumb = io.MousePos.y >= thumbY && io.MousePos.y <= thumbY + thumbH;
        thumbGrab = onThumb ? io.MousePos.y - thumbY : thumbH * 0.5f;
    }
    if (held) {
        Input::cancel_scroll();   // the finger drives the thumb, not the list
        frac = (io.MousePos.y - thumbGrab - top) / travel;
        frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
        ImGui::SetScrollY(frac * maxY);
        thumbY = top + frac * travel;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float x1 = wp.x + ws.x - 8.0f;
    dl->AddRectFilled(ImVec2(x1 - 8.0f, thumbY), ImVec2(x1, thumbY + thumbH),
                      held ? COL_GREENV : COL_TRACK, 4.0f);
    if (!held) return;

    // The row at the middle of the screen once the scroll lands there.
    float midY = frac * maxY + ws.y * 0.5f;
    size_t row = static_cast<size_t>(std::max(0.0f, (midY - y0) / step));
    if (row >= view.size()) row = view.size() - 1;
    std::string label = scrollLabel(pl.tracks[view[row]], pl.sort, row);
    ImFont *font = gui->font_bold;
    ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label.c_str());
    float bw = std::max(ts.x + 28.0f, 56.0f), bh = ts.y + 16.0f;
    ImVec2 b0(x1 - 8.0f - 14.0f - bw, thumbY + thumbH * 0.5f - bh * 0.5f);
    dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), COL_GREENV, bh * 0.5f);
    dl->AddText(font, font->FontSize, ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + 8.0f), COL_DARK,
                label.c_str());
}

void PlaybackScreen::drawPlaylist(const PlayerModel::Snapshot& snap, float avail) {
    Playlist& pl = playlists[openIndex];
    if (drawBackHeader(pl.name, avail)) {
        goBack();
        return;
    }
    if (pl.external) {
        int shown = openIndex;
        drawListHeader(pl, avail);
        if (openIndex != shown) return;   // an artist button opened a page
    }
    if (viewDirty || viewOf != openIndex) buildView();
    const std::string &filter = pl.filter;

    // "3797 songs, 11 h 20 min", or the matches of the filter.
    std::string summary;
    if (!pl.tracks.empty()) {
        summary = listSummary(filter.empty() ? view.size() : pl.tracks.size(), viewTotalMs);
        if (!filter.empty()) summary = std::to_string(view.size()) + " of " + summary;
        if (viewMissing > 0 && pl.tracksState == LoadState::LOADING) {
            summary += ", " + std::to_string(viewMissing) + " titles to load";
        }
    }
    ImGui::PushFont(gui->log_font);
    greyText(summary);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    drawPlaylistActions(pl, snap, avail);
    if (!filter.empty()) {
        std::string chip = "Clear filter \"" + fitText(ImGui::GetFont(), filter, avail - 240.0f) + "\"";
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        if (pillButton((chip + "##clearfilter").c_str(), ImVec2(0.0f, 40.0f), COL_GREENV, COL_DARK)) {
            pl.filter.clear();
            viewDirty = true;
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    fastScroll = false;
    if (pl.tracks.empty()) {
        if (pl.tracksState == LoadState::LOADING) {
            Spinner("Loading tracks...");
        } else if (pl.tracksState == LoadState::FAILED) {
            greyText("Could not load this list.");
            if (pillButton("Try again", ImVec2(160.0f, 44.0f), COL_WHITE, COL_DARK)) {
                pl.tracksState = LoadState::NONE;
                ensureTracks(openIndex);
            }
        } else {
            greyText("No playable tracks here.");
        }
        return;
    }
    if (view.empty()) {
        greyText("No song matches this filter.");
        return;
    }

    // Only the rows on screen (and a few around them, so the D-pad can move
    // onto the next one) are laid out: a 4000-row list costs the same as a
    // short one.
    fastScroll = view.size() > FAST_SCROLL_MIN;
    float rowW = (fastScroll ? avail - 34.0f : avail) - 48.0f;   // room for the menu button
    float step = TRACK_ROW_H + ImGui::GetStyle().ItemSpacing.y;
    float y0 = ImGui::GetCursorPosY();
    float winH = ImGui::GetWindowHeight();
    float scrollY = ImGui::GetScrollY();
    lastScrollY = scrollY;
    if (scrollToRow >= 0) {
        float target = scrollToRow == 0 ? 0.0f : y0 + scrollToRow * step - (winH - step) * 0.5f;
        float maxY = y0 + view.size() * step - winH;
        target = std::max(0.0f, std::min(target, std::max(0.0f, maxY)));
        ImGui::SetScrollY(target);
        scrollY = target;
        scrollToRow = -1;
    }
    int n = static_cast<int>(view.size());
    int first = std::max(0, static_cast<int>((scrollY - y0) / step) - 2);
    int last = std::min(n, static_cast<int>((scrollY + winH - y0) / step) + 3);
    for (int r = first; r < last; r++) {
        ImGui::SetCursorPosY(y0 + r * step);
        const TrackRow &row = pl.tracks[view[r]];
        std::string id = "##t" + std::to_string(view[r]);
        bool current = snap.uri.empty() ? !row.name.empty() && row.name == snap.name : row.uri == snap.uri;
        ImU32 fg = row.durationMs == 0 ? COL_DIM : (current ? COL_GREENV : COL_WHITE);
        std::string right = row.durationMs > 0 ? fmtTime(row.durationMs) : std::string();
        bool tapped = listRow(id.c_str(), row.name.empty() ? std::string("...") : row.name,
                              row.artist.empty() ? std::string(" ") : row.artist, rowW, fg, gui->log_font,
                              nullptr, nullptr, right);
        bool focused = ImGui::IsItemFocused();
        ImGui::SameLine(0.0f, 8.0f);
        bool more = moreButton(id);
        if (focused || more) {
            std::string uri = row.uri, name = row.name, artist = row.artist;
            auto open = [this, uri, name, artist] { openTrackMenu(uri, name, artist, Link(), {}); };
            if (more) open();
            if (focused) focusedMenu = open;
        }
        if (tapped) playView(static_cast<size_t>(r), false);
    }
    ImGui::SetCursorPosY(y0 + n * step);
    if (pl.tracksState == LoadState::LOADING) {
        Spinner("");
    }
    if (fastScroll) drawFastScroll(pl, y0, step);
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

    drawSleepTimer(avail);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    if (ImGui::Button("Refresh library", ImVec2(avail, 48.0f))) {
        // Playlists fetch their track lists again when next opened.
        for (auto &p : playlists) p.fresh = false;
        loadLibrary();
    }
    if (ImGui::Button("Show log", ImVec2(avail, 48.0f))) {
        tab = Tab::LOG;
    }
    if (ImGui::Button("Unlink this Spotify account", ImVec2(avail, 48.0f))) {
        remove(CREDENTIALS_FILE_NAME);
        remove(PLAYLIST_CACHE_PATH);
        remove(SORT_PREFS_PATH);
        clearTrackCache();
        gui->isRunning = false;
    }
    if (ImGui::Button("Exit", ImVec2(avail, 48.0f))) {
        gui->isRunning = false;
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::PushFont(gui->log_font);
    greyText("START play/pause, L previous, R next, SELECT next tab, circle back. "
             "The right stick scrolls, faster the longer it is held.");
    ImGui::PopFont();
}

// Pauses after a set time, or at the end of the song playing.
void PlaybackScreen::drawSleepTimer(float avail) {
    ImGui::TextUnformatted("Sleep timer");
    static const struct { const char *label; int min; } kTimes[] = {
        {"Off##sl0", 0}, {"15 min##sl15", 15}, {"30 min##sl30", 30}, {"1 hour##sl60", 60},
    };
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float pillW = (avail - spacing * 3.0f) / 4.0f;
    uint64_t now = sceKernelGetProcessTimeWide();
    bool armed = sleepAtUs != 0 || sleepEndOfTrack;
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        bool on = kTimes[i].min == 0 && !armed;
        if (pillButton(kTimes[i].label, ImVec2(pillW, 44.0f), on ? COL_GREENV : COL_CARD,
                       on ? COL_DARK : COL_WHITE)) {
            sleepEndOfTrack = false;
            sleepAtUs = kTimes[i].min == 0 ? 0
                        : now + static_cast<uint64_t>(kTimes[i].min) * 60000000ULL;
        }
    }
    PlayerModel::Snapshot snap = gui->player.snapshot();
    if (pillButton("At the end of this song##slend", ImVec2(avail, 44.0f),
                   sleepEndOfTrack ? COL_GREENV : COL_CARD, sleepEndOfTrack ? COL_DARK : COL_WHITE)) {
        if (snap.name.empty()) {
            gui->toast("Nothing is playing.");
        } else {
            sleepAtUs = 0;
            sleepEndOfTrack = true;
            sleepTrack = snap.name;
        }
    }
    std::string status = "Off.";
    if (sleepEndOfTrack) {
        status = "Pauses at the end of " + sleepTrack + ".";
    } else if (sleepAtUs != 0) {
        uint64_t min = sleepAtUs > now ? (sleepAtUs - now + 59999999ULL) / 60000000ULL : 0;
        status = "Pauses in " + std::to_string(min) + " min.";
    }
    ImGui::PushFont(gui->log_font);
    greyText(fitText(gui->log_font, status, avail));
    ImGui::PopFont();
}

void PlaybackScreen::drawBrowse(const PlayerModel::Snapshot& snap) {
    float avail = ImGui::GetContentRegionAvail().x;
    const Page *top = topPage();
    if (top != nullptr) {
        Page page = *top;   // drawing may push or pop pages
        switch (page.kind) {
            case Page::LIST:
                if (openIndex >= 0 && openIndex < static_cast<int>(playlists.size())) drawPlaylist(snap, avail);
                break;
            case Page::ARTIST:
                drawArtist(page.uri, snap.uri, avail);
                break;
            case Page::SECTION:
                drawSection(page.uri, page.section, snap.uri, avail);
                break;
        }
        return;
    }
    switch (tab) {
        case Tab::LIBRARY:
            drawLibrary(snap, avail);
            break;
        case Tab::SEARCH:
            drawSearch(snap, avail);
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
            // Tapping the tab shown again goes back to its top.
            if (tab == kTabs[i].tab && tab == Tab::LIBRARY) {
                openFolder = -1;
                clearPages(0);
            } else if (tab == kTabs[i].tab && tab == Tab::SEARCH) {
                clearPages(1);
            }
            tab = kTabs[i].tab;
            syncOpen();
        }
    }

    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

void PlaybackScreen::draw() {
    thumbs.newFrame();
    syncOpen();
    PlayerModel::Snapshot snap = gui->player.snapshot();
    // Another device plays and this one does not: the pane follows it.
    remoteMode = remoteShown(snap);
    PlayerModel::Snapshot shown = remoteMode ? remoteSnapshot() : snap;

    // Global shortcuts, independent of where the gamepad focus is.
    uint32_t pressed = Input::pressed();
    if (gui->cspot_started || remoteMode) {
        if (pressed & SCE_CTRL_START) togglePlay(shown);
        if (pressed & SCE_CTRL_LTRIGGER) previous(shown);
        if (pressed & SCE_CTRL_RTRIGGER) skipNext();
    }
    if (pressed & SCE_CTRL_SELECT) {
        tab = tab == Tab::LIBRARY ? Tab::SEARCH : (tab == Tab::SEARCH ? Tab::SETTINGS : Tab::LIBRARY);
        syncOpen();
    }
    // Circle closes a menu (ImGui does it) without leaving the page. A menu
    // sets the flag again each frame it is drawn.
    bool menuWasOpen = popupOpen;
    popupOpen = false;
    if (Input::back_pressed() && !menuWasOpen) {
        goBack();
    }
    // Triangle: the menu of the track row the gamepad is on (last frame).
    if ((pressed & SCE_CTRL_TRIANGLE) && focusedMenu && !menuWasOpen) focusedMenu();
    focusedMenu = nullptr;

    float fullW = ImGui::GetContentRegionAvail().x;
    float leftW = fullW * 0.46f;

    // Now-playing is fixed-size: no scrolling, so a press near the bottom
    // can't auto-scroll the pane.
    const ImGuiWindowFlags kNoScroll = ImGuiWindowFlags_NavFlattened |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::BeginChild("nowplaying", ImVec2(leftW, 0.0f), false, kNoScroll);
    drawNowPlaying(shown);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0.0f, 0.0f), false, kNoScroll);
    {
        float navH = 76.0f;
        // One scroll position per view: the ID changes with the view, so
        // opening a playlist or a folder starts at its top.
        // Back returns to the scroll position the page had.
        std::string browseId = "browse" + std::to_string(static_cast<int>(tab));
        const Page *top = topPage();
        if (top != nullptr) {
            browseId += "_p" + std::to_string(static_cast<int>(top->kind)) + "_" + top->uri + "_" +
                        std::to_string(top->section);
        } else if (tab == Tab::LIBRARY) {
            browseId += "_f" + std::to_string(openFolder);
        } else if (tab == Tab::SEARCH) {
            browseId += "_c" + std::to_string(searchChip) + "_" + std::to_string(searchGen.load());
        }
        // A long list has its own thumb in place of the scrollbar.
        bool thumb = fastScroll && top != nullptr && top->kind == Page::LIST && openIndex >= 0;
        ImGui::BeginChild(browseId.c_str(), ImVec2(0.0f, ImGui::GetContentRegionAvail().y - navH),
                          false, ImGuiWindowFlags_NavFlattened |
                                 (thumb ? ImGuiWindowFlags_NoScrollbar : 0));
        Input::scroll_area();
        drawBrowse(snap);
        ImGui::EndChild();

        ImGui::BeginChild("nav", ImVec2(0.0f, 0.0f), false, kNoScroll);
        drawNav();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    drawTrackMenu();
}

std::string PlaybackScreen::debugState() {
    static const char *kTabs[] = {"library", "search", "log", "settings"};
    PlayerModel::Snapshot snap = gui->player.snapshot();
    std::string out = "{\"tab\":" + json_quote(kTabs[static_cast<int>(tab)]) +
        ",\"quality\":" + std::to_string(gui->quality_kbps.load()) +
        ",\"loading\":" + (snap.loading ? "true" : "false") +
        ",\"shuffle\":" + (shuffleOn ? "true" : "false") +
        ",\"repeat\":" + std::to_string(repeatMode) +
        ",\"remote\":" + (remoteShown(snap) ? json_quote(remote.deviceName + ": " + remoteSnapshot().name +
                                                         (remote.playing ? " (playing)" : " (paused)"))
                                            : std::string("null")) +
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
        if (viewDirty || viewOf != openIndex) buildView();
        out += "{\"index\":" + std::to_string(openIndex) + ",\"name\":" + json_quote(p.name) +
               ",\"state\":" + json_quote(stateName(p.tracksState)) +
               ",\"fresh\":" + (p.fresh ? "true" : "false") +
               ",\"count\":" + std::to_string(p.tracks.size()) +
               ",\"rows\":" + std::to_string(view.size()) +
               ",\"missing\":" + std::to_string(viewMissing) +
               ",\"total_min\":" + std::to_string(viewTotalMs / 60000) +
               ",\"sort\":" + json_quote(kSortKeys[p.sort]) +
               ",\"desc\":" + (p.sortDesc ? "true" : "false") +
               ",\"filter\":" + json_quote(p.filter) +
               ",\"external\":" + (p.external ? "true" : "false") +
               ",\"subtitle\":" + json_quote(p.subtitle) +
               ",\"artists\":" + std::to_string(p.artists.size()) +
               ",\"image\":" + (p.imageUrl.empty() ? "false" : "true") +
               ",\"scroll\":" + std::to_string(static_cast<int>(lastScrollY)) +
               ",\"tracks\":[";
        // The view in display order: "row:title - artist (m:ss)".
        for (size_t r = 0; r < view.size() && r < 12; r++) {
            const TrackRow &t = p.tracks[view[r]];
            if (r) out += ",";
            out += json_quote(std::to_string(r) + ":" + t.name + " - " + t.artist +
                              " (" + fmtTime(t.durationMs) + ")");
        }
        out += "]}";
    } else {
        out += "null";
    }
    uint64_t now = sceKernelGetProcessTimeWide();
    out += ",\"queue\":{\"size\":" + std::to_string(queueUris.size()) +
           ",\"next\":" + std::to_string(queueNext) + "}" +
           ",\"sleep\":" + (sleepEndOfTrack ? json_quote("track")
                              : sleepAtUs == 0 ? json_quote("off")
                              : std::to_string(sleepAtUs > now ? (sleepAtUs - now) / 1000000ULL : 0));
    out += "," + browseState() + "}";
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
        // open N: library row N, over the library's top.
        int i = atoi(arg.c_str());
        if (i < 0 || i >= static_cast<int>(playlists.size())) return false;
        tab = Tab::LIBRARY;
        clearPages(0);
        openPlaylist(i);
        return true;
    }
    if (cmd == "folder") {
        int i = atoi(arg.c_str());
        if (i < 0 || i >= static_cast<int>(folders.size())) return false;
        tab = Tab::LIBRARY;
        clearPages(0);
        openFolder = i;
        return true;
    }
    if (cmd == "back") {
        goBack();
        return true;
    }
    syncOpen();
    bool listOpen = openIndex >= 0 && openIndex < static_cast<int>(playlists.size());
    if (cmd == "play") {
        // play N: row N of the open playlist, as shown (sorted, filtered).
        if (!listOpen) return false;
        if (viewDirty || viewOf != openIndex) buildView();
        size_t r = static_cast<size_t>(atoi(arg.c_str()));
        if (r >= view.size()) return false;
        playView(r, false);
        return true;
    }
    if (cmd == "shuffleplay") {
        if (!listOpen) return false;
        playView(SIZE_MAX, true);
        return true;
    }
    if (cmd == "sort") {
        // sort MODE [desc]: MODE is one of kSortKeys.
        if (!listOpen) return false;
        std::string mode = arg.substr(0, arg.find(' '));
        bool desc = arg.find(" desc") != std::string::npos;
        for (int m = 0; m < SORT_COUNT; m++) {
            if (mode != kSortKeys[m]) continue;
            Playlist &p = playlists[openIndex];
            p.sort = m;
            p.sortDesc = !desc;   // setSort on the same mode flips it
            setSort(m);
            return true;
        }
        return false;
    }
    if (cmd == "filter") {
        if (!listOpen) return false;
        playlists[openIndex].filter = arg;
        viewDirty = true;
        scrollToRow = 0;
        return true;
    }
    if (cmd == "locate") {
        if (!listOpen) return false;
        locateCurrent(gui->player.snapshot());
        return true;
    }
    if (cmd == "row") {
        // row N: scroll the open list to center row N.
        if (!listOpen) return false;
        scrollToRow = atoi(arg.c_str());
        return true;
    }
    if (cmd == "sleep") {
        // sleep off | track | MINUTES
        sleepEndOfTrack = false;
        sleepAtUs = 0;
        if (arg == "off") return true;
        if (arg == "track") {
            sleepTrack = gui->player.snapshot().name;
            sleepEndOfTrack = !sleepTrack.empty();
            return sleepEndOfTrack;
        }
        int min = atoi(arg.c_str());
        if (min <= 0) return false;
        sleepAtUs = sceKernelGetProcessTimeWide() + static_cast<uint64_t>(min) * 60000000ULL;
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
    if (cmd == "connect") {
        // connect cmd ENDPOINT [MS]: a command to the active device.
        // connect here: transfer to this Vita. connect fake NAME [paused]: a
        // made-up device playing the track this Vita shows, held until
        // "connect fake off" (UI tests).
        std::string sub = arg.substr(0, arg.find(' '));
        std::string rest = arg.size() > sub.size() ? arg.substr(sub.size() + 1) : "";
        if (sub == "cmd") {
            std::string endpoint = rest.substr(0, rest.find(' '));
            int64_t ms = rest.size() > endpoint.size() ? atoll(rest.c_str() + endpoint.size() + 1) : -1;
            std::string target = gui->connect.snapshot().activeId;
            GUI *g = gui;
            gui->net.post([g, target, endpoint, ms] {
                bool ok = g->connect.command(target, endpoint, ms);
                g->net.deliver([g, ok] { g->toast(ok ? "Command sent." : "Command refused."); });
            }, true);
            return !target.empty();
        }
        if (sub == "here") {
            playHere();
            return true;
        }
        if (sub == "fake" && rest == "off") {
            gui->connect.release();
            return true;
        }
        if (sub == "fake") {
            PlayerModel::Snapshot local = gui->player.snapshot();
            ConnectState f;
            bool paused = rest.size() > 7 && rest.compare(rest.size() - 7, 7, " paused") == 0;
            f.activeId = "fake_device";
            f.deviceName = paused ? rest.substr(0, rest.size() - 7) : rest;
            f.deviceType = "SMARTPHONE";
            f.trackUri = "spotify:track:fake";
            f.title = local.name;
            f.artist = local.artist;
            f.imageUrl = local.imageUrl;
            f.durationMs = local.durationMs;
            f.positionMs = local.positionMs;
            f.playing = !paused;
            f.volume = 40000;
            // "connect fake spotify:track:<id>": only the URI, like a speaker's
            // cluster, so the metadata and the cover come from this app.
            if (startsWith(f.deviceName, "spotify:track:")) {
                f.trackUri = f.deviceName;
                f.deviceName = "Test speaker";
                f.deviceType = "SPEAKER";
                f.title.clear();
                f.artist.clear();
                f.imageUrl.clear();
                f.durationMs = 0;
                f.positionMs = 0;
            }
            gui->connect.inject(f);
            return !f.deviceName.empty();
        }
        return false;
    }
    return browseCommand(cmd, arg);
}
