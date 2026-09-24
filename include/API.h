#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>  // NOLINT
#include <string>

#define SPOTIFY_PLAYLIST_HEADER            "spotify:playlist:"
#define SPOTIFY_TRACK_HEADER               "spotify:track:"
#define SPOTIFY_ROOTLIST_LENGTH            200
#define SPOTIFY_PLAYLIST_TRACK_LIMIT       100

#define SPOTIFY_API_BASE                   "https://api.spotify.com/v1"

// Internal Spotify serving host (spclient). The public api.spotify.com/v1 is
// rate-limited PER client_id, and the keymaster/android client_id we mint the
// token with is shared by every librespot-based app on earth -> permanent 429.
// spclient is the internal path the official apps use; the SAME session token
// works there and is NOT subject to that shared public quota. Verified live:
// spclient rootlist -> 200 while api.spotify.com/v1 -> 429 with the same token.
// Browsing therefore goes through spclient; the Web API is kept only for what
// spclient has no simple equivalent for (player commands, search).
#define SPCLIENT_BASE                      "https://spclient.wg.spotify.com"

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

    // spclient, protobuf (playlist4 SelectedListContent / metadata Track).
    ApiResult get_rootlist();
    ApiResult get_playlist(const std::string &playlistId);
    ApiResult get_track_metadata(const std::string &trackId);

    // Web API, JSON.
    ApiResult get_playlist_tracks_web(const std::string &playlistId);
    ApiResult search(const std::string &query, uint16_t limit);
    ApiResult play_context(const std::string &contextUri, uint32_t offset);
    ApiResult play_track(const std::string &trackUri);
    ApiResult seek(uint32_t positionMs);
    ApiResult set_shuffle(bool on);
    ApiResult set_repeat(const char *mode);  // "off" | "context" | "track"

 private:
    std::string bearer(bool forceRefresh);
    ApiResult web(const char *method, const std::string &url, const std::string &body = "");
    ApiResult spclient(const std::string &url, const char *accept = nullptr);

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
