#include "ConnectWatch.h"
#include "API.h"
#include "Utils.h"
#include <Logger.h>
#include <cJSON.h>
#include <curl/curl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// curl + OpenSSL handshakes and cJSON on a whole cluster (see NetWorker.cpp).
const int WATCH_STACK_SIZE = 0x80000;
// The dealer drops a connection that stays silent; the web player pings
// every 30 s.
const uint64_t PING_US = 30ULL * 1000000ULL;
// A cluster with a long queue runs to a few hundred KB; anything past this is
// not one.
const size_t MAX_MESSAGE = 4 * 1024 * 1024;

uint64_t nowUs() { return sceKernelGetProcessTimeWide(); }

std::string str(cJSON *o, const char *key) {
    cJSON *v = o != NULL ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(v) && v->valuestring != NULL ? std::string(v->valuestring) : std::string();
}

// Protobuf JSON writes int64 as a string, int32 as a number.
int64_t num(cJSON *o, const char *key) {
    cJSON *v = o != NULL ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    if (cJSON_IsNumber(v)) return static_cast<int64_t>(v->valuedouble);
    if (cJSON_IsString(v) && v->valuestring != NULL) return strtoll(v->valuestring, NULL, 10);
    return 0;
}

bool flag(cJSON *o, const char *key) {
    cJSON *v = o != NULL ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsTrue(v);
}

std::string jsonQuote(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// "spotify:image:<hex>" or a bare hex id to a CDN URL.
std::string imageUrl(const std::string &s) {
    if (s.empty() || s.compare(0, 8, "https://") == 0) return s;
    size_t colon = s.rfind(':');
    return "https://i.scdn.co/image/" + (colon == std::string::npos ? s : s.substr(colon + 1));
}

}  // namespace

void ConnectWatch::start() {
    if (started_.exchange(true)) return;
    char id[41];
    unsigned int seed = static_cast<unsigned int>(nowUs());
    for (int i = 0; i < 40; i++) id[i] = "0123456789abcdef"[rand_r(&seed) % 16];
    id[40] = '\0';
    hobsId_ = std::string("hobs_") + id;
    if (!start_pthread(threadMain, this, WATCH_STACK_SIZE)) {
        CSPOT_LOG(error, "connect watch: thread creation failed");
    }
}

void *ConnectWatch::threadMain(void *arg) {
    static_cast<ConnectWatch *>(arg)->loop();
    return nullptr;
}

void ConnectWatch::setStatus(const std::string &s) {
    std::lock_guard<std::mutex> g(mutex_);
    status_ = s;
}

void ConnectWatch::loop() {
    int failures = 0;
    while (true) {
        if (!api_->has_token()) {
            sceKernelDelayThread(1000 * 1000);
            continue;
        }
        uint64_t began = nowUs();
        bool ok = session();
        // A session that held a while was fine; start the backoff over.
        if (ok && nowUs() - began > 60ULL * 1000000ULL) failures = 0;
        failures = failures < 6 ? failures + 1 : 6;
        sceKernelDelayThread(static_cast<SceUInt>(5 << (failures - 1)) * 1000 * 1000 / 2);
    }
}

// One websocket connection, until it drops. False when it never connected.
bool ConnectWatch::session() {
    setStatus("connecting");
    CURL *c = curl_easy_init();
    if (c == NULL) return false;
    std::string url = "wss://dealer.spotify.com/?access_token=" + api_->access_token();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);   // websocket, driven by curl_ws_*
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_apply_tls(c);
    CURLcode rc = curl_easy_perform(c);
    url.clear();
    if (rc != CURLE_OK) {
        // Never log the URL: it carries the token.
        CSPOT_LOG(error, "connect watch: dealer connect failed: %s", curl_easy_strerror(rc));
        setStatus(std::string("connect failed: ") + curl_easy_strerror(rc));
        curl_easy_cleanup(c);
        return false;
    }
    setStatus("connected");
    CSPOT_LOG(info, "connect watch: dealer connected");

    std::string msg;
    uint64_t lastPing = nowUs();
    char buf[16384];
    while (true) {
        size_t n = 0;
        const struct curl_ws_frame *meta = NULL;
        rc = curl_ws_recv(c, buf, sizeof(buf), &n, &meta);
        if (rc == CURLE_AGAIN) {
            if (nowUs() - lastPing > PING_US) {
                static const char ping[] = "{\"type\":\"ping\"}";
                size_t sent = 0;
                rc = curl_ws_send(c, ping, sizeof(ping) - 1, &sent, 0, CURLWS_TEXT);
                if (rc != CURLE_OK && rc != CURLE_AGAIN) break;
                lastPing = nowUs();
            }
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        if (rc != CURLE_OK || meta == NULL) break;
        if (meta->flags & CURLWS_CLOSE) break;
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;   // curl answers pings
        msg.append(buf, n);
        if (msg.size() > MAX_MESSAGE) {
            CSPOT_LOG(error, "connect watch: message over %u bytes, dropped", (unsigned) MAX_MESSAGE);
            break;
        }
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            bool keep = handleMessage(msg);
            msg.clear();
            if (!keep) break;
        }
    }
    CSPOT_LOG(info, "connect watch: dealer closed (%s)", curl_easy_strerror(rc));
    setStatus(std::string("closed: ") + curl_easy_strerror(rc));
    curl_easy_cleanup(c);
    {
        std::lock_guard<std::mutex> g(mutex_);
        connectionId_.clear();
    }
    return true;
}

bool ConnectWatch::handleMessage(const std::string &msg) {
    cJSON *root = cJSON_Parse(msg.c_str());
    if (root == NULL) return true;
    bool keep = true;
    {
        std::lock_guard<std::mutex> g(mutex_);
        messages_++;
    }
    std::string uri = str(root, "uri");
    std::string connId = str(cJSON_GetObjectItemCaseSensitive(root, "headers"), "Spotify-Connection-Id");
    if (!connId.empty()) {
        {
            std::lock_guard<std::mutex> g(mutex_);
            connectionId_ = connId;
        }
        keep = registerObserver();
    } else if (uri == "hm://connect-state/v1/cluster") {
        cJSON *payloads = cJSON_GetObjectItemCaseSensitive(root, "payloads");
        cJSON *first = cJSON_IsArray(payloads) ? cJSON_GetArrayItem(payloads, 0) : NULL;
        if (cJSON_IsObject(first)) {
            applyCluster(cJSON_GetObjectItemCaseSensitive(first, "cluster"));
        } else if (first != NULL) {
            setStatus("cluster update in an unknown encoding");
        }
    }
    cJSON_Delete(root);
    return keep;
}

// False when Spotify refused it: the session ends and the loop retries later.
bool ConnectWatch::registerObserver() {
    std::string connId;
    {
        std::lock_guard<std::mutex> g(mutex_);
        connId = connectionId_;
    }
    static const char body[] =
        "{\"member_type\":\"CONNECT_STATE\",\"device\":{\"device_info\":{\"capabilities\":"
        "{\"can_be_player\":false,\"hidden\":true}}}}";
    ApiResult r = api_->connect_state("PUT", "devices/" + hobsId_, body, connId);
    if (!r.ok()) {
        setStatus("register failed: HTTP " + std::to_string(r.status));
        return false;
    }
    setStatus("listening");
    cJSON *cluster = cJSON_Parse(r.body.c_str());
    if (cluster != NULL) {
        applyCluster(cluster);
        cJSON_Delete(cluster);
    }
    return true;
}

void ConnectWatch::applyCluster(cJSON *cluster) {
    if (!cJSON_IsObject(cluster)) return;
    ConnectState s;
    s.valid = true;
    s.activeId = str(cluster, "active_device_id");
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(cluster, "devices");
    cJSON *dev = s.activeId.empty() ? NULL : cJSON_GetObjectItemCaseSensitive(devices, s.activeId.c_str());
    s.deviceName = str(dev, "name");
    s.deviceType = str(dev, "device_type");
    if (cJSON_GetObjectItemCaseSensitive(dev, "volume") != NULL) {
        s.volume = static_cast<int>(num(dev, "volume"));
    }

    cJSON *ps = cJSON_GetObjectItemCaseSensitive(cluster, "player_state");
    cJSON *track = cJSON_GetObjectItemCaseSensitive(ps, "track");
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(track, "metadata");
    s.trackUri = str(track, "uri");
    s.title = str(meta, "title");
    s.artist = str(meta, "artist_name");
    s.imageUrl = imageUrl(str(meta, "image_url"));
    s.durationMs = static_cast<int>(num(ps, "duration"));
    s.playing = flag(ps, "is_playing") && !flag(ps, "is_paused");
    // Where the track was at `timestamp`, moved on to the server's now.
    int64_t pos = num(ps, "position_as_of_timestamp");
    int64_t at = num(ps, "timestamp"), server = num(cluster, "server_timestamp_ms");
    if (s.playing && at > 0 && server > at) pos += server - at;
    s.positionMs = pos;
    s.receivedAtUs = nowUs();

    std::string list;
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devices) {
        if (d->string == NULL) continue;
        if (!list.empty()) list += ", ";
        list += std::string(d->string) + "=" + str(d, "name") + "/" + str(d, "device_type");
    }
    s.devices = list;

    char *printed = cJSON_PrintUnformatted(cluster);
    std::lock_guard<std::mutex> g(mutex_);
    clusters_++;
    if (s.activeId != state_.activeId || s.playing != state_.playing || s.trackUri != state_.trackUri) {
        CSPOT_LOG(info, "cluster: %s %s %s", s.deviceName.empty() ? "(none)" : s.deviceName.c_str(),
                  s.playing ? "playing" : "paused", s.trackUri.c_str());
    }
    if (!injected_) {
        s.version = state_.version + 1;
        state_ = s;
    }
    if (printed != NULL) {
        sample_.assign(printed, strnlen(printed, 6000));
        free(printed);
    }
}

ConnectState ConnectWatch::snapshot() const {
    std::lock_guard<std::mutex> g(mutex_);
    return state_;
}

std::string ConnectWatch::debugJson() const {
    std::lock_guard<std::mutex> g(mutex_);
    const ConnectState &s = state_;
    return "{\"status\":" + jsonQuote(status_) +
           ",\"messages\":" + std::to_string(messages_) +
           ",\"clusters\":" + std::to_string(clusters_) +
           ",\"active\":" + jsonQuote(s.activeId) +
           ",\"device\":" + jsonQuote(s.deviceName) +
           ",\"type\":" + jsonQuote(s.deviceType) +
           ",\"track\":" + jsonQuote(s.trackUri) +
           ",\"title\":" + jsonQuote(s.title) +
           ",\"artist\":" + jsonQuote(s.artist) +
           ",\"image\":" + jsonQuote(s.imageUrl) +
           ",\"duration_ms\":" + std::to_string(s.durationMs) +
           ",\"position_ms\":" + std::to_string(s.positionMs) +
           ",\"playing\":" + (s.playing ? "true" : "false") +
           ",\"volume\":" + std::to_string(s.volume) +
           ",\"devices\":" + jsonQuote(s.devices) +
           ",\"sample\":" + jsonQuote(sample_) + "}";
}

bool ConnectWatch::command(const std::string &targetId, const std::string &endpoint, int64_t valueMs) {
    if (targetId.empty()) return false;
    std::string body = "{\"command\":{\"endpoint\":" + jsonQuote(endpoint);
    if (valueMs >= 0) body += ",\"value\":" + std::to_string(valueMs);
    body += "}}";
    ApiResult r = api_->connect_state("POST", "player/command/from/" + hobsId_ + "/to/" + targetId,
                                      body, "");
    return r.ok();
}

bool ConnectWatch::setVolume(const std::string &targetId, int volume) {
    if (targetId.empty()) return false;
    ApiResult r = api_->connect_state("PUT", "connect/volume/from/" + hobsId_ + "/to/" + targetId,
                                      "{\"volume\":" + std::to_string(volume) + "}", "");
    return r.ok();
}

bool ConnectWatch::transfer(const std::string &targetId) {
    ConnectState s = snapshot();
    if (s.activeId.empty() || s.activeId == targetId) return false;
    ApiResult r = api_->connect_state("POST", "connect/transfer/from/" + s.activeId + "/to/" + targetId,
                                      "{\"transfer_options\":{\"restore_paused\":\"restore\"}}", "");
    return r.ok();
}

void ConnectWatch::inject(ConnectState s) {
    std::lock_guard<std::mutex> g(mutex_);
    injected_ = true;
    s.valid = true;
    s.receivedAtUs = nowUs();
    s.version = state_.version + 1;
    state_ = s;
}

void ConnectWatch::release() {
    std::lock_guard<std::mutex> g(mutex_);
    injected_ = false;
    unsigned version = state_.version + 1;
    state_ = ConnectState();
    state_.version = version;
}
