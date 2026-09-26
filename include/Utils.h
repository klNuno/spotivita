#pragma once

#include "Render.h"
#include <string>
#include <vector>
#include <utility>

typedef struct SceAppMgrEvent {
    int event;       /* Event ID */
    SceUID appId;    /* Application ID. Added when required by the event */
    char param[56];  /* Parameters to pass with the event */
} SceAppMgrEvent;

typedef struct SceShellSvcSvcObjVtable SceShellSvcSvcObjVtable;

typedef struct SceShellSvcSvcObj {
    SceShellSvcSvcObjVtable *vptr;
    // more ...
} SceShellSvcSvcObj;

extern "C" {
    int sceAppMgrReceiveEvent(SceAppMgrEvent *appEvent);
    SceInt32 sceNotificationUtilBgAppInitialize(void);
}

#define SCE_APP_EVENT_UNK0                  (0x00000003)
#define SCE_APP_EVENT_ON_ACTIVATE           (0x10000001)
#define SCE_APP_EVENT_ON_DEACTIVATE         (0x10000002)
#define SCE_APP_EVENT_UNK1                  (0x10000300)
#define SCE_APP_EVENT_REQUEST_QUIT          (0x20000001)
#define SCE_APP_EVENT_UNK2                  (0x30000003)

// HTTP Headers
typedef std::vector<std::string> Headers;

// GUI thread: PNG/JPEG file to a new texture.
bool LoadTextureFromFile(const char* filename, vita2d_texture** out_texture, int* out_width, int* out_height);
// Decode PNG/JPEG to RGBA8 on any thread, shrinking by an integer factor until
// neither side exceeds max_side. Free the result with free().
uint8_t *decode_image(const uint8_t *buf, size_t len, int max_side, int *w, int *h);
int is_dir(const char *path);
bool init_network();
void term_network();
// Synchronous boot-stage marker: appends a line to ux0:data/cspot/stage.txt with
// sceIo (open/write/close) so the trail survives a hang/GPU-wedge, unlike the
// buffered text logger. Used to pin down where startup blocks on-device.
void dbg_mark(const char *s);
// True when ux0:data/cspot/loopback exists: every listening socket stays on
// 127.0.0.1 and Zeroconf is skipped. Used under Vita3K, where a socket bound to
// all interfaces would raise the Windows firewall prompt on the host.
bool loopback_mode();
// Detached thread through pthread-embedded. Any thread that waits on a
// std::condition_variable must be created this way: pthread_cond_wait checks
// cancellation through the calling thread's pthread record, which a raw
// sceKernelCreateThread thread does not have (NULL write, then a crash).
bool start_pthread(void *(*fn)(void *), void *arg, size_t stack_size);
// *status (optional) receives the HTTP status, or 0 when the transfer itself
// failed (DNS, TLS, timeout).
int download(const char *url, uint8_t **return_buffer, const char *method = "GET",
             std::string post_data = "", Headers headers = {}, long *status = nullptr);
// GET over a PERSISTENT, reused curl handle (keep-alive). A burst of playlist
// name lookups all hit the same host (spclient), so reusing one connection
// means a single DNS resolve + TLS handshake instead of one per request. The
// Vita's resolver chokes on rapid getaddrinfo bursts and the socket churn
// starves the Mercury link into a crash; one reused connection avoids it.
// With a body it POSTs it as contentType (protobuf requests such as the
// collection pages).
int spclient_get(const char *url, const std::string &bearer, uint8_t **return_buffer,
                 long *status = nullptr, const char *accept = nullptr,
                 const std::string *body = nullptr, const char *contentType = nullptr);
bool cache_cover_art(std::string url, const uint8_t *buffer, uint32_t length);
std::string cover_art_path(std::string url);
bool is_cover_cached(std::string url);
