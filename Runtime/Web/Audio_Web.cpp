/**
 * @file Audio_Web.cpp
 * @brief Web audio implementation over Emscripten's built-in OpenAL
 *        (which maps to Web Audio; -lopenal, no external deps).
 *
 * Architecture — unlike the console ports (software mixer + one HW stream),
 * OpenAL IS the mixer, so this backend is a thin voice-table -> AL-source
 * mapping:
 *
 *   - AUD_Play(voiceIndex, ...) binds a cached AL buffer for the SoundWave's
 *     PCM to the pooled AL source for that voice and plays it. One AL source
 *     per engine voice slot (AUDIO_MAX_VOICES).
 *   - AL buffers are cached keyed by the wave's PCM pointer; the cache entry
 *     is dropped in AUD_FreeWaveBuffer (the engine frees a wave's PCM exactly
 *     once, when the SoundWave unloads).
 *   - Panning: the engine pre-computes left/right volumes (attenuation + pan)
 *     and hands them to AUD_SetVolume. OpenAL has no per-ear gain, so we set
 *     AL_GAIN = max(l, r) and pan mono sources by placing them on the
 *     listener-relative X axis. (Stereo buffers don't spatialize in OpenAL —
 *     they take the gain only, which is the common engine tradeoff.)
 *   - Streaming voices (video player etc.) use AL queue buffers: submit
 *     copies into pooled AL buffers, unqueue processed ones in AUD_Update,
 *     and derive the played-samples clock from the dequeued total plus
 *     AL_SAMPLE_OFFSET.
 *
 * Autoplay: browsers gate audio on a user gesture. Emscripten's OpenAL
 * resumes its AudioContext on the first gesture automatically (shell.html
 * carries a fallback poke); sounds triggered before that are simply silent —
 * the AL calls all succeed.
 *
 * Single-threaded: no locks anywhere; everything runs on the main thread.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <unordered_map>
#include <vector>

namespace
{
    ALCdevice*  sDevice  = nullptr;
    ALCcontext* sContext = nullptr;
    bool        sInited  = false;

    // ----- Voice pool ------------------------------------------------------
    struct WebVoice
    {
        ALuint source   = 0;
        bool   inUse    = false;   // an AUD_Play happened and no AUD_Stop yet
        bool   mono     = false;   // mono buffers can be panned via position
        float  leftVol  = 1.0f;
        float  rightVol = 1.0f;
    };

    static WebVoice sVoices[AUDIO_MAX_VOICES];

    // AL buffer per SoundWave PCM blob, keyed by the PCM pointer the engine
    // owns (AUD_AllocWaveBuffer's return). Dropped in AUD_FreeWaveBuffer.
    static std::unordered_map<const void*, ALuint> sWaveBuffers;

    // ----- Streaming voices ------------------------------------------------
    constexpr uint32_t kMaxStreams          = 4;
    constexpr uint32_t kStreamBuffersPer    = 8;      // AL queue depth
    constexpr double   kStreamMaxQueuedSecs = 0.5;    // backpressure bound

    struct WebStream
    {
        bool     inUse         = false;
        bool     paused        = false;
        ALuint   source        = 0;
        uint32_t sampleRate    = 0;
        uint8_t  numChannels   = 1;
        ALenum   format        = AL_FORMAT_MONO16;

        ALuint   allBuffers[kStreamBuffersPer]  = {};  // every generated id, for teardown
        ALuint   freeBuffers[kStreamBuffersPer] = {};
        uint32_t numFree       = 0;

        // Frames per queued AL buffer, oldest first — consumed as buffers
        // complete so the playback clock stays exact.
        std::vector<uint32_t> queuedFrames;

        uint64_t playedFramesBase = 0;   // frames from fully-completed buffers
        uint64_t queuedFramesTotal = 0;  // currently in the AL queue
    };

    static WebStream sStreams[kMaxStreams];

    ALenum FormatFor(uint32_t channels, uint32_t bitsPerSample)
    {
        if (channels == 1) return (bitsPerSample == 8) ? AL_FORMAT_MONO8   : AL_FORMAT_MONO16;
        else               return (bitsPerSample == 8) ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
    }

    // Apply the engine's precomputed left/right volumes onto an AL source:
    // gain = the louder ear, pan (mono only) = normalized L/R difference
    // mapped onto the listener-relative X axis.
    void ApplyVolumes(WebVoice& v)
    {
        const float gain = (v.leftVol > v.rightVol) ? v.leftVol : v.rightVol;
        alSourcef(v.source, AL_GAIN, gain);

        if (v.mono)
        {
            const float sum = v.leftVol + v.rightVol;
            const float pan = (sum > 0.0001f) ? (v.rightVol - v.leftVol) / sum : 0.0f;
            // Keep some forward distance so extreme pans don't hard-cut.
            alSource3f(v.source, AL_POSITION, pan, 0.0f, -sqrtf(1.0f - pan * pan * 0.5f));
        }
        else
        {
            alSource3f(v.source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        }
    }

    ALuint GetOrCreateWaveBuffer(SoundWave* soundWave)
    {
        const void* pcm = soundWave->GetWaveData();
        if (pcm == nullptr) return 0;

        auto it = sWaveBuffers.find(pcm);
        if (it != sWaveBuffers.end()) return it->second;

        const uint32_t frames   = soundWave->GetNumSamples();
        const uint32_t channels = soundWave->GetNumChannels();
        const uint32_t bps      = soundWave->GetBitsPerSample();
        const uint32_t bytes    = frames * channels * (bps / 8);
        if (frames == 0 || bytes == 0) return 0;

        ALuint buffer = 0;
        alGenBuffers(1, &buffer);
        if (buffer == 0) return 0;

        alBufferData(buffer, FormatFor(channels, bps), pcm, (ALsizei)bytes,
                     (ALsizei)soundWave->GetSampleRate());
        if (alGetError() != AL_NO_ERROR)
        {
            alDeleteBuffers(1, &buffer);
            LogError("Audio_Web: alBufferData failed (%u bytes, %u ch, %u bps)",
                     bytes, channels, bps);
            return 0;
        }

        sWaveBuffers.emplace(pcm, buffer);
        return buffer;
    }
}

// ----- API impl ------------------------------------------------------------

void AUD_Initialize()
{
    sDevice = alcOpenDevice(nullptr);
    if (sDevice == nullptr)
    {
        LogError("Audio_Web: alcOpenDevice failed — audio disabled");
        return;
    }

    sContext = alcCreateContext(sDevice, nullptr);
    if (sContext == nullptr || !alcMakeContextCurrent(sContext))
    {
        LogError("Audio_Web: alcCreateContext/MakeCurrent failed — audio disabled");
        if (sContext) { alcDestroyContext(sContext); sContext = nullptr; }
        alcCloseDevice(sDevice);
        sDevice = nullptr;
        return;
    }

    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);

    for (uint32_t i = 0; i < AUDIO_MAX_VOICES; ++i)
    {
        alGenSources(1, &sVoices[i].source);
        // The engine pre-computes attenuation into the L/R volumes it hands
        // us — AL's own distance model must not double-attenuate.
        alSourcef(sVoices[i].source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(sVoices[i].source, AL_SOURCE_RELATIVE, AL_TRUE);
    }

    sInited = true;
    LogDebug("Audio_Web: OpenAL up (%u voices, %u stream slots)",
             AUDIO_MAX_VOICES, kMaxStreams);
}

void AUD_Shutdown()
{
    if (!sInited) return;

    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        if (sStreams[i].inUse) AUD_CloseStream(i + 1);
    }
    for (uint32_t i = 0; i < AUDIO_MAX_VOICES; ++i)
    {
        if (sVoices[i].source != 0)
        {
            alSourceStop(sVoices[i].source);
            alDeleteSources(1, &sVoices[i].source);
            sVoices[i] = WebVoice{};
        }
    }
    for (auto& kv : sWaveBuffers)
    {
        alDeleteBuffers(1, &kv.second);
    }
    sWaveBuffers.clear();

    alcMakeContextCurrent(nullptr);
    if (sContext) { alcDestroyContext(sContext); sContext = nullptr; }
    if (sDevice)  { alcCloseDevice(sDevice);     sDevice  = nullptr; }
    sInited = false;
}

void AUD_Update()
{
    if (!sInited) return;

    // Reclaim completed stream buffers and keep stalled stream sources
    // playing (an underrun stops an AL source; restart once data is queued).
    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        WebStream& s = sStreams[i];
        if (!s.inUse) continue;

        ALint processed = 0;
        alGetSourcei(s.source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0 && s.numFree < kStreamBuffersPer)
        {
            ALuint done = 0;
            alSourceUnqueueBuffers(s.source, 1, &done);
            if (done == 0) break;
            s.freeBuffers[s.numFree++] = done;

            if (!s.queuedFrames.empty())
            {
                s.playedFramesBase += s.queuedFrames.front();
                s.queuedFramesTotal -= s.queuedFrames.front();
                s.queuedFrames.erase(s.queuedFrames.begin());
            }
        }

        if (!s.paused)
        {
            ALint state = 0, queued = 0;
            alGetSourcei(s.source, AL_SOURCE_STATE, &state);
            alGetSourcei(s.source, AL_BUFFERS_QUEUED, &queued);
            if (state != AL_PLAYING && queued > 0)
            {
                alSourcePlay(s.source);
            }
        }
    }
}

void AUD_Play(uint32_t voiceIndex, SoundWave* soundWave, float volume,
              float pitch, bool loop, float startTime, bool spatial)
{
    if (!sInited || voiceIndex >= AUDIO_MAX_VOICES || soundWave == nullptr) return;

    const ALuint buffer = GetOrCreateWaveBuffer(soundWave);
    if (buffer == 0) return;

    WebVoice& v = sVoices[voiceIndex];
    alSourceStop(v.source);
    alSourcei(v.source, AL_BUFFER, (ALint)buffer);
    alSourcei(v.source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    alSourcef(v.source, AL_PITCH, (pitch > 0.01f) ? pitch : 1.0f);
    if (startTime > 0.0f)
    {
        alSourcef(v.source, AL_SEC_OFFSET, startTime);
    }

    v.inUse    = true;
    v.mono     = (soundWave->GetNumChannels() == 1);
    v.leftVol  = volume;
    v.rightVol = volume;
    (void)spatial;   // the engine folds spatialization into SetVolume's L/R
    ApplyVolumes(v);

    alSourcePlay(v.source);
}

void AUD_Stop(uint32_t voiceIndex)
{
    if (!sInited || voiceIndex >= AUDIO_MAX_VOICES) return;
    WebVoice& v = sVoices[voiceIndex];
    alSourceStop(v.source);
    alSourcei(v.source, AL_BUFFER, 0);
    v.inUse = false;
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    if (!sInited || voiceIndex >= AUDIO_MAX_VOICES) return false;
    ALint state = 0;
    alGetSourcei(sVoices[voiceIndex].source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING || state == AL_PAUSED;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    if (!sInited || voiceIndex >= AUDIO_MAX_VOICES) return;
    WebVoice& v = sVoices[voiceIndex];
    v.leftVol  = (leftVolume  < 0.0f) ? 0.0f : leftVolume;
    v.rightVol = (rightVolume < 0.0f) ? 0.0f : rightVolume;
    ApplyVolumes(v);
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    if (!sInited || voiceIndex >= AUDIO_MAX_VOICES) return;
    alSourcef(sVoices[voiceIndex].source, AL_PITCH, (pitch > 0.01f) ? pitch : 1.0f);
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }

void AUD_FreeWaveBuffer(void* buffer)
{
    // The wave's PCM is going away — drop the AL buffer built from it.
    auto it = sWaveBuffers.find(buffer);
    if (it != sWaveBuffers.end())
    {
        // Detach from any voice still bound to it before deleting.
        for (uint32_t i = 0; i < AUDIO_MAX_VOICES; ++i)
        {
            ALint bound = 0;
            alGetSourcei(sVoices[i].source, AL_BUFFER, &bound);
            if ((ALuint)bound == it->second)
            {
                alSourceStop(sVoices[i].source);
                alSourcei(sVoices[i].source, AL_BUFFER, 0);
                sVoices[i].inUse = false;
            }
        }
        alDeleteBuffers(1, &it->second);
        sWaveBuffers.erase(it);
    }
    free(buffer);
}

void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

// ----- Streaming voices ---------------------------------------------------

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels, uint32_t bitsPerSample)
{
    if (!sInited) return 0;
    if (numChannels != 1 && numChannels != 2)
    {
        LogWarning("AUD_OpenStream: only mono/stereo supported (got %u channels)", numChannels);
        return 0;
    }
    if (bitsPerSample != 16)
    {
        LogWarning("AUD_OpenStream: only 16-bit PCM supported (got %u bps)", bitsPerSample);
        return 0;
    }

    uint32_t pickedIdx = kMaxStreams;
    for (uint32_t i = 0; i < kMaxStreams; ++i)
        if (!sStreams[i].inUse) { pickedIdx = i; break; }

    if (pickedIdx == kMaxStreams)
    {
        LogWarning("AUD_OpenStream: no free stream slots (pool=%u)", kMaxStreams);
        return 0;
    }

    WebStream& s = sStreams[pickedIdx];
    alGenSources(1, &s.source);
    if (s.source == 0) return 0;
    alSourcef(s.source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcei(s.source, AL_SOURCE_RELATIVE, AL_TRUE);

    alGenBuffers(kStreamBuffersPer, s.allBuffers);
    memcpy(s.freeBuffers, s.allBuffers, sizeof(s.allBuffers));
    s.numFree = kStreamBuffersPer;

    s.sampleRate       = sampleRate;
    s.numChannels      = (uint8_t)numChannels;
    s.format           = FormatFor(numChannels, bitsPerSample);
    s.playedFramesBase = 0;
    s.queuedFramesTotal = 0;
    s.queuedFrames.clear();
    s.paused = false;
    s.inUse  = true;

    return pickedIdx + 1;   // 1-based; 0 is the failure sentinel
}

void AUD_CloseStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    WebStream& s = sStreams[streamId - 1];
    if (!s.inUse) return;

    alSourceStop(s.source);
    alSourcei(s.source, AL_BUFFER, 0);   // detaches the whole queue
    alDeleteBuffers(kStreamBuffersPer, s.allBuffers);   // full generated set
    alDeleteSources(1, &s.source);

    s = WebStream{};
}

int32_t AUD_SubmitStreamBuffer(uint32_t streamId, const uint8_t* data, uint32_t byteSize)
{
    if (!sInited || streamId == 0 || streamId > kMaxStreams ||
        data == nullptr || byteSize == 0)
        return 0;

    WebStream& s = sStreams[streamId - 1];
    if (!s.inUse) return 0;

    // Backpressure: bound the queued duration so A/V sync stays tight and
    // callers get the retry signal the API promises.
    const uint32_t bpf = s.numChannels * 2;   // 16-bit guaranteed
    const uint32_t submitFrames = byteSize / bpf;
    if (submitFrames == 0) return 0;

    const double queuedSecs = (double)s.queuedFramesTotal / (double)s.sampleRate;
    if (s.numFree == 0 || queuedSecs >= kStreamMaxQueuedSecs)
    {
        return 0;   // queue full — caller retries next tick
    }

    const ALuint buffer = s.freeBuffers[--s.numFree];
    alBufferData(buffer, s.format, data, (ALsizei)(submitFrames * bpf),
                 (ALsizei)s.sampleRate);
    alSourceQueueBuffers(s.source, 1, &buffer);
    if (alGetError() != AL_NO_ERROR)
    {
        s.freeBuffers[s.numFree++] = buffer;   // put it back
        return 0;
    }

    s.queuedFrames.push_back(submitFrames);
    s.queuedFramesTotal += submitFrames;

    if (!s.paused)
    {
        ALint state = 0;
        alGetSourcei(s.source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(s.source);
    }

    return (int32_t)(submitFrames * bpf);
}

uint64_t AUD_GetStreamPlayedSamples(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return 0;
    const WebStream& s = sStreams[streamId - 1];
    if (!s.inUse) return 0;

    ALint sampleOffset = 0;
    alGetSourcei(s.source, AL_SAMPLE_OFFSET, &sampleOffset);
    return s.playedFramesBase + (uint64_t)((sampleOffset > 0) ? sampleOffset : 0);
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    WebStream& s = sStreams[streamId - 1];
    if (s.inUse) alSourcef(s.source, AL_GAIN, (volume < 0.0f) ? 0.0f : volume);
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    WebStream& s = sStreams[streamId - 1];
    if (!s.inUse) return;
    s.paused = paused;
    if (paused) alSourcePause(s.source);
    else        alSourcePlay(s.source);
}

void AUD_FlushStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    WebStream& s = sStreams[streamId - 1];
    if (!s.inUse) return;

    // Stop, then unqueue everything (stopping marks all queued buffers
    // processed). SamplesPlayed keeps climbing per the API contract.
    alSourceStop(s.source);
    ALint processed = 0;
    alGetSourcei(s.source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0 && s.numFree < kStreamBuffersPer)
    {
        ALuint done = 0;
        alSourceUnqueueBuffers(s.source, 1, &done);
        if (done == 0) break;
        s.freeBuffers[s.numFree++] = done;
    }
    s.playedFramesBase += s.queuedFramesTotal;
    s.queuedFramesTotal = 0;
    s.queuedFrames.clear();
}

#endif // POLYPHASE_PLATFORM_ADDON
