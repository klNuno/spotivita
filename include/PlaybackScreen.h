#pragma once

#include <imgui_vita.h>
#include <vitaGL.h>
#include <vector>
#include <string>
#include "Screen.h"
#include "PlayerModel.h"

struct Playlist {
    std::string name;
    std::string uri;
    std::vector<std::string> tracks;
    bool tracks_loaded;
};

struct SearchTrack {
    std::string label;   // "Title  -  Artist"
    std::string uri;
};

class PlaybackScreen: public Screen {
 public:
    explicit PlaybackScreen(GUI *gui);
    ~PlaybackScreen();
    void draw();
    // Runs OUTSIDE the ImGui frame (called by the GUI loop after the swap):
    // downloads + uploads a pending cover. Network inside an open frame would
    // hold the GPU mid-frame for up to 30 s and can wedge SceGxm.
    void processPendingCover();

    void getTracks(uint16_t index);
    void getPlaylists();
    void runSearch(const std::string& query);
    void setCoverArt(std::string url);
    // Executes the queued network actions. MUST be called by the GUI loop AFTER
    // vglSwapBuffers, never inside the ImGui frame: curl blocks, and blocking
    // mid-frame holds the GPU and wedges the whole console on slow wifi.
    void runDeferred();

 private:
    enum class Tab { LIBRARY, SEARCH, LOG, SETTINGS };

    void drawNowPlaying(const PlayerModel::Snapshot& snap);
    void drawBrowse(const PlayerModel::Snapshot& snap);
    void drawNav();

    // Cover art. placeholder_tex is the bundled default; cover_art_tex points at
    // it until a real cover loads, and the old downloaded texture is freed on
    // each change (the previous code leaked one GL texture per track).
    GLuint placeholder_tex = 0;
    GLuint cover_art_tex = 0;
    int cover_art_width = 0;
    int cover_art_height = 0;
    std::string loadedCoverUrl;
    std::string pendingCoverUrl;

    // Browse state
    std::vector<Playlist> playlists;
    bool playlistsRequested = false;
    bool rateLimited = false;
    uint64_t backoffUntilUs = 0;
    int backoffStep = 0;

    // Deferred network intents, set by the UI, run in runDeferred() off-frame.
    bool wantPlaylists = false;
    int wantTracks = -1;
    std::string wantSearch;
    bool wantPlay = false;
    std::string wantPlayUri;
    uint32_t wantPlayOffset = 0;
    std::string wantPlayTrack;
    int wantSeek = -1;
    int wantShuffle = -1;
    int wantRepeat = -1;
    int wantVolume = -1;
    std::vector<SearchTrack> searchResults;
    std::string searchQuery;
    Tab tab = Tab::LIBRARY;
    int openPlaylist = -1;   // -1 = playlist list, else index into playlists

    // Optimistic local mirrors of shuffle/repeat (set through the Web API;
    // Spotify routes the change back to this device via spirc).
    bool shuffleOn = false;
    int repeatMode = 0;      // 0 off, 1 context, 2 track

    // Scrubber / volume drag state (commit on release).
    bool scrubbing = false;
    float scrubFrac = 0.0f;
    bool volSliding = false;
    float volSlideFrac = 0.0f;
};
