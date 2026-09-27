#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Minimal protobuf wire reader: enough to walk the playlist4, metadata and
// extended-metadata messages without generated code.
using PbField = std::pair<const uint8_t*, size_t>;

// Reads one base-128 varint; false on truncation or overflow.
bool pbVarint(const uint8_t **p, const uint8_t *end, uint64_t *v);
// Every length-delimited (wire type 2) field numbered want inside [p, end).
std::vector<PbField> pbLenFields(const uint8_t *p, const uint8_t *end, int want);
std::vector<PbField> pbLenFields(const PbField &msg, int want);
std::string pbString(const PbField &f);
// First varint field numbered want (0 if absent).
uint64_t pbVarintField(const uint8_t *p, const uint8_t *end, int want);
uint64_t pbVarintField(const PbField &msg, int want);
// sint32 fields are zigzag encoded.
int32_t pbZigzag(uint64_t z);

// 16-byte gid to the 22-character base62 id of a Spotify URI ("" if the gid
// is not 16 bytes).
std::string gid_to_base62(const std::string &gid);
std::string bytes_to_hex(const std::string &bytes);
