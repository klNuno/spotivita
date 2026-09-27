#include "SpircController.h"
#include "ConfigJSON.h"
#include "Logger.h"
#include "SpotifyTrack.h"
#include <cstring>

SpircController::SpircController(std::shared_ptr<MercuryManager> manager,
                                 std::string username,
                                 std::shared_ptr<AudioSink> audioSink) {

    this->manager = manager;
    this->player = std::make_unique<Player>(manager, audioSink);
    this->state = std::make_unique<PlayerState>(manager->timeProvider);
    this->username = username;

    player->endOfFileCallback = [=]() {
        // nextTrack() wraps to index 0 and pauses at the end of the queue.
        if (state->nextTrack()) {
            loadTrack();
        } else if (queueContinues) {
            queueEnded();   // the app sends the next part of a long list
        } else if (repeatQueue) {
            loadTrack();
        }
    };

    player->setVolume(configMan->volume);
    subscribe();
}

SpircController::~SpircController() {
}

void SpircController::subscribe() {
    mercuryCallback responseLambda = [=](std::unique_ptr<MercuryResponse> res) {
        // this->trackInformationCallback(std::move(res));
        sendCmd(MessageType_kMessageTypeHello);
        CSPOT_LOG(debug, "Sent kMessageTypeHello!");
    };
    mercuryCallback subLambda = [=](std::unique_ptr<MercuryResponse> res) {
        this->handleFrame(res->parts[0]);
    };

    manager->execute(MercuryType::SUB,
                     "hm://remote/user/" + this->username + "/", responseLambda,
                     subLambda);
}

void SpircController::setPause(bool isPaused, bool notifyPlayer) {
    if (loading) pausedWhileLoading = isPaused;
    // Playing again after yieldPlayback: claim the active slot back, so Spotify
    // stops the device that took it.
    if (!isPaused && !state->isActive()) state->setActive(true);
    sendEvent(CSpotEventType::PLAY_PAUSE, isPaused);
    if (isPaused) {
        CSPOT_LOG(debug, "External pause command");
        if (notifyPlayer) player->pause();
        state->setPlaybackState(PlaybackState::Paused);
    } else {
        CSPOT_LOG(debug, "External play command");
        if (notifyPlayer) player->play();
        state->setPlaybackState(PlaybackState::Playing);
    }
    notify();
}

void SpircController::disconnect(void) {
    skipTo();
    state->setActive(false);
    notify();
    // Send the event at the end at it might be a last gasp
    sendEvent(CSpotEventType::DISC);
}

void SpircController::yieldPlayback() {
    state->setActive(false);
    setPause(true);
}

void SpircController::playToggle() {
    bool paused = loading ? pausedWhileLoading.load()
                          : state->innerFrame.state.status == PlayStatus_kPlayStatusPause;
    setPause(!paused);
}

void SpircController::adjustVolume(int by) {
    if (state->innerFrame.device_state.has_volume) {
        int volume = state->innerFrame.device_state.volume + by;
        if (volume < 0) volume = 0;
        else if (volume > MAX_VOLUME) volume = MAX_VOLUME;
        setVolume(volume);
    }
}

void SpircController::setVolume(int volume) {
    setRemoteVolume(volume);
    player->setVolume(volume);
    configMan->save();
}

void SpircController::setRemoteVolume(int volume) {
    state->setVolume(volume);
    notify();
}

// A skip asked by a user (here or on another device): stop the current track and
// its buffered audio now instead of letting them play while the next one loads.
// The end of a track takes endOfFileCallback instead and stays gapless.
void SpircController::skipTo() {
    player->cancelCurrentTrack();
    flushAudio();
}

void SpircController::nextSong() {
    skipTo();
    if (state->nextTrack()) {
        loadTrack();
    } else if (queueContinues) {
        queueEnded();
    } else if (repeatQueue) {
        loadTrack();
    }
    notify();
}

void SpircController::prevSong() {
    skipTo();
    state->prevTrack();
    loadTrack();
    notify();
}

void SpircController::handleFrame(std::vector<uint8_t> &data) {
    pb_release(Frame_fields, &state->remoteFrame);
    pbDecode(state->remoteFrame, Frame_fields, data);

    switch (state->remoteFrame.typ) {
    case MessageType_kMessageTypeNotify: {
        CSPOT_LOG(debug, "Notify frame");
        // Pause the playback if another player took control
        if (state->isActive() &&
            state->remoteFrame.device_state.is_active) {
            disconnect();
        }
        break;
    }
    case MessageType_kMessageTypeSeek: {
        CSPOT_LOG(debug, "Seek command");
        sendEvent(CSpotEventType::SEEK, (int) state->remoteFrame.position);
        state->updatePositionMs(state->remoteFrame.position);
        this->player->seekMs(state->remoteFrame.position);
        flushAudio();
        notify();
        break;
    }
    case MessageType_kMessageTypeVolume:
        sendEvent(CSpotEventType::VOLUME, (int) state->remoteFrame.volume);
        setVolume(state->remoteFrame.volume);
        break;
    case MessageType_kMessageTypePause:
        setPause(true);
        break;
    case MessageType_kMessageTypePlay:
        setPause(false);
        break;
    case MessageType_kMessageTypeNext:
        sendEvent(CSpotEventType::NEXT);
        nextSong();
        break;
    case MessageType_kMessageTypePrev:
        sendEvent(CSpotEventType::PREV);
        prevSong();
        break;
    case MessageType_kMessageTypeLoad: {
        CSPOT_LOG(debug, "Load frame!");

        state->setActive(true);
        skipTo();
        queueContinues = false;   // a phone's queue, not the app's long list

        // Every sane person on the planet would expect std::move to work here.
        // And it does... on every single platform EXCEPT for ESP32 for some
        // reason. For which it corrupts memory and makes printf fail. so yeah.
        // its cursed.
        state->updateTracks();

        // bool isPaused = (state->remoteFrame.state->status.value() ==
        // PlayStatus::kPlayStatusPlay) ? false : true;
        loadTrack(state->remoteFrame.state.position_ms, false);
        state->updatePositionMs(state->remoteFrame.state.position_ms);

        this->notify();
        break;
    }
    case MessageType_kMessageTypeReplace: {
        CSPOT_LOG(debug, "Got replace frame!");
        state->updateTracks();
        this->notify();
        break;
    }
    case MessageType_kMessageTypeShuffle: {
        CSPOT_LOG(debug, "Got shuffle frame");
        state->setShuffle(state->remoteFrame.state.shuffle);
        this->notify();
        break;
    }
    case MessageType_kMessageTypeRepeat: {
        CSPOT_LOG(debug, "Got repeat frame");
        state->setRepeat(state->remoteFrame.state.repeat);
        this->notify();
        break;
    }
    default:
        break;
    }
}

// spotify:track:<22 base62 chars> -> the 16-byte big-endian gid. Returns an
// empty vector for anything else (episodes, local files, bad input).
static std::vector<uint8_t> trackGid(const std::string &uri) {
    static const std::string header = "spotify:track:";
    static const char alphabet[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (uri.compare(0, header.size(), header) != 0) return {};
    std::string id = uri.substr(header.size());
    if (id.size() != 22) return {};
    std::vector<uint8_t> gid(16, 0);
    for (char c : id) {
        const char *pos = strchr(alphabet, c);
        if (pos == NULL || pos - alphabet >= 62) return {};  // strchr also finds the terminator
        uint32_t carry = static_cast<uint32_t>(pos - alphabet);
        for (int i = 15; i >= 0; i--) {
            carry += gid[i] * 62u;
            gid[i] = carry & 0xFF;
            carry >>= 8;
        }
        if (carry != 0) return {};
    }
    return gid;
}

void SpircController::playTracks(const std::vector<std::string> &uris,
                                 const std::string &contextUri, uint32_t index,
                                 bool continues) {
    queueContinues = continues;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> tracks;
    uint32_t start = 0;
    for (size_t i = 0; i < uris.size(); i++) {
        auto gid = trackGid(uris[i]);
        if (gid.empty()) continue;
        if (i == index) start = tracks.size();
        tracks.push_back({uris[i], gid});
    }
    if (tracks.empty()) {
        CSPOT_LOG(error, "playTracks: no playable track");
        return;
    }

    // Build the queue in remoteFrame and let updateTracks copy it, the same
    // path a Load frame from a phone takes.
    pb_release(Frame_fields, &state->remoteFrame);
    memset(&state->remoteFrame, 0, sizeof(Frame));
    State &rs = state->remoteFrame.state;
    rs.track = (TrackRef *) calloc(tracks.size(), sizeof(TrackRef));
    rs.track_count = tracks.size();
    for (size_t i = 0; i < tracks.size(); i++) {
        const auto &gid = tracks[i].second;
        rs.track[i].gid = (pb_bytes_array_t *) malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(gid.size()));
        rs.track[i].gid->size = gid.size();
        memcpy(rs.track[i].gid->bytes, gid.data(), gid.size());
        rs.track[i].uri = strdup(tracks[i].first.c_str());
        rs.track[i].context = strdup(contextUri.c_str());
    }
    rs.context_uri = strdup(contextUri.c_str());
    rs.has_playing_track_index = true;
    rs.playing_track_index = start;

    state->setActive(true);
    skipTo();
    state->updateTracks();
    state->updatePositionMs(0);
    loadTrack(0, false);
    notify();
}

void SpircController::seek(uint32_t positionMs) {
    sendEvent(CSpotEventType::SEEK, (int) positionMs);
    state->updatePositionMs(positionMs);
    player->seekMs(positionMs);
    flushAudio();
    notify();
}

void SpircController::setShuffle(bool shuffle) {
    if (state->innerFrame.state.track_count == 0) return;
    state->setShuffle(shuffle);
    notify();
}

void SpircController::setRepeat(int mode) {
    // PlayerState's repeat flag replays the current track; the queue repeat
    // lives here.
    repeatQueue = (mode == 1);
    state->setRepeat(mode == 2);
    notify();
}

void SpircController::loadTrack(uint32_t position_ms, bool isPaused) {
    sendEvent(CSpotEventType::LOAD, (int) position_ms);
    state->setPlaybackState(PlaybackState::Loading);
    pausedWhileLoading = false;
    loading = true;
    std::function<void()> loadedLambda = [=]() {
        // Loading finished, notify that playback started. A pause asked while
        // it loaded (a tap, the sleep timer) holds.
        bool paused = isPaused || pausedWhileLoading;
        loading = false;
        setPause(paused, false);
        sendEvent(CSpotEventType::PLAYBACK_START);
    };

    player->handleLoad(state->getCurrentTrack(), loadedLambda, position_ms,
                       isPaused);
}

void SpircController::notify() {
    this->sendCmd(MessageType_kMessageTypeNotify);
}

void SpircController::sendEvent(CSpotEventType eventType, std::variant<TrackInfo, int, bool> data) {
    if (eventHandler != nullptr) {
        CSpotEvent event = {
            .eventType = eventType,
            .data = data,
        };

        eventHandler(event);
    }
}

void SpircController::setEventHandler(cspotEventHandler callback) {
    this->eventHandler = callback;

    player->trackChanged = ([this](TrackInfo &track) {
            TrackInfo info;
            info.album = track.album;
            info.artist = track.artist;
            info.imageUrl = track.imageUrl;
            info.name = track.name;
            info.duration = track.duration;
            this->sendEvent(CSpotEventType::TRACK_INFO, info);
    });
}

void SpircController::stopPlayer() { this->player->stop(); }

void SpircController::sendCmd(MessageType typ) {
    // Serialize current player state
    auto encodedFrame = state->encodeCurrentFrame(typ);

    mercuryCallback responseLambda = [=](std::unique_ptr<MercuryResponse> res) {
    };
    auto parts = mercuryParts({encodedFrame});
    this->manager->execute(MercuryType::SEND,
                           "hm://remote/user/" + this->username + "/",
                           responseLambda, parts);
}
