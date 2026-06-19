#pragma once

#include <atomic>
#include <mutex>  // NOLINT
#include <string>
#include <cstdint>
#include <psp2/kernel/processmgr.h>

// Observable playback state shared between the cspot worker thread (writer) and
// the GUI thread (reader). This is the decoupling layer: the UI never touches
// cspot directly, so a protocol break stays contained to the adapter in main.
//
// cspot exposes no position getter, so position is interpolated locally from the
// last anchor (set on track load / seek / play-pause) plus elapsed wall time.
class PlayerModel {
 public:
    void setTrack(const std::string& name, const std::string& album,
                  const std::string& artist, const std::string& imageUrl,
                  int durationMs) {
        std::lock_guard<std::mutex> g(mutex_);
        name_ = name;
        album_ = album;
        artist_ = artist;
        imageUrl_ = imageUrl;
        durationMs_ = durationMs;
        positionAnchorMs_ = 0;
        anchorAtUs_ = nowUs();
    }

    void setPaused(bool paused) {
        std::lock_guard<std::mutex> g(mutex_);
        // Re-anchor at the current interpolated position so the clock freezes
        // (or resumes) cleanly across the state flip.
        positionAnchorMs_ = interpLocked();
        anchorAtUs_ = nowUs();
        paused_ = paused;
    }

    // From SEEK / LOAD / PLAYBACK_START events, or an optimistic local seek.
    void setPosition(int positionMs) {
        std::lock_guard<std::mutex> g(mutex_);
        positionAnchorMs_ = positionMs;
        anchorAtUs_ = nowUs();
    }

    void setVolume(int volume0_65535) { volume_.store(volume0_65535); }

    struct Snapshot {
        std::string name, album, artist, imageUrl;
        int durationMs = 0;
        int positionMs = 0;
        bool paused = true;
        int volume = 32767;
    };

    Snapshot snapshot() {
        std::lock_guard<std::mutex> g(mutex_);
        Snapshot s;
        s.name = name_;
        s.album = album_;
        s.artist = artist_;
        s.imageUrl = imageUrl_;
        s.durationMs = durationMs_;
        s.positionMs = interpLocked();
        s.paused = paused_;
        s.volume = volume_.load();
        return s;
    }

 private:
    static uint64_t nowUs() { return sceKernelGetProcessTimeWide(); }

    int interpLocked() {
        int pos = positionAnchorMs_;
        if (!paused_) {
            pos += static_cast<int>((nowUs() - anchorAtUs_) / 1000);
        }
        if (durationMs_ > 0 && pos > durationMs_) pos = durationMs_;
        if (pos < 0) pos = 0;
        return pos;
    }

    std::mutex mutex_;
    std::string name_ = "Not playing";
    std::string album_;
    std::string artist_;
    std::string imageUrl_;
    int durationMs_ = 0;
    int positionAnchorMs_ = 0;
    uint64_t anchorAtUs_ = 0;
    bool paused_ = true;
    std::atomic<int> volume_{32767};
};
