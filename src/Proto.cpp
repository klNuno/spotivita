#include "Proto.h"
#include <cstring>
#include <string>
#include <vector>

bool pbVarint(const uint8_t **p, const uint8_t *end, uint64_t *v) {
    *v = 0;
    for (int sh = 0; *p < end && sh < 64; sh += 7) {
        uint8_t b = *(*p)++;
        *v |= static_cast<uint64_t>(b & 0x7F) << sh;
        if (!(b & 0x80)) {
            return true;
        }
    }
    return false;
}

std::vector<PbField> pbLenFields(const uint8_t *p, const uint8_t *end, int want) {
    std::vector<PbField> out;
    uint64_t key = 0;
    while (p < end && pbVarint(&p, end, &key)) {
        int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        uint64_t len = 0;
        if (wt == 2) {
            if (!pbVarint(&p, end, &len) || len > static_cast<uint64_t>(end - p)) {
                break;
            }
            if (field == want) {
                out.push_back({p, static_cast<size_t>(len)});
            }
        } else if (wt == 0) {
            if (!pbVarint(&p, end, &len)) {
                break;
            }
            len = 0;
        } else if (wt == 5 || wt == 1) {
            len = wt == 5 ? 4 : 8;
            if (static_cast<uint64_t>(end - p) < len) {
                break;
            }
        } else {
            break;
        }
        p += len;
    }
    return out;
}

std::vector<PbField> pbLenFields(const PbField &msg, int want) {
    return pbLenFields(msg.first, msg.first + msg.second, want);
}

std::string pbString(const PbField &f) {
    return std::string(reinterpret_cast<const char*>(f.first), f.second);
}

uint64_t pbVarintField(const uint8_t *p, const uint8_t *end, int want) {
    uint64_t key = 0;
    while (p < end && pbVarint(&p, end, &key)) {
        int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
        uint64_t v = 0;
        if (wt == 0) {
            if (!pbVarint(&p, end, &v)) break;
            if (field == want) return v;
        } else if (wt == 2) {
            if (!pbVarint(&p, end, &v) || v > static_cast<uint64_t>(end - p)) break;
            p += v;
        } else if (wt == 5 || wt == 1) {
            size_t len = wt == 5 ? 4 : 8;
            if (static_cast<size_t>(end - p) < len) break;
            p += len;
        } else {
            break;
        }
    }
    return 0;
}

uint64_t pbVarintField(const PbField &msg, int want) {
    return pbVarintField(msg.first, msg.first + msg.second, want);
}

int32_t pbZigzag(uint64_t z) {
    return static_cast<int32_t>((z >> 1) ^ (~(z & 1) + 1));
}

// The gid is a big-endian 128-bit number; its base62 form has 22 digits.
std::string gid_to_base62(const std::string &gid) {
    static const char *digits = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (gid.size() != 16) return "";
    uint8_t n[16];
    memcpy(n, gid.data(), sizeof(n));
    char out[22];
    for (int k = 21; k >= 0; k--) {
        unsigned rem = 0;
        for (int i = 0; i < 16; i++) {
            unsigned v = (rem << 8) | n[i];
            n[i] = static_cast<uint8_t>(v / 62);
            rem = v % 62;
        }
        out[k] = digits[rem];
    }
    return std::string(out, sizeof(out));
}

std::string bytes_to_hex(const std::string &bytes) {
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += hex[c >> 4];
        out += hex[c & 0x0F];
    }
    return out;
}
