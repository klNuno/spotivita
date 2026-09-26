#include "DevKit.h"

// Standard headers stay outside the #ifdef: cpplint does not look inside it.
#include <condition_variable>  // NOLINT
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>  // NOLINT
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef SPOTIVITA_DEVKIT

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <Logger.h>
#include "Gui.h"
#include "GuiUtils.h"
#include "Input.h"
#include "Utils.h"

namespace DevKit {
namespace {

const int PORT = 2138;
// Idle time before a silent client is dropped (a 3 MB eboot upload never
// stalls this long between packets).
const int CLIENT_TIMEOUT_S = 20;

// A request that must run on the GUI thread; the server thread waits for it.
struct GuiCall {
    std::function<std::string()> fn;
    std::string result;
    bool done = false;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::shared_ptr<GuiCall>> g_calls;
GUI *g_gui = nullptr;

std::string callOnGui(std::function<std::string()> fn) {
    auto call = std::make_shared<GuiCall>();
    call->fn = std::move(fn);
    std::unique_lock<std::mutex> lk(g_mutex);
    g_calls.push_back(call);
    // The GUI loop pumps at least every 100 ms, even when idle or paused.
    if (!g_cv.wait_for(lk, std::chrono::seconds(5), [&] { return call->done; })) {
        return "ERR gui thread did not answer";
    }
    return call->result;
}

bool sendAll(int fd, const void *data, size_t len) {
    const char *p = static_cast<const char *>(data);
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool sendText(int fd, const std::string &s) {
    return sendAll(fd, s.data(), s.size());
}

bool sendBlob(int fd, const std::string &blob) {
    std::string head = "OK " + std::to_string(blob.size()) + "\n";
    return sendText(fd, head) && sendAll(fd, blob.data(), blob.size());
}

bool recvLine(int fd, std::string *line) {
    line->clear();
    char c;
    while (true) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') line->push_back(c);
        if (line->size() > 4096) return false;
    }
    return true;
}

bool recvAll(int fd, char *buf, size_t len) {
    while (len > 0) {
        int n = recv(fd, buf, len, 0);
        if (n <= 0) return false;
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool readFile(const std::string &path, std::string *out) {
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    out->clear();
    char buf[16384];
    int n;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0) {
        out->append(buf, static_cast<size_t>(n));
    }
    sceIoClose(fd);
    return true;
}

// Displayed framebuffer -> 24-bit BMP. Reads what is on screen right now from
// any thread, without touching the GPU. Returns false on an all-black frame:
// Vita3K renders on the host and never writes the guest framebuffer back.
bool screenshot(std::string *bmp) {
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || fb.base == NULL) {
        return false;
    }
    const int w = static_cast<int>(fb.width), h = static_cast<int>(fb.height);
    const int rowBytes = (w * 3 + 3) & ~3;
    const uint32_t dataSize = static_cast<uint32_t>(rowBytes * h);
    const uint32_t fileSize = 54 + dataSize;
    bmp->assign(fileSize, '\0');
    uint8_t *o = reinterpret_cast<uint8_t *>(&(*bmp)[0]);
    auto put32 = [&](int at, uint32_t v) { memcpy(o + at, &v, 4); };
    auto put16 = [&](int at, uint16_t v) { memcpy(o + at, &v, 2); };
    o[0] = 'B'; o[1] = 'M';
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    put16(26, 1);
    put16(28, 24);
    put32(34, dataSize);
    const uint32_t *src = static_cast<const uint32_t *>(fb.base);
    uint32_t any = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + static_cast<size_t>(h - 1 - y) * fb.pitch;
        uint8_t *dst = o + 54 + static_cast<size_t>(y) * rowBytes;
        for (int x = 0; x < w; x++) {
            uint32_t px = row[x];            // A8B8G8R8: R in the low byte
            any |= px & 0xFFFFFF;
            dst[x * 3 + 0] = (px >> 16) & 0xFF;
            dst[x * 3 + 1] = (px >> 8) & 0xFF;
            dst[x * 3 + 2] = px & 0xFF;
        }
    }
    return any != 0;
}

uint32_t buttonMask(const std::string &names) {
    static const struct { const char *name; uint32_t mask; } kButtons[] = {
        {"cross", SCE_CTRL_CROSS}, {"circle", SCE_CTRL_CIRCLE},
        {"square", SCE_CTRL_SQUARE}, {"triangle", SCE_CTRL_TRIANGLE},
        {"up", SCE_CTRL_UP}, {"down", SCE_CTRL_DOWN},
        {"left", SCE_CTRL_LEFT}, {"right", SCE_CTRL_RIGHT},
        {"l", SCE_CTRL_LTRIGGER}, {"r", SCE_CTRL_RTRIGGER},
        {"start", SCE_CTRL_START}, {"select", SCE_CTRL_SELECT},
    };
    uint32_t mask = 0;
    std::stringstream ss(names);
    std::string part;
    while (std::getline(ss, part, '+')) {
        for (const auto &b : kButtons) {
            if (part == b.name) mask |= b.mask;
        }
    }
    return mask;
}

// One client connection: commands until it closes. Returns false to stop.
void serve(int fd) {
    std::string line;
    while (recvLine(fd, &line)) {
        std::stringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "ping") {
            sendText(fd, "OK pong\n");
        } else if (cmd == "state") {
            std::string s = callOnGui([] { return g_gui->debugState(); });
            sendText(fd, "OK " + s + "\n");
        } else if (cmd == "tap") {
            int x = 0, y = 0;
            in >> x >> y;
            Input::inject_tap(x, y);
            sendText(fd, "OK\n");
        } else if (cmd == "swipe") {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0, frames = 12;
            in >> x1 >> y1 >> x2 >> y2;
            if (!(in >> frames)) frames = 12;
            Input::inject_swipe(x1, y1, x2, y2, frames);
            sendText(fd, "OK\n");
        } else if (cmd == "press") {
            std::string names;
            in >> names;
            uint32_t mask = buttonMask(names);
            if (mask == 0) {
                sendText(fd, "ERR unknown button\n");
            } else {
                Input::inject_buttons(mask);
                sendText(fd, "OK\n");
            }
        } else if (cmd == "ui") {
            std::string sub, arg;
            in >> sub;
            std::getline(in, arg);
            if (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
            std::string r = callOnGui([sub, arg] {
                return g_gui->debugCommand(sub, arg) ? std::string("OK\n")
                                                     : std::string("ERR unknown ui command\n");
            });
            sendText(fd, r);
        } else if (cmd == "shot") {
            std::string bmp;
            if (screenshot(&bmp)) {
                sendBlob(fd, bmp);
            } else {
                sendText(fd, "ERR framebuffer unreadable or blank (under Vita3K use vita3k.ps1 shot)\n");
            }
        } else if (cmd == "log") {
            size_t bytes = 8192;
            in >> bytes;
            sendBlob(fd, log_tail(bytes));
        } else if (cmd == "get") {
            std::string path, data;
            in >> path;
            if (readFile(path, &data)) {
                sendBlob(fd, data);
            } else {
                sendText(fd, "ERR cannot read\n");
            }
        } else if (cmd == "put") {
            std::string path;
            size_t len = 0;
            in >> path >> len;
            if (path.empty() || len == 0 || len > 64 * 1024 * 1024) {
                sendText(fd, "ERR usage: put PATH LEN\n");
                return;
            }
            std::vector<char> buf(len);
            if (!recvAll(fd, buf.data(), len)) {
                return;
            }
            // Write beside the target, then swap, so a dropped transfer never
            // leaves a truncated eboot.bin behind.
            std::string tmp = path + ".part";
            SceUID out = sceIoOpen(tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            bool ok = out >= 0 && sceIoWrite(out, buf.data(), len) == static_cast<int>(len);
            if (out >= 0) sceIoClose(out);
            if (ok) {
                sceIoRemove(path.c_str());
                ok = sceIoRename(tmp.c_str(), path.c_str()) >= 0;
            }
            sendText(fd, ok ? "OK\n" : "ERR write failed\n");
        } else if (cmd == "relaunch") {
            sendText(fd, "OK\n");
            flush_logger();
            sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
        } else if (cmd == "quit") {
            sendText(fd, "OK\n");
            g_gui->isRunning = false;
        } else if (!cmd.empty()) {
            sendText(fd, "ERR unknown command\n");
        }
    }
}

void *serverMain(void *) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        CSPOT_LOG(error, "devkit: socket failed");
        return nullptr;
    }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(loopback_mode() ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        listen(listener, 2) < 0) {
        CSPOT_LOG(error, "devkit: cannot listen on %d", PORT);
        close(listener);
        return nullptr;
    }
    CSPOT_LOG(info, "devkit: listening on port %d", PORT);
    // Blocking accept loop: vitasdk's select() never reports a listening
    // socket readable (same trap as bell's HTTPServer, see patch/bell).
    while (true) {
        // Real out-parameters: Vita3K's accept() writes them unconditionally
        // and a NULL here crashed the whole emulator on the first connection.
        sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        int fd = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peerLen);
        if (fd < 0) {
            sceKernelDelayThread(100000);
            continue;
        }
        // One client at a time: a peer that vanished without closing (Wi-Fi
        // asleep, PC gone) held the server in recv forever, and every later
        // connection queued behind it and timed out.
        struct timeval to;
        to.tv_sec = CLIENT_TIMEOUT_S;
        to.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
        serve(fd);
        close(fd);
    }
    return nullptr;
}

}  // namespace

void start(GUI *gui) {
    g_gui = gui;
    // A pthread: its callOnGui waits on a std::condition_variable.
    start_pthread(serverMain, nullptr, 0x10000);
}

bool pump(GUI *) {
    std::deque<std::shared_ptr<GuiCall>> calls;
    {
        std::lock_guard<std::mutex> g(g_mutex);
        calls.swap(g_calls);
    }
    for (auto &c : calls) {
        std::string r = c->fn();
        std::lock_guard<std::mutex> g(g_mutex);
        c->result = r;
        c->done = true;
    }
    if (!calls.empty()) {
        g_cv.notify_all();
    }
    return !calls.empty();
}

}  // namespace DevKit

#else

namespace DevKit {
void start(GUI *) {}
bool pump(GUI *) { return false; }
}  // namespace DevKit

#endif
