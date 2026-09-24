#include "API.h"
#include "Utils.h"
#include <Logger.h>
#include <psp2/kernel/processmgr.h>
#include "Config.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

// Percent-encode a query string for safe use in a URL (RFC 3986 unreserved set).
static std::string urlencode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// JSON string literal body for the small player payloads we build by hand.
static std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string spotify_base62_to_hex(const std::string &id) {
    static const char *digits =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (id.size() != 22) {
        return "";
    }
    uint8_t n[16] = {0};
    for (char c : id) {
        const char *d = strchr(digits, c);
        if (d == NULL || c == '\0') {
            return "";
        }
        // n = n * 62 + digit, big-endian bytes.
        unsigned carry = static_cast<unsigned>(d - digits);
        for (int i = 15; i >= 0; i--) {
            unsigned v = n[i] * 62u + carry;
            n[i] = static_cast<uint8_t>(v & 0xFF);
            carry = v >> 8;
        }
    }
    static const char *hex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : n) {
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }
    return out;
}

void API::set_token(const std::string &token, int expiresInS) {
    std::lock_guard<std::mutex> g(mutex_);
    token_ = token;
    // Refresh a minute early so a request never races the expiry.
    int life = expiresInS > 120 ? expiresInS - 60 : 3000;
    expiresAtUs_ = sceKernelGetProcessTimeWide() + static_cast<uint64_t>(life) * 1000000ULL;
    hasToken_ = !token.empty();
}

void API::set_user(const std::string &user) {
    std::lock_guard<std::mutex> g(mutex_);
    user_ = user;
}

void API::set_refresher(TokenRefresher refresher) {
    std::lock_guard<std::mutex> g(mutex_);
    refresher_ = std::move(refresher);
}

// login5 tokens last one hour. The old code minted one at login and never
// again, so every browse call silently failed with 401 an hour into a session.
std::string API::bearer(bool forceRefresh) {
    TokenRefresher refresher;
    {
        std::lock_guard<std::mutex> g(mutex_);
        bool expired = sceKernelGetProcessTimeWide() >= expiresAtUs_;
        if (!forceRefresh && !expired && !token_.empty()) {
            return token_;
        }
        refresher = refresher_;
    }
    if (!refresher) {
        std::lock_guard<std::mutex> g(mutex_);
        return token_;
    }
    int expiresIn = 0;
    std::string fresh = refresher(&expiresIn);
    if (fresh.empty()) {
        CSPOT_LOG(error, "token refresh failed, keeping the old token");
        std::lock_guard<std::mutex> g(mutex_);
        return token_;
    }
    CSPOT_LOG(info, "access token refreshed (%d s)", expiresIn);
    set_token(fresh, expiresIn);
    return fresh;
}

ApiResult API::web(const char *method, const std::string &url, const std::string &body) {
    ApiResult r;
    if (!has_token()) {
        return r;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string tok = bearer(attempt > 0);
        Headers headers = { "Accept: application/json",
                            "Content-Type: application/json",
                            "Authorization: Bearer " + tok };
        uint8_t *buf = NULL;
        int len = download(url.c_str(), &buf, method, body, headers, &r.status);
        r.body.assign(buf != NULL ? reinterpret_cast<const char *>(buf) : "",
                      (buf != NULL && len > 0) ? static_cast<size_t>(len) : 0);
        free(buf);
        if (r.status != 401) {
            break;
        }
    }
    if (!r.ok()) {
        CSPOT_LOG(error, "%s %s -> %ld", method, url.c_str(), r.status);
    }
    return r;
}

ApiResult API::spclient(const std::string &url, const char *accept) {
    ApiResult r;
    if (!has_token()) {
        return r;
    }
    // The Vita resolver often fails the FIRST lookup of a host; one retry on a
    // transport error covers it, and keep-alive makes the follow-ups cheap.
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string tok = bearer(r.status == 401);
        uint8_t *buf = NULL;
        int len = spclient_get(url.c_str(), tok, &buf, &r.status, accept);
        r.body.assign(buf != NULL ? reinterpret_cast<const char *>(buf) : "",
                      (buf != NULL && len > 0) ? static_cast<size_t>(len) : 0);
        free(buf);
        if (r.status != 0 && r.status != 401) {
            break;
        }
    }
    return r;
}

// User's playlist list (rootlist). Response is playlist4 SelectedListContent
// protobuf, NOT JSON. Replaces the rate-limited api.spotify.com/v1/me/playlists.
ApiResult API::get_rootlist() {
    std::string user;
    {
        std::lock_guard<std::mutex> g(mutex_);
        user = user_;
    }
    if (user.empty()) {
        return ApiResult();
    }
    std::string url = SPCLIENT_BASE "/playlist/v2/user/" + urlencode(user) +
                      "/rootlist?from=0&length=" + std::to_string(SPOTIFY_ROOTLIST_LENGTH);
    return spclient(url);
}

// One playlist (protobuf): attributes carry the name, contents the item URIs.
ApiResult API::get_playlist(const std::string &playlistId) {
    return spclient(SPCLIENT_BASE "/playlist/v2/playlist/" + playlistId);
}

// Track metadata (protobuf Track: name=2, album=3, artist=4, duration=7).
ApiResult API::get_track_metadata(const std::string &trackId) {
    std::string gid = spotify_base62_to_hex(trackId);
    if (gid.empty()) {
        return ApiResult();
    }
    return spclient(SPCLIENT_BASE "/metadata/4/track/" + gid + "?market=from_token",
                    "application/x-protobuf");
}

ApiResult API::get_playlist_tracks_web(const std::string &playlistId) {
    std::string url = SPOTIFY_API_BASE "/playlists/" + playlistId +
                      "/tracks?fields=items(track(name,uri,artists(name)))&limit=" +
                      std::to_string(SPOTIFY_PLAYLIST_TRACK_LIMIT > 100 ? 100 : SPOTIFY_PLAYLIST_TRACK_LIMIT);
    return web("GET", url);
}

ApiResult API::search(const std::string &query, uint16_t limit) {
    std::string url = SPOTIFY_API_BASE "/search?q=" + urlencode(query) +
                      "&type=track&limit=" + std::to_string(limit);
    return web("GET", url);
}

ApiResult API::play_context(const std::string &contextUri, uint32_t offset) {
    std::string body = "{\"context_uri\":" + jsonString(contextUri) +
                       ",\"offset\":{\"position\":" + std::to_string(offset) +
                       "},\"position_ms\":0}";
    return web("PUT", SPOTIFY_API_BASE "/me/player/play?device_id=" DEVICE_ID, body);
}

ApiResult API::play_track(const std::string &trackUri) {
    std::string body = "{\"uris\":[" + jsonString(trackUri) + "]}";
    return web("PUT", SPOTIFY_API_BASE "/me/player/play?device_id=" DEVICE_ID, body);
}

// Spotify routes a SPIRC seek frame back to this Vita's cspot, which already
// handles it -- so no new cspot code is needed.
ApiResult API::seek(uint32_t positionMs) {
    return web("PUT", SPOTIFY_API_BASE "/me/player/seek?position_ms=" +
                      std::to_string(positionMs) + "&device_id=" DEVICE_ID);
}

ApiResult API::set_shuffle(bool on) {
    return web("PUT", std::string(SPOTIFY_API_BASE "/me/player/shuffle?state=") +
                      (on ? "true" : "false") + "&device_id=" DEVICE_ID);
}

ApiResult API::set_repeat(const char *mode) {
    return web("PUT", std::string(SPOTIFY_API_BASE "/me/player/repeat?state=") + mode +
                      "&device_id=" DEVICE_ID);
}
