#include "Thumbs.h"
#include "NetWorker.h"
#include "Utils.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <Logger.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

const char *THUMB_DIR = "ux0:data/cspot/thumbs";
// Textures kept: a 96 px thumbnail is 36 KB of GPU memory.
const size_t THUMB_KEEP = 96;
const int THUMB_DIR_MAX_FILES = 800;

// Covers from pickasso all end in "/en" or "/fr", so the file name is a hash
// of the whole URL, not its last segment.
std::string thumbPath(const std::string &url) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char name[24];
    snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(h));
    return std::string(THUMB_DIR) + "/" + name;
}

bool readFile(const std::string &path, std::string *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return !out->empty();
}

}  // namespace

ThumbCache::~ThumbCache() {
    for (auto &e : entries_) Render::free_texture(e.second.tex);
}

vita2d_texture *ThumbCache::get(const std::string &url, int maxSide) {
    if (url.empty()) return nullptr;
    std::string key = std::to_string(maxSide) + ":" + url;
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.used = frame_;
        return it->second.tex;
    }
    Entry &e = entries_[key];
    e.used = frame_;
    NetWorker *w = worker_;
    worker_->post([this, w, key, url, maxSide] {
        std::string bytes;
        std::string path = thumbPath(url);
        bool cached = readFile(path, &bytes);
        if (!cached) {
            uint8_t *buf = nullptr;
            int len = image_get(url.c_str(), &buf);
            if (len > 0 && buf != nullptr) bytes.assign(reinterpret_cast<char*>(buf), len);
            free(buf);
        }
        int iw = 0, ih = 0;
        uint8_t *rgba = bytes.empty() ? nullptr
            : decode_image(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), maxSide, &iw, &ih);
        if (rgba != nullptr && !cached) {
            sceIoMkdir(THUMB_DIR, 0777);
            FILE *f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(bytes.data(), 1, bytes.size(), f);
                fclose(f);
            }
        }
        w->deliver([this, key, rgba, iw, ih] {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                found->second.loading = false;
                if (rgba != nullptr) found->second.tex = Render::texture_from_rgba(rgba, iw, ih);
            }
            free(rgba);
        });
    });
    return nullptr;
}

void ThumbCache::trim() {
    if (entries_.size() <= THUMB_KEEP) return;
    // Oldest first; never one still loading (its job delivers by key) or one
    // drawn in the last frame.
    std::vector<std::pair<uint32_t, std::string>> old;
    for (const auto &e : entries_) {
        if (!e.second.loading && e.second.used + 1 < frame_) old.push_back({e.second.used, e.first});
    }
    std::sort(old.begin(), old.end());
    size_t drop = entries_.size() - THUMB_KEEP * 3 / 4;
    for (size_t i = 0; i < old.size() && i < drop; i++) {
        auto it = entries_.find(old[i].second);
        Render::free_texture(it->second.tex);
        entries_.erase(it);
    }
}

void prune_thumb_cache() {
    SceUID d = sceIoDopen(THUMB_DIR);
    if (d < 0) return;
    std::vector<std::string> names;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        if (!SCE_S_ISDIR(e.d_stat.st_mode)) names.push_back(e.d_name);
    }
    sceIoDclose(d);
    if (static_cast<int>(names.size()) <= THUMB_DIR_MAX_FILES) return;
    for (const auto &n : names) sceIoRemove((std::string(THUMB_DIR) + "/" + n).c_str());
    CSPOT_LOG(info, "thumbnail cache pruned (%d files)", static_cast<int>(names.size()));
}
