#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>  // NOLINT
#include <string>
#include <vector>

#define SPOTIFY_PLAYLIST_HEADER            "spotify:playlist:"
#define SPOTIFY_TRACK_HEADER               "spotify:track:"
#define SPOTIFY_ROOTLIST_LENGTH            200
// Spotify caps playlists at 10000 tracks; the same bound guards Liked Songs.
#define SPOTIFY_PLAYLIST_TRACK_LIMIT       10000
#define SPOTIFY_LIKED_PAGE                 200
#define SPOTIFY_LIKED_LIMIT                10000
#define LIKED_SONGS_URI                    "spotify:collection:tracks"

#define SPOTIFY_API_BASE                   "https://api.spotify.com/v1"

// Internal Spotify serving host (spclient). The public api.spotify.com/v1 is
// rate-limited PER client_id, and the keymaster/android client_id we mint the
// token with is shared by every librespot-based app on earth -> permanent 429.
// spclient is the internal path the official apps use; the SAME session token
// works there and is NOT subject to that shared public quota. Verified live:
// spclient rootlist -> 200 while api.spotify.com/v1 -> 429 with the same token.
// Browsing therefore goes through spclient, playback through cspot itself,
// search through the web player's GraphQL endpoint.
#define SPCLIENT_BASE                      "https://spclient.wg.spotify.com"

// Web player GraphQL. The persisted-query hashes come from the web player
// bundle (taken on 2026-09-27); Spotify can rotate them, and then the request
// answers errors until they are refreshed (.agents/TRAPS.md says how).
#define PATHFINDER_URL                     "https://api-partner.spotify.com/pathfinder/v2/query"
#define PF_SEARCH_TRACKS                   "b02683192a98dde7966b5e6655a79eeb62713eab703eda9902c932818dd52751"
#define PF_SEARCH_ARTISTS                  "7bf95d754fdbe32c8b161fbbe54d1ae50974900df4dce4c8f1afcbcad153224d"
#define PF_SEARCH_ALBUMS                   "202cb3305e31e5a0767ba7925f28bd728cf8f8b0217e6da43909056071cd70e9"
#define PF_SEARCH_PLAYLISTS                "d520014e748f9ea44f7707d8df1819867ac1205e8b7f3e28f22fe5fc858921b1"
#define PF_ARTIST_OVERVIEW                 "9f8134ef565e78621f1e1793555bd6633c5ac144ae0f89604ed3ae3f80b3c8e6"
#define PF_ARTIST_DISCOGRAPHY              "5e07d323febb57b4a56a42abbf781490e58764aa45feb6e3dc0591564fc56599"
#define PF_ARTIST_APPEARS_ON               "9a4bb7a20d6720fe52d7b47bc001cfa91940ddf5e7113761460b4a288d18a4c1"

// status: HTTP status, or 0 when the transfer failed (DNS, TLS, timeout).
// body: raw response (protobuf for spclient, JSON for the Web API).
struct ApiResult {
    long status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// Spotify HTTP API client. Every request method BLOCKS and must run on the
// NetWorker thread, never on the GUI thread. The token is shared with the cspot
// thread (which mints it) and guarded by a mutex.
class API {
 public:
    // Mints a fresh access token (login5). Returns "" on failure and writes the
    // lifetime in seconds to *expiresInS.
    using TokenRefresher = std::function<std::string(int *expiresInS)>;

    void set_token(const std::string &token, int expiresInS);
    void set_user(const std::string &user);
    void set_refresher(TokenRefresher refresher);
    bool has_token() const { return hasToken_.load(); }
    std::string user() const;

    // spclient, protobuf (playlist4 SelectedListContent / metadata Track).
    ApiResult get_rootlist();
    ApiResult get_playlist(const std::string &playlistId);
    ApiResult get_track_metadata(const std::string &trackId);
    ApiResult get_tracks_metadata(const std::vector<std::string> &uris) { return get_extended(uris, 10); }
    // extended-metadata for the URIs, one extension kind (9 album, 10 track).
    ApiResult get_extended(const std::vector<std::string> &uris, int kind);
    // One page of Liked Songs (collection PageResponse protobuf), newest first.
    ApiResult get_liked_page(const std::string &pageToken, int limit);

    // pathfinder GraphQL, JSON. kind: 0 tracks, 1 artists, 2 albums,
    // 3 playlists (SearchKind).
    ApiResult search(int kind, const std::string &query, int offset, int limit);
    ApiResult artist_overview(const std::string &uri);
    // section: 1 albums, 2 singles, 3 compilations, 4 appears on (ArtistSection).
    ApiResult artist_releases(const std::string &uri, int section, int offset, int limit);
    // Any pathfinder request (a JSON body with operationName, variables and
    // the persisted-query hash), over the keep-alive connection.
    ApiResult pathfinder(const std::string &body) {
        return spclient(PATHFINDER_URL, "application/json", &body, "application/json");
    }

    // The current token, refreshed when it expired (blocking).
    std::string access_token() { return bearer(false); }

    // spclient connect-state, JSON, from any thread (a fresh connection per
    // call). connectionId is the dealer websocket's Spotify-Connection-Id.
    ApiResult connect_state(const char *method, const std::string &path, const std::string &body,
                            const std::string &connectionId);

    // Any spclient path (devkit exploration): POST when body is set.
    ApiResult raw(const std::string &path, const std::string *body, const char *contentType) {
        return spclient(SPCLIENT_BASE + path, nullptr, body, contentType);
    }

 private:
    std::string bearer(bool forceRefresh);
    ApiResult web(const char *method, const std::string &url, const std::string &body = "");
    ApiResult spclient(const std::string &url, const char *accept = nullptr,
                       const std::string *body = nullptr, const char *contentType = nullptr);

    mutable std::mutex mutex_;
    std::string token_;
    std::string user_;
    uint64_t expiresAtUs_ = 0;
    TokenRefresher refresher_;
    std::atomic<bool> hasToken_{false};
};

// Spotify base62 id (22 chars) -> 32 hex chars of the 128-bit gid, as the
// metadata endpoints want it. "" if the id is malformed.
std::string spotify_base62_to_hex(const std::string &id);
// The reverse: 32 hex chars to the base62 id, "" if malformed.
std::string spotify_hex_to_base62(const std::string &hex);
