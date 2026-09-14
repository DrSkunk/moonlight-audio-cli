#include "audio_renderer.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <opus/opus_multistream.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

struct AudioRenderer::Impl {
    OpusMSDecoder* decoder = nullptr;
    AudioQueueRef queue = nullptr;
    std::vector<int16_t> ring;
    std::vector<int16_t> decodeBuffer;
    std::atomic<uint32_t> read {0};
    std::atomic<uint32_t> write {0};
    std::atomic<bool> running {false};
    std::atomic<bool> streamStarted {false};
    std::atomic<bool> queueStarted {false};
    std::atomic<uint64_t> decodedPackets {0};
    std::atomic<uint64_t> queuedSamples {0};
    std::atomic<uint64_t> droppedSamples {0};
    std::atomic<uint64_t> underrunSamples {0};
    std::atomic<uint64_t> outputCallbacks {0};
    std::atomic<int> deviceLatencyFrames {0};
    uint32_t capacity = 0;
    int sampleRate = 48000;
    int channels = 2;
    int samplesPerFrame = 240;
};

namespace {
constexpr int kQueueFrames = 480; // 10 ms at the negotiated 48 kHz rate
constexpr int kQueueBuffers = 3;  // 30 ms queued by CoreAudio

void outputCallback(void* context, AudioQueueRef, AudioQueueBufferRef buffer) {
    auto* impl = static_cast<AudioRenderer::Impl*>(context);
    const uint32_t wanted = buffer->mAudioDataBytesCapacity / sizeof(int16_t);
    auto* out = static_cast<int16_t*>(buffer->mAudioData);
    const uint32_t read = impl->read.load(std::memory_order_relaxed);
    const uint32_t write = impl->write.load(std::memory_order_acquire);
    const uint32_t available = write - read;
    const uint32_t copied = std::min(wanted, available);
    for (uint32_t i = 0; i < copied; ++i) out[i] = impl->ring[(read + i) % impl->capacity];
    if (copied < wanted) {
        std::memset(out + copied, 0, (wanted - copied) * sizeof(int16_t));
        impl->underrunSamples.fetch_add(wanted - copied, std::memory_order_relaxed);
    }
    impl->read.store(read + copied, std::memory_order_release);
    impl->outputCallbacks.fetch_add(1, std::memory_order_relaxed);
    buffer->mAudioDataByteSize = wanted * sizeof(int16_t);
    AudioQueueEnqueueBuffer(impl->queue, buffer, 0, nullptr);
}

int callbackInit(int cfg, const POPUS_MULTISTREAM_CONFIGURATION opus, void* context, int flags) {
    return static_cast<AudioRenderer*>(context)->initialize(cfg, opus, flags);
}
AudioRenderer* activeRenderer = nullptr;
void callbackStart() { if (activeRenderer) activeRenderer->start(); }
void callbackStop() { if (activeRenderer) activeRenderer->stop(); }
void callbackCleanup() { if (activeRenderer) activeRenderer->cleanup(); }
// moonlight-common-c has no per-callback context, so this is bound below through
// its one active connection restriction.
void callbackDecode(char* data, int length) { if (activeRenderer) activeRenderer->decodeAndQueue(data, length); }
}

AudioRenderer::AudioRenderer() : m_impl(new Impl) {}
AudioRenderer::~AudioRenderer() { cleanup(); delete m_impl; }

AUDIO_RENDERER_CALLBACKS AudioRenderer::callbacks() {
    activeRenderer = this;
    AUDIO_RENDERER_CALLBACKS callbacks;
    LiInitializeAudioCallbacks(&callbacks);
    callbacks.init = callbackInit;
    callbacks.start = callbackStart;
    callbacks.stop = callbackStop;
    callbacks.cleanup = callbackCleanup;
    callbacks.decodeAndPlaySample = callbackDecode;
    callbacks.capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
    return callbacks;
}

int AudioRenderer::initialize(int, const OPUS_MULTISTREAM_CONFIGURATION* config, int) {
    cleanup();
    if (!config || config->sampleRate <= 0 || config->channelCount <= 0 || config->channelCount > 8 || config->samplesPerFrame <= 0) return -1;
    m_impl->sampleRate = config->sampleRate;
    m_impl->channels = config->channelCount;
    m_impl->samplesPerFrame = config->samplesPerFrame;
    int error = OPUS_OK;
    m_impl->decoder = opus_multistream_decoder_create(config->sampleRate, config->channelCount, config->streams, config->coupledStreams, config->mapping, &error);
    if (!m_impl->decoder || error != OPUS_OK) return -1;

    // 100 ms is a hard upper bound. A full buffer drops new samples, preventing
    // latency from growing when the output device is temporarily delayed.
    m_impl->capacity = static_cast<uint32_t>(config->sampleRate * config->channelCount / 10);
    m_impl->ring.assign(m_impl->capacity, 0);
    m_impl->decodeBuffer.assign(static_cast<size_t>(config->samplesPerFrame) * config->channelCount, 0);
    AudioStreamBasicDescription format {};
    format.mSampleRate = config->sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = config->channelCount;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = config->channelCount * sizeof(int16_t);
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (AudioQueueNewOutput(&format, outputCallback, m_impl, nullptr, nullptr, 0, &m_impl->queue) != noErr) { cleanup(); return -1; }
    for (int i = 0; i < kQueueBuffers; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(m_impl->queue, kQueueFrames * format.mBytesPerFrame, &buffer) != noErr) { cleanup(); return -1; }
        std::memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
        buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(m_impl->queue, buffer, 0, nullptr);
    }
    m_impl->running.store(true, std::memory_order_release);
    return 0;
}

void AudioRenderer::startOutputIfReady(uint32_t availableSamples) {
    const uint32_t minimumSamples = static_cast<uint32_t>(m_impl->sampleRate * m_impl->channels / 50); // 20 ms
    if (availableSamples < minimumSamples || !m_impl->streamStarted.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!m_impl->queueStarted.compare_exchange_strong(expected, true)) return;
    if (AudioQueueStart(m_impl->queue, nullptr) != noErr) {
        m_impl->queueStarted.store(false);
        return;
    }
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioQueueGetProperty(m_impl->queue, kAudioQueueProperty_CurrentDevice, &device, &size) == noErr && device != kAudioObjectUnknown) {
        AudioObjectPropertyAddress property { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        UInt32 frames = 0;
        size = sizeof(frames);
        if (AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &frames) == noErr) m_impl->deviceLatencyFrames.store(frames);
    }
}

void AudioRenderer::start() {
    m_impl->streamStarted.store(true, std::memory_order_release);
    const uint32_t available = m_impl->write.load(std::memory_order_acquire) - m_impl->read.load(std::memory_order_acquire);
    startOutputIfReady(available);
}
void AudioRenderer::stop() {
    if (m_impl->queue && m_impl->queueStarted.load()) AudioQueueStop(m_impl->queue, true);
    m_impl->running.store(false);
    m_impl->streamStarted.store(false);
    m_impl->queueStarted.store(false);
}
void AudioRenderer::cleanup() {
    stop();
    if (m_impl->queue) { AudioQueueDispose(m_impl->queue, true); m_impl->queue = nullptr; }
    if (m_impl->decoder) { opus_multistream_decoder_destroy(m_impl->decoder); m_impl->decoder = nullptr; }
    m_impl->ring.clear(); m_impl->decodeBuffer.clear(); m_impl->read = 0; m_impl->write = 0;
}

void AudioRenderer::decodeAndQueue(const char* data, int length) {
    if (!m_impl->running.load(std::memory_order_acquire) || !m_impl->decoder || length <= 0) return;
    const int frames = opus_multistream_decode(m_impl->decoder, reinterpret_cast<const unsigned char*>(data), length, m_impl->decodeBuffer.data(), m_impl->samplesPerFrame, 0);
    if (frames <= 0) return;
    const uint32_t samples = static_cast<uint32_t>(frames * m_impl->channels);
    m_impl->decodedPackets.fetch_add(1, std::memory_order_relaxed);
    const uint32_t write = m_impl->write.load(std::memory_order_relaxed);
    const uint32_t read = m_impl->read.load(std::memory_order_acquire);
    if (samples > m_impl->capacity - (write - read)) {
        m_impl->droppedSamples.fetch_add(samples, std::memory_order_relaxed);
        return;
    }
    for (uint32_t i = 0; i < samples; ++i) m_impl->ring[(write + i) % m_impl->capacity] = m_impl->decodeBuffer[i];
    m_impl->write.store(write + samples, std::memory_order_release);
    m_impl->queuedSamples.fetch_add(samples, std::memory_order_relaxed);
    startOutputIfReady(write + samples - read);
}

int AudioRenderer::configuredLatencyMs() const { return kQueueBuffers * kQueueFrames * 1000 / m_impl->sampleRate; }
int AudioRenderer::deviceLatencyMs() const { return m_impl->deviceLatencyFrames.load() * 1000 / std::max(1, m_impl->sampleRate); }

QString AudioRenderer::statistics() const {
    const auto ms = [this](uint64_t samples) { return samples * 1000 / static_cast<uint64_t>(std::max(1, m_impl->sampleRate * m_impl->channels)); };
    return QString("Audio statistics: %1 Opus packets decoded, %2 ms queued, %3 ms dropped (ring full), %4 ms silence inserted across %5 output callbacks.")
        .arg(m_impl->decodedPackets.load())
        .arg(ms(m_impl->queuedSamples.load()))
        .arg(ms(m_impl->droppedSamples.load()))
        .arg(ms(m_impl->underrunSamples.load()))
        .arg(m_impl->outputCallbacks.load());
}
