#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// An artist or album linked from another item.
struct Link {
    std::string uri, name;
};

// One row of search results or of an artist page.
struct CatalogItem {
    enum Kind { TRACK, ARTIST, ALBUM, PLAYLIST };
    Kind kind = TRACK;
    std::string uri, name;
    std::string subtitle;        // artists, "Album, 2001", "By Spotify"
    std::string imageUrl;        // small cover or avatar, "" if none
    int durationMs = 0;          // tracks
    bool playable = true;        // tracks
    Link album;                  // tracks
    std::vector<Link> artists;   // tracks and albums
};

// Part of a longer list: the items fetched so far and Spotify's total.
struct ItemList {
    std::vector<CatalogItem> items;
    int total = 0;
};

enum SearchKind { SEARCH_TRACKS, SEARCH_ARTISTS, SEARCH_ALBUMS, SEARCH_PLAYLISTS, SEARCH_KINDS };

// Sections of an artist page, in page order.
enum ArtistSection {
    AS_POPULAR, AS_ALBUMS, AS_SINGLES, AS_COMPILATIONS, AS_APPEARS_ON, AS_PLAYLISTS,
    AS_RELATED, AS_COUNT,
};

struct ArtistInfo {
    std::string uri, name;
    std::string imageUrl;        // avatar, about 320 px
    int64_t monthlyListeners = 0;
    ItemList sections[AS_COUNT];
};

// metadata Track (extended-metadata TRACK_V4).
struct TrackMeta {
    std::string name, artist, album;
    std::string coverUrl;        // about 300 px, "" if none
    int durationMs = 0;
    Link albumLink;
    std::vector<Link> artists;
};

// metadata Album (extended-metadata ALBUM_V4).
struct AlbumMeta {
    std::string name;
    std::string type;            // "Album", "Single", "EP", "Compilation"
    int year = 0;
    std::string coverUrl;
    std::vector<Link> artists;
    std::vector<std::string> trackUris;   // disc by disc
};

// extended-metadata extension kinds.
const int EXT_ALBUM_V4 = 9;
const int EXT_TRACK_V4 = 10;

// The message of each entity of an extended-metadata answer, by URI.
std::map<std::string, std::string> parseExtendedEntities(const std::string &body);
TrackMeta parseTrackMeta(const std::string &msg);
void parseExtendedMetadata(const std::string &body, std::map<std::string, TrackMeta> *out);
bool parseAlbumMeta(const std::string &msg, AlbumMeta *out);

// pathfinder answers. False when the JSON lacks the expected shape: a rotated
// query hash answers 200 with only errors.
bool parseSearch(SearchKind kind, const std::string &json, ItemList *out);
bool parseArtistOverview(const std::string &json, ArtistInfo *out);
// One page of an artist's albums, singles, compilations or appearances.
bool parseReleases(ArtistSection section, const std::string &json, ItemList *out);

// "28,849,673": groups of three digits.
std::string groupDigits(int64_t n);
