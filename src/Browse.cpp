// PlaybackScreen, the catalog part: page navigation, search, artist pages,
// the track menu and the links of the track playing.
#include "PlaybackScreen.h"
#include "Font.h"
#include "Gui.h"
#include "GuiUtils.h"
#include "Keyboard.h"
#include "ScreenUtil.h"
#include "Widgets.h"
#include <imgui_vita2d/imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

// Pages a stack keeps; the oldest go first past it.
const size_t MAX_PAGES = 30;
// Albums and playlists opened from the catalog kept in memory with their
// tracks. The oldest no page shows go first.
const size_t MAX_EXTERNAL = 24;
const size_t MAX_ARTISTS = 16;
const int SEARCH_PAGE = 20;
const int RELEASES_PAGE = 50;
// Rows of each category on the "All" results and on an artist page.
const size_t ALL_TRACKS = 4;
const size_t ALL_OTHERS = 3;
const size_t ARTIST_POPULAR = 5;
const size_t ARTIST_OTHERS = 4;
const float MORE_W = 40.0f;

const char *const kChipKeys[SEARCH_KINDS] = {"songs", "artists", "albums", "playlists"};
const char *const kChipNames[SEARCH_KINDS] = {"Songs", "Artists", "Albums", "Playlists"};
const char *const kSectionKeys[AS_COUNT] = {
    "popular", "albums", "singles", "compilations", "appears", "playlists", "related",
};
const char *const kSectionTitles[AS_COUNT] = {
    "Popular", "Albums", "Singles and EPs", "Compilations", "Appears on", "Featuring", "Fans also like",
};

std::string lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string joinNames(const std::vector<Link> &links) {
    std::string out;
    for (const auto &l : links) {
        if (l.name.empty()) continue;
        if (!out.empty()) out += ", ";
        out += l.name;
    }
    return out;
}

void appendNew(ItemList *list, const ItemList &page) {
    std::set<std::string> have;
    for (const auto &it : list->items) have.insert(it.uri);
    for (const auto &it : page.items) {
        if (have.insert(it.uri).second) list->items.push_back(it);
    }
    if (page.total > 0) list->total = page.total;
    // An empty page: Spotify has nothing past what is shown.
    if (page.items.empty()) list->total = static_cast<int>(list->items.size());
}

// Bold title with an optional "See all" at the right end. True on a tap of it.
bool sectionHeader(ImFont *bold, ImFont *small, const std::string &title, const std::string &id,
                   bool more, float avail) {
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    float x0 = ImGui::GetCursorPosX();
    ImGui::PushFont(bold);
    ImGui::TextUnformatted(fitText(bold, title, avail - (more ? 120.0f : 0.0f)).c_str());
    ImGui::PopFont();
    if (!more) return false;
    ImGui::PushFont(small);
    const char *label = "See all";
    float w = ImGui::CalcTextSize(label).x + 28.0f;
    ImGui::SameLine(x0 + avail - w);
    bool tap = pillButton((std::string(label) + id).c_str(), ImVec2(w, 34.0f), COL_CARD, COL_WHITE);
    ImGui::PopFont();
    return tap;
}

std::string pageKey(const Page &p) {
    static const char *kKinds[] = {"list", "artist", "section"};
    std::string out = std::string(kKinds[p.kind]) + ":" + p.uri;
    if (p.kind == Page::SECTION) out += ":" + std::string(kSectionKeys[p.section]);
    return out;
}

std::string linksJson(const TrackLinks &l) {
    std::string out = "{\"uri\":" + json_quote(l.uri) + ",\"state\":" + json_quote(stateName(l.state)) +
                      ",\"name\":" + json_quote(l.name) + ",\"album\":" + json_quote(l.album.name) +
                      ",\"album_uri\":" + json_quote(l.album.uri) + ",\"artists\":[";
    for (size_t k = 0; k < l.artists.size(); k++) {
        if (k) out += ",";
        out += json_quote(l.artists[k].name + " " + l.artists[k].uri);
    }
    return out + "]}";
}

// "n/total: first, second, ..." for the debug state.
std::string listJson(const ItemList &l, size_t names) {
    std::string out = "{\"n\":" + std::to_string(l.items.size()) + ",\"total\":" + std::to_string(l.total) +
                      ",\"first\":[";
    for (size_t k = 0; k < l.items.size() && k < names; k++) {
        if (k) out += ",";
        out += json_quote(l.items[k].name + (l.items[k].subtitle.empty() ? "" : " - " + l.items[k].subtitle));
    }
    return out + "]}";
}

}  // namespace

// ---------------------------------------------------------------- pages

std::vector<Page> *PlaybackScreen::pageStack() {
    if (tab == Tab::LIBRARY) return &pages[0];
    if (tab == Tab::SEARCH) return &pages[1];
    return nullptr;
}

const Page *PlaybackScreen::topPage() {
    std::vector<Page> *s = pageStack();
    return s != nullptr && !s->empty() ? &s->back() : nullptr;
}

void PlaybackScreen::pushPage(const Page &page) {
    // Settings and the log open pages over the library.
    if (pageStack() == nullptr) tab = Tab::LIBRARY;
    std::vector<Page> *s = pageStack();
    if (!s->empty()) {
        const Page &t = s->back();
        if (t.kind == page.kind && t.uri == page.uri && t.section == page.section) return;
    }
    if (s->size() >= MAX_PAGES) s->erase(s->begin());
    s->push_back(page);
    scrollToRow = -1;
    syncOpen();
}

void PlaybackScreen::clearPages(int which) {
    bool hadList = false;
    for (const auto &p : pages[which]) hadList |= p.kind == Page::LIST;
    pages[which].clear();
    if (hadList) cancelTrackLoads();
    syncOpen();
}

void PlaybackScreen::syncOpen() {
    std::vector<Page> *s = pageStack();
    // A list the library refresh removed: its page goes too.
    while (s != nullptr && !s->empty() && s->back().kind == Page::LIST &&
           findPlaylist(playlists, s->back().uri) < 0) {
        s->pop_back();
    }
    const Page *top = topPage();
    int idx = top != nullptr && top->kind == Page::LIST ? findPlaylist(playlists, top->uri) : -1;
    if (idx != openIndex) {
        openIndex = idx;
        viewDirty = true;
    }
    if (idx >= 0 && playlists[idx].tracksState == LoadState::NONE) ensureTracks(idx);
    if (top != nullptr && top->kind != Page::LIST) {
        auto it = artists.find(top->uri);
        if (it == artists.end() || it->second.state == LoadState::NONE) loadArtist(top->uri);
    }
}

void PlaybackScreen::openList(const std::string &uri, const std::string &name,
                              const std::string &imageUrl, const std::string &subtitle) {
    if (uri.empty()) return;
    if (findPlaylist(playlists, uri) < 0) {
        pruneExternal();
        Playlist p;
        p.uri = uri;
        p.name = name.empty() ? std::string("...") : name;
        p.external = true;
        p.imageUrl = imageUrl;
        p.subtitle = subtitle;
        auto pref = sortPrefs.find(uri);
        if (pref != sortPrefs.end()) {
            p.sort = pref->second.first;
            p.sortDesc = pref->second.second;
        }
        playlists.push_back(std::move(p));
    }
    Page page;
    page.kind = Page::LIST;
    page.uri = uri;
    pushPage(page);
}

void PlaybackScreen::openArtist(const std::string &uri) {
    if (!startsWith(uri, "spotify:artist:")) return;
    Page page;
    page.kind = Page::ARTIST;
    page.uri = uri;
    pushPage(page);
}

void PlaybackScreen::openSection(const std::string &artistUri, int section) {
    if (section < 0 || section >= AS_COUNT) return;
    Page page;
    page.kind = Page::SECTION;
    page.uri = artistUri;
    page.section = section;
    pushPage(page);
}

void PlaybackScreen::openItem(const CatalogItem &item) {
    CatalogItem it = item;   // the page change may drop the list it came from
    switch (it.kind) {
        case CatalogItem::ARTIST:
            openArtist(it.uri);
            break;
        case CatalogItem::ALBUM:
        case CatalogItem::PLAYLIST: {
            openList(it.uri, it.name, it.imageUrl, it.subtitle);
            int i = findPlaylist(playlists, it.uri);
            if (i >= 0 && playlists[i].external && playlists[i].artists.empty()) {
                playlists[i].artists = it.artists;
            }
            break;
        }
        case CatalogItem::TRACK:
            break;
    }
}

// External lists sit after the library's, oldest first.
void PlaybackScreen::pruneExternal() {
    size_t count = 0;
    for (const auto &p : playlists) count += p.external ? 1 : 0;
    if (count < MAX_EXTERNAL) return;
    std::set<std::string> shown;
    for (const auto &s : pages) {
        for (const auto &pg : s) {
            if (pg.kind == Page::LIST) shown.insert(pg.uri);
        }
    }
    std::string openUri = openIndex >= 0 && openIndex < static_cast<int>(playlists.size())
                              ? playlists[openIndex].uri : "";
    for (size_t i = 0; i < playlists.size() && count >= MAX_EXTERNAL;) {
        const Playlist &p = playlists[i];
        if (p.external && shown.count(p.uri) == 0 && p.tracksState != LoadState::LOADING) {
            playlists.erase(playlists.begin() + static_cast<std::ptrdiff_t>(i));
            count--;
        } else {
            i++;
        }
    }
    openIndex = openUri.empty() ? -1 : findPlaylist(playlists, openUri);
    viewDirty = true;
}

// ---------------------------------------------------------------- search

void PlaybackScreen::startSearch(const std::string &query) {
    searchQuery = query;
    int gen = ++searchGen;
    searchChip = -1;
    clearPages(1);
    for (int k = 0; k < SEARCH_KINDS; k++) {
        searchLists[k] = ItemList();
        searchStates[k] = LoadState::LOADING;
        searchMore[k] = false;
    }
    // Songs first: they lead the results. Each category shows as it arrives.
    GUI *g = gui;
    gui->net.post([this, g, gen, query] {
        for (int k = 0; k < SEARCH_KINDS; k++) {
            if (gen != searchGen) return;
            ApiResult r = g->api.search(k, query, 0, SEARCH_PAGE);
            auto list = std::make_shared<ItemList>();
            bool ok = r.ok() && parseSearch(static_cast<SearchKind>(k), r.body, list.get());
            long status = r.ok() ? -1 : r.status;
            g->net.deliver([this, gen, k, list, ok, status] {
                if (gen != searchGen) return;
                if (ok) {
                    searchLists[k] = std::move(*list);
                    searchStates[k] = LoadState::LOADED;
                    return;
                }
                bool first = true;
                for (int j = 0; j < SEARCH_KINDS; j++) first &= searchStates[j] != LoadState::FAILED;
                searchStates[k] = LoadState::FAILED;
                if (first) gui->toast(describeStatus(status));
            });
        }
    }, true);
}

void PlaybackScreen::loadSearch(int kind, int offset) {
    if (kind < 0 || kind >= SEARCH_KINDS || searchMore[kind] || searchQuery.empty()) return;
    searchMore[kind] = true;
    int gen = searchGen;
    std::string query = searchQuery;
    GUI *g = gui;
    gui->net.post([this, g, gen, kind, offset, query] {
        if (gen != searchGen) return;
        ApiResult r = g->api.search(kind, query, offset, SEARCH_PAGE);
        auto page = std::make_shared<ItemList>();
        bool ok = r.ok() && parseSearch(static_cast<SearchKind>(kind), r.body, page.get());
        long status = r.ok() ? -1 : r.status;
        g->net.deliver([this, gen, kind, page, ok, status] {
            if (gen != searchGen) return;
            searchMore[kind] = false;
            if (!ok) {
                gui->toast(describeStatus(status));
                return;
            }
            appendNew(&searchLists[kind], *page);
        });
    }, true);
}

void PlaybackScreen::activateSearch(int kind, size_t index) {
    if (kind < 0 || kind >= SEARCH_KINDS) return;
    const std::vector<CatalogItem> &items = searchLists[kind].items;
    if (index >= items.size()) return;
    if (kind == SEARCH_TRACKS) {
        playItems(items, index, "", false);
    } else {
        openItem(items[index]);
    }
}

void PlaybackScreen::drawSearch(const PlayerModel::Snapshot &snap, float avail) {
    ImGui::PushFont(gui->font_bold);
    ImGui::TextUnformatted("Search");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    std::string label = searchQuery.empty() ? std::string("Songs, artists, albums, playlists") : searchQuery;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.05f, 0.5f));
    bool open = pillButton((fitText(ImGui::GetFont(), label, avail - 40.0f) + "##q").c_str(),
                           ImVec2(avail, 46.0f), COL_CARD, searchQuery.empty() ? COL_GREY : COL_WHITE);
    ImGui::PopStyleVar();
    if (open) {
        Keyboard::Open("Search Spotify", searchQuery, [this](const std::string &q) {
            if (!q.empty()) startSearch(q);
        });
    }
    if (searchQuery.empty()) return;
    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    // Category chips.
    ImGui::PushFont(gui->log_font);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
    if (chipButton("All##chip", searchChip < 0, 40.0f)) searchChip = -1;
    for (int k = 0; k < SEARCH_KINDS; k++) {
        ImGui::SameLine();
        std::string id = std::string(kChipNames[k]) + "##chip" + std::to_string(k);
        if (chipButton(id.c_str(), searchChip == k, 40.0f)) searchChip = k;
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();

    if (searchChip < 0) {
        drawSearchAll(snap.uri, avail);
    } else {
        drawSearchKind(searchChip, snap.uri, avail);
    }
}

void PlaybackScreen::drawSearchAll(const std::string &playing, float avail) {
    int order[SEARCH_KINDS] = {SEARCH_TRACKS, SEARCH_ARTISTS, SEARCH_ALBUMS, SEARCH_PLAYLISTS};
    // An artist named like the query leads, as Spotify's top result does.
    const auto &found = searchLists[SEARCH_ARTISTS].items;
    if (!found.empty() && lower(found[0].name) == lower(searchQuery)) {
        order[0] = SEARCH_ARTISTS;
        order[1] = SEARCH_TRACKS;
    }
    // Taps act once the page is drawn: a page cut short for a frame would
    // clamp its scroll.
    int seeAll = -1, pickKind = -1;
    size_t pick = 0;
    bool loading = false, shown = false, failed = false;
    for (int k : order) {
        loading |= searchStates[k] == LoadState::LOADING;
        failed |= searchStates[k] == LoadState::FAILED;
        if (searchStates[k] != LoadState::LOADED) continue;
        const std::vector<CatalogItem> &items = searchLists[k].items;
        if (items.empty()) continue;
        shown = true;
        size_t count = k == SEARCH_TRACKS ? ALL_TRACKS : ALL_OTHERS;
        bool more = items.size() > count || searchLists[k].total > static_cast<int>(items.size());
        if (sectionHeader(gui->font_bold, gui->log_font, kChipNames[k], "##all" + std::to_string(k),
                          more, avail)) {
            seeAll = k;
        }
        for (size_t i = 0; i < items.size() && i < count; i++) {
            std::string id = "##s" + std::to_string(k) + "_" + std::to_string(i);
            bool current = k == SEARCH_TRACKS && !playing.empty() && items[i].uri == playing;
            if (drawItem(items[i], id, avail, current)) {
                pickKind = k;
                pick = i;
            }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (loading) {
        Spinner("Searching...");
    } else if (!shown) {
        greyText(failed ? "Search failed. Tap the field to try again." : "No results.");
    }
    if (seeAll >= 0) searchChip = seeAll;
    if (pickKind >= 0) activateSearch(pickKind, pick);
}

void PlaybackScreen::drawSearchKind(int kind, const std::string &playing, float avail) {
    const ItemList &l = searchLists[kind];
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (searchStates[kind] == LoadState::LOADING) {
        Spinner("Searching...");
        return;
    }
    if (l.items.empty()) {
        if (searchStates[kind] == LoadState::FAILED) {
            greyText("Search failed.");
            if (pillButton("Try again##search", ImVec2(160.0f, 44.0f), COL_WHITE, COL_DARK)) {
                startSearch(searchQuery);
                searchChip = kind;
            }
        } else {
            greyText("No results.");
        }
        return;
    }
    size_t pick = SIZE_MAX;
    for (size_t i = 0; i < l.items.size(); i++) {
        std::string id = "##k" + std::to_string(kind) + "_" + std::to_string(i);
        bool current = kind == SEARCH_TRACKS && !playing.empty() && l.items[i].uri == playing;
        if (drawItem(l.items[i], id, avail, current)) pick = i;
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (searchMore[kind]) {
        Spinner("");
    } else if (static_cast<int>(l.items.size()) < l.total) {
        if (pillButton("Load more##search", ImVec2(180.0f, 44.0f), COL_CARD, COL_WHITE)) {
            loadSearch(kind, static_cast<int>(l.items.size()));
        }
    }
    if (pick != SIZE_MAX) activateSearch(kind, pick);
}

// ---------------------------------------------------------------- rows

bool PlaybackScreen::drawItem(const CatalogItem &it, const std::string &id, float width, bool current) {
    static const RowArt kTrackArt = {ICON_FA_MUSIC, COL_CARD, COL_GREY};
    static const RowArt kArtistArt = {ICON_FA_USER, COL_CARD, COL_GREY};
    static const RowArt kAlbumArt = {ICON_FA_COMPACT_DISC, COL_CARD, COL_GREY};
    const RowArt *art = it.kind == CatalogItem::ARTIST ? &kArtistArt
                      : it.kind == CatalogItem::ALBUM ? &kAlbumArt : &kTrackArt;
    bool isTrack = it.kind == CatalogItem::TRACK;
    float rowW = isTrack ? width - MORE_W - 8.0f : width;
    ImTextureID thumb = nullptr;
    // Only rows on screen fetch their cover.
    if (!it.imageUrl.empty() && ImGui::IsRectVisible(ImVec2(rowW, LIST_ROW_H))) {
        vita2d_texture *t = thumbs.get(it.imageUrl, 96);
        if (t != nullptr) thumb = Render::tex_id(t);
    }
    ImU32 fg = isTrack && !it.playable ? COL_DIM : (current ? COL_GREENV : COL_WHITE);
    std::string right = isTrack && it.durationMs > 0 ? fmtTime(it.durationMs) : std::string();
    bool tapped = listRow(id.c_str(), it.name, it.subtitle, rowW, fg, gui->log_font, art,
                          gui->small_icon_font, right, thumb, it.kind == CatalogItem::ARTIST);
    if (!isTrack) return tapped;
    bool focused = ImGui::IsItemFocused();
    ImGui::SameLine(0.0f, 8.0f);
    bool more = moreButton(id);
    if (focused || more) {
        std::string uri = it.uri, name = it.name, artist = joinNames(it.artists);
        Link album = it.album;
        std::vector<Link> links = it.artists;
        auto open = [this, uri, name, artist, album, links] { openTrackMenu(uri, name, artist, album, links); };
        if (more) open();
        if (focused) focusedMenu = open;
    }
    return tapped;
}

bool PlaybackScreen::moreButton(const std::string &id) {
    ImGui::PushFont(gui->small_icon_font);
    bool r = iconButton((std::string(ICON_FA_ELLIPSIS_V) + "##more" + id).c_str(), ImVec2(MORE_W, LIST_ROW_H),
                        COL_GREY, COL_CLEAR);
    ImGui::PopFont();
    return r;
}

// Cover, "Album, 2001" or "By Spotify", and a button per artist.
void PlaybackScreen::drawListHeader(const Playlist &pl, float avail) {
    const float side = 112.0f;
    ImVec2 c = ImGui::GetCursorPos();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    vita2d_texture *tex = thumbs.get(pl.imageUrl, 160);
    if (tex != nullptr) {
        dl->AddImageRounded(Render::tex_id(tex), p, ImVec2(p.x + side, p.y + side), ImVec2(0, 0), ImVec2(1, 1),
                            COL_WHITE, 6.0f);
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), COL_CARD, 6.0f);
        ImFont *icon = gui->small_icon_font;
        const char *glyph = startsWith(pl.uri, "spotify:album:") ? ICON_FA_COMPACT_DISC : ICON_FA_MUSIC;
        ImVec2 isz = icon->CalcTextSizeA(icon->FontSize, FLT_MAX, 0.0f, glyph);
        dl->AddText(icon, icon->FontSize, ImVec2(p.x + (side - isz.x) * 0.5f, p.y + (side - isz.y) * 0.5f),
                    COL_GREY, glyph);
    }
    float tx = c.x + side + 14.0f, tw = avail - side - 14.0f;
    ImGui::SetCursorPos(ImVec2(tx, c.y + 4.0f));
    ImGui::PushFont(gui->log_font);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    ImGui::TextUnformatted(fitText(gui->log_font, pl.subtitle.empty() ? std::string(" ") : pl.subtitle, tw).c_str());
    ImGui::PopStyleColor();
    for (size_t k = 0; k < pl.artists.size() && k < 3; k++) {
        const Link &a = pl.artists[k];
        if (a.uri.empty()) continue;
        ImGui::SetCursorPosX(tx);
        std::string label = fitText(gui->log_font, a.name, tw - 32.0f) + "##hdr" + std::to_string(k);
        if (pillButton(label.c_str(), ImVec2(0.0f, 36.0f), COL_CARD, COL_WHITE)) {
            std::string uri = a.uri;
            ImGui::PopFont();
            openArtist(uri);
            return;
        }
    }
    ImGui::PopFont();
    float end = std::max(c.y + side, ImGui::GetCursorPosY());
    ImGui::SetCursorPos(ImVec2(c.x, end + 8.0f));
}

// ---------------------------------------------------------------- artists

void PlaybackScreen::loadArtist(const std::string &uri) {
    if (artists.size() >= MAX_ARTISTS && artists.find(uri) == artists.end()) {
        std::set<std::string> shown;
        for (const auto &s : pages) {
            for (const auto &pg : s) {
                if (pg.kind != Page::LIST) shown.insert(pg.uri);
            }
        }
        for (auto it = artists.begin(); it != artists.end();) {
            if (shown.count(it->first) == 0 && it->second.state != LoadState::LOADING) {
                it = artists.erase(it);
            } else {
                ++it;
            }
        }
    }
    ArtistView &v = artists[uri];
    if (v.state == LoadState::LOADING || v.state == LoadState::LOADED) return;
    v.state = LoadState::LOADING;
    GUI *g = gui;
    gui->net.post([this, g, uri] {
        ApiResult r = g->api.artist_overview(uri);
        auto info = std::make_shared<ArtistInfo>();
        bool ok = r.ok() && parseArtistOverview(r.body, info.get());
        long status = r.ok() ? -1 : r.status;
        g->net.deliver([this, uri, info, ok, status] {
            ArtistView &v = artists[uri];
            if (!ok) {
                v.state = LoadState::FAILED;
                gui->toast(describeStatus(status));
                return;
            }
            v.info = std::move(*info);
            v.info.uri = uri;
            v.state = LoadState::LOADED;
            for (int s = 0; s < AS_COUNT; s++) {
                v.paged[s] = false;
                v.loadingMore[s] = false;
            }
        });
    }, true);
}

void PlaybackScreen::loadReleases(const std::string &uri, int section) {
    if (section < AS_ALBUMS || section > AS_APPEARS_ON) return;
    auto found = artists.find(uri);
    if (found == artists.end() || found->second.state != LoadState::LOADED) return;
    ArtistView &v = found->second;
    if (v.loadingMore[section]) return;
    v.loadingMore[section] = true;
    bool first = !v.paged[section];
    int offset = first ? 0 : static_cast<int>(v.info.sections[section].items.size());
    GUI *g = gui;
    gui->net.post([this, g, uri, section, offset, first] {
        ApiResult r = g->api.artist_releases(uri, section, offset, RELEASES_PAGE);
        auto page = std::make_shared<ItemList>();
        bool ok = r.ok() && parseReleases(static_cast<ArtistSection>(section), r.body, page.get());
        long status = r.ok() ? -1 : r.status;
        g->net.deliver([this, uri, section, first, page, ok, status] {
            auto it = artists.find(uri);
            if (it == artists.end()) return;
            ArtistView &v = it->second;
            v.loadingMore[section] = false;
            // A failed first page keeps the overview's items; "Load more"
            // continues from them.
            v.paged[section] = true;
            if (!ok) {
                gui->toast(describeStatus(status));
                return;
            }
            ItemList &l = v.info.sections[section];
            if (first) l.items.clear();
            appendNew(&l, *page);
        });
    }, true);
}

void PlaybackScreen::drawArtist(const std::string &uri, const std::string &playing, float avail) {
    auto found = artists.find(uri);
    ArtistView *v = found == artists.end() ? nullptr : &found->second;
    std::string name = v != nullptr && !v->info.name.empty() ? v->info.name : std::string("Artist");
    if (drawBackHeader(name, avail)) {
        goBack();
        return;
    }
    if (v == nullptr || v->state == LoadState::NONE || v->state == LoadState::LOADING) {
        Spinner("Loading...");
        return;
    }
    if (v->state == LoadState::FAILED) {
        greyText("Could not load this artist.");
        if (pillButton("Try again##artist", ImVec2(160.0f, 44.0f), COL_WHITE, COL_DARK)) {
            v->state = LoadState::NONE;
            loadArtist(uri);
        }
        return;
    }
    const ArtistInfo &a = v->info;

    // Round avatar, listeners, play and shuffle play of the popular songs.
    const float side = 120.0f;
    ImVec2 c = ImGui::GetCursorPos();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    vita2d_texture *tex = thumbs.get(a.imageUrl, 160);
    if (tex != nullptr) {
        dl->AddImageRounded(Render::tex_id(tex), p, ImVec2(p.x + side, p.y + side), ImVec2(0, 0), ImVec2(1, 1),
                            COL_WHITE, side * 0.5f);
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), COL_CARD, side * 0.5f);
        ImFont *icon = gui->small_icon_font;
        ImVec2 isz = icon->CalcTextSizeA(icon->FontSize, FLT_MAX, 0.0f, ICON_FA_USER);
        dl->AddText(icon, icon->FontSize, ImVec2(p.x + (side - isz.x) * 0.5f, p.y + (side - isz.y) * 0.5f),
                    COL_GREY, ICON_FA_USER);
    }
    float tx = c.x + side + 16.0f;
    if (a.monthlyListeners > 0) {
        ImGui::SetCursorPos(ImVec2(tx, c.y + 10.0f));
        ImGui::PushFont(gui->log_font);
        ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
        std::string listeners = groupDigits(a.monthlyListeners) + " monthly listeners";
        ImGui::TextUnformatted(fitText(gui->log_font, listeners, avail - side - 16.0f).c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    const ItemList &popular = a.sections[AS_POPULAR];
    if (!popular.items.empty()) {
        const float h = 44.0f;
        ImGui::SetCursorPos(ImVec2(tx, c.y + 56.0f));
        bool play = pillButton("Play##artistplay", ImVec2(96.0f, h), COL_GREENV, COL_DARK);
        ImGui::SameLine();
        ImGui::PushFont(gui->small_icon_font);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
        bool shuffle = iconButton(ICON_FA_RANDOM "##artistshuffle", ImVec2(48.0f, h), COL_WHITE, COL_CARD);
        ImGui::PopStyleVar();
        ImGui::PopFont();
        if (play || shuffle) playItems(popular.items, SIZE_MAX, uri, shuffle);
    }
    ImGui::SetCursorPos(ImVec2(c.x, c.y + side + 4.0f));

    int seeAll = -1, pickSection = -1;
    size_t pick = 0;
    for (int s = 0; s < AS_COUNT; s++) {
        const ItemList &l = a.sections[s];
        if (l.items.empty()) continue;
        size_t count = s == AS_POPULAR ? ARTIST_POPULAR : ARTIST_OTHERS;
        std::string title = s == AS_PLAYLISTS ? "Featuring " + a.name : std::string(kSectionTitles[s]);
        bool more = l.items.size() > count || l.total > static_cast<int>(l.items.size());
        if (sectionHeader(gui->font_bold, gui->log_font, title, "##sec" + std::to_string(s), more, avail)) {
            seeAll = s;
        }
        for (size_t i = 0; i < l.items.size() && i < count; i++) {
            const CatalogItem &it = l.items[i];
            std::string id = "##a" + std::to_string(s) + "_" + std::to_string(i);
            bool current = it.kind == CatalogItem::TRACK && !playing.empty() && it.uri == playing;
            if (drawItem(it, id, avail, current)) {
                pickSection = s;
                pick = i;
            }
        }
    }
    if (pickSection >= 0) {
        const std::vector<CatalogItem> items = a.sections[pickSection].items;
        if (items[pick].kind == CatalogItem::TRACK) {
            playItems(items, pick, uri, false);
        } else {
            openItem(items[pick]);
        }
    } else if (seeAll >= 0) {
        openSection(uri, seeAll);
    }
}

void PlaybackScreen::drawSection(const std::string &uri, int section, const std::string &playing, float avail) {
    auto found = artists.find(uri);
    ArtistView *v = found == artists.end() ? nullptr : &found->second;
    std::string title = section == AS_PLAYLISTS ? "Featuring" : std::string(kSectionTitles[section]);
    if (v != nullptr && section == AS_PLAYLISTS) title += " " + v->info.name;
    if (drawBackHeader(title, avail)) {
        goBack();
        return;
    }
    if (v == nullptr || v->state != LoadState::LOADED) {
        if (v != nullptr && v->state == LoadState::FAILED) {
            greyText("Could not load this artist.");
        } else {
            Spinner("Loading...");
        }
        return;
    }
    if (section != AS_PLAYLISTS) {
        ImGui::PushFont(gui->log_font);
        greyText(v->info.name);
        ImGui::PopFont();
    }
    bool pageable = section >= AS_ALBUMS && section <= AS_APPEARS_ON;
    // The overview holds the first releases; the page shows them all.
    if (pageable && !v->paged[section] && !v->loadingMore[section]) loadReleases(uri, section);
    const ItemList &l = v->info.sections[section];
    size_t pick = SIZE_MAX;
    for (size_t i = 0; i < l.items.size(); i++) {
        const CatalogItem &it = l.items[i];
        std::string id = "##x" + std::to_string(i);
        bool current = it.kind == CatalogItem::TRACK && !playing.empty() && it.uri == playing;
        if (drawItem(it, id, avail, current)) pick = i;
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (v->loadingMore[section]) {
        Spinner("");
    } else if (pageable && static_cast<int>(l.items.size()) < l.total) {
        if (pillButton("Load more##section", ImVec2(180.0f, 44.0f), COL_CARD, COL_WHITE)) {
            loadReleases(uri, section);
        }
    }
    if (pick == SIZE_MAX) return;
    const std::vector<CatalogItem> items = l.items;
    if (items[pick].kind == CatalogItem::TRACK) {
        playItems(items, pick, uri, false);
    } else {
        openItem(items[pick]);
    }
}

// ---------------------------------------------------------------- playing

void PlaybackScreen::playItems(const std::vector<CatalogItem> &items, size_t index,
                               const std::string &context, bool shuffle) {
    std::vector<std::string> uris;
    size_t row = SIZE_MAX;
    for (size_t i = 0; i < items.size(); i++) {
        const CatalogItem &it = items[i];
        if (it.kind != CatalogItem::TRACK || !it.playable) continue;
        if (i == index) row = uris.size();
        uris.push_back(it.uri);
    }
    if (index < items.size() && row == SIZE_MAX) {
        gui->toast("This song is not available.");
        return;
    }
    if (uris.empty()) {
        gui->toast("Nothing to play here.");
        return;
    }
    playUris(std::move(uris), row, shuffle, context);
}

// ---------------------------------------------------------------- track menu

void PlaybackScreen::fetchLinks(const std::string &uri) {
    GUI *g = gui;
    gui->net.post([this, g, uri] {
        ApiResult m = g->api.get_tracks_metadata({uri});
        std::map<std::string, TrackMeta> metas;
        if (m.ok()) parseExtendedMetadata(m.body, &metas);
        auto it = metas.find(uri);
        bool ok = it != metas.end() && (!it->second.albumLink.uri.empty() || !it->second.artists.empty());
        TrackMeta t = ok ? it->second : TrackMeta();
        g->net.deliver([this, uri, t, ok] {
            for (TrackLinks *l : {&menu, &nowLinks}) {
                if (l->uri != uri || l->state != LoadState::LOADING) continue;
                l->state = ok ? LoadState::LOADED : LoadState::FAILED;
                if (!ok) continue;
                l->album = t.albumLink;
                if (l->album.name.empty()) l->album.name = t.album;
                l->artists = t.artists;
                if (l->name.empty()) l->name = t.name;
                if (l->artist.empty()) l->artist = t.artist;
            }
            if (nowLinks.uri == uri && nowPending != 0) {
                int what = nowPending;
                nowPending = 0;
                if (what == 1) {
                    goNowAlbum();
                } else {
                    goNowArtist();
                }
            }
        });
    }, true);
}

void PlaybackScreen::openTrackMenu(const std::string &uri, const std::string &name, const std::string &artist,
                                   const Link &album, const std::vector<Link> &links) {
    menu = TrackLinks();
    menu.uri = uri;
    menu.name = name;
    menu.artist = artist;
    menu.album = album;
    menu.artists = links;
    if (!album.uri.empty() && !links.empty()) {
        menu.state = LoadState::LOADED;
    } else if (uri == nowLinks.uri && nowLinks.state == LoadState::LOADED) {
        menu = nowLinks;
    } else {
        menu.state = LoadState::LOADING;
        fetchLinks(uri);
    }
    menuRequested = true;
}

void PlaybackScreen::drawTrackMenu() {
    if (menuRequested) {
        ImGui::OpenPopup("##trackmenu");
        menuRequested = false;
    }
    const float w = 440.0f, inner = w - 32.0f, rowH = 50.0f;
    ImGui::SetNextWindowPos(ImVec2(480.0f, 272.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
    bool open = ImGui::BeginPopup("##trackmenu");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) return;
    popupOpen = true;
    // Dim the whole screen behind the card, like a modal: over a bright
    // player and list, a bare card read as a glitch cutting them in half.
    // The popup draws its own background after the dim, so it stays lit.
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        dl->PushClipRectFullScreen();
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 170));
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), COL_CARD, 12.0f);
        dl->PopClipRect();
    }
    if (menu.uri.empty()) {   // closed by a debug command
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushFont(gui->font_bold);
    ImGui::TextUnformatted(fitText(gui->font_bold, menu.name.empty() ? std::string("Song") : menu.name,
                                   inner).c_str());
    ImGui::PopFont();
    ImGui::PushFont(gui->log_font);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_GREY);
    ImGui::TextUnformatted(fitText(gui->log_font, menu.artist.empty() ? std::string(" ") : menu.artist,
                                   inner).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    // One row: an icon and a label, the whole width.
    auto entry = [this, inner, rowH](const std::string &id, const char *icon, const std::string &text) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool pick = ImGui::Selectable(id.c_str(), false, 0, ImVec2(inner, rowH));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImFont *ic = gui->small_icon_font;
        ImVec2 isz = ic->CalcTextSizeA(ic->FontSize, FLT_MAX, 0.0f, icon);
        dl->AddText(ic, ic->FontSize, ImVec2(p.x + 6.0f + (28.0f - isz.x) * 0.5f, p.y + (rowH - isz.y) * 0.5f),
                    COL_GREY, icon);
        ImFont *f = ImGui::GetFont();
        dl->AddText(f, f->FontSize, ImVec2(p.x + 46.0f, p.y + (rowH - f->FontSize) * 0.5f), COL_WHITE,
                    fitText(f, text, inner - 52.0f).c_str());
        return pick;
    };
    Link go;
    bool goAlbum = false;
    if (menu.state == LoadState::LOADING) {
        Spinner("Loading...");
    } else if (menu.state == LoadState::FAILED) {
        greyText("Could not find this song's album and artists.");
    } else {
        if (!menu.album.uri.empty() &&
            entry("##menualbum", ICON_FA_COMPACT_DISC,
                  menu.album.name.empty() ? std::string("Go to album") : "Go to album: " + menu.album.name)) {
            go = menu.album;
            goAlbum = true;
        }
        for (size_t k = 0; k < menu.artists.size() && k < 4; k++) {
            const Link &a = menu.artists[k];
            if (a.uri.empty()) continue;
            if (entry("##menuartist" + std::to_string(k), ICON_FA_USER, "Go to artist: " + a.name)) go = a;
        }
    }
    if (!go.uri.empty()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (go.uri.empty()) return;
    if (goAlbum) {
        openList(go.uri, go.name);
    } else {
        openArtist(go.uri);
    }
}

void PlaybackScreen::goNowAlbum() {
    if (nowLinks.uri.empty()) {
        gui->toast("Nothing to open.");
    } else if (nowLinks.state == LoadState::LOADING) {
        nowPending = 1;   // opens once the links arrive
    } else if (nowLinks.state != LoadState::LOADED || nowLinks.album.uri.empty()) {
        gui->toast("Could not find this song's album.");
    } else {
        openList(nowLinks.album.uri, nowLinks.album.name);
    }
}

void PlaybackScreen::goNowArtist() {
    if (nowLinks.uri.empty()) {
        gui->toast("Nothing to open.");
    } else if (nowLinks.state == LoadState::LOADING) {
        nowPending = 2;
    } else if (nowLinks.state != LoadState::LOADED || nowLinks.artists.empty()) {
        gui->toast("Could not find this song's artist.");
    } else if (nowLinks.artists.size() == 1) {
        openArtist(nowLinks.artists[0].uri);
    } else {
        // Several artists: the menu lists them.
        openTrackMenu(nowLinks.uri, nowLinks.name, nowLinks.artist, nowLinks.album, nowLinks.artists);
    }
}

// ---------------------------------------------------------------- debug

std::string PlaybackScreen::browseState() {
    std::string out = "\"pages\":[";
    const std::vector<Page> *s = pageStack();
    for (size_t k = 0; s != nullptr && k < s->size(); k++) {
        if (k) out += ",";
        out += json_quote(pageKey((*s)[k]));
    }
    out += "],\"search\":{\"query\":" + json_quote(searchQuery) + ",\"chip\":" +
           json_quote(searchChip < 0 ? "all" : kChipKeys[searchChip]);
    for (int k = 0; k < SEARCH_KINDS; k++) {
        std::string l = listJson(searchLists[k], 5);
        l.insert(1, "\"state\":" + json_quote(stateName(searchStates[k])) + ",");
        out += ",\"" + std::string(kChipKeys[k]) + "\":" + l;
    }
    out += "},\"artist\":";
    const Page *top = topPage();
    auto found = top != nullptr && top->kind != Page::LIST ? artists.find(top->uri) : artists.end();
    if (found != artists.end()) {
        const ArtistView &v = found->second;
        out += "{\"name\":" + json_quote(v.info.name) + ",\"state\":" + json_quote(stateName(v.state)) +
               ",\"listeners\":" + std::to_string(v.info.monthlyListeners) +
               ",\"image\":" + (v.info.imageUrl.empty() ? "false" : "true");
        for (int k = 0; k < AS_COUNT; k++) {
            out += ",\"" + std::string(kSectionKeys[k]) + "\":" + listJson(v.info.sections[k], 4);
        }
        out += "}";
    } else {
        out += "null";
    }
    out += ",\"now_links\":" + linksJson(nowLinks) + ",\"menu\":" + linksJson(menu) +
           ",\"popup\":" + (popupOpen ? "true" : "false") +
           ",\"row_focused\":" + (focusedMenu ? "true" : "false") + ",\"thumbs\":" + std::to_string(thumbs.size());
    return out;
}

bool PlaybackScreen::browseCommand(const std::string &cmd, const std::string &arg) {
    std::string first = arg.substr(0, arg.find(' '));
    std::string rest = arg.size() > first.size() ? arg.substr(first.size() + 1) : "";
    auto sectionOf = [](const std::string &key) {
        for (int k = 0; k < AS_COUNT; k++) {
            if (key == kSectionKeys[k]) return k;
        }
        return isdigit(static_cast<unsigned char>(key.empty() ? 'x' : key[0])) ? atoi(key.c_str()) : -1;
    };
    auto kindOf = [](const std::string &key) {
        for (int k = 0; k < SEARCH_KINDS; k++) {
            if (key == kChipKeys[k]) return k;
        }
        return key == "all" ? -1 : -2;
    };
    if (cmd == "chip") {
        int k = kindOf(arg);
        if (k < -1) return false;
        tab = Tab::SEARCH;
        clearPages(1);
        searchChip = k;
        return true;
    }
    if (cmd == "result") {
        // result KIND N: tap result N of a category.
        int k = kindOf(first);
        size_t n = static_cast<size_t>(atoi(rest.c_str()));
        if (k < 0 || n >= searchLists[k].items.size()) return false;
        tab = Tab::SEARCH;
        activateSearch(k, n);
        return true;
    }
    if (cmd == "artist") {
        if (!startsWith(arg, "spotify:artist:")) return false;
        openArtist(arg);
        return true;
    }
    if (cmd == "album" || cmd == "list") {
        if (!startsWith(arg, "spotify:album:") && !startsWith(arg, SPOTIFY_PLAYLIST_HEADER)) return false;
        openList(arg);
        return true;
    }
    const Page *top = topPage();
    std::string artistUri = top != nullptr && top->kind != Page::LIST ? top->uri : "";
    if (cmd == "section") {
        int s = sectionOf(arg);
        if (artistUri.empty() || s < 0 || s >= AS_COUNT) return false;
        openSection(artistUri, s);
        return true;
    }
    if (cmd == "item") {
        // item SECTION N: row N of an artist page section.
        int s = sectionOf(first);
        auto found = artists.find(artistUri);
        if (found == artists.end() || s < 0 || s >= AS_COUNT) return false;
        const std::vector<CatalogItem> items = found->second.info.sections[s].items;
        size_t n = static_cast<size_t>(atoi(rest.c_str()));
        if (n >= items.size()) return false;
        if (items[n].kind == CatalogItem::TRACK) {
            playItems(items, n, artistUri, false);
        } else {
            openItem(items[n]);
        }
        return true;
    }
    if (cmd == "more") {
        if (top != nullptr && top->kind == Page::SECTION) {
            loadReleases(top->uri, top->section);
            return true;
        }
        if (tab == Tab::SEARCH && top == nullptr && searchChip >= 0) {
            loadSearch(searchChip, static_cast<int>(searchLists[searchChip].items.size()));
            return true;
        }
        return false;
    }
    if (cmd == "nowalbum") {
        goNowAlbum();
        return true;
    }
    if (cmd == "nowartist") {
        goNowArtist();
        return true;
    }
    if (cmd == "menu") {
        // menu N: the menu of track row N of the page shown (open list, song
        // results, popular songs).
        size_t n = static_cast<size_t>(atoi(arg.c_str()));
        if (top != nullptr && top->kind == Page::LIST && openIndex >= 0) {
            if (viewDirty || viewOf != openIndex) buildView();
            if (n >= view.size()) return false;
            const TrackRow &r = playlists[openIndex].tracks[view[n]];
            openTrackMenu(r.uri, r.name, r.artist, Link(), {});
            return true;
        }
        const std::vector<CatalogItem> *items = nullptr;
        auto found = artists.find(artistUri);
        if (found != artists.end()) {
            items = &found->second.info.sections[top->kind == Page::SECTION ? top->section : AS_POPULAR].items;
        } else if (tab == Tab::SEARCH && top == nullptr) {
            items = &searchLists[SEARCH_TRACKS].items;
        }
        if (items == nullptr || n >= items->size() || (*items)[n].kind != CatalogItem::TRACK) return false;
        const CatalogItem &it = (*items)[n];
        openTrackMenu(it.uri, it.name, joinNames(it.artists), it.album, it.artists);
        return true;
    }
    if (cmd == "menupick") {
        // menupick album | artist [K]
        if (menu.state != LoadState::LOADED) return false;
        TrackLinks picked = menu;
        size_t k = static_cast<size_t>(atoi(rest.c_str()));
        if (first == "album" && !picked.album.uri.empty()) {
            menu = TrackLinks();
            openList(picked.album.uri, picked.album.name);
            return true;
        }
        if (first == "artist" && k < picked.artists.size()) {
            menu = TrackLinks();
            openArtist(picked.artists[k].uri);
            return true;
        }
        return false;
    }
    return false;
}
