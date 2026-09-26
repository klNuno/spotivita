#pragma once

#include "AudioSink.h"

class VitaAudioSink : public AudioSink {
 public:
    VitaAudioSink();
    ~VitaAudioSink();
    void feedPCMFrames(const uint8_t *buffer, size_t bytes);
    void volumeChanged(uint16_t volume);
    // Stops or resumes output at once. The ring holds about 1.5 s, which
    // used to keep playing after a pause until it drained.
    void setPaused(bool paused);
    // Drops the queued PCM (skip, seek, another device taking over).
    void flush();
 private:
    int threadid;
};
