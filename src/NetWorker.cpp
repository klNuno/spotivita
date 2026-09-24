#include "NetWorker.h"
#include <psp2/kernel/threadmgr.h>
#include <Logger.h>
#include <exception>
#include <utility>

// curl + OpenSSL handshakes and cJSON parsing need far more than the 64 KB the
// other threads get; 512 KB leaves room for a TLS 1.2 handshake with margin.
static const int NET_STACK_SIZE = 0x80000;

void NetWorker::start() {
    SceUID id = sceKernelCreateThread("net", (SceKernelThreadEntry) threadMain,
                                      0x10000100, NET_STACK_SIZE, 0, 0, NULL);
    NetWorker *self = this;
    sceKernelStartThread(id, sizeof(self), &self);
}

int NetWorker::threadMain(unsigned int, void *argp) {
    NetWorker *self = *static_cast<NetWorker **>(argp);
    self->loop();
    return 0;
}

void NetWorker::post(Job job, bool urgent) {
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (urgent) {
            jobs_.push_front(std::move(job));
        } else {
            jobs_.push_back(std::move(job));
        }
        pending_++;
    }
    cv_.notify_one();
}

void NetWorker::deliver(Job done) {
    std::lock_guard<std::mutex> g(resultsMutex_);
    results_.push_back(std::move(done));
}

bool NetWorker::drainResults() {
    std::deque<Job> ready;
    {
        std::lock_guard<std::mutex> g(resultsMutex_);
        ready.swap(results_);
    }
    for (auto &r : ready) {
        r();
    }
    return !ready.empty();
}

void NetWorker::loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        // A job that throws must not take the process down with it: the UI
        // just keeps its previous state and the user can retry.
        try {
            job();
        } catch (const std::exception &e) {
            CSPOT_LOG(error, "net job failed: %s", e.what());
        } catch (...) {
            CSPOT_LOG(error, "net job failed (unknown exception)");
        }
        pending_--;
    }
}
