#pragma once

#include <atomic>
#include <condition_variable>  // NOLINT
#include <deque>
#include <functional>
#include <mutex>  // NOLINT

// One background thread that owns every blocking HTTP call the UI makes.
//
// The GUI thread never waits on the network: it posts a job, keeps rendering,
// and gets the outcome back as a closure that runs on the GUI thread between
// frames (drainResults). GL textures, ImGui state and the screen's members are
// therefore only ever touched by the GUI thread, and a 12 s curl timeout on bad
// wifi costs a spinner instead of a frozen console.
class NetWorker {
 public:
    using Job = std::function<void()>;

    void start();

    // Queue work for the worker thread. Urgent jobs (a tap on play/seek) jump
    // ahead of background fetches such as playlist-name resolution.
    void post(Job job, bool urgent = false);

    // Called FROM a job: run `done` on the GUI thread at the next drain.
    void deliver(Job done);

    // GUI thread, outside the ImGui frame. Returns true if anything ran, so the
    // frame gate knows the UI changed.
    bool drainResults();

    // Jobs queued or running. Cheap, lock-free, for spinners.
    bool busy() const { return pending_.load() > 0; }

 private:
    static void *threadMain(void *arg);
    void loop();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::mutex resultsMutex_;
    std::deque<Job> results_;
    std::atomic<int> pending_{0};
};
