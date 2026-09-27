#ifndef SPIRCCONTROLLER_H
#define SPIRCCONTROLLER_H

#include <vector>
#include <string>
#include <functional>
#include "Utils.h"
#include "MercuryManager.h"
#include "Session.h"
#include "PlayerState.h"
#include "SpotifyTrack.h"
#include "ConstantParameters.h"
#include "Player.h"
#include "ConfigJSON.h"
#include <cassert>
#include <variant>
#include <atomic>

enum class CSpotEventType {
    PLAY_PAUSE,
    VOLUME,
    TRACK_INFO,
    DISC,
    NEXT,
    PREV,
    SEEK,
	LOAD,
    PLAYBACK_START
};

struct CSpotEvent {
    CSpotEventType eventType;
    std::variant<TrackInfo, int, bool> data;
};

typedef std::function<void(CSpotEvent&)> cspotEventHandler;

class SpircController {
private:
    std::shared_ptr<MercuryManager> manager;
    std::string username;
    bool firstFrame = true;
    bool repeatQueue = false;
    bool queueContinues = false;   // the app has more of the list after this queue
    // PlayerState reports "paused" for every loading track, so a toggle during a
    // load reads these instead, and a pause asked then holds once it loads.
    std::atomic<bool> loading{false};
    std::atomic<bool> pausedWhileLoading{false};
    std::unique_ptr<Player> player;
    std::unique_ptr<PlayerState> state;
    std::shared_ptr<AudioSink> audioSink;
    std::shared_ptr<ConfigJSON> config;

    cspotEventHandler eventHandler;
    void sendCmd(MessageType typ);
    void notify();
	void sendEvent(CSpotEventType eventType, std::variant<TrackInfo, int, bool> data = 0);
    void handleFrame(std::vector<uint8_t> &data);
    void loadTrack(uint32_t position_ms = 0, bool isPaused = 0);
    void skipTo();
public:
    /**
     * @brief Drops the PCM already queued in the sink
     *
     * The sink buffers about 1.5 s; without this a skip or a seek kept playing
     * the old audio until the buffer drained.
     */
    std::function<void()> flushAudio = []() {};

    /**
     * @brief Runs on the cspot thread when a queue from playTracks(..., true)
     * runs out (end of its last track, or next on it): the app then sends the
     * next part of its list. Repeat-all does not wrap such a queue.
     */
    std::function<void()> queueEnded = []() {};

    SpircController(std::shared_ptr<MercuryManager> manager, std::string username, std::shared_ptr<AudioSink> audioSink);
    ~SpircController();
    void subscribe();

    /** 
     * @brief Pauses / Plays current song
     *
     * Calling this function will pause or resume playback, setting the 
     * necessary state value and notifying spotify SPIRC.
     *
     * @param pause if true pause content, play otherwise
     */
    void setPause(bool pause, bool notifyPlayer = true);

    /** 
     * @brief Toggle Play/Pause
     */
	void playToggle();

    /** 
     * @brief Notifies spotify servers about volume change
     *
     * @param volume int between 0 and `MAX_VOLUME`
     */
    void setRemoteVolume(int volume);

	/** 
     * @brief Set device volume and notifies spotify
     *
     * @param volume int between 0 and `MAX_VOLUME`
     */
	void setVolume(int volume);

	/** 
     * @brief change volume by a given value
     *
     * @param volume int between 0 and `MAX_VOLUME`
     */
    void adjustVolume(int by);

    /** 
     * @brief Goes back to previous track and notfies spotify SPIRC
     */
    void prevSong();

    /** 
    * @brief Skips to next track and notfies spotify SPIRC
    */
    void nextSong();
    /**
     * @brief Starts local playback of a track list, no remote controller needed
     *
     * The Web API player endpoints answer 429 to tokens minted for this
     * client, so the Vita builds its own queue and becomes the active device.
     *
     * @param uris spotify:track:<base62> URIs, played in order
     * @param contextUri playlist URI shown to other clients, may be empty
     * @param index position in `uris` to start from
     */
    void playTracks(const std::vector<std::string> &uris, const std::string &contextUri, uint32_t index,
                    bool continues = false);

    /**
     * @brief Seeks the current track and notifies spotify SPIRC
     */
    void seek(uint32_t positionMs);

    /**
     * @brief Sets shuffle on the local queue and notifies spotify SPIRC
     */
    void setShuffle(bool shuffle);

    /**
     * @brief Sets repeat: 0 off, 1 whole queue, 2 current track
     */
    void setRepeat(int mode);

    void setEventHandler(cspotEventHandler handler);
    void stopPlayer();

    /** 
     * @brief Disconnect players and notify
     */
    void disconnect();
};

#endif
