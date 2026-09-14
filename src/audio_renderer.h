#pragma once

#include <QString>

extern "C" {
#include <Limelight.h>
}

class AudioRenderer {
public:
    struct Impl;
    AudioRenderer();
    ~AudioRenderer();
    AudioRenderer(const AudioRenderer&) = delete;
    AudioRenderer& operator=(const AudioRenderer&) = delete;

    AUDIO_RENDERER_CALLBACKS callbacks();
    int initialize(int audioConfiguration, const OPUS_MULTISTREAM_CONFIGURATION* opusConfig, int flags);
    void start();
    void stop();
    void cleanup();
    void decodeAndQueue(const char* data, int length);
    int configuredLatencyMs() const;
    int deviceLatencyMs() const;
    QString statistics() const;

private:
    void startOutputIfReady(uint32_t availableSamples);
    Impl* m_impl;
};
