// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "AudioPlayer.h"

#include <ge/audio.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include <minimp3.h>
#include <minimp3_ex.h>

#include <cstring>
#include <vector>

namespace {

struct Clip {
    SDL_AudioSpec spec{};
    std::vector<uint8_t> pcmData;
    bool loop = false;
    float volume = 1.0f;
    SDL_AudioStream* stream = nullptr;
};

void loopCallback(void* userdata, SDL_AudioStream* stream,
                  int additional_amount, int /*total_amount*/) {
    auto* clip = static_cast<Clip*>(userdata);
    if (clip->loop && additional_amount > 0 && !clip->pcmData.empty()) {
        SDL_PutAudioStreamData(stream, clip->pcmData.data(),
                               static_cast<int>(clip->pcmData.size()));
    }
}

} // namespace

struct AudioPlayer::M {
    SDL_AudioDeviceID device = 0;
    ge::audio::Registration audioReg;  // RAII; engine pauses/resumes on lifecycle
    std::vector<Clip> clips;

    ~M() {
        for (auto& clip : clips) {
            if (clip.stream) {
                SDL_DestroyAudioStream(clip.stream);
            }
        }
        if (device) {
            SDL_CloseAudioDevice(device);
        }
    }
};

AudioPlayer::AudioPlayer() : m(std::make_unique<M>()) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SPDLOG_ERROR("AudioPlayer: SDL_InitSubSystem(AUDIO) failed: {}",
                         SDL_GetError());
            return;
        }
    }

    m->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!m->device) {
        SPDLOG_ERROR("AudioPlayer: Failed to open audio device: {}",
                     SDL_GetError());
        return;
    }

    SDL_ResumeAudioDevice(m->device);

    // Register with ge::audio so the engine manages pause/resume on
    // background, foreground, audio focus loss, and AVAudioSession
    // interruptions. The RAII handle in m->audioReg unregisters on ~M.
    m->audioReg = ge::audio::registerDevice(m->device);

    SPDLOG_INFO("AudioPlayer: Opened audio device");
}

AudioPlayer::~AudioPlayer() = default;

void AudioPlayer::pause() {
    if (m->device) {
        SPDLOG_INFO("AudioPlayer: pause");
        SDL_PauseAudioDevice(m->device);
    }
}

void AudioPlayer::resume() {
    if (m->device) {
        SPDLOG_INFO("AudioPlayer: resume");
        SDL_ResumeAudioDevice(m->device);
    }
}

void AudioPlayer::loadClip(uint32_t id, uint32_t format, uint32_t flags,
                           const void* data, size_t size) {
    while (m->clips.size() <= id) {
        m->clips.emplace_back();
    }

    auto& clip = m->clips[id];
    clip.loop = (flags & 1) != 0;

    if (format == 0) {
        // WAV — decode via SDL3
        SDL_IOStream* io = SDL_IOFromConstMem(data, size);
        if (!io) {
            SPDLOG_ERROR("AudioPlayer: SDL_IOFromConstMem failed");
            return;
        }

        Uint8* wavBuf = nullptr;
        Uint32 wavLen = 0;
        if (!SDL_LoadWAV_IO(io, true, &clip.spec, &wavBuf, &wavLen)) {
            SPDLOG_ERROR("AudioPlayer: SDL_LoadWAV_IO failed: {}",
                         SDL_GetError());
            return;
        }

        clip.pcmData.assign(wavBuf, wavBuf + wavLen);
        SDL_free(wavBuf);

        SPDLOG_INFO("AudioPlayer: Loaded WAV clip {}: {}Hz {}ch {:.1f}KB",
                     id, clip.spec.freq, clip.spec.channels,
                     clip.pcmData.size() / 1024.0);
    } else if (format == 1) {
        // MP3 — decode via minimp3
        mp3dec_t mp3d;
        mp3dec_init(&mp3d);

        mp3dec_file_info_t info{};
        int err = mp3dec_load_buf(&mp3d,
                                  static_cast<const uint8_t*>(data),
                                  size, &info, nullptr, nullptr);
        if (err || !info.buffer || info.samples == 0) {
            SPDLOG_ERROR("AudioPlayer: MP3 decode failed (err={})", err);
            if (info.buffer) free(info.buffer);
            return;
        }

        clip.spec.format = SDL_AUDIO_S16;
        clip.spec.channels = info.channels;
        clip.spec.freq = info.hz;

        size_t pcmBytes = info.samples * sizeof(mp3d_sample_t);
        clip.pcmData.assign(reinterpret_cast<uint8_t*>(info.buffer),
                            reinterpret_cast<uint8_t*>(info.buffer) + pcmBytes);
        free(info.buffer);

        SPDLOG_INFO("AudioPlayer: Loaded MP3 clip {}: {}Hz {}ch {:.1f}KB PCM",
                     id, clip.spec.freq, clip.spec.channels,
                     clip.pcmData.size() / 1024.0);
    } else {
        SPDLOG_WARN("AudioPlayer: Unknown format {} for clip {}", format, id);
    }
}

void AudioPlayer::handleCommand(uint32_t command, uint32_t id, float volume) {
    if (!m->device || id >= m->clips.size()) return;

    auto& clip = m->clips[id];
    if (clip.pcmData.empty()) return;

    switch (command) {
    case 0: { // play
        if (!clip.stream) {
            clip.stream = SDL_CreateAudioStream(&clip.spec, nullptr);
            if (!clip.stream) {
                SPDLOG_ERROR("AudioPlayer: CreateAudioStream failed: {}",
                             SDL_GetError());
                return;
            }
            if (!SDL_BindAudioStream(m->device, clip.stream)) {
                SPDLOG_ERROR("AudioPlayer: BindAudioStream failed: {}",
                             SDL_GetError());
                SDL_DestroyAudioStream(clip.stream);
                clip.stream = nullptr;
                return;
            }
            if (clip.loop) {
                SDL_SetAudioStreamGetCallback(clip.stream, loopCallback, &clip);
            }
        }

        SDL_ResumeAudioStreamDevice(clip.stream);
        SDL_ClearAudioStream(clip.stream);
        SDL_PutAudioStreamData(clip.stream, clip.pcmData.data(),
                               static_cast<int>(clip.pcmData.size()));
        SDL_SetAudioStreamGain(clip.stream, volume);
        clip.volume = volume;

        SPDLOG_INFO("AudioPlayer: Play clip {} (vol={:.2f}, loop={})",
                     id, volume, clip.loop);
        break;
    }
    case 1: // stop
        if (clip.stream) {
            SDL_PauseAudioStreamDevice(clip.stream);
            SDL_ClearAudioStream(clip.stream);
        }
        SPDLOG_INFO("AudioPlayer: Stop clip {}", id);
        break;

    case 2: // setVolume
        clip.volume = volume;
        if (clip.stream) {
            SDL_SetAudioStreamGain(clip.stream, volume);
        }
        break;
    }
}
