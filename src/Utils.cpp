#include "Utils.h"
#include <curl/curl.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <cstring>
#include <string>
#include <Logger.h>
#include "Config.h"

void dbg_mark(const char *s) {
    SceUID fd = sceIoOpen("ux0:data/cspot/stage.txt",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, s, strlen(s));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

bool loopback_mode() {
    static int cached = -1;
    if (cached < 0) {
        SceIoStat st;
        cached = sceIoGetstat("ux0:data/cspot/loopback", &st) >= 0 ? 1 : 0;
    }
    return cached == 1;
}

#define STB_IMAGE_IMPLEMENTATION
#include "image/stb_image.h"


// Hard cap on any single HTTP response held in RAM. Cover art is tens of KB and
// API JSON is small; this just stops a runaway or hostile transfer from OOMing.
#define MAX_DOWNLOAD_SIZE (8 * 1024 * 1024)

struct MemoryStruct {
  char *memory;
  size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    if (mem->size + realsize > MAX_DOWNLOAD_SIZE) {
        CSPOT_LOG(error, "download exceeds %d byte cap, aborting", MAX_DOWNLOAD_SIZE);
        return 0;
    }

    char *ptr = (char *) realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        CSPOT_LOG(error, "not enough memory (realloc returned NULL)");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

std::string cover_art_path(std::string url) {
    size_t found = url.find_last_of("/\\");
    std::string path = "ux0:data/cspot/cache/" + url.substr(found+1);
    return path;
}

bool is_cover_cached(std::string url) {
    SceIoStat stat;
    std::string path = cover_art_path(url);
    return sceIoGetstat(path.c_str(), &stat) == 0;
}

bool cache_cover_art(std::string url, const uint8_t *buffer, uint32_t length) {
    std::string path = cover_art_path(url);
    int fd = sceIoOpen(path.c_str(), SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0666);
    if (fd < 0) {
        return false;
    }

    sceIoWrite(fd, buffer, length);
    sceIoClose(fd);
    return true;
}

// When inet_pton says the host is an IP literal, curl matches the certificate
// against IP entries instead of the DNS name. Vita3K's inet_pton answers
// nonzero for host names, so every HTTPS call failed there with "no
// alternative certificate subject name matches target ipv4 address". A real
// Vita answers 0 and keeps the full check; only a broken inet_pton turns the
// name check off, and the chain is still verified against the CA bundle.
static long verify_host_level() {
    static long level = -1;
    if (level < 0) {
        struct in_addr a;
        int r = inet_pton(AF_INET, "spotify.com", &a);
        level = (r == 0) ? 2L : 0L;
        if (level == 0) {
            CSPOT_LOG(info, "inet_pton(host name) returned %d: TLS name check off", r);
        }
    }
    return level;
}

void curl_apply_tls(void *handle) {
    CURL *h = static_cast<CURL *>(handle);
    curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, verify_host_level());
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_CAINFO, TLS_CA_BUNDLE);
    curl_easy_setopt(h, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
}

int download(const char *url, uint8_t **return_buffer, const char *method, std::string post_data, Headers headers,
             long *status) {
    CURL *curl_handle;
    CURLcode res;

    struct MemoryStruct chunk;
    chunk.memory = (char *) malloc(1);
    chunk.size = 0;

    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT);
    // Verify the server cert against the bundled CA store. This was disabled
    // (VERIFYHOST/PEER 0), which let anyone on the network MITM the Spotify Web
    // API traffic -- and that traffic carries the bearer token. Needs a correct
    // system clock on the Vita.
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, verify_host_level());
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_CAINFO, TLS_CA_BUNDLE);
    curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    // Never let a stalled transfer wedge the caller (cover art + API run on the
    // cspot and GUI threads); bound both connect and total time.
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, method);
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, errbuf);

    if (post_data.size() != 0) {
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_data.size()));
    } else if (strcmp(method, "GET") != 0) {
        // A bodiless PUT (seek, shuffle, repeat) must still say
        // "Content-Length: 0"; without it the Web API answers 411.
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, 0L);
    }

    struct curl_slist *headerchunk = NULL;
    for (auto it : headers) {
        headerchunk = curl_slist_append(headerchunk, it.c_str());
    }
    res = curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerchunk);

    // Perform the request
    res = curl_easy_perform(curl_handle);
    long httpresponsecode = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpresponsecode);
    if (httpresponsecode < 200 || httpresponsecode >= 300) {
        CSPOT_LOG(debug, "response code: %ld", httpresponsecode);
    }
    if (status != NULL) {
        *status = (res == CURLE_OK) ? httpresponsecode : 0;
    }

    if (res != CURLE_OK) {
        CSPOT_LOG(error, "curl_easy_perform() failed: %s (%s)", curl_easy_strerror(res), errbuf);
        free(chunk.memory);
        *return_buffer = NULL;
        chunk.size = 0;
    } else {
        CSPOT_LOG(debug, "%lu bytes retrieved", (unsigned long) chunk.size);
        *return_buffer = (uint8_t *) chunk.memory;
    }
    curl_easy_cleanup(curl_handle);
    curl_slist_free_all(headerchunk);  // was leaked on every request (NULL-safe)
    return chunk.size;
}

// Persistent curl handle for spclient. NEVER cleaned up between calls: keeping
// it alive lets curl reuse the TCP+TLS connection (keep-alive), so a burst of
// playlist-name fetches does ONE DNS resolve + handshake total instead of one
// per request. The per-request fresh-handle path (download) triggered "Could
// not resolve hostname" under the burst and starved Mercury into a crash.
static CURL *s_spclient_handle = NULL;

int spclient_get(const char *url, const std::string &bearer, uint8_t **return_buffer, long *status,
                 const char *accept, const std::string *body, const char *contentType) {
    if (status != NULL) {
        *status = 0;
    }
    struct MemoryStruct chunk;
    chunk.memory = (char *) malloc(1);
    chunk.size = 0;

    if (s_spclient_handle == NULL) {
        s_spclient_handle = curl_easy_init();
    }
    CURL *h = s_spclient_handle;
    if (h == NULL) {
        free(chunk.memory);
        *return_buffer = NULL;
        return 0;
    }

    // Reset options (keeps the connection cache, so keep-alive still works) and
    // re-apply. Same TLS hardening as download().
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, verify_host_level());
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_CAINFO, TLS_CA_BUNDLE);
    curl_easy_setopt(h, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);

    struct curl_slist *hl = NULL;
    std::string auth = "Authorization: Bearer " + bearer;
    hl = curl_slist_append(hl, auth.c_str());
    if (accept != NULL) {
        std::string acc = std::string("Accept: ") + accept;
        hl = curl_slist_append(hl, acc.c_str());
    }
    if (body != NULL) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
        if (contentType != NULL) {
            std::string ct = std::string("Content-Type: ") + contentType;
            hl = curl_slist_append(hl, ct.c_str());
        }
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);

    CURLcode res = curl_easy_perform(h);
    curl_slist_free_all(hl);

    if (res != CURLE_OK) {
        CSPOT_LOG(error, "spclient_get failed: %s (%s)", curl_easy_strerror(res), errbuf);
        free(chunk.memory);
        *return_buffer = NULL;
        return 0;
    }

    long status_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status_code);
    if (status != NULL) {
        *status = status_code;
    }
    if (status_code != 200) {
        CSPOT_LOG(error, "spclient HTTP %ld: %s", status_code, url);
        free(chunk.memory);
        *return_buffer = NULL;
        return 0;
    }

    *return_buffer = (uint8_t *) chunk.memory;
    return static_cast<int>(chunk.size);
}

bool LoadTextureFromFile(const char* filename, vita2d_texture** out_texture, int* out_width, int* out_height) {
    int w = 0, h = 0;
    unsigned char* pixels = stbi_load(filename, &w, &h, NULL, 4);
    if (pixels == NULL) {
        return false;
    }
    *out_texture = Render::texture_from_rgba(pixels, w, h);
    stbi_image_free(pixels);
    if (*out_texture == nullptr) {
        return false;
    }
    *out_width = w;
    *out_height = h;
    return true;
}

uint8_t *decode_image(const uint8_t *buf, size_t len, int max_side, int *w, int *h) {
    int iw = 0, ih = 0;
    unsigned char *px = stbi_load_from_memory(buf, static_cast<int>(len), &iw, &ih, NULL, 4);
    if (px == NULL) {
        return NULL;
    }
    int f = 1;
    while (iw / f > max_side || ih / f > max_side) {
        f++;
    }
    if (f == 1) {
        *w = iw;
        *h = ih;
        return px;
    }
    // Box filter: a 640 px cover drawn at 190 px wastes VRAM and upload time.
    int ow = iw / f, oh = ih / f;
    uint8_t *out = static_cast<uint8_t *>(malloc(static_cast<size_t>(ow) * oh * 4));
    if (out == NULL) {
        stbi_image_free(px);
        return NULL;
    }
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            unsigned acc[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < f; dy++) {
                const uint8_t *src = px + (static_cast<size_t>(y * f + dy) * iw + x * f) * 4;
                for (int dx = 0; dx < f; dx++) {
                    for (int c = 0; c < 4; c++) acc[c] += src[dx * 4 + c];
                }
            }
            uint8_t *dst = out + (static_cast<size_t>(y) * ow + x) * 4;
            for (int c = 0; c < 4; c++) dst[c] = static_cast<uint8_t>(acc[c] / (f * f));
        }
    }
    stbi_image_free(px);
    *w = ow;
    *h = oh;
    return out;
}

bool start_pthread(void *(*fn)(void *), void *arg, size_t stack_size) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

int is_dir(const char *path) {
    SceIoStat stat = {0};
    if (sceIoGetstat(path, &stat) < 0) {
        return 0;
    }
    return SCE_S_ISDIR(stat.st_mode);
}

bool init_network() {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    int res;

    SceNetInitParam net_init_param;
    net_init_param.size = 0x200000;
    net_init_param.flags = 0;

    SceUID memid = sceKernelAllocMemBlock("SceNetMemory", 0x0C20D060, net_init_param.size, NULL);
    if (memid < 0) {
        CSPOT_LOG(error, "sceKernelAllocMemBlock failed (0x%X)\n", memid);
        return false;
    }

    sceKernelGetMemBlockBase(memid, &net_init_param.memory);

    res = sceNetInit(&net_init_param);
    if (res < 0) {
        CSPOT_LOG(error, "sceNetInit failed (0x%X)\n", res);
        return false;
    }

    res = sceNetCtlInit();
    if (res < 0) {
        CSPOT_LOG(error, "sceNetCtlInit failed (0x%X)\n", res);
        return false;
    }

    return true;
}

void term_network() {
    sceNetCtlTerm();
    sceNetTerm();
}
