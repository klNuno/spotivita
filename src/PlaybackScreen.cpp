#include "PlaybackScreen.h"
#include "Font.h"
#include "Gui.h"
#include "GuiUtils.h"
#include "Input.h"
#include "Keyboard.h"
#include "Utils.h"
#include "Config.h"
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <JSONObject.h>
#include <Logger.h>
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

const char *PLAYLIST_CACHE_PATH = "ux0:data/cspot/playlists.json";
const char *COVER_CACHE_DIR = "ux0:data/cspot/cache";
const int COVER_MAX_SIDE = 256;
const int COVER_CACHE_MAX_FILES = 400;
// Consecutive spclient failures before a name/metadata burst gives up: past
// that the network is down and grinding on only starves Mercury.
const int MAX_FAIL_STREAK = 5;
// Rows delivered to the GUI per batch while metadata streams in.
const int TRACK_BATCH = 6;

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
        case -1:  return "Spotify changed its search API. Update psvitify.";
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

// Full-width list row: title, optional grey subtitle, highlight while pressed.
// Text is clipped to the row instead of spilling into the next column.
bool listRow(const char *id, const std::string &title, const std::string &subtitle,
             float width, ImU32 fg, ImFont *subFont) {
    const float h = subtitle.empty() ? 50.0f : 60.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h));
    bool held = ImGui::IsItemActive();
    bool focused = ImGui::IsItemFocused();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (held || focused) {
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), COL_ROW_HI, 6.0f);
    }
    ImVec4 clip(p.x + 8.0f, p.y, p.x + width - 8.0f, p.y + h);
    ImFont *font = ImGui::GetFont();
    float ty = subtitle.empty() ? p.y + (h - font->FontSize) * 0.5f : p.y + 6.0f;
    dl->AddText(font, font->FontSize, ImVec2(p.x + 8.0f, ty), fg, title.c_str(), NULL, 0.0f, &clip);
    if (!subtitle.empty()) {
        dl->AddText(subFont, subFont->FontSize, ImVec2(p.x + 8.0f, p.y + 8.0f + font->FontSize),
                    COL_GREY, subtitle.c_str(), NULL, 0.0f, &clip);
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

// The playlist list is cached so a relaunch shows the library instantly with
// zero network.
std::vector<Playlist> loadPlaylistCache() {
    std::vector<Playlist> out;
    FILE* f = fopen(PLAYLIST_CACHE_PATH, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return out;
    }
    std::string buf(static_cast<size_t>(n), '\0');
    size_t rd = fread(&buf[0], 1, static_cast<size_t>(n), f);
    fclose(f);
    buf.resize(rd);
    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) return out;
    if (cJSON_IsArray(root)) {
        int c = cJSON_GetArraySize(root);
        for (int i = 0; i < c; i++) {
            cJSON* it = cJSON_GetArrayItem(root, i);
            cJSON* nm = cJSON_GetObjectItem(it, "name");
            cJSON* ur = cJSON_GetObjectItem(it, "uri");
            if (cJSON_IsString(nm) && nm->valuestring &&
                cJSON_IsString(ur) && ur->valuestring) {
                Playlist p;
                p.name = nm->valuestring;
                p.uri = ur->valuestring;
                out.push_back(std::move(p));
            }
        }
    }
    cJSON_Delete(root);
    return out;
}

void savePlaylistCache(const std::vector<Playlist>& pls) {
    cJSON* root = cJSON_CreateArray();
    for (const auto& p : pls) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name.c_str());
        cJSON_AddStringToObject(o, "uri", p.uri.c_str());
        cJSON_AddItemToArray(root, o);
    }
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

std::string jsonArtists(cJSON *track) {
    std::string out;
    cJSON *artists = cJSON_GetObjectItem(track, "artists");
    int n = cJSON_IsArray(artists) ? cJSON_GetArraySize(artists) : 0;
    for (int i = 0; i < n && i < 3; i++) {
        std::string a = jsonStr(cJSON_GetArrayItem(artists, i), "name");
        if (a.empty()) continue;
        if (!out.empty()) out += ", ";
        out += a;
    }
    return out;
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

    // Cached playlists: the library shows instantly, no network at boot.
    playlists = loadPlaylistCache();
    if (!playlists.empty()) {
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
    // First run with no cache: fetch the library as soon as a token exists.
    if (libraryState == LoadState::NONE && gui->api.has_token()) {
        loadLibrary();
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
    libraryState = playlists.empty() ? LoadState::LOADING : libraryState;
    libraryError.clear();
    int gen = ++libraryGen;
    cancelTrackLoads();
    namesLeft = 0;
    std::map<std::string, std::string> known;
    for (const auto &p : playlists) known[p.uri] = p.name;

    GUI *g = gui;
    gui->net.post([this, g, gen, known] {
        ApiResult r = g->api.get_rootlist();
        if (gen != libraryGen) return;
        std::vector<std::string> uris;
        if (r.ok()) {
            for (auto &u : parseListUris(r.body, SPOTIFY_ROOTLIST_LENGTH)) {
                if (u.compare(0, strlen(SPOTIFY_PLAYLIST_HEADER), SPOTIFY_PLAYLIST_HEADER) == 0) {
                    uris.push_back(u);
                }
            }
        }
        if (!r.ok() || uris.empty()) {
            long status = r.status;
            bool empty = r.ok();
            g->net.deliver([this, gen, status, empty] {
                if (gen != libraryGen) return;
                libraryError = empty ? "No playlists in this account." : describeStatus(status);
                libraryState = playlists.empty() ? LoadState::FAILED : LoadState::LOADED;
                if (!playlists.empty()) gui->toast(libraryError);
            });
            return;
        }
        g->net.deliver([this, gen, uris, known] {
            if (gen != libraryGen) return;
            std::vector<Playlist> fresh;
            for (const auto &u : uris) {
                Playlist p;
                p.uri = u;
                auto it = known.find(u);
                p.name = it != known.end() ? it->second : std::string("Playlist");
                fresh.push_back(std::move(p));
            }
            playlists = std::move(fresh);
            openIndex = -1;
            libraryState = LoadState::LOADED;
            namesLeft = static_cast<int>(playlists.size());
        });

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
            savePlaylistCache(playlists);
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
            ApiResult d = g->api.get_playlist(plId);
            if (!d.ok()) {
                long status = d.status;
                g->net.deliver([this, uri, status] {
                    int i = findPlaylist(playlists, uri);
                    if (i < 0) return;
                    playlists[i].tracksState = LoadState::FAILED;
                    gui->toast(describeStatus(status));
                });
                return;
            }
            auto items = parseListUris(d.body, SPOTIFY_PLAYLIST_TRACK_LIMIT);
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
        // batches. If spclient refuses metadata outright, fall back to one Web
        // API call for the whole page.
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
        bool useWeb = false;
        for (size_t k = 0; k < rows.size(); k++) {
            if (gen != tracksGen) return;
            ApiResult m = g->api.get_track_metadata(idFromUri(rows[k].uri, SPOTIFY_TRACK_HEADER));
            std::string name, artist;
            if (m.ok()) {
                failStreak = 0;
                parseTrackMeta(m.body, &name, &artist);
            } else if (k == 0 && m.status >= 400) {
                useWeb = true;
                break;
            } else if (++failStreak >= MAX_FAIL_STREAK) {
                break;
            }
            batch.push_back({k, {name.empty() ? std::string("Unavailable") : name, artist}});
            if (static_cast<int>(batch.size()) >= TRACK_BATCH) flush();
        }
        flush();

        if (useWeb && gen == tracksGen) {
            ApiResult w = g->api.get_playlist_tracks_web(plId);
            std::map<std::string, std::pair<std::string, std::string>> byUri;
            if (w.ok()) {
                cJSON *root = cJSON_Parse(w.body.c_str());
                cJSON *items = root ? cJSON_GetObjectItem(root, "items") : NULL;
                int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
                for (int i = 0; i < n; i++) {
                    cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(items, i), "track");
                    std::string u = jsonStr(t, "uri");
                    if (!u.empty()) byUri[u] = {jsonStr(t, "name"), jsonArtists(t)};
                }
                cJSON_Delete(root);
            }
            for (size_t k = 0; k < rows.size(); k++) {
                auto it = byUri.find(rows[k].uri);
                batch.push_back({k, it != byUri.end() ? it->second
                                     : std::make_pair(std::string("Unavailable"), std::string())});
            }
            flush();
            if (!w.ok()) {
                long status = w.status;
                g->net.deliver([this, status] { gui->toast(describeStatus(status)); });
            }
        }
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
    gui->playTracksCallback(uris, uri, index);
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

void PlaybackScreen::drawNowPlaying(const PlayerModel::Snapshot& snap) {
    float paneW = ImGui::GetContentRegionAvail().x;
    float coverSz = paneW - 90.0f;
    if (coverSz > 190.0f) coverSz = 190.0f;
    if (coverSz < 110.0f) coverSz = 110.0f;

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::SetCursorPosX((paneW - coverSz) * 0.5f);
    ImGui::Image(Render::tex_id(cover_art_tex), ImVec2(coverSz, coverSz));

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    bool idle = snap.durationMs == 0 && snap.artist.empty();
    ImGui::PushFont(gui->font_bold);
    TextCentered(idle ? std::string("Nothing playing") : snap.name);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    if (idle) {
        TextCentered(gui->cspot_started ? "Pick a playlist, or play from your phone"
                                        : "Connecting to Spotify...");
    } else {
        TextCentered(snap.artist.empty() ? snap.album : snap.artist);
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    // Scrubber (position interpolated locally; seek committed on release).
    float barW = paneW - 32.0f;
    float frac = (snap.durationMs > 0) ? static_cast<float>(snap.positionMs) / snap.durationMs : 0.0f;
    if (scrubbing) frac = scrubFrac;
    float held = 0.0f;
    ImGui::SetCursorPosX(16.0f);
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
    ImGui::SetCursorPosX(16.0f);
    ImGui::TextUnformatted(left.c_str());
    ImGui::SameLine();
    float rw = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SetCursorPosX(16.0f + barW - rw);
    ImGui::TextUnformatted(right.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    // Transport, Spotify order: shuffle / prev / play / next / repeat.
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
        gui->prevCallback();
    }
    ImGui::PopFont();
    ImGui::SameLine();

    ImGui::PushFont(gui->playback_icon_font);
    const char* playIcon = snap.paused ? ICON_FA_PLAY_CIRCLE "###pp" : ICON_FA_PAUSE_CIRCLE "###pp";  // NOLINT
    if (iconButton(playIcon, ImVec2(72.0f, 64.0f), COL_WHITE, COL_CLEAR) && ready) {
        gui->playToggleCallback();
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

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // Volume (committed to cspot on release).
    float volFrac = snap.volume / 65535.0f;
    if (volSliding) volFrac = volSlideFrac;
    float volHeld = 0.0f;
    ImGui::SetCursorPosX(16.0f);
    bool volNow = barControl("vol", volFrac, ImVec2(barW, 22.0f), COL_GREENV, false, &volHeld);
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

void PlaybackScreen::drawLibrary(const PlayerModel::Snapshot&, float avail) {
    ImGui::PushFont(gui->font_bold);
    ImGui::TextUnformatted("Your Library");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (namesLeft > 0) {
        Spinner("Updating playlists...");
    }
    if (playlists.empty()) {
        if (libraryState == LoadState::LOADING) {
            Spinner("Loading your playlists...");
        } else if (!gui->api.has_token()) {
            greyText(gui->cspot_started ? "Signing in to Spotify..." : "Connecting to Spotify...");
        } else {
            greyText(libraryError.empty() ? "No playlists yet." : libraryError);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (pillButton("Try again", ImVec2(160.0f, 44.0f), COL_WHITE, IM_COL32(18, 18, 18, 255))) {
                loadLibrary();
            }
        }
    }
    for (size_t i = 0; i < playlists.size(); i++) {
        std::string id = "##pl" + std::to_string(i);
        const Playlist &p = playlists[i];
        if (listRow(id.c_str(), p.name, "", avail, COL_WHITE, gui->log_font)) {
            openPlaylist(static_cast<int>(i));
        }
    }
}

void PlaybackScreen::drawPlaylist(const PlayerModel::Snapshot& snap, float avail) {
    Playlist& pl = playlists[openIndex];
    ImGui::PushFont(gui->icon_font);
    bool back = iconButton(ICON_FA_ARROW_LEFT "##back", ImVec2(56.0f, 48.0f), COL_WHITE, COL_CLEAR);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(gui->font_bold);
    ImGui::TextUnformatted(pl.name.c_str());
    ImGui::PopFont();
    if (back) {
        openIndex = -1;
        cancelTrackLoads();   // stop streaming titles for a list nobody looks at
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    if (pillButton("Play", ImVec2(120.0f, 44.0f), COL_GREENV, IM_COL32(18, 18, 18, 255))) {
        playContext(pl.uri, 0);
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (pl.tracks.empty()) {
        if (pl.tracksState == LoadState::LOADING) {
            Spinner("Loading tracks...");
        } else if (pl.tracksState == LoadState::FAILED) {
            greyText("Could not load this playlist.");
            if (pillButton("Try again", ImVec2(160.0f, 44.0f), COL_WHITE, IM_COL32(18, 18, 18, 255))) {
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
    bool open = pillButton((label + "##q").c_str(), ImVec2(avail, 46.0f), IM_COL32(40, 40, 40, 255),
                           searchQuery.empty() ? COL_GREY : COL_WHITE);
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
    for (size_t i = 0; i < searchResults.size(); i++) {
        const std::string &l = searchResults[i].label;
        size_t nl = l.find('\n');
        std::string id = "##r" + std::to_string(i);
        if (listRow(id.c_str(), l.substr(0, nl), nl == std::string::npos ? "" : l.substr(nl + 1),
                    avail, COL_WHITE, gui->log_font)) {
            playTrack(searchResults[i].uri);
        }
    }
}

void PlaybackScreen::drawLog() {
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

    if (ImGui::Button("Refresh playlists", ImVec2(avail, 48.0f))) {
        loadLibrary();
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
    greyText("Shortcuts: START play/pause, L/R previous/next, SELECT next tab, right stick scrolls.");
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
            drawLog();
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

    ImVec2 nb(64.0f, 60.0f);
    AlignForWidth(nb.x * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f);

    static const struct { const char *icon; Tab tab; } kTabs[] = {
        {ICON_FA_MUSIC "##tl", Tab::LIBRARY}, {ICON_FA_SEARCH "##ts", Tab::SEARCH},
        {ICON_FA_BOOK "##tg", Tab::LOG}, {ICON_FA_COG "##tc", Tab::SETTINGS},
    };
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        if (StyleButton(kTabs[i].icon, nb, tab == kTabs[i].tab)) {
            // Tapping Library while inside a playlist goes back to the list.
            if (tab == Tab::LIBRARY && kTabs[i].tab == Tab::LIBRARY) openIndex = -1;
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
        if (pressed & SCE_CTRL_LTRIGGER) gui->prevCallback();
        if (pressed & SCE_CTRL_RTRIGGER) gui->nextCallback();
    }
    if (pressed & SCE_CTRL_SELECT) {
        tab = static_cast<Tab>((static_cast<int>(tab) + 1) % 4);
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
        // One scroll position per tab/view: the ID changes with the view, so
        // opening a playlist starts at its top instead of the library's offset.
        std::string browseId = "browse" + std::to_string(static_cast<int>(tab)) +
                               (tab == Tab::LIBRARY && openIndex >= 0 ? "_" + std::to_string(openIndex) : "");
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
    std::string out = "{\"tab\":" + json_quote(kTabs[static_cast<int>(tab)]) +
        ",\"library\":{\"state\":" + json_quote(stateName(libraryState)) +
        ",\"error\":" + json_quote(libraryError) +
        ",\"count\":" + std::to_string(playlists.size()) +
        ",\"names_left\":" + std::to_string(namesLeft) +
        ",\"playlists\":[";
    for (size_t i = 0; i < playlists.size() && i < 12; i++) {
        if (i) out += ",";
        out += json_quote(playlists[i].name);
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
    if (cmd == "back") {
        openIndex = -1;
        return true;
    }
    if (cmd == "refresh") {
        loadLibrary();
        return true;
    }
    return false;
}
