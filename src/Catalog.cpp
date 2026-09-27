#include "Catalog.h"
#include "Proto.h"
#include <cJSON.h>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

const char *IMAGE_BASE = "https://i.scdn.co/image/";
// Rows show 48 px art: the smallest source at least this wide.
const int ROW_IMAGE_MIN = 60;
const int HEADER_IMAGE_MIN = 300;

// ---------------------------------------------------------------- protobuf

// cover_group=17 { image=1 { file_id=1, size=2 } }: the default size (0,
// about 300 px), else the first image.
std::string coverFromGroup(const PbField &msg) {
    auto group = pbLenFields(msg, 17);
    if (group.empty()) return "";
    std::string fileId;
    for (auto &img : pbLenFields(group[0], 1)) {
        auto id = pbLenFields(img, 1);
        if (id.empty()) continue;
        bool isDefault = pbVarintField(img, 2) == 0;
        if (fileId.empty() || isDefault) fileId = pbString(id[0]);
        if (isDefault) break;
    }
    return fileId.empty() ? std::string() : IMAGE_BASE + bytes_to_hex(fileId);
}

// { gid=1, name=2 } of an artist or album.
Link linkOf(const PbField &msg, const char *prefix) {
    Link l;
    auto gid = pbLenFields(msg, 1);
    auto name = pbLenFields(msg, 2);
    std::string id = gid.empty() ? std::string() : gid_to_base62(pbString(gid[0]));
    if (!id.empty()) l.uri = prefix + id;
    if (!name.empty()) l.name = pbString(name[0]);
    return l;
}

// ---------------------------------------------------------------- JSON

// Dotted path of object keys: "data.searchV2.tracksV2".
cJSON *jget(cJSON *o, const char *path) {
    std::string p(path);
    size_t s = 0;
    while (o != nullptr && s <= p.size()) {
        size_t e = p.find('.', s);
        if (e == std::string::npos) e = p.size();
        o = cJSON_GetObjectItem(o, p.substr(s, e - s).c_str());
        s = e + 1;
    }
    return o;
}

std::string jstr(cJSON *o, const char *path) {
    cJSON *v = jget(o, path);
    return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string();
}

// Numbers, and the counts Spotify sends as strings ("playcount").
int64_t jnum(cJSON *o, const char *path) {
    cJSON *v = jget(o, path);
    if (cJSON_IsNumber(v)) return static_cast<int64_t>(v->valuedouble);
    if (cJSON_IsString(v) && v->valuestring) return strtoll(v->valuestring, nullptr, 10);
    return 0;
}

// sources: [{url, width, height}]. The smallest one at least `want` wide, else
// the largest. Some lists leave the sizes out; their 64 px album cover is
// recognised by its URL.
std::string pickImage(cJSON *sources, int want) {
    std::string best, largest, small, any;
    int bestW = 0, largestW = 0;
    int n = cJSON_IsArray(sources) ? cJSON_GetArraySize(sources) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_GetArrayItem(sources, i);
        std::string url = jstr(s, "url");
        if (url.empty()) continue;
        if (any.empty()) any = url;
        int w = static_cast<int>(jnum(s, "width"));
        if (w >= want && (bestW == 0 || w < bestW)) {
            best = url;
            bestW = w;
        }
        if (w > largestW) {
            largest = url;
            largestW = w;
        }
        if (w == 0 && want <= 64 && url.find("ab67616d00004851") != std::string::npos) small = url;
    }
    if (!best.empty()) return best;
    if (!small.empty()) return small;
    return !largest.empty() ? largest : any;
}

// {items: [{uri, profile: {name}}]}
std::vector<Link> readArtists(cJSON *artists) {
    std::vector<Link> out;
    cJSON *items = cJSON_GetObjectItem(artists, "items");
    int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_GetArrayItem(items, i);
        Link l{jstr(a, "uri"), jstr(a, "profile.name")};
        if (!l.name.empty()) out.push_back(l);
    }
    return out;
}

std::string joinNames(const std::vector<Link> &links) {
    std::string out;
    for (size_t i = 0; i < links.size() && i < 3; i++) {
        if (i) out += ", ";
        out += links[i].name;
    }
    return out;
}

std::string releaseType(const std::string &t) {
    if (t == "SINGLE") return "Single";
    if (t == "EP") return "EP";
    if (t == "COMPILATION") return "Compilation";
    return "Album";
}

CatalogItem trackItem(cJSON *t) {
    CatalogItem it;
    it.kind = CatalogItem::TRACK;
    it.uri = jstr(t, "uri");
    it.name = jstr(t, "name");
    it.durationMs = static_cast<int>(jnum(t, "duration.totalMilliseconds"));
    it.artists = readArtists(cJSON_GetObjectItem(t, "artists"));
    it.album.uri = jstr(t, "albumOfTrack.uri");
    it.album.name = jstr(t, "albumOfTrack.name");
    it.imageUrl = pickImage(jget(t, "albumOfTrack.coverArt.sources"), ROW_IMAGE_MIN);
    it.playable = !cJSON_IsFalse(jget(t, "playability.playable"));
    it.subtitle = joinNames(it.artists);
    return it;
}

CatalogItem artistItem(cJSON *a) {
    CatalogItem it;
    it.kind = CatalogItem::ARTIST;
    it.uri = jstr(a, "uri");
    it.name = jstr(a, "profile.name");
    it.imageUrl = pickImage(jget(a, "visuals.avatarImage.sources"), ROW_IMAGE_MIN);
    it.subtitle = "Artist";
    return it;
}

// An album of search results ("Daft Punk, 2001") or of an artist page
// ("Album, 2001"; "The Weeknd, 2016" for an appearance).
CatalogItem albumItem(cJSON *a, bool byArtist) {
    CatalogItem it;
    it.kind = CatalogItem::ALBUM;
    it.uri = jstr(a, "uri");
    it.name = jstr(a, "name");
    it.artists = readArtists(cJSON_GetObjectItem(a, "artists"));
    it.imageUrl = pickImage(jget(a, "coverArt.sources"), ROW_IMAGE_MIN);
    int64_t year = jnum(a, "date.year");
    std::string first = byArtist && !it.artists.empty() ? joinNames(it.artists)
                                                        : releaseType(jstr(a, "type"));
    it.subtitle = year > 0 ? first + ", " + std::to_string(year) : first;
    return it;
}

CatalogItem playlistItem(cJSON *p) {
    CatalogItem it;
    it.kind = CatalogItem::PLAYLIST;
    it.uri = jstr(p, "uri");
    it.name = jstr(p, "name");
    cJSON *first = cJSON_GetArrayItem(jget(p, "images.items"), 0);
    it.imageUrl = pickImage(cJSON_GetObjectItem(first, "sources"), ROW_IMAGE_MIN);
    std::string owner = jstr(p, "ownerV2.data.name");
    it.subtitle = owner.empty() ? std::string("Playlist") : "By " + owner;
    return it;
}

void addItem(ItemList *out, const CatalogItem &it) {
    if (!it.uri.empty() && !it.name.empty()) out->items.push_back(it);
}

// {totalCount, items: [{releases: {items: [release]}}]}
bool readReleases(cJSON *list, bool byArtist, ItemList *out) {
    cJSON *items = cJSON_GetObjectItem(list, "items");
    if (!cJSON_IsArray(items)) return false;
    out->total = static_cast<int>(jnum(list, "totalCount"));
    int n = cJSON_GetArraySize(items);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(jget(cJSON_GetArrayItem(items, i), "releases.items"), 0);
        if (r != nullptr) addItem(out, albumItem(r, byArtist));
    }
    return true;
}

const char *releasesPath(ArtistSection s) {
    switch (s) {
        case AS_ALBUMS: return "data.artistUnion.discography.albums";
        case AS_SINGLES: return "data.artistUnion.discography.singles";
        case AS_COMPILATIONS: return "data.artistUnion.discography.compilations";
        case AS_APPEARS_ON: return "data.artistUnion.relatedContent.appearsOn";
        default: return nullptr;
    }
}

}  // namespace

// BatchedExtensionResponse: extended_metadata=2 { extension_data=3 {
// header=1, entity_uri=2, extension_data=3 (Any) { value=2 } } }.
std::map<std::string, std::string> parseExtendedEntities(const std::string &body) {
    std::map<std::string, std::string> out;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(body.data());
    for (auto &arr : pbLenFields(data, data + body.size(), 2)) {
        for (auto &d : pbLenFields(arr, 3)) {
            auto uri = pbLenFields(d, 2);
            auto any = pbLenFields(d, 3);
            if (uri.empty() || any.empty()) continue;
            auto value = pbLenFields(any[0], 2);
            if (!value.empty()) out[pbString(uri[0])] = pbString(value[0]);
        }
    }
    return out;
}

// metadata Track: name=2, album=3 { gid=1, name=2, cover_group=17 },
// artist=4 { gid=1, name=2 }, duration=7 (sint32).
TrackMeta parseTrackMeta(const std::string &msg) {
    TrackMeta m;
    PbField all(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    auto n = pbLenFields(all, 2);
    if (!n.empty()) m.name = pbString(n[0]);
    auto album = pbLenFields(all, 3);
    if (!album.empty()) {
        m.albumLink = linkOf(album[0], "spotify:album:");
        m.album = m.albumLink.name;
        m.coverUrl = coverFromGroup(album[0]);
    }
    for (auto &a : pbLenFields(all, 4)) {
        Link l = linkOf(a, "spotify:artist:");
        if (l.name.empty()) continue;
        if (!m.artist.empty()) m.artist += ", ";
        m.artist += l.name;
        m.artists.push_back(l);
    }
    m.durationMs = pbZigzag(pbVarintField(all, 7));
    return m;
}

void parseExtendedMetadata(const std::string &body, std::map<std::string, TrackMeta> *out) {
    for (const auto &e : parseExtendedEntities(body)) {
        (*out)[e.first] = parseTrackMeta(e.second);
    }
}

// metadata Album: name=2, artist=3 { gid=1, name=2 }, type=4 (1 album,
// 2 single, 3 compilation, 4 EP), date=6 { year=1 (sint32) },
// disc=11 { track=3 { gid=1 } }, cover_group=17.
bool parseAlbumMeta(const std::string &msg, AlbumMeta *out) {
    PbField all(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    auto n = pbLenFields(all, 2);
    if (n.empty()) return false;
    out->name = pbString(n[0]);
    for (auto &a : pbLenFields(all, 3)) {
        Link l = linkOf(a, "spotify:artist:");
        if (!l.name.empty()) out->artists.push_back(l);
    }
    static const char *kTypes[] = {"Album", "Album", "Single", "Compilation", "EP"};
    uint64_t type = pbVarintField(all, 4);
    out->type = type < 5 ? kTypes[type] : "Album";
    auto date = pbLenFields(all, 6);
    if (!date.empty()) out->year = pbZigzag(pbVarintField(date[0], 1));
    out->coverUrl = coverFromGroup(all);
    for (auto &disc : pbLenFields(all, 11)) {
        for (auto &t : pbLenFields(disc, 3)) {
            auto gid = pbLenFields(t, 1);
            std::string id = gid.empty() ? std::string() : gid_to_base62(pbString(gid[0]));
            if (!id.empty()) out->trackUris.push_back("spotify:track:" + id);
        }
    }
    return true;
}

bool parseSearch(SearchKind kind, const std::string &json, ItemList *out) {
    static const char *kPaths[SEARCH_KINDS] = {
        "data.searchV2.tracksV2", "data.searchV2.artists", "data.searchV2.albumsV2",
        "data.searchV2.playlists",
    };
    cJSON *root = cJSON_Parse(json.c_str());
    cJSON *list = jget(root, kPaths[kind]);
    cJSON *items = cJSON_GetObjectItem(list, "items");
    bool ok = cJSON_IsArray(items);
    if (ok) out->total = static_cast<int>(jnum(list, "totalCount"));
    int n = ok ? cJSON_GetArraySize(items) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(items, i);
        switch (kind) {
            case SEARCH_TRACKS: addItem(out, trackItem(jget(it, "item.data"))); break;
            case SEARCH_ARTISTS: addItem(out, artistItem(jget(it, "data"))); break;
            case SEARCH_ALBUMS: addItem(out, albumItem(jget(it, "data"), true)); break;
            default: addItem(out, playlistItem(jget(it, "data"))); break;
        }
    }
    cJSON_Delete(root);
    return ok;
}

// queryArtistOverview: data.artistUnion { uri, profile.name,
// visuals.avatarImage, stats.monthlyListeners, discography { topTracks,
// albums, singles, compilations }, relatedContent { appearsOn, featuringV2,
// relatedArtists } }.
bool parseArtistOverview(const std::string &json, ArtistInfo *out) {
    cJSON *root = cJSON_Parse(json.c_str());
    cJSON *a = jget(root, "data.artistUnion");
    out->name = jstr(a, "profile.name");
    bool ok = !out->name.empty();
    if (ok) {
        out->uri = jstr(a, "uri");
        out->imageUrl = pickImage(jget(a, "visuals.avatarImage.sources"), HEADER_IMAGE_MIN);
        out->monthlyListeners = jnum(a, "stats.monthlyListeners");

        cJSON *top = jget(a, "discography.topTracks.items");
        int n = cJSON_IsArray(top) ? cJSON_GetArraySize(top) : 0;
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(top, i), "track");
            CatalogItem it = trackItem(t);
            int64_t plays = jnum(t, "playcount");
            if (plays > 0) it.subtitle = groupDigits(plays) + " plays";
            addItem(&out->sections[AS_POPULAR], it);
        }
        out->sections[AS_POPULAR].total = static_cast<int>(out->sections[AS_POPULAR].items.size());

        for (int s = AS_ALBUMS; s <= AS_APPEARS_ON; s++) {
            std::string path = releasesPath(static_cast<ArtistSection>(s));
            readReleases(jget(root, path.c_str()), s == AS_APPEARS_ON, &out->sections[s]);
        }

        cJSON *pl = jget(a, "relatedContent.featuringV2");
        cJSON *items = cJSON_GetObjectItem(pl, "items");
        n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
        for (int i = 0; i < n; i++) {
            addItem(&out->sections[AS_PLAYLISTS], playlistItem(jget(cJSON_GetArrayItem(items, i), "data")));
        }
        out->sections[AS_PLAYLISTS].total = static_cast<int>(out->sections[AS_PLAYLISTS].items.size());

        cJSON *rel = jget(a, "relatedContent.relatedArtists.items");
        n = cJSON_IsArray(rel) ? cJSON_GetArraySize(rel) : 0;
        for (int i = 0; i < n; i++) addItem(&out->sections[AS_RELATED], artistItem(cJSON_GetArrayItem(rel, i)));
        out->sections[AS_RELATED].total = static_cast<int>(out->sections[AS_RELATED].items.size());
    }
    cJSON_Delete(root);
    return ok;
}

bool parseReleases(ArtistSection section, const std::string &json, ItemList *out) {
    const char *path = releasesPath(section);
    if (path == nullptr) return false;
    cJSON *root = cJSON_Parse(json.c_str());
    bool ok = readReleases(jget(root, path), section == AS_APPEARS_ON, out);
    cJSON_Delete(root);
    return ok;
}

std::string groupDigits(int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return n < 0 ? "-" + out : out;
}
