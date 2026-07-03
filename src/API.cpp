#include "API.h"
#include "Utils.h"
#include <Logger.h>
#include "Config.h"
#include <cctype>
#include <cstdlib>
#include <string>

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

void API::set_token(std::string _token) {
    token = _token;
}

void API::play_by_uri(std::string uri, uint32_t offset_pos, uint32_t position_ms) {
    if (token.size() == 0) {
        return;
    }

    std::string url = SPOTIFY_API_PLAY_URL;
    url += "?device_id=";
    url += DEVICE_ID;

    // TODO(michal4132): Replace with json object
    std::string post_data = "{\"context_uri\": \"";
    post_data += uri;
    post_data += "\",\"offset\": {\"position\": ";
    post_data += std::to_string(offset_pos);
    post_data += "},\"position_ms\": ";
    post_data += std::to_string(position_ms);
    post_data += "}";
    uint8_t *buf;
    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };
    int len = download(url.c_str(), &buf, "PUT", post_data, headers);
    if (len > 0) {
        CSPOT_LOG(info, "play_by_uri response: %.*s", len, buf);
        free(buf);
    }
}

// TODO(michal4132): limit, offset
int API::get_current_users_playlists(uint8_t **buf, uint16_t limit, uint16_t offset) {
    if (token.size() == 0) {
        return -1;
    }

    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };

    std::string url = SPOTIFY_API_GET_USERS_PLAYLISTS;
    url += "?limit=";
    url += std::to_string(limit);
    url += "&offset=";
    url += std::to_string(offset);

    int len = download(url.c_str(), buf, "GET", "", headers);
    if (len <= 0) {
        buf = NULL;
        return 0;
    }

    CSPOT_LOG(info, "get_current_users_playlists response: %.*s", len, *buf);
    return len;
}

int API::get_playlist_items(uint8_t **buf, std::string playlist_id, std::string fields,
                                        uint16_t limit, uint16_t offset) {
    if (token.size() == 0) {
        return -1;
    }

    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };

    std::string url = "";
    if (playlist_id.starts_with("https://api.spotify.com/v1/playlists/")) {
        url += playlist_id;

    } else {
        url += SPOTIFY_API_GET_PLAYLIST_ITEMS_s;
        url += playlist_id;
        url += SPOTIFY_API_GET_PLAYLIST_ITEMS_e;
    }
    url += "?fields=";
    url += fields;
    url += "&limit=";
    url += std::to_string(limit);
    url += "&offset=";
    url += std::to_string(offset);

    int len = download(url.c_str(), buf, "GET", "", headers);
    if (len <= 0) {
        buf = NULL;
        return 0;
    }

    CSPOT_LOG(info, "get_playlist_items response: %.*s", len, *buf);
    return len;
}

int API::get_available_devices(uint8_t **buf) {
    if (token.size() == 0) {
        return -1;
    }

    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };

    int len = download(SPOTIFY_API_GET_AVAILABLE_DEVICES, buf, "GET", "", headers);
    if (len <= 0) {
        buf = NULL;
        return 0;
    }

    CSPOT_LOG(info, "get_available_devices response: %.*s", len, *buf);
    return len;
}

// Play a single track (uses "uris" rather than a context_uri + offset).
void API::play_track(std::string track_uri) {
    if (token.size() == 0) {
        return;
    }
    std::string url = SPOTIFY_API_PLAY_URL;
    url += "?device_id=";
    url += DEVICE_ID;

    std::string post_data = "{\"uris\": [\"" + track_uri + "\"]}";
    uint8_t *buf = NULL;
    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };
    download(url.c_str(), &buf, "PUT", post_data, headers);
    if (buf) free(buf);
}

// Seek the active device. Spotify routes a SPIRC seek frame back to this Vita's
// cspot, which already handles it -- so no new cspot code is needed.
void API::seek(uint32_t position_ms) {
    if (token.size() == 0) {
        return;
    }
    std::string url = SPOTIFY_API_SEEK_URL;
    url += "?position_ms=";
    url += std::to_string(position_ms);
    url += "&device_id=";
    url += DEVICE_ID;

    uint8_t *buf = NULL;
    Headers headers = { {"Authorization: Bearer " + token} };
    download(url.c_str(), &buf, "PUT", "", headers);
    if (buf) free(buf);
}

void API::set_shuffle(bool on) {
    if (token.size() == 0) {
        return;
    }
    std::string url = SPOTIFY_API_SHUFFLE_URL;
    url += on ? "?state=true" : "?state=false";
    url += "&device_id=";
    url += DEVICE_ID;

    uint8_t *buf = NULL;
    Headers headers = { {"Authorization: Bearer " + token} };
    download(url.c_str(), &buf, "PUT", "", headers);
    if (buf) free(buf);
}

void API::set_repeat(const char *mode) {
    if (token.size() == 0) {
        return;
    }
    std::string url = SPOTIFY_API_REPEAT_URL;
    url += "?state=";
    url += mode;
    url += "&device_id=";
    url += DEVICE_ID;

    uint8_t *buf = NULL;
    Headers headers = { {"Authorization: Bearer " + token} };
    download(url.c_str(), &buf, "PUT", "", headers);
    if (buf) free(buf);
}

int API::search(uint8_t **buf, std::string query, std::string type, uint16_t limit) {
    if (token.size() == 0) {
        return -1;
    }
    Headers headers = { {"Accept: application/json"},
                        {"Authorization: Bearer " + token} };

    std::string url = SPOTIFY_API_SEARCH_URL;
    url += "?q=";
    url += urlencode(query);
    url += "&type=";
    url += type;
    url += "&limit=";
    url += std::to_string(limit);

    int len = download(url.c_str(), buf, "GET", "", headers);
    if (len <= 0) {
        return 0;
    }
    return len;
}

