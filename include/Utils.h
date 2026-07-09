#pragma once

#include <imgui_vita.h>
#include <vitaGL.h>
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

bool LoadTextureFromFile(const char* filename, GLuint* out_texture, int* out_width, int* out_height);
bool LoadTextureFromMemory(const uint8_t* buffer, uint32_t length,
                           GLuint* out_texture, int* out_width, int* out_height);
int is_dir(const char *path);
bool init_network();
void term_network();
// Synchronous boot-stage marker: appends a line to ux0:data/cspot/stage.txt with
// sceIo (open/write/close) so the trail survives a hang/GPU-wedge, unlike the
// buffered text logger. Used to pin down where startup blocks on-device.
void dbg_mark(const char *s);
int download(const char *url, uint8_t **return_buffer, const char *method = "GET",
                        std::string post_data = "", Headers headers = {});
// GET over a PERSISTENT, reused curl handle (keep-alive). A burst of playlist
// name lookups all hit the same host (spclient), so reusing one connection
// means a single DNS resolve + TLS handshake instead of one per request. The
// Vita's resolver chokes on rapid getaddrinfo bursts and the socket churn
// starves the Mercury link into a crash; one reused connection avoids it.
int spclient_get(const char *url, const std::string &bearer, uint8_t **return_buffer);
bool cache_cover_art(std::string url, uint8_t *buffer, uint32_t length);
std::string cover_art_path(std::string url);
bool is_cover_cached(std::string url);
