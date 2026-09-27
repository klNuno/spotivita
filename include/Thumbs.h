#pragma once

#include "Render.h"
#include <cstdint>
#include <map>
#include <string>

class NetWorker;

// Small covers and avatars for list rows and page headers. Each is fetched
// once on the image worker (disk cache first), decoded there, and kept as a
// texture until the cache grows past its limit, when the least recently drawn
// go first. GUI thread only, apart from the jobs it posts.
class ThumbCache {
 public:
    explicit ThumbCache(NetWorker *worker) : worker_(worker) {}
    ~ThumbCache();
    // The texture, or nullptr while it loads or when it failed. The first
    // call starts the load; decoded no bigger than maxSide pixels.
    vita2d_texture *get(const std::string &url, int maxSide);
    // Once per drawn frame, before the draws.
    void newFrame() { frame_++; }
    // Between frames: frees the textures past the limit.
    void trim();
    size_t size() const { return entries_.size(); }

 private:
    struct Entry {
        vita2d_texture *tex = nullptr;
        uint32_t used = 0;
        bool loading = true;
    };
    NetWorker *worker_;
    std::map<std::string, Entry> entries_;
    uint32_t frame_ = 1;
};

// Empties the thumbnail disk cache once it holds too many files (image worker).
void prune_thumb_cache();
