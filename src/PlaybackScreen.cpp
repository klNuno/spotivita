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

}  // namespace

PlaybackScreen::PlaybackScreen(GUI *gui) : Screen(gui) {
    LoadTextureFromFile("app0:cover_art.png", &placeholder_tex, &cover_art_width, &cover_art_height);
    cover_art_tex = placeholder_tex;
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
        gui->player.setPosition(ms);
        gui->api.seek(static_cast<uint32_t>(ms));
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
            gui->api.set_shuffle(shuffleOn);
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
            static const char* kModes[] = { "off", "context", "track" };
            gui->api.set_repeat(kModes[repeatMode]);
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
        gui->player.setVolume(v);
        if (gui->cspot_started && gui->volumeCallback) {
            gui->volumeCallback(v);
        }
    }
}

void PlaybackScreen::drawBrowse(const PlayerModel::Snapshot& snap) {
    float avail = ImGui::GetContentRegionAvail().x;

    switch (tab) {
        case Tab::LIBRARY: {
            // Gate on the token, not on cspot_started: it avoids pointless
            // 401s AND keeps this first fetch from racing the login5 curl
            // calls happening on the cspot thread at boot.
            if (!playlistsRequested && gui->api.has_token()) {
                playlistsRequested = true;
                getPlaylists();
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
                    gui->activateDevice();
                    gui->api.play_by_uri(pl.uri, 0, 0);
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::Dummy(ImVec2(0.0f, 4.0f));

                if (!pl.tracks_loaded) {
                    getTracks(openPlaylist);
                }
                for (uint32_t t = 0; t < pl.tracks.size(); t++) {
                    std::string lbl = pl.tracks[t] + "##t" + std::to_string(t);
                    // Currently playing track is tinted Spotify green.
                    ImU32 fg = (pl.tracks[t] == snap.name) ? COL_GREENV : COL_WHITE;
                    if (listRow(lbl.c_str(), avail, fg)) {
                        gui->activateDevice();
                        gui->api.play_by_uri(pl.uri, t, 0);
                    }
                }
            } else {
                ImGui::PushFont(gui->font_bold);
                ImGui::TextUnformatted("Your Library");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, 6.0f));

                if (playlists.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
                    ImGui::TextUnformatted(gui->api.has_token() ? "No playlists." : "Connecting...");
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
                    runSearch(q);
                }
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));

            for (size_t i = 0; i < searchResults.size(); i++) {
                std::string lbl = searchResults[i].label + "##r" + std::to_string(i);
                if (listRow(lbl.c_str(), avail, COL_WHITE)) {
                    gui->activateDevice();
                    gui->api.play_track(searchResults[i].uri);
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
            if (ImGui::Button("Refresh playlists", ImVec2(avail, 0.0f))) {
                playlists.clear();
                playlistsRequested = false;   // re-fetch on next Library visit
                getPlaylists();
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
    CSPOT_LOG(debug, "Get tracks for playlist: %d", index);
    std::string uri = playlists[index].uri;

    if (!uri.starts_with(SPOTIFY_PLAYLIST_HEADER)) {
        playlists[index].tracks_loaded = true;
        return;
    }
    uri.erase(0, strlen(SPOTIFY_PLAYLIST_HEADER));
    playlists[index].tracks.clear();

    bool next = true;
    uint32_t pos = 0;

    while (next) {
        uint8_t *json_data = NULL;
        // int, not size_t: the API returns -1 on no-token, which as an unsigned
        // size_t passes the `<= 0` check and dereferences uninitialized json_data.
        int json_len = gui->api.get_playlist_items(&json_data, uri, SPOTIFY_PLAYLIST_FIELDS,
                                                    SPOTIFY_TRACK_FETCH_CHUNK_SIZE, pos);

        if (json_len <= 0) {
            CSPOT_LOG(error, "error requesting songs from playlist");
            playlists[index].tracks_loaded = true;
            playlists[index].tracks.push_back("No tracks");
            return;
        }

        cJSON *root = cJSON_Parse((const char *) json_data);
        if (root == NULL || !cJSON_HasObjectItem(root, "items")) {
            playlists[index].tracks_loaded = true;
            playlists[index].tracks.push_back("No tracks");
            cJSON_Delete(root);
            free(json_data);
            return;
        }
        cJSON *json_next = cJSON_GetObjectItem(root, "next");
        cJSON *json_items = cJSON_GetObjectItem(root, "items");
        uint32_t tracks_in_chunk = cJSON_GetArraySize(json_items);

        for (uint32_t i = 0; i < tracks_in_chunk; i++) {
            cJSON *item = cJSON_GetArrayItem(json_items, i);
            cJSON *track = cJSON_GetObjectItem(item, "track");
            cJSON *trackName = cJSON_GetObjectItem(track, "name");
            if (cJSON_IsString(trackName) && trackName->valuestring != NULL) {
                playlists[index].tracks.push_back(std::string(trackName->valuestring));
            }
            pos++;
        }

        next = !cJSON_IsNull(json_next);
        cJSON_Delete(root);
        free(json_data);
    }
    playlists[index].tracks_loaded = true;
    CSPOT_LOG(debug, "Got %d tracks", pos);
}

void PlaybackScreen::getPlaylists() {
    if (!gui->api.has_token()) {
        return;
    }
    CSPOT_LOG(debug, "Get playlists");
    playlists.clear();

    bool next = true;
    uint32_t pos = 0;

    while (next) {
        uint8_t *json_data = NULL;
        int json_len = gui->api.get_current_users_playlists(&json_data,
                                                            SPOTIFY_PLAYLIST_FETCH_CHUNK_SIZE, pos);

        if (json_len <= 0) {
            CSPOT_LOG(error, "error requesting playlists");
            return;
        }

        cJSON *root = cJSON_Parse((const char *) json_data);
        if (root == NULL || !cJSON_HasObjectItem(root, "items")) {
            cJSON_Delete(root);
            free(json_data);
            return;
        }
        cJSON *json_next = cJSON_GetObjectItem(root, "next");
        cJSON *json_items = cJSON_GetObjectItem(root, "items");
        uint32_t playlists_in_chunk = cJSON_GetArraySize(json_items);

        for (uint32_t i = 0; i < playlists_in_chunk; i++) {
            cJSON *item = cJSON_GetArrayItem(json_items, i);
            cJSON *pname = cJSON_GetObjectItem(item, "name");
            cJSON *puri = cJSON_GetObjectItem(item, "uri");
            if (cJSON_IsString(pname) && pname->valuestring != NULL &&
                cJSON_IsString(puri) && puri->valuestring != NULL) {
                playlists.push_back({std::string(pname->valuestring),
                                     std::string(puri->valuestring), {}, false});
            }
            pos++;
        }

        next = !cJSON_IsNull(json_next);
        cJSON_Delete(root);
        free(json_data);
    }

    CSPOT_LOG(debug, "Got %d playlists", playlists.size());
}

void PlaybackScreen::runSearch(const std::string& query) {
    searchResults.clear();

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
