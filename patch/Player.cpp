#include "Player.h"
#include "Logger.h"

// #include <valgrind/memcheck.h>

Player::Player(std::shared_ptr<MercuryManager> manager, std::shared_ptr<AudioSink> audioSink): bell::Task("player", 10 * 1024, -2, 1)
{
    this->audioSink = audioSink;
    this->manager = manager;
    startTask();
}

// currentTrackMutex guards currentTrack: the player task deletes it when a
// track ends while the spirc thread pauses, seeks or cancels it. Upstream
// declared the mutex and never took it.
void Player::pause()
{
    currentTrackMutex.lock();
    if (currentTrack != nullptr)
    {
        if (currentTrack->audioStream != nullptr)
        {
            this->currentTrack->audioStream->isPaused = true;
        }
    }
    currentTrackMutex.unlock();
}

void Player::play()
{
    currentTrackMutex.lock();
    if (currentTrack != nullptr)
    {
        if (currentTrack->audioStream != nullptr)
        {
            this->currentTrack->audioStream->isPaused = false;
        }
    }
    currentTrackMutex.unlock();
}

void Player::setVolume(uint32_t volume)
{
    this->volume = (volume / (double)MAX_VOLUME) * 255;

    // Calculate and cache log volume value
    auto vol = 255 - this->volume;
    uint32_t value = (log10(255 / ((float)vol + 1)) * 105.54571334);
    if (value >= 254) value = 256;
    logVolume = value << 8; // *256

    // Pass volume event to the sink if volume is sink-handled
    if (!this->audioSink->softwareVolumeControl)
    {
        this->audioSink->volumeChanged(volume);
    }
}

void Player::seekMs(size_t positionMs)
{
    currentTrackMutex.lock();
    if (currentTrack != nullptr)
    {
        if (currentTrack->audioStream != nullptr)
        {
            this->currentTrack->audioStream->seekMs(positionMs);
        }
    }
    currentTrackMutex.unlock();
    // VALGRIND_DO_LEAK_CHECK;
}

void Player::feedPCM(uint8_t *data, size_t len)
{
    // Simple digital volume control alg
    // @TODO actually extract it somewhere
    if (this->audioSink->softwareVolumeControl)
    {
        int16_t* psample;
        int32_t temp;
        psample = (int16_t*)(data);
        size_t half_len = len / 2;
        for (uint32_t i = 0; i < half_len; i++)
        {
            // Offset data for unsigned sinks
            if (this->audioSink->usign)
            {
                temp = ((int32_t)psample[i] + 0x8000) * logVolume;
            }
            else
            {
                temp = ((int32_t)psample[i]) * logVolume;
            }
            psample[i] = (temp >> 16) & 0xFFFF;
        }
    }

    this->audioSink->feedPCMFrames(data, len);
}

void Player::runTask()
{
    uint8_t *pcmOut = (uint8_t *) malloc(4096 / 4);
    std::scoped_lock lock(this->runningMutex);
    this->isRunning = true;
    while (isRunning)
    {
        if(nextTrack != nullptr && nextTrack->loaded)
        {
            this->nextTrackMutex.lock();
            currentTrackMutex.lock();
            currentTrack = this->nextTrack;
            this->nextTrack = nullptr;
            currentTrackMutex.unlock();
            this->nextTrackMutex.unlock();

            currentTrack->audioStream->startPlaybackLoop(pcmOut, 4096 / 4);
            currentTrack->loadedTrackCallback = nullptr;
            currentTrack->audioStream->streamFinishedCallback = nullptr;
            currentTrack->audioStream->audioSink = nullptr;
            currentTrack->audioStream->pcmCallback = nullptr;

            // Deleted outside the lock: the destructor waits for a Mercury
            // callback on the cspot thread, which may be taking this lock in
            // cancelCurrentTrack.
            currentTrackMutex.lock();
            SpotifyTrack *finished = currentTrack;
            currentTrack = nullptr;
            currentTrackMutex.unlock();
            delete finished;
        }
        else
        {
            usleep(10000);
        }

    }
    free(pcmOut);
}

void Player::stop() {
    CSPOT_LOG(info, "Trying to stop");
    this->isRunning = false;
    cancelCurrentTrack();
    std::scoped_lock lock(this->runningMutex);
    if(this->nextTrack != nullptr)
    {
        delete this->nextTrack;
    }
    this->isRunning = false;
    CSPOT_LOG(info, "Track cancelled");
    cancelCurrentTrack();
    CSPOT_LOG(info, "Stopping player");
}

void Player::cancelCurrentTrack()
{
    currentTrackMutex.lock();
    if (currentTrack != nullptr)
    {
        if (currentTrack->audioStream != nullptr && currentTrack->audioStream->isRunning)
        {
            currentTrack->audioStream->isRunning = false;
        }
    }
    currentTrackMutex.unlock();
}

void Player::handleLoad(std::shared_ptr<TrackReference> trackReference, std::function<void()>& trackLoadedCallback, uint32_t position_ms, bool isPaused)
{
    std::lock_guard<std::mutex> guard(loadTrackMutex);

    pcmDataCallback framesCallback = [=](uint8_t *frames, size_t len) {
        this->feedPCM(frames, len);
     };

    // The track being replaced is deleted outside nextTrackMutex: its
    // destructor waits for its Mercury callback, which takes that mutex in
    // loadedTrackCallback. Skipping fast used to delete it under the callback
    // (use after free in the audio key callback).
    this->nextTrackMutex.lock();
    SpotifyTrack *replaced = this->nextTrack;
    this->nextTrack = nullptr;
    this->nextTrackMutex.unlock();
    delete replaced;

    SpotifyTrack *track = new SpotifyTrack(this->manager, trackReference, position_ms, isPaused);
    track->trackInfoReceived = this->trackChanged;
    track->loadedTrackCallback = [this, framesCallback, trackLoadedCallback, track]() {
        this->nextTrackMutex.lock();
        bool current = this->nextTrack == track;   // not replaced while its key was on the way
        this->nextTrackMutex.unlock();
        if (!current) return;
        trackLoadedCallback();

        this->nextTrackMutex.lock();
        current = this->nextTrack == track;
        if (current)
        {
            track->audioStream->streamFinishedCallback = this->endOfFileCallback;
            track->audioStream->audioSink = this->audioSink;
            track->audioStream->pcmCallback = framesCallback;
            track->loaded = true;
        }
        this->nextTrackMutex.unlock();
        if (!current) return;

        cancelCurrentTrack();
    };
    this->nextTrackMutex.lock();
    this->nextTrack = track;
    this->nextTrackMutex.unlock();
}
