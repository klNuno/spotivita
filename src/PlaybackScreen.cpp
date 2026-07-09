#include "PlaybackScreen.h"
#include "Font.h"
#include "Gui.h"
#include "GuiUtils.h"
#include "Keyboard.h"
#include "Utils.h"
#include "Config.h"
#include <JSONObject.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const ImU32 COL_TRACK  = IM_COL32(83, 83, 83, 255);    // #535353
const ImU32 COL_WHITE  = IM_COL32(255, 255, 255, 255);
const ImU32 COL_GREENV = IM_COL32(30, 215, 96, 255);   // #1ED760
const ImU32 COL_GREY   = IM_COL32(179, 179, 179, 255);  // #B3B3B3
const ImU32 COL_CLEAR  = IM_COL32(0, 0, 0, 0);

// Flat icon button with explicit glyph/background colors (StyleButton's
// active/inactive palette does not fit the Spotify transport row).
bool iconButton(const char* label, ImVec2 size, ImU32 fg, ImU32 bg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// Full-width, left-aligned list row (Spotify list style).
bool listRow(const char* label, float width, ImU32 fg, float height = 48.0f) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, COL_CLEAR);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 40, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    bool r = ImGui::Button(label, ImVec2(width, height));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
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
// left to the caller (on the held->released transition) so a drag shows live and
// the seek/volume action only fires once, on release.
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
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        *outFrac = frac;
    }
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = p.y + size.y * 0.5f;
    float h = 5.0f;
    dl->AddRectFilled(ImVec2(p.x, cy - h * 0.5f),
                      ImVec2(p.x + size.x, cy + h * 0.5f), COL_TRACK, h * 0.5f);
    dl->AddRectFilled(ImVec2(p.x, cy - h * 0.5f),
                      ImVec2(p.x + size.x * frac, cy + h * 0.5f), fill, h * 0.5f);
    if (knob) {
        dl->AddCircleFilled(ImVec2(p.x + size.x * frac, cy), 8.0f, fill);
    }
    ImGui::PopID();
    return held;
}

// Spinning arc + label, drawn inline. The heavy network runs off-frame (after
// the swap), so during a blocking curl the last frame stays on screen frozen;
// a visible spinner on that frame at least tells the user "working", instead of
// a dead still image with no feedback. When ticks are frequent (progressive
// name/track resolution) it actually animates.
void Spinner(const char* label, ImU32 col) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 9.0f;
    float cx = p.x + r, cy = p.y + r + 2.0f;
    float t = static_cast<float>(ImGui::GetTime());
    float a0 = t * 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathClear();
    dl->PathArcTo(ImVec2(cx, cy), r, a0, a0 + 4.2f, 24);
    dl->PathStroke(col, false, 2.5f);
    ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, r * 2.0f + 4.0f));
    if (label && *label) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(179, 179, 179, 255));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    }
}

// Playlist list is cached to disk so a relaunch shows the library instantly
// with ZERO network. The keymaster client_id is globally rate-limited, so
// re-fetching on every launch is what triggers the permanent 429 storm; fetch
// once, persist, and only refresh on explicit user request.
const char* PLAYLIST_CACHE_PATH = "ux0:data/cspot/playlists.json";

std::vector<Playlist> loadPlaylistCache() {
    std::vector<Playlist> out;
    FILE* f = fopen(PLAYLIST_CACHE_PATH, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return out; }
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
                out.push_back({std::string(nm->valuestring),
                               std::string(ur->valuestring), {}, false});
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

// Minimal protobuf wire reader: collect every length-delimited (wire type 2)
// field numbered `want` inside [p,end). Enough to walk the playlist4
// SelectedListContent message (nested length-delimited submessages) without
// generating a new nanopb proto. Field numbers verified against live responses:
//   rootlist:  contents=5 -> items=3(repeated) -> Item.uri=1
//   detail:    attributes=3 -> ListAttributes.name=1
std::vector<std::pair<const uint8_t*, size_t>> pbLenFields(
        const uint8_t* p, const uint8_t* end, int want) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    while (p < end) {
        uint64_t key = 0; int sh = 0; bool ok = false;
        while (p < end) { uint8_t b = *p++; 
                          if (sh >= 64) { ok = false; break; }
                          key |= static_cast<uint64_t>(b & 0x7F) << sh;
                          if (!(b & 0x80)) { ok = true; break; } sh += 7; }
        if (!ok) break;
        int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        if (wt == 2) {
            uint64_t len = 0; sh = 0; ok = false;
            while (p < end) { uint8_t b = *p++; 
                              if (sh >= 64) { ok = false; break; }
                              len |= static_cast<uint64_t>(b & 0x7F) << sh;
                              if (!(b & 0x80)) { ok = true; break; } sh += 7; }
            if (!ok || len > static_cast<size_t>(end - p)) break;
            if (field == want) out.push_back({p, static_cast<size_t>(len)});
            p += len;
        } else if (wt == 0) {
            while (p < end && (*p & 0x80)) p++;
            if (p < end) p++;
        } else if (wt == 5) { if (end - p < 4) break; p += 4; }
        else if (wt == 1) { if (end - p < 8) break; p += 8; }
        else break;
    }
    return out;
}

// Playlist ids (base62, after "spotify:playlist:") from a rootlist protobuf.
std::vector<std::string> parseRootlist(const uint8_t* data, size_t len) {
    std::vector<std::string> ids;
    const uint8_t* end = data + len;
    auto contents = pbLenFields(data, end, 5);       // SelectedListContent.contents
    if (contents.empty()) return ids;
    auto items = pbLenFields(contents[0].first, contents[0].first + contents[0].second, 3);
    const std::string pre = "spotify:playlist:";
    for (auto& it : items) {
        auto uris = pbLenFields(it.first, it.first + it.second, 1);   // Item.uri
        if (uris.empty()) continue;
        std::string uri(reinterpret_cast<const char*>(uris[0].first), uris[0].second);
        if (uri.rfind(pre, 0) == 0) ids.push_back(uri.substr(pre.size()));
    }
    return ids;
}

// Display name from a playlist detail protobuf ("" if absent, e.g. HTTP 400 on
// editorial 37i9... playlists whose detail endpoint rejects the request).
std::string parsePlaylistName(const uint8_t* data, size_t len) {
    const uint8_t* end = data + len;
    auto attrs = pbLenFields(data, end, 3);          // SelectedListContent.attributes
    if (attrs.empty()) return "";
    auto names = pbLenFields(attrs[0].first, attrs[0].first + attrs[0].second, 1);
    if (names.empty()) return "";
    return std::string(reinterpret_cast<const char*>(names[0].first), names[0].second);
}

}  // namespace

PlaybackScreen::PlaybackScreen(GUI *gui) : Screen(gui) {
    LoadTextureFromFile("app0:cover_art.png", &placeholder_tex, &cover_art_width, &cover_art_height);
    cover_art_tex = placeholder_tex;

    // Restore the cached playlist list: instant, no network. Marking the fetch
    // as already done stops the auto-fetch on the first Library visit (that is
    // what rate-limits the app on every launch).
    playlists = loadPlaylistCache();
    if (!playlists.empty()) {
        playlistsRequested = true;
    }
}

PlaybackScreen::~PlaybackScreen() {
    if (cover_art_tex != placeholder_tex && cover_art_tex != 0) {
        glDeleteTextures(1, &cover_art_tex);
    }
    if (placeholder_tex != 0) {
        glDeleteTextures(1, &placeholder_tex);
    }
}

void PlaybackScreen::processPendingCover() {
    if (pendingCoverUrl.empty()) {
        return;
    }
    std::string url = pendingCoverUrl;
    pendingCoverUrl.clear();
    setCoverArt(url);
}

// Off-frame executor: every blocking curl call the UI queued runs here, AFTER
// the swap, so the GPU is never held mid-frame. One action per call keeps the
// per-frame pause bounded to a single request.
void PlaybackScreen::runDeferred() {
    processPendingCover();

    // User-initiated actions first (one per frame so the pause stays bounded),
    // then the background browse fetches. This ordering matters: while playlists
    // are rate-limited, wantPlaylists is re-set every frame, so it must not
    // starve a play/seek the user just tapped.
    if (wantPlay) {
        wantPlay = false;
        gui->activateDevice();
        gui->api.play_by_uri(wantPlayUri, wantPlayOffset, 0);
        return;
    }
    if (!wantPlayTrack.empty()) {
        std::string u = wantPlayTrack;
        wantPlayTrack.clear();
        gui->activateDevice();
        gui->api.play_track(u);
        return;
    }
    if (wantSeek >= 0) {
        int s = wantSeek;
        wantSeek = -1;
        gui->api.seek(static_cast<uint32_t>(s));
        return;
    }
    if (wantShuffle >= 0) {
        int s = wantShuffle;
        wantShuffle = -1;
        gui->api.set_shuffle(s != 0);
        return;
    }
    if (wantRepeat >= 0) {
        static const char* kModes[] = { "off", "context", "track" };
        int r = wantRepeat;
        wantRepeat = -1;
        if (r >= 0 && r < 3) gui->api.set_repeat(kModes[r]);
        return;
    }
    if (wantVolume >= 0) {
        int v = wantVolume;
        wantVolume = -1;
        if (gui->cspot_started && gui->volumeCallback) gui->volumeCallback(v);
        return;
    }
    if (!wantSearch.empty()) {
        std::string q = wantSearch;
        wantSearch.clear();
        runSearch(q);
        return;
    }
    if (wantTracks >= 0) {
        int i = wantTracks;
        wantTracks = -1;
        if (i < static_cast<int>(playlists.size())) getTracks(static_cast<uint16_t>(i));
        return;
    }
    if (wantPlaylists) {
        wantPlaylists = false;
        getPlaylists();
        return;
    }
    // Lowest priority: resolve one playlist name per frame after a rootlist
    // fetch. Runs off-frame like everything else, so the ~50-request burst
    // never blocks the GPU; the UI shows names filling in, then it's cached.
    if (namesPending) {
        resolveNextName();
        return;
    }
}

// Runs on the GUI thread (texture creation must not happen on the cspot worker).
void PlaybackScreen::setCoverArt(std::string url) {
    GLuint tex = 0;
    int w = 0, h = 0;
    bool ok = false;

    if (is_cover_cached(url)) {
        ok = LoadTextureFromFile(cover_art_path(url).c_str(), &tex, &w, &h);
    } else {
        uint8_t* png = NULL;
        int len = download(url.c_str(), &png);
        if (len > 0 && png != NULL) {
            ok = LoadTextureFromMemory(png, len, &tex, &w, &h);
            if (ok) {
                cache_cover_art(url, png, len);
            }
        }
        if (png != NULL) {
            free(png);
        }
    }

    if (ok) {
        if (cover_art_tex != placeholder_tex && cover_art_tex != 0) {
            glDeleteTextures(1, &cover_art_tex);
        }
        cover_art_tex = tex;
        cover_art_width = w;
        cover_art_height = h;
    } else if (tex != 0) {
        glDeleteTextures(1, &tex);
    }
}

void PlaybackScreen::drawNowPlaying(const PlayerModel::Snapshot& snap) {
    float paneW = ImGui::GetContentRegionAvail().x;
    // Sized to fit inside 544 with no scroll: cover 190 + text + scrubber +
    // 64 px transport + volume all sum under the ~480 px child content area.
    float coverSz = paneW - 90.0f;
    if (coverSz > 190.0f) coverSz = 190.0f;
    if (coverSz < 110.0f) coverSz = 110.0f;

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::SetCursorPosX((paneW - coverSz) * 0.5f);
    ImGui::Image((void*)(intptr_t)cover_art_tex, ImVec2(coverSz, coverSz));

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    ImGui::PushFont(gui->font_bold);
    TextCentered(snap.name);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    TextCentered(snap.artist.empty() ? snap.album : snap.artist);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    // Scrubber (position interpolated locally; seek committed on release).
    float barW = paneW - 32.0f;
    float frac = (snap.durationMs > 0) ? static_cast<float>(snap.positionMs) / snap.durationMs : 0.0f;
    if (scrubbing) frac = scrubFrac;
    float held = 0.0f;
    ImGui::SetCursorPosX(16.0f);
    bool nowHeld = barControl("scrub", frac, ImVec2(barW, 20.0f), COL_WHITE, true, &held);
    if (nowHeld) {
        scrubbing = true;
        scrubFrac = held;
    } else if (scrubbing) {
        scrubbing = false;
        int ms = static_cast<int>(scrubFrac * snap.durationMs);
        if (ms < 0) ms = 0;
        gui->player.setPosition(ms);   // instant local feedback
        wantSeek = ms;                 // Web API seek runs off-frame
    }

    int shownMs = scrubbing ? static_cast<int>(scrubFrac * snap.durationMs) : snap.positionMs;
    std::string left = fmtTime(shownMs);
    std::string right = fmtTime(snap.durationMs);
    ImGui::PushFont(gui->log_font);
    ImGui::SetCursorPosX(16.0f);
    ImGui::TextUnformatted(left.c_str());
    ImGui::SameLine();
    float rw = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SetCursorPosX(16.0f + barW - rw);
    ImGui::TextUnformatted(right.c_str());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    // Transport, Spotify order: shuffle / prev / play / next / repeat.
    // Uniform 64 px height keeps the row aligned; widths vary.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 50.0f);
    AlignForWidth(52.0f + 64.0f + 72.0f + 64.0f + 52.0f + 8.0f * 4.0f);

    ImGui::PushFont(gui->icon_font);
    if (iconButton(ICON_FA_RANDOM "##shuffle", ImVec2(52.0f, 64.0f),
                   shuffleOn ? COL_GREENV : COL_GREY, COL_CLEAR)) {
        if (gui->cspot_started) {
            shuffleOn = !shuffleOn;
            wantShuffle = shuffleOn ? 1 : 0;
        }
    }
    ImGui::SameLine();

    if (iconButton(ICON_FA_STEP_BACKWARD "##prev", ImVec2(64.0f, 64.0f), COL_WHITE, COL_CLEAR)) {
        if (gui->cspot_started) gui->prevCallback();
    }
    ImGui::PopFont();
    ImGui::SameLine();

    ImGui::PushFont(gui->playback_icon_font);
    const char* playIcon = snap.paused ? ICON_FA_PLAY_CIRCLE "###pp" : ICON_FA_PAUSE_CIRCLE "###pp";  // NOLINT
    if (iconButton(playIcon, ImVec2(72.0f, 64.0f), COL_WHITE, COL_CLEAR)) {
        if (gui->cspot_started) gui->playToggleCallback();
    }
    ImGui::PopFont();
    ImGui::SameLine();

    ImGui::PushFont(gui->icon_font);
    if (iconButton(ICON_FA_STEP_FORWARD "##next", ImVec2(64.0f, 64.0f), COL_WHITE, COL_CLEAR)) {
        if (gui->cspot_started) gui->nextCallback();
    }
    ImGui::SameLine();

    if (iconButton(ICON_FA_REDO "##repeat", ImVec2(52.0f, 64.0f),
                   repeatMode != 0 ? COL_GREENV : COL_GREY, COL_CLEAR)) {
        if (gui->cspot_started) {
            repeatMode = (repeatMode + 1) % 3;
            wantRepeat = repeatMode;
        }
    }
    ImGui::PopFont();

    ImGui::PopStyleVar(2);

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // Volume (committed to cspot on release).
    float volFrac = snap.volume / 65535.0f;
    if (volSliding) volFrac = volSlideFrac;
    float volHeld = 0.0f;
    ImGui::SetCursorPosX(16.0f);
    bool volNow = barControl("vol", volFrac, ImVec2(barW, 18.0f), COL_GREENV, false, &volHeld);
    if (volNow) {
        volSliding = true;
        volSlideFrac = volHeld;
    } else if (volSliding) {
        volSliding = false;
        int v = static_cast<int>(volSlideFrac * 65535.0f);
        gui->player.setVolume(v);   // instant local feedback
        wantVolume = v;             // pushed to cspot off-frame
    }
}

void PlaybackScreen::drawBrowse(const PlayerModel::Snapshot& snap) {
    float avail = ImGui::GetContentRegionAvail().x;

    switch (tab) {
        case Tab::LIBRARY: {
            // ONE fetch, ever: only when we have a token, have never fetched,
            // and have no cached list. NO auto-retry on rate limit -- retrying
            // is exactly what sustains the global 429. If it fails the user hits
            // "Refresh playlists" in Settings, which clears playlistsRequested.
            // Gate on the token (not cspot_started) to avoid racing the login5
            // curl on the cspot thread at boot.
            if (gui->api.has_token() && !playlistsRequested) {
                playlistsRequested = true;
                wantPlaylists = true;
            }

            if (openPlaylist >= 0 && openPlaylist < static_cast<int>(playlists.size())) {
                // Track view of one playlist, with a back row on top.
                Playlist& pl = playlists[openPlaylist];
                ImGui::PushFont(gui->icon_font);
                bool back = iconButton(ICON_FA_ARROW_LEFT "##back", ImVec2(56.0f, 48.0f),
                                       COL_WHITE, COL_CLEAR);
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::PushFont(gui->font_bold);
                ImGui::TextUnformatted(pl.name.c_str());
                ImGui::PopFont();
                if (back) {
                    openPlaylist = -1;
                    break;
                }
                ImGui::Dummy(ImVec2(0.0f, 4.0f));

                ImGui::PushStyleColor(ImGuiCol_Button, COL_GREENV);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_GREENV);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(18, 18, 18, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22.0f);
                if (ImGui::Button("Play", ImVec2(120.0f, 44.0f))) {
                    wantPlay = true; wantPlayUri = pl.uri; wantPlayOffset = 0;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::Dummy(ImVec2(0.0f, 4.0f));

                if (!pl.tracks_loaded) {
                    wantTracks = openPlaylist;
                }
                if (!pl.tracks_loaded && pl.tracks.empty()) {
                    Spinner("Chargement des titres...", COL_GREENV);
                }
                for (uint32_t t = 0; t < pl.tracks.size(); t++) {
                    std::string lbl = pl.tracks[t] + "##t" + std::to_string(t);
                    // Currently playing track is tinted Spotify green.
                    ImU32 fg = (pl.tracks[t] == snap.name) ? COL_GREENV : COL_WHITE;
                    if (listRow(lbl.c_str(), avail, fg)) {
                        wantPlay = true; wantPlayUri = pl.uri; wantPlayOffset = t;
                    }
                }
            } else {
                ImGui::PushFont(gui->font_bold);
                ImGui::TextUnformatted("Your Library");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, 6.0f));

                if (namesPending) {
                    Spinner("Chargement...", COL_GREENV);
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                }
                if (playlists.empty()) {
                    const char* msg = !gui->api.has_token() ? "Connecting..."
                                    : rateLimited ? "Rate limited. Try Refresh in Settings later."
                                    : "No playlists yet. Refresh in Settings.";
                    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
                    ImGui::TextUnformatted(msg);
                    ImGui::PopStyleColor();
                }
                for (uint16_t i = 0; i < playlists.size(); i++) {
                    std::string lbl = playlists[i].name + "##p" + std::to_string(i);
                    if (listRow(lbl.c_str(), avail, COL_WHITE)) {
                        openPlaylist = i;
                    }
                }
            }
            break;
        }
        case Tab::SEARCH: {
            ImGui::PushFont(gui->font_bold);
            ImGui::TextUnformatted("Search");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22.0f);
            std::string label = searchQuery.empty() ? std::string("Tap to search Spotify...")
                                                     : searchQuery;
            if (ImGui::Button(label.c_str(), ImVec2(avail, 44.0f))) {
                std::string q = Keyboard::GetText("Search Spotify");
                if (!q.empty()) {
                    searchQuery = q;
                    wantSearch = q;
                    searchPending = true;
                    searchResults.clear();
                }
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));

            if (searchPending) {
                Spinner("Recherche...", COL_GREENV);
            } else if (!searchQuery.empty() && searchResults.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
                ImGui::TextUnformatted("Aucun resultat.");
                ImGui::PopStyleColor();
            }
            for (size_t i = 0; i < searchResults.size(); i++) {
                std::string lbl = searchResults[i].label + "##r" + std::to_string(i);
                if (listRow(lbl.c_str(), avail, COL_WHITE)) {
                    wantPlayTrack = searchResults[i].uri;
                }
            }
            break;
        }
        case Tab::LOG:
            ImGui::PushFont(gui->log_font);
            ImGui::TextUnformatted(getBuf()->begin());
            ImGui::PopFont();
            if (getScrollToBottom()) {
                ImGui::SetScrollHere(1.0f);
            }
            setScrollToBottom(false);
            break;
        case Tab::SETTINGS:
            ImGui::PushFont(gui->font_bold);
            ImGui::TextUnformatted("Settings");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            // Keep the current (cached) list on screen; getPlaylists only
            // replaces it on a successful fetch, so a failed refresh doesn't
            // wipe what the user could already browse.
            if (ImGui::Button("Refresh playlists", ImVec2(avail, 0.0f))) {
                playlistsRequested = false;   // allow exactly one more fetch
                backoffStep = 0;
                backoffUntilUs = 0;
                wantPlaylists = true;
            }
            if (ImGui::Button("Logout", ImVec2(avail, 0.0f))) {
                remove(CREDENTIALS_FILE_NAME);
                gui->isRunning = false;
            }
            if (ImGui::Button("Exit", ImVec2(avail, 0.0f))) {
                gui->isRunning = false;
            }
            break;
    }
}

void PlaybackScreen::drawNav() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushFont(gui->icon_font);

    ImVec2 nb(60.0f, 60.0f);
    AlignForWidth(nb.x * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f);

    if (StyleButton(ICON_FA_MUSIC, nb, tab == Tab::LIBRARY)) {
        tab = Tab::LIBRARY;
    }
    ImGui::SameLine();
    if (StyleButton(ICON_FA_SEARCH, nb, tab == Tab::SEARCH)) {
        tab = Tab::SEARCH;
    }
    ImGui::SameLine();
    if (StyleButton(ICON_FA_BOOK, nb, tab == Tab::LOG)) {
        tab = Tab::LOG;
    }
    ImGui::SameLine();
    if (StyleButton(ICON_FA_COG, nb, tab == Tab::SETTINGS)) {
        tab = Tab::SETTINGS;
    }

    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

void PlaybackScreen::draw() {
    // One coherent snapshot per frame. A changed cover URL is only RECORDED
    // here; the download + GL upload happen in processPendingCover(), outside
    // the open ImGui frame (network mid-frame can wedge SceGxm).
    PlayerModel::Snapshot snap = gui->player.snapshot();
    if (snap.imageUrl != loadedCoverUrl) {
        loadedCoverUrl = snap.imageUrl;
        if (!snap.imageUrl.empty()) {
            pendingCoverUrl = snap.imageUrl;
        }
    }

    float fullW = ImGui::GetContentRegionAvail().x;
    float leftW = fullW * 0.46f;

    // Now-playing is fixed-size: forbid scrolling so a button press near the
    // bottom can't auto-scroll the pane (the "interface scrolls down on
    // actions" bug). The browse list keeps its scrollbar for long playlists.
    const ImGuiWindowFlags kNoScroll = ImGuiWindowFlags_NavFlattened |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::BeginChild("nowplaying", ImVec2(leftW, 0.0f), false, kNoScroll);
    drawNowPlaying(snap);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NavFlattened);
    {
        float navH = 80.0f;
        ImGui::BeginChild("browse", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - navH),
                          false, ImGuiWindowFlags_NavFlattened);
        drawBrowse(snap);
        ImGui::EndChild();

        ImGui::BeginChild("nav", ImVec2(0.0f, 0.0f), false, kNoScroll);
        drawNav();
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void PlaybackScreen::getTracks(uint16_t index) {
    // Shared rate-limit backoff (same window as getPlaylists): don't retry a
    // 429'd track fetch every frame.
    if (sceKernelGetProcessTimeWide() < backoffUntilUs) {
        return;
    }
    CSPOT_LOG(debug, "Get tracks for playlist: %d", index);
    std::string uri = playlists[index].uri;

    if (!uri.starts_with(SPOTIFY_PLAYLIST_HEADER)) {
        playlists[index].tracks_loaded = true;
        return;
    }
    uri.erase(0, strlen(SPOTIFY_PLAYLIST_HEADER));
    playlists[index].tracks.clear();

    // ONE request, first 50 tracks. Same shared-client rate limit as the
    // playlist list: paginating a long playlist in a burst instantly 429s.
    uint8_t *json_data = NULL;
    int json_len = gui->api.get_playlist_items(&json_data, uri, SPOTIFY_PLAYLIST_FIELDS,
                                               SPOTIFY_TRACK_FETCH_CHUNK_SIZE, 0);

    if (json_len <= 0) {
        CSPOT_LOG(error, "error requesting songs from playlist");
        if (json_data != NULL) free(json_data);
        // Not tracks_loaded: let the user retry by reopening; a transient 429
        // shouldn't permanently show an empty playlist.
        return;
    }

    cJSON *root = cJSON_Parse((const char *) json_data);
    free(json_data);
    if (root == NULL) {
        return;
    }
    if (cJSON_HasObjectItem(root, "error")) {
        rateLimited = true;
        if (backoffStep < 8) backoffStep++;
        backoffUntilUs = sceKernelGetProcessTimeWide() +
                         static_cast<uint64_t>(backoffStep) * 30000000ULL;
        cJSON_Delete(root);
        return;
    }
    cJSON *json_items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(json_items)) {
        playlists[index].tracks_loaded = true;
        cJSON_Delete(root);
        return;
    }
    int count = cJSON_GetArraySize(json_items);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(json_items, i);
        cJSON *track = cJSON_GetObjectItem(item, "track");
        cJSON *trackName = cJSON_GetObjectItem(track, "name");
        if (cJSON_IsString(trackName) && trackName->valuestring != NULL) {
            playlists[index].tracks.push_back(std::string(trackName->valuestring));
        }
    }
    playlists[index].tracks_loaded = true;
    cJSON_Delete(root);
    CSPOT_LOG(debug, "Got %d tracks", count);
}

void PlaybackScreen::getPlaylists() {
    if (!gui->api.has_token()) {
        return;
    }
    CSPOT_LOG(debug, "Get rootlist (spclient)");
    // Fetch the user's playlist list over spclient (NOT api.spotify.com, which
    // 429s on the shared keymaster client_id). Response is playlist4 protobuf:
    // a list of playlist URIs only -- names are resolved one-by-one afterwards
    // by resolveNextName(), off-frame, then cached. Retry a few times: the
    // Vita's DNS resolver is flaky on the FIRST lookup for a host (cold
    // connection); once it resolves, keep-alive reuses it for the name fetches.
    uint8_t *data = NULL;
    int len = 0;
    for (int attempt = 0; attempt < 3 && len <= 0; attempt++) {
        if (data != NULL) {
            free(data);
            data = NULL;
        }
        len = gui->api.get_rootlist(&data, SPOTIFY_PLAYLIST_FETCH_CHUNK_SIZE);
    }
    if (len <= 0 || data == NULL) {
        CSPOT_LOG(error, "rootlist request failed");
        if (data != NULL) free(data);
        return;
    }

    std::vector<std::string> ids = parseRootlist(data, static_cast<size_t>(len));
    free(data);
    if (ids.empty()) {
        CSPOT_LOG(error, "rootlist parse: no playlists");
        return;
    }

    rateLimited = false;
    backoffStep = 0;
    std::vector<Playlist> fresh;
    for (const auto& id : ids) {
        // Placeholder name until resolveNextName() fills it in; keep the full
        // "spotify:playlist:<id>" URI so getTracks/play still work unchanged.
        fresh.push_back({"\xE2\x80\xA6",  // horizontal ellipsis
                         SPOTIFY_PLAYLIST_HEADER + id, {}, false});
    }
    playlists = std::move(fresh);
    namesPending = true;
    nameCursor = 0;
    nameFailStreak = 0;
    CSPOT_LOG(debug, "Rootlist: %d playlists, resolving names", playlists.size());
}

// One spclient playlist-detail fetch per call to resolve a display name. Walks
// nameCursor through the list; when done, persists the named list to disk so
// the next launch is instant with zero network.
void PlaybackScreen::resolveNextName() {
    if (!namesPending) {
        return;
    }
    if (nameCursor >= static_cast<int>(playlists.size())) {
        namesPending = false;
        savePlaylistCache(playlists);
        CSPOT_LOG(debug, "Playlist names resolved, cached");
        return;
    }

    Playlist& pl = playlists[nameCursor];
    std::string id = pl.uri;
    if (id.rfind(SPOTIFY_PLAYLIST_HEADER, 0) == 0) {
        id.erase(0, strlen(SPOTIFY_PLAYLIST_HEADER));
    }

    uint8_t *data = NULL;
    int len = gui->api.get_playlist_detail(&data, id);
    if (len > 0 && data != NULL) {
        std::string name = parsePlaylistName(data, static_cast<size_t>(len));
        pl.name = name.empty() ? std::string("Playlist") : name;
        nameFailStreak = 0;
    } else {
        pl.name = "Playlist";
        // spclient unreachable (DNS/network). Don't grind through all 45 -- that
        // is what starved Mercury and crashed. Bail after a short streak; the
        // user can retry via Settings > Refresh once the network settles.
        if (++nameFailStreak >= 5) {
            CSPOT_LOG(error, "name resolution: %d consecutive failures, aborting", nameFailStreak);
            namesPending = false;
            savePlaylistCache(playlists);
            if (data != NULL) free(data);
            return;
        }
    }
    if (data != NULL) free(data);
    nameCursor++;
}

void PlaybackScreen::runSearch(const std::string& query) {
    searchResults.clear();
    searchPending = false;   // request done (success or fail); stop the spinner

    uint8_t* data = NULL;
    int len = gui->api.search(&data, query, "track", 10);
    if (len <= 0 || data == NULL) {
        if (data != NULL) free(data);
        return;
    }

    cJSON* root = cJSON_Parse((const char *) data);
    free(data);
    if (root == NULL) {
        return;
    }

    cJSON* tracks = cJSON_GetObjectItem(root, "tracks");
    cJSON* items = tracks ? cJSON_GetObjectItem(tracks, "items") : NULL;
    if (items != NULL) {
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n; i++) {
            cJSON* it = cJSON_GetArrayItem(items, i);
            cJSON* name = cJSON_GetObjectItem(it, "name");
            cJSON* uri = cJSON_GetObjectItem(it, "uri");
            cJSON* artists = cJSON_GetObjectItem(it, "artists");

            std::string artistName;
            if (artists != NULL && cJSON_GetArraySize(artists) > 0) {
                cJSON* a0 = cJSON_GetArrayItem(artists, 0);
                cJSON* an = cJSON_GetObjectItem(a0, "name");
                if (cJSON_IsString(an) && an->valuestring != NULL) {
                    artistName = an->valuestring;
                }
            }
            if (cJSON_IsString(name) && name->valuestring != NULL &&
                cJSON_IsString(uri) && uri->valuestring != NULL) {
                std::string lbl = std::string(name->valuestring);
                if (!artistName.empty()) {
                    lbl += "  -  " + artistName;
                }
                searchResults.push_back({lbl, std::string(uri->valuestring)});
            }
        }
    }
    cJSON_Delete(root);
}
