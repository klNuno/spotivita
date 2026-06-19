#include "Login5.h"

#include <curl/curl.h>
#include <psp2/kernel/processmgr.h>

#include <cstring>

#include <Logger.h>
#include "Crypto.h"

// Hand-rolled minimal protobuf + the login5/clienttoken request flow. The
// messages involved are tiny and fixed-shape, so a full nanopb wiring would be
// overkill; we emit/parse the wire format directly.
//
// Wire types used here: 0 = varint, 2 = length-delimited (string/bytes/message).

namespace {

// ----------------------------------------------------------------------------
// Protobuf encoding
// ----------------------------------------------------------------------------

void pbVarint(std::string &o, uint64_t v) {
    while (v >= 0x80) {
        o.push_back((char)(uint8_t)(v | 0x80));
        v >>= 7;
    }
    o.push_back((char)(uint8_t)v);
}

void pbKey(std::string &o, int field, int wire) {
    pbVarint(o, ((uint64_t)field << 3) | (uint64_t)wire);
}

void pbVarintField(std::string &o, int field, uint64_t v) {
    pbKey(o, field, 0);
    pbVarint(o, v);
}

void pbLenField(std::string &o, int field, const void *d, size_t n) {
    pbKey(o, field, 2);
    pbVarint(o, n);
    o.append((const char *)d, n);
}

void pbStrField(std::string &o, int field, const std::string &s) {
    pbLenField(o, field, s.data(), s.size());
}

void pbMsgField(std::string &o, int field, const std::string &m) {
    pbLenField(o, field, m.data(), m.size());
}

// ----------------------------------------------------------------------------
// Protobuf decoding
// ----------------------------------------------------------------------------

struct PbField {
    int field = 0;
    int wire = 0;
    uint64_t val = 0;        // wire 0
    const uint8_t *data = nullptr;  // wire 2
    size_t len = 0;
};

struct PbReader {
    const uint8_t *p;
    const uint8_t *end;

    PbReader(const uint8_t *b, size_t n) : p(b), end(b + n) {}
    explicit PbReader(const std::vector<uint8_t> &v) : p(v.data()), end(v.data() + v.size()) {}

    bool varint(uint64_t &v) {
        v = 0;
        int shift = 0;
        while (p < end) {
            uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
            if (shift > 63) return false;
        }
        return false;
    }

    bool next(PbField &f) {
        if (p >= end) return false;
        uint64_t tag;
        if (!varint(tag)) return false;
        f.field = (int)(tag >> 3);
        f.wire = (int)(tag & 7);
        f.val = 0;
        f.data = nullptr;
        f.len = 0;
        switch (f.wire) {
            case 0:
                return varint(f.val);
            case 2: {
                uint64_t l;
                if (!varint(l)) return false;
                if ((uint64_t)(end - p) < l) return false;
                f.data = p;
                f.len = (size_t)l;
                p += l;
                return true;
            }
            case 5:  // fixed32
                if (end - p < 4) return false;
                p += 4;
                return true;
            case 1:  // fixed64
                if (end - p < 8) return false;
                p += 8;
                return true;
            default:
                return false;
        }
    }
};

// ----------------------------------------------------------------------------
// HTTP (binary-safe POST). The shared download() helper sizes the body with
// strlen, which truncates protobuf bodies at the first null byte, so login5
// needs its own POST.
// ----------------------------------------------------------------------------

size_t writeCb(void *ptr, size_t sz, size_t nm, void *up) {
    size_t n = sz * nm;
    auto *out = (std::vector<uint8_t> *)up;
    out->insert(out->end(), (uint8_t *)ptr, (uint8_t *)ptr + n);
    return n;
}

bool httpPost(const std::string &url, const std::string &body,
              const std::vector<std::string> &headers,
              std::vector<uint8_t> &out, long *status) {
    CURL *h = curl_easy_init();
    if (!h) return false;

    struct curl_slist *hl = nullptr;
    for (const auto &s : headers) hl = curl_slist_append(hl, s.c_str());

    out.clear();
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 25L);

    CURLcode res = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;

    curl_slist_free_all(hl);
    if (res != CURLE_OK) {
        CSPOT_LOG(error, "login5 POST %s failed: %s", url.c_str(), curl_easy_strerror(res));
        curl_easy_cleanup(h);
        return false;
    }
    curl_easy_cleanup(h);
    return true;
}

// ----------------------------------------------------------------------------
// Hashcash proof of work (matches go-librespot / librespot)
// ----------------------------------------------------------------------------

void sha1(Crypto &c, const uint8_t *d, size_t n, uint8_t out[20]) {
    c.sha1Init();
    c.sha1Update(std::vector<uint8_t>(d, d + n));
    std::vector<uint8_t> v = c.sha1FinalBytes();
    memcpy(out, v.data(), 20);
}

int trailingZeros8(uint8_t b) {
    if (b == 0) return 8;
    int n = 0;
    while (!(b & 1)) {
        n++;
        b >>= 1;
    }
    return n;
}

// True when the digest has at least `length` trailing zero bits (LSB = last byte).
bool checkHashcash(const uint8_t *hash, int hashLen, int length) {
    int idx = hashLen - 1;
    while (idx >= 0) {
        int zeros = trailingZeros8(hash[idx]);
        if (zeros >= length) return true;
        if (zeros < 8) return false;
        length -= 8;
        idx--;
    }
    return false;
}

// Big-endian +1 on the 8-byte region with LSB at `idx`.
void increment8(uint8_t *d, int idx) {
    d[idx]++;
    if (d[idx] == 0 && idx > 0) increment8(d, idx - 1);
}

void solveHashcash(Crypto &c, const std::vector<uint8_t> &loginCtx,
                   const uint8_t *prefix, size_t prefixLen, int length,
                   uint8_t suffix[16], int64_t *durSec, int32_t *durNanos) {
    uint8_t ctxSum[20];
    sha1(c, loginCtx.data(), loginCtx.size(), ctxSum);

    memset(suffix, 0, 16);
    memcpy(suffix, ctxSum + 12, 8);  // seed suffix[0:8] from sha1(login_context)[12:20]

    std::vector<uint8_t> buf(prefixLen + 16);
    if (prefixLen) memcpy(buf.data(), prefix, prefixLen);

    uint64_t start = sceKernelGetProcessTimeWide();  // microseconds
    uint8_t sum[20];
    for (;;) {
        memcpy(buf.data() + prefixLen, suffix, 16);
        sha1(c, buf.data(), buf.size(), sum);
        if (checkHashcash(sum, 20, length)) break;
        increment8(suffix, 7);
        increment8(suffix + 8, 7);
    }
    uint64_t us = sceKernelGetProcessTimeWide() - start;
    *durSec = (int64_t)(us / 1000000ULL);
    *durNanos = (int32_t)((us % 1000000ULL) * 1000ULL);
}

// ----------------------------------------------------------------------------
// clienttoken: POST https://clienttoken.spotify.com/v1/clienttoken
// ----------------------------------------------------------------------------

std::string getClientToken(const std::string &clientId, const std::string &deviceId,
                           const std::string &userAgent) {
    // NativeIOSData { user_interface_idiom=0, hw_machine, system_version }
    std::string ios;
    pbVarintField(ios, 1, 0);                 // user_interface_idiom (phone)
    pbStrField(ios, 3, "iPhone11,8");         // hw_machine
    pbStrField(ios, 4, "15.1");               // system_version

    // PlatformSpecificData { ios = NativeIOSData }  (oneof field 2)
    std::string platform;
    pbMsgField(platform, 2, ios);

    // ConnectivitySdkData { platform_specific_data, device_id }
    std::string conn;
    pbMsgField(conn, 1, platform);
    pbStrField(conn, 2, deviceId);

    // ClientDataRequest { client_version, client_id, connectivity_sdk_data }
    std::string clientData;
    pbStrField(clientData, 1, "8.6.84");      // client_version
    pbStrField(clientData, 2, clientId);
    pbMsgField(clientData, 3, conn);          // oneof data -> connectivity_sdk_data

    // ClientTokenRequest { request_type=1, client_data }
    std::string req;
    pbVarintField(req, 1, 1);                 // REQUEST_CLIENT_DATA_REQUEST
    pbMsgField(req, 2, clientData);

    std::vector<std::string> headers = {
        "Accept: application/x-protobuf",
        "Content-Type: application/x-protobuf",
        "User-Agent: " + userAgent,
    };

    std::vector<uint8_t> resp;
    long status = 0;
    if (!httpPost("https://clienttoken.spotify.com/v1/clienttoken", req, headers, resp, &status)) {
        return "";
    }
    if (status != 200) {
        CSPOT_LOG(error, "clienttoken HTTP %ld (%d bytes)", status, (int)resp.size());
        return "";
    }

    // ClientTokenResponse { response_type=1, granted_token=2 { token=1 } }
    PbReader r(resp);
    PbField f;
    while (r.next(f)) {
        if (f.field == 2 && f.wire == 2) {  // granted_token
            PbReader g(f.data, f.len);
            PbField gf;
            while (g.next(gf)) {
                if (gf.field == 1 && gf.wire == 2) {
                    std::string token((const char *)gf.data, gf.len);
                    CSPOT_LOG(info, "clienttoken granted (%d bytes)", (int)token.size());
                    return token;
                }
            }
        } else if (f.field == 3 && f.wire == 2) {  // challenges -> unsupported
            CSPOT_LOG(error, "clienttoken returned a challenge (unsupported)");
            return "";
        }
    }
    CSPOT_LOG(error, "clienttoken: no granted token in response");
    return "";
}

// ----------------------------------------------------------------------------
// login5: POST https://login5.spotify.com/v3/login
// ----------------------------------------------------------------------------

std::string buildLoginRequest(const std::string &clientId, const std::string &deviceId,
                              const std::string &username, const std::vector<uint8_t> &authData,
                              const std::vector<uint8_t> &loginContext,
                              const std::string &challengeSolutions) {
    // ClientInfo { client_id, device_id }
    std::string clientInfo;
    pbStrField(clientInfo, 1, clientId);
    pbStrField(clientInfo, 2, deviceId);

    // StoredCredential { username, data }
    std::string stored;
    pbStrField(stored, 1, username);
    pbLenField(stored, 2, authData.data(), authData.size());

    std::string req;
    pbMsgField(req, 1, clientInfo);
    if (!loginContext.empty()) pbLenField(req, 2, loginContext.data(), loginContext.size());
    if (!challengeSolutions.empty()) pbMsgField(req, 3, challengeSolutions);
    pbMsgField(req, 100, stored);  // oneof login_method -> stored_credential
    return req;
}

}  // namespace

std::string login5_get_access_token(const std::string &clientId, const std::string &deviceId,
                                    const std::string &userAgent, const std::string &username,
                                    const std::vector<uint8_t> &authData) {
    if (username.empty() || authData.empty()) {
        CSPOT_LOG(error, "login5: missing stored credentials");
        return "";
    }

    std::string clientToken = getClientToken(clientId, deviceId, userAgent);
    if (clientToken.empty()) return "";

    Crypto crypto;

    std::vector<std::string> headers = {
        "Accept: application/x-protobuf",
        "Content-Type: application/x-protobuf",
        "User-Agent: " + userAgent,
        "Client-Token: " + clientToken,
    };

    std::vector<uint8_t> loginContext;     // empty on the first attempt
    std::string challengeSolutions;        // empty on the first attempt

    for (int attempt = 0; attempt < 3; attempt++) {
        std::string req = buildLoginRequest(clientId, deviceId, username, authData,
                                            loginContext, challengeSolutions);

        std::vector<uint8_t> resp;
        long status = 0;
        if (!httpPost("https://login5.spotify.com/v3/login", req, headers, resp, &status)) {
            return "";
        }
        if (status != 200) {
            CSPOT_LOG(error, "login5 HTTP %ld (%d bytes)", status, (int)resp.size());
            return "";
        }

        // Parse LoginResponse: ok=1, error=2, challenges=3, login_context=5.
        std::string accessToken;
        bool hasError = false;
        uint64_t errorCode = 0;
        const uint8_t *challengesData = nullptr;
        size_t challengesLen = 0;
        std::vector<uint8_t> respLoginCtx;

        PbReader r(resp);
        PbField f;
        while (r.next(f)) {
            if (f.field == 1 && f.wire == 2) {  // LoginOk
                PbReader ok(f.data, f.len);
                PbField okf;
                while (ok.next(okf)) {
                    if (okf.field == 2 && okf.wire == 2) {  // access_token
                        accessToken.assign((const char *)okf.data, okf.len);
                    }
                }
            } else if (f.field == 2 && f.wire == 0) {  // error
                hasError = true;
                errorCode = f.val;
            } else if (f.field == 3 && f.wire == 2) {  // challenges
                challengesData = f.data;
                challengesLen = f.len;
            } else if (f.field == 5 && f.wire == 2) {  // login_context
                respLoginCtx.assign(f.data, f.data + f.len);
            }
        }

        if (!accessToken.empty()) {
            CSPOT_LOG(info, "login5: access token acquired (%d bytes)", (int)accessToken.size());
            return accessToken;
        }

        if (challengesData) {
            // Challenges { repeated Challenge=1 { hashcash=1 { prefix=1, length=2 } } }
            loginContext = respLoginCtx;
            challengeSolutions.clear();

            PbReader chs(challengesData, challengesLen);
            PbField cf;
            int solved = 0;
            while (chs.next(cf)) {
                if (cf.field != 1 || cf.wire != 2) continue;  // Challenge
                PbReader ch(cf.data, cf.len);
                PbField chf;
                while (ch.next(chf)) {
                    if (chf.field != 1 || chf.wire != 2) continue;  // hashcash
                    const uint8_t *prefix = nullptr;
                    size_t prefixLen = 0;
                    int length = 0;
                    PbReader hc(chf.data, chf.len);
                    PbField hf;
                    while (hc.next(hf)) {
                        if (hf.field == 1 && hf.wire == 2) {
                            prefix = hf.data;
                            prefixLen = hf.len;
                        } else if (hf.field == 2 && hf.wire == 0) {
                            length = (int)hf.val;
                        }
                    }

                    uint8_t suffix[16];
                    int64_t sec = 0;
                    int32_t nanos = 0;
                    solveHashcash(crypto, loginContext, prefix, prefixLen, length, suffix, &sec, &nanos);
                    CSPOT_LOG(info, "login5: solved hashcash (len=%d) in %llds %dns", length,
                              (long long)sec, nanos);

                    // Duration { seconds=1, nanos=2 }
                    std::string duration;
                    pbVarintField(duration, 1, (uint64_t)sec);
                    pbVarintField(duration, 2, (uint64_t)(uint32_t)nanos);

                    // HashcashSolution { suffix=1, duration=2 }
                    std::string hcSol;
                    pbLenField(hcSol, 1, suffix, 16);
                    pbMsgField(hcSol, 2, duration);

                    // ChallengeSolution { hashcash=1 }
                    std::string sol;
                    pbMsgField(sol, 1, hcSol);

                    // ChallengeSolutions { solutions=1 (repeated) }
                    pbMsgField(challengeSolutions, 1, sol);
                    solved++;
                }
            }

            if (solved == 0) {
                CSPOT_LOG(error, "login5: challenge present but no hashcash to solve");
                return "";
            }
            continue;  // resubmit with solutions
        }

        if (hasError) {
            CSPOT_LOG(error, "login5: login error code %llu", (unsigned long long)errorCode);
            return "";
        }

        CSPOT_LOG(error, "login5: response had neither token, error, nor challenge");
        return "";
    }

    CSPOT_LOG(error, "login5: giving up after retries");
    return "";
}
