#pragma once

#define DEVICE_NAME           "Spotivita"
#define CREDENTIALS_FILE_NAME "ux0:data/cspot/authBlob.json"
#define CONFIG_FILE_NAME      "ux0:data/cspot/config.json"

// CA bundle shipped inside the VPK (app0:); used to verify TLS server certs on
// the Spotify Web API + image CDN. Verification needs a correct Vita clock.
#define TLS_CA_BUNDLE         "app0:cacert.pem"

#define CLIENT_ID_ANDROID     "65b708073fc0480ea92a077233ca87bd"
#define DEVICE_ID             "142137fd329622137a14901634264e6f332e2411"
#define SCOPES                "user-read-playback-state,user-modify-playback-state,playlist-read-private,playlist-read-collaborative"  // NOLINT
#define USER_AGENT            "Spotify/8.6.84 iOS/15.1 (iPhone11,8)"

// DEBUG
// #define CRASH_TEST
