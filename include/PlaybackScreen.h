#pragma once

#include <imgui_vita2d/imgui.h>
#include "Render.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "Screen.h"
#include "PlayerModel.h"
#include "ConnectWatch.h"
#include "Catalog.h"
#include "Thumbs.h"

enum class LoadState { NONE, LOADING, LOADED, FAILED };

struct TrackRow {
    std::string name;     // "" until metadata arrives
    std::string artist;
    std::string album;
    std::string uri;      // spotify:track:<id>
    uint32_t position = 0;  // index inside the playlist (custom order)
    int durationMs = 0;
    int64_t addedMs = 0;  // when it joined the playlist, 0 if unknown
};

// Track orders of the sort menu, in menu order.
enum TrackSort { SORT_CUSTOM, SORT_TITLE, SORT_ARTIST, SORT_ALBUM, SORT_ADDED, SORT_DURATION,
                 SORT_COUNT };

// A track list: a library playlist, Liked Songs, or an album or playlist
// opened from search or an artist page (external: kept for this run only).
struct Playlist {
    std::string name;
    std::string uri;      // spotify:playlist:<id>, spotify:album:<id>, or LIKED_SONGS_URI
    int folder = -1;      // index into folders, -1 = library root
    int length = 0;       // track count from the rootlist, 0 if unknown
    std::vector<TrackRow> tracks;
    LoadState tracksState = LoadState::NONE;
    bool fresh = false;   // track list fetched from Spotify this run
    int sort = SORT_CUSTOM;
    bool sortDesc = false;
    std::string filter;   // matched on title, artist and album
    bool external = false;
    // Header of an external list: cover, "Album, 2001" or "By Spotify", and
    // the album's artists.
    std::string imageUrl;
    std::string subtitle;
    std::vector<Link> artists;
};

// A folder of the Spotify library: the rootlist brackets its playlists with
// spotify:start-group:<id>:<name> and spotify:end-group:<id>.
struct Folder {
    std::string id;
    std::string name;
    int parent = -1;      // -1 = library root
};

// One library row, in rootlist order.
struct LibraryEntry {
    bool isFolder;
    int index;            // into folders or playlists
};

// Metadata of the track another device plays (the cluster carries only its
// URI when the device is a Spotify Connect speaker).
struct RemoteTrack {
    std::string uri, name, artist, album, imageUrl;
    int durationMs = 0;
};

// A page opened over the library or the search results; back pops it.
struct Page {
    enum Kind { LIST, ARTIST, SECTION };
    Kind kind = LIST;
    std::string uri;      // the list, or the artist
    int section = 0;      // SECTION: the ArtistSection shown whole
};

struct ArtistView {
    ArtistInfo info;
    LoadState state = LoadState::NONE;
    bool loadingMore[AS_COUNT] = {};
    bool paged[AS_COUNT] = {};   // the section came from its own query
};

// A track's album and artists: the track menu, and the links of the track
// the now-playing pane shows.
struct TrackLinks {
    std::string uri, name, artist;
    Link album;
    std::vector<Link> artists;
    LoadState state = LoadState::NONE;
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
    // The part of tick() that also runs while the app is in the background:
    // the next part of a long queue, the sleep timer.
    void tickPlayback();

 private:
    enum class Tab { LIBRARY, SEARCH, LOG, SETTINGS };

    void drawNowPlaying(const PlayerModel::Snapshot& snap);
    void drawTransport(const PlayerModel::Snapshot& snap);
    void drawBrowse(const PlayerModel::Snapshot& snap);
    void drawLibrary(const PlayerModel::Snapshot& snap, float avail);
    void drawPlaylist(const PlayerModel::Snapshot& snap, float avail);
    void drawSearch(const PlayerModel::Snapshot& snap, float avail);
    void drawLog(float avail);
    bool drawBackHeader(const std::string &title, float avail);
    void drawSettings(float avail);
    void drawSleepTimer(float avail);
    void drawNav();
    void drawPlaylistActions(Playlist &pl, const PlayerModel::Snapshot& snap, float avail);
    void drawFastScroll(const Playlist &pl, float y0, float step);
    void drawListHeader(const Playlist &pl, float avail);
    void drawSearchAll(const std::string &playing, float avail);
    void drawSearchKind(int kind, const std::string &playing, float avail);
    void drawArtist(const std::string &uri, const std::string &playing, float avail);
    void drawSection(const std::string &uri, int section, const std::string &playing, float avail);
    // A search or artist-page row with its cover; tracks get a menu button.
    // True when the row itself was tapped.
    bool drawItem(const CatalogItem &it, const std::string &id, float width, bool current);
    // The three-dot button at the end of a track row.
    bool moreButton(const std::string &id);
    void drawTrackMenu();
    // Debug server: the catalog part of the state (JSON members, no braces)
    // and its commands.
    std::string browseState();
    bool browseCommand(const std::string &cmd, const std::string &arg);

    // Previous restarts the track past its first seconds, like Spotify.
    void previous(const PlayerModel::Snapshot& snap);
    void togglePlay(const PlayerModel::Snapshot& snap);
    void skipNext();
    // Pops the page shown, then leaves the open folder. False at the top.
    bool goBack();

    // Navigation. Library and Search each keep a stack of pages; the other
    // tabs open pages in the library.
    std::vector<Page> *pageStack();
    const Page *topPage();
    void pushPage(const Page &page);
    void clearPages(int which);
    // Points openIndex at the list the top page shows and starts what the
    // page still needs to load.
    void syncOpen();
    void openList(const std::string &uri, const std::string &name = "",
                  const std::string &imageUrl = "", const std::string &subtitle = "");
    void openArtist(const std::string &uri);
    void openSection(const std::string &artistUri, int section);
    void openItem(const CatalogItem &it);
    void openTrackMenu(const std::string &uri, const std::string &name, const std::string &artist,
                       const Link &album, const std::vector<Link> &links);
    void goNowAlbum();
    void goNowArtist();

    // Swaps in a fresh library, keeping loaded tracks and the open view.
    void setLibrary(std::vector<Folder> f, std::vector<Playlist> p,
                    std::vector<LibraryEntry> o);

    // Network actions (post jobs to the worker).
    void loadLibrary();
    void openPlaylist(int index);
    // Fetches the track list and the missing titles of playlists[index].
    void ensureTracks(int index);
    void cancelTrackLoads();
    // Drops the oldest external lists no page shows once there are too many.
    void pruneExternal();
    void startSearch(const std::string& query);
    void loadSearch(int kind, int offset);
    void activateSearch(int kind, size_t index);
    void loadArtist(const std::string &uri);
    // The whole section from its own query (the overview has the first ten),
    // or its next page.
    void loadReleases(const std::string &uri, int section);
    void fetchLinks(const std::string &uri);
    void fetchCover(const std::string& url);

    // Open playlist: view = its rows after filter and sort.
    void buildView();
    void setSort(int mode);
    // Plays the view from row (SIZE_MAX: first row, or a random one when
    // shuffling). Shuffle plays the whole list in random order.
    void playView(size_t row, bool shuffle);
    // Plays the tracks among items from items[index], the same way.
    void playItems(const std::vector<CatalogItem> &items, size_t index, const std::string &context,
                   bool shuffle);
    void playUris(std::vector<std::string> uris, size_t row, bool shuffle, const std::string &context);
    // Plays queueUris[start] from a window of QUEUE_MAX tracks around it; the
    // rest follows through continueQueue().
    void sendWindow(size_t start);
    void continueQueue();
    void locateCurrent(const PlayerModel::Snapshot& snap);
    // Spotify Connect. Only one device of the account plays: the one started
    // last wins and the other pauses. While another device is active and this
    // one is not playing, its track fills the now-playing pane and the
    // transport drives it.
    void tickConnect(const PlayerModel::Snapshot& local);
    bool otherActive() const;
    bool remoteShown(const PlayerModel::Snapshot& local) const;
    PlayerModel::Snapshot remoteSnapshot() const;
    void remoteCommand(const std::string &endpoint, int64_t valueMs = -1);
    void fetchRemoteTrack(const std::string &uri);
    void playHere();
    void drawRemoteBar(float paneW);
    void sendSeek(int ms);
    void sendShuffle(bool on);
    void sendRepeat(int mode);
    void reportPlayerError(long status);

    // Cover art. placeholder_tex is the bundled default; cover_art_tex points at
    // it until a real cover loads, and the old texture is freed on each change.
    vita2d_texture *placeholder_tex = nullptr;
    vita2d_texture *cover_art_tex = nullptr;
    std::string coverUrl;          // url of the cover shown or being fetched

    // Library. playlists[0] is always Liked Songs; external lists follow the
    // library's.
    std::vector<Playlist> playlists;
    std::vector<Folder> folders;
    std::vector<LibraryEntry> order;
    LoadState libraryState = LoadState::NONE;
    std::string libraryError;
    bool libraryRefreshed = false;  // the cached library was refreshed this run
    int namesLeft = 0;             // names still being resolved (spinner)
    int openFolder = -1;           // folder shown by the library, -1 = root
    int openIndex = -1;            // list the top page shows, else -1 (syncOpen)
    // Page stacks: 0 over the library's folder view, 1 over the search results.
    std::vector<Page> pages[2];
    std::map<std::string, ArtistView> artists;
    ThumbCache thumbs;
    // Bumped to cancel an in-flight job: a job compares its captured value with
    // the live one before each request and before delivering.
    std::atomic<int> libraryGen{0};
    std::atomic<int> tracksGen{0};

    // Open playlist view (indices into its tracks) and what it was built from.
    std::vector<uint32_t> view;
    int viewOf = -1;
    bool viewDirty = true;
    int viewMissing = 0;           // rows still without a title
    int64_t viewTotalMs = 0;
    int scrollToRow = -1;          // view row to center on the next frame
    bool fastScroll = false;       // the open list shows the fast-scroll thumb
    float thumbGrab = 0.0f;        // finger offset inside the thumb
    bool popupOpen = false;        // circle closes the menu, not the page
    float lastScrollY = 0.0f;      // of the open list, for the debug state

    // Sort choices per playlist URI, saved in ux0:data/cspot/sorts.json.
    std::map<std::string, std::pair<int, bool>> sortPrefs;

    // The list playing, in play order. cspot holds a window of it; the rest
    // is sent as each window ends.
    std::string queueContext;
    std::vector<std::string> queueUris;
    size_t queueNext = 0;          // first index not sent to cspot yet
    unsigned int shuffleSeed = 1;  // rand_r state: cspot's thread uses rand()

    // Last cluster from ConnectWatch (a tap on the remote transport edits it
    // until the next one) and the metadata of the track it names.
    ConnectState remote;
    unsigned connectVersion = 0;
    RemoteTrack remoteTrack;
    bool remoteMode = false;       // the pane shows the other device this frame
    // Who started playing last, in process time: another device (a cluster
    // named it active and playing) or this Vita (its player left pause).
    std::string otherPlayingId;
    uint64_t remoteStartUs = 0;
    uint64_t localStartUs = 0;
    bool localWasPlaying = false;
    uint64_t handledRemoteStart = 0;   // takeover already paused this Vita
    uint64_t handledLocalStart = 0;    // the other device was already paused

    // Sleep timer: pause at sleepAtUs (process time), or when the track
    // named sleepTrack ends.
    uint64_t sleepAtUs = 0;
    bool sleepEndOfTrack = false;
    std::string sleepTrack;

    // Search: four categories, each fetched and paged on its own.
    std::string searchQuery;
    ItemList searchLists[SEARCH_KINDS];
    LoadState searchStates[SEARCH_KINDS] = {};
    bool searchMore[SEARCH_KINDS] = {};   // a next page is loading
    int searchChip = -1;           // -1 = every category, else a SearchKind
    std::atomic<int> searchGen{0};

    // Track menu, and the links of the track the now-playing pane shows.
    TrackLinks menu;
    bool menuRequested = false;
    TrackLinks nowLinks;
    int nowPending = 0;            // 1 open its album, 2 its artist, once the links arrive
    // Triangle opens the menu of the focused track row (set while drawing).
    std::function<void()> focusedMenu;

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
