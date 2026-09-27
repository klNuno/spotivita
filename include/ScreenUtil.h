#pragma once

#include <string>
#include <vector>
#include "PlaybackScreen.h"

// Helpers shared by the parts of PlaybackScreen (PlaybackScreen.cpp, Browse.cpp).
const char *stateName(LoadState s);
// What a failed request means for the user; -1 = Spotify changed its API.
std::string describeStatus(long status);
bool startsWith(const std::string &s, const char *prefix);
int findPlaylist(const std::vector<Playlist> &pls, const std::string &uri);
