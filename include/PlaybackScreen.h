#pragma once

#include <imgui_vita2d/imgui.h>
#include "Render.h"
#include <atomic>
#include <string>
#include <vector>
#include "Screen.h"
#include "PlayerModel.h"

enum class LoadState { NONE, LOADING, LOADED, FAILED };

struct TrackRow {
    std::string name;     // "" until metadata arrives
    std::string artist;
    std::string uri;      // spotify:track:<id>
    uint32_t position;    // index inside the playlist, for play_context offsets
};

struct Playlist {
    std::string name;
    std::string uri;      // spotify:playlist:<id>
    std::vector<TrackRow> tracks;
    LoadState tracksState = LoadState::NONE;
};

struct SearchTrack {
    std::string label;    // "Title  -  Artist"
    std::string uri;
};

// Everything here runs on the GUI thread. Network work is posted to
// gui->net and its results come back as closures drained between frames, so no
// member is ever touched by two threads.
class PlaybackScreen: public Screen {
 public:
    explicit PlaybackScreen(GUI *gui);
    ~PlaybackScreen();
    void draw() override;
    std::string debugState() override;
    bool debugCommand(const std::string &cmd, const std::string &arg) override;

    // Called by the GUI loop between frames: starts a cover fetch when the
    // track changed.
    void tick();

 private:
    enum class Tab { LIBRARY, SEARCH, LOG, SETTINGS };

    void drawNowPlaying(const PlayerModel::Snapshot& snap);
    void drawBrowse(const PlayerModel::Snapshot& snap);
    void drawLibrary(const PlayerModel::Snapshot& snap, float avail);
    void drawPlaylist(const PlayerModel::Snapshot& snap, float avail);
    void drawSearch(float avail);
    void drawLog();
    void drawSettings(float avail);
    void drawNav();

    // Network actions (post jobs to the worker).
    void loadLibrary();
    void openPlaylist(int index);
    void cancelTrackLoads();
    void startSearch(const std::string& query);
    void fetchCover(const std::string& url);
    void playContext(const std::string& uri, uint32_t offset);
    void playTrack(const std::string& uri);
    void sendSeek(int ms);
    void sendShuffle(bool on);
    void sendRepeat(int mode);
    void reportPlayerError(long status);

    // Cover art. placeholder_tex is the bundled default; cover_art_tex points at
    // it until a real cover loads, and the old texture is freed on each change.
    vita2d_texture *placeholder_tex = nullptr;
    vita2d_texture *cover_art_tex = nullptr;
    std::string coverUrl;          // url of the cover shown or being fetched

    // Library
    std::vector<Playlist> playlists;
    LoadState libraryState = LoadState::NONE;
    std::string libraryError;
    int namesLeft = 0;             // names still being resolved (spinner)
    int openIndex = -1;            // -1 = playlist list, else index into playlists
    // Bumped to cancel an in-flight job: a job compares its captured value with
    // the live one before each request and before delivering.
    std::atomic<int> libraryGen{0};
    std::atomic<int> tracksGen{0};

    // Search
    std::string searchQuery;
    std::vector<SearchTrack> searchResults;
    LoadState searchState = LoadState::NONE;

    // Log view: copied from the logger only when it changed.
    std::string logCopy;
    unsigned logVersion = ~0u;

    Tab tab = Tab::LIBRARY;

    // Optimistic local mirrors of shuffle/repeat (set through the Web API;
    // Spotify routes the change back to this device via spirc).
    bool shuffleOn = false;
    int repeatMode = 0;            // 0 off, 1 context, 2 track

    // Scrubber / volume drag state (commit on release).
    bool scrubbing = false;
    float scrubFrac = 0.0f;
    bool volSliding = false;
    float volSlideFrac = 0.0f;
};
