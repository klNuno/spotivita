#include "API.h"
#include "Utils.h"
#include "Proto.h"
#include <Logger.h>
#include <psp2/kernel/processmgr.h>
#include "Config.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

// JSON string literal for the request bodies built by hand.
static std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned char>(c));
            out += esc;
        } else {
            out += c;
        }
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

std::string spotify_hex_to_base62(const std::string &hex) {
    if (hex.size() != 32) {
        return "";
    }
    std::string bytes;
    for (size_t i = 0; i < 32; i += 2) {
        char pair[3] = {hex[i], hex[i + 1], 0};
        char *end = NULL;
        long v = strtol(pair, &end, 16);
        if (end != pair + 2) {
            return "";
        }
        bytes += static_cast<char>(v);
    }
    return gid_to_base62(bytes);
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

ApiResult API::spclient(const std::string &url, const char *accept,
                        const std::string *body, const char *contentType) {
    ApiResult r;
    if (!has_token()) {
        return r;
    }
    // The Vita resolver often fails the FIRST lookup of a host; one retry on a
    // transport error covers it, and keep-alive makes the follow-ups cheap.
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string tok = bearer(r.status == 401);
        uint8_t *buf = NULL;
        int len = spclient_get(url.c_str(), tok, &buf, &r.status, accept, body, contentType);
        r.body.assign(buf != NULL ? reinterpret_cast<const char *>(buf) : "",
                      (buf != NULL && len > 0) ? static_cast<size_t>(len) : 0);
        free(buf);
        if (r.status != 0 && r.status != 401) {
            break;
        }
    }
    return r;
}

ApiResult API::connect_state(const char *method, const std::string &path, const std::string &body,
                             const std::string &connectionId) {
    ApiResult r;
    if (!has_token()) {
        return r;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        Headers headers = { "Accept: application/json",
                            "Content-Type: application/json",
                            "Authorization: Bearer " + bearer(attempt > 0) };
        if (!connectionId.empty()) {
            headers.push_back("X-Spotify-Connection-Id: " + connectionId);
        }
        uint8_t *buf = NULL;
        std::string url = SPCLIENT_BASE "/connect-state/v1/" + path;
        int len = download(url.c_str(), &buf, method, body, headers, &r.status);
        r.body.assign(buf != NULL ? reinterpret_cast<const char *>(buf) : "",
                      (buf != NULL && len > 0) ? static_cast<size_t>(len) : 0);
        free(buf);
        if (r.status != 401) {
            break;
        }
    }
    if (!r.ok()) {
        CSPOT_LOG(error, "connect-state %s %s -> %ld", method, path.c_str(), r.status);
    }
    return r;
}

std::string API::user() const {
    std::lock_guard<std::mutex> g(mutex_);
    return user_;
}

// User's playlist list (rootlist). Response is playlist4 SelectedListContent
// protobuf, NOT JSON. Replaces the rate-limited api.spotify.com/v1/me/playlists.
// decorate=attributes adds a MetaItem per item with the playlist name, so the
// whole library comes in one request.
ApiResult API::get_rootlist() {
    std::string u = user();
    if (u.empty()) {
        return ApiResult();
    }
    std::string url = SPCLIENT_BASE "/playlist/v2/user/" + urlencode(u) +
                      "/rootlist?decorate=revision,attributes,length&from=0&length=" +
                      std::to_string(SPOTIFY_ROOTLIST_LENGTH);
    return spclient(url);
}

namespace {

void pbPutVarint(std::string *out, uint64_t v) {
    while (v >= 0x80) {
        out->push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<char>(v));
}

void pbPutString(std::string *out, int field, const std::string &s) {
    pbPutVarint(out, (static_cast<uint64_t>(field) << 3) | 2);
    pbPutVarint(out, s.size());
    out->append(s);
}

}  // namespace

// collection2v2 PageRequest { username=1, set=2, pagination_token=3, limit=4 };
// the "collection" set holds the liked tracks.
ApiResult API::get_liked_page(const std::string &pageToken, int limit) {
    std::string u = user();
    if (u.empty()) {
        return ApiResult();
    }
    std::string body;
    pbPutString(&body, 1, u);
    pbPutString(&body, 2, "collection");
    if (!pageToken.empty()) {
        pbPutString(&body, 3, pageToken);
    }
    pbPutVarint(&body, (4 << 3) | 0);
    pbPutVarint(&body, static_cast<uint64_t>(limit));
    const char *type = "application/vnd.collection-v2.spotify.proto";
    return spclient(SPCLIENT_BASE "/collection/v2/paging", type, &body, type);
}

// One playlist (protobuf): attributes carry the name, contents the item URIs.
ApiResult API::get_playlist(const std::string &playlistId) {
    return spclient(SPCLIENT_BASE "/playlist/v2/playlist/" + playlistId);
}

// Many entities in one request, the way the official clients load a list:
// extended-metadata BatchedEntityRequest { entity_request=2 { entity_uri=1,
// query=2 { extension_kind=1 } } }. The answer carries one message per URI,
// in any order.
ApiResult API::get_extended(const std::vector<std::string> &uris, int kind) {
    std::string body;
    for (const auto &uri : uris) {
        std::string query;
        pbPutVarint(&query, (1 << 3) | 0);
        pbPutVarint(&query, static_cast<uint64_t>(kind));
        std::string request;
        pbPutString(&request, 1, uri);
        pbPutString(&request, 2, query);
        pbPutString(&body, 2, request);
    }
    const char *type = "application/protobuf";
    return spclient(SPCLIENT_BASE "/extended-metadata/v0/extended-metadata", type, &body, type);
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

static std::string pathfinderBody(const char *operation, const char *hash, const std::string &variables) {
    return "{\"variables\":" + variables + ",\"operationName\":\"" + operation +
           "\",\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"" + hash + "\"}}}";
}

// Search through the web player's GraphQL endpoint (pathfinder): the public
// /v1/search answers 429 to this client_id. One category per request, so
// each can page on its own.
ApiResult API::search(int kind, const std::string &query, int offset, int limit) {
    static const struct { const char *operation, *hash; } kOps[] = {
        {"searchTracks", PF_SEARCH_TRACKS}, {"searchArtists", PF_SEARCH_ARTISTS},
        {"searchAlbums", PF_SEARCH_ALBUMS}, {"searchPlaylists", PF_SEARCH_PLAYLISTS},
    };
    if (kind < 0 || kind > 3) return ApiResult();
    std::string vars =
        "{\"searchTerm\":" + jsonString(query) + ",\"offset\":" + std::to_string(offset) +
        ",\"limit\":" + std::to_string(limit) +
        ",\"numberOfTopResults\":5,\"includeAudiobooks\":false,\"includePreReleases\":false"
        ",\"includeAlbumPreReleases\":false,\"includeAuthors\":false"
        ",\"includeEpisodeContentRatingsV2\":false}";
    return pathfinder(pathfinderBody(kOps[kind].operation, kOps[kind].hash, vars));
}

// Everything the artist page shows in one answer (about 100 KB): profile,
// top tracks, the first releases of each kind, related artists.
ApiResult API::artist_overview(const std::string &uri) {
    std::string vars = "{\"uri\":" + jsonString(uri) + ",\"locale\":\"\",\"includePrerelease\":false}";
    return pathfinder(pathfinderBody("queryArtistOverview", PF_ARTIST_OVERVIEW, vars));
}

ApiResult API::artist_releases(const std::string &uri, int section, int offset, int limit) {
    static const char *kOps[] = {
        "queryArtistDiscographyAlbums", "queryArtistDiscographySingles",
        "queryArtistDiscographyCompilations",
    };
    std::string vars = "{\"uri\":" + jsonString(uri) + ",\"offset\":" + std::to_string(offset) +
                       ",\"limit\":" + std::to_string(limit);
    if (section == 4) {
        return pathfinder(pathfinderBody("queryArtistAppearsOn", PF_ARTIST_APPEARS_ON, vars + "}"));
    }
    if (section < 1 || section > 3) return ApiResult();
    return pathfinder(pathfinderBody(kOps[section - 1], PF_ARTIST_DISCOGRAPHY,
                                     vars + ",\"order\":\"DATE_DESC\"}"));
}
