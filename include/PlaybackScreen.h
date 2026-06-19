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

    void getTracks(uint16_t index);
    void getPlaylists();
    void runSearch(const std::string& query);
    void setCoverArt(std::string url);

 private:
    enum class Tab { LIBRARY, SEARCH, LOG, SETTINGS };

    void drawNowPlaying(const PlayerModel::Snapshot& snap);
    void drawBrowse();
    void drawNav();

    // Cover art. placeholder_tex is the bundled default; cover_art_tex points at
    // it until a real cover loads, and the old downloaded texture is freed on
    // each change (the previous code leaked one GL texture per track).
    GLuint placeholder_tex = 0;
    GLuint cover_art_tex = 0;
    int cover_art_width = 0;
    int cover_art_height = 0;
    std::string loadedCoverUrl;

    // Browse state
    std::vector<Playlist> playlists;
    bool playlistsRequested = false;
    std::vector<SearchTrack> searchResults;
    std::string searchQuery;
    Tab tab = Tab::LIBRARY;

    // Scrubber / volume drag state (commit on release).
    bool scrubbing = false;
    float scrubFrac = 0.0f;
    bool volSliding = false;
    float volSlideFrac = 0.0f;
};
