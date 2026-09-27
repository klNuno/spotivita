#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>  // NOLINT
#include <string>

class API;
struct cJSON;

// What Spotify Connect says the account is playing, on any device, this Vita
// included. Filled from the cluster that connect-state sends.
struct ConnectState {
    bool valid = false;            // a cluster arrived
    std::string activeId;          // active device, "" when none
    std::string deviceName;        // its name
    std::string deviceType;        // "SMARTPHONE", "COMPUTER", ...
    std::string trackUri;
    std::string title;             // from the cluster, may be empty
    std::string artist;
    std::string imageUrl;          // https://i.scdn.co/image/<hex>, or ""
    int durationMs = 0;
    int volume = -1;               // of the active device, 0..65535, -1 unknown
    int64_t positionMs = 0;        // at receivedAtUs
    bool playing = false;          // playing and not paused
    uint64_t receivedAtUs = 0;     // process time the state was received
    std::string devices;           // "id=name/type, ..." (devkit)
    unsigned version = 0;          // bumped on each change
};

// A hidden observer on the dealer websocket, the way the web player listens:
// it opens wss://dealer.spotify.com, reads the connection id, registers a
// hidden device that cannot play (PUT connect-state/v1/devices/hobs_<id>) and
// then receives every cluster update of the account. Runs on its own thread
// and reconnects on its own.
class ConnectWatch {
 public:
    explicit ConnectWatch(API *api) : api_(api) {}
    void start();
    ConnectState snapshot() const;
    std::string debugJson() const;

    // Asks device targetId to pause, resume, skip_next, skip_prev or seek_to
    // valueMs. Blocking, like the two below: call from the net worker.
    bool command(const std::string &targetId, const std::string &endpoint, int64_t valueMs = -1);
    // Sets the volume of device targetId (0..65535).
    bool setVolume(const std::string &targetId, int volume);
    // Moves playback from the active device to targetId, where it continues.
    bool transfer(const std::string &targetId);
    // Replaces the state as if a cluster said so and holds it against real
    // clusters until release() (devkit tests).
    void inject(ConnectState s);
    void release();

 private:
    static void *threadMain(void *arg);
    void loop();
    bool session();
    bool handleMessage(const std::string &msg);
    bool registerObserver();
    void applyCluster(cJSON *cluster);
    void setStatus(const std::string &s);

    API *api_;
    std::atomic<bool> started_{false};
    mutable std::mutex mutex_;
    ConnectState state_;
    std::string status_ = "idle";
    std::string connectionId_;
    std::string hobsId_;
    std::string sample_;           // start of the last cluster (devkit)
    int messages_ = 0;
    int clusters_ = 0;
    bool injected_ = false;
};
