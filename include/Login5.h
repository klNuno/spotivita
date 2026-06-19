#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Acquire a Spotify Web API access token through the login5 + clienttoken flow.
//
// Spotify retired the old Mercury keymaster token endpoint
// (hm://keymaster/token/authenticated now answers
// {"code":4,"errorDescription":"Invalid request"} for stored-credential
// sessions), so the in-app Web API calls need a token minted by login5. This
// reuses the Zeroconf stored credentials (username + authData) already held in
// the LoginBlob.
//
// Returns the bearer token string, or an empty string on any failure (the app
// stays usable as a phone-controlled Connect target without it).
std::string login5_get_access_token(const std::string &clientId,
                                    const std::string &deviceId,
                                    const std::string &userAgent,
                                    const std::string &username,
                                    const std::vector<uint8_t> &authData);
