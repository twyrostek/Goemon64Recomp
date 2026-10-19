#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "SDL.h"

#include "goemon_support.h"
#include "goemon_tts.h"

namespace {
    struct Clip {
        Uint8* data = nullptr;
        Uint32 length = 0;
        SDL_AudioSpec spec{};
    };

    std::mutex tts_mutex;
    std::unordered_map<std::string, bool> exists_cache;
    std::unordered_map<std::string, Clip> clips;
    SDL_AudioDeviceID device = 0;
    std::atomic<uint32_t> device_id{ 0 }; // copy of device that the audio thread can read without the lock
    SDL_AudioSpec device_spec{};

    std::filesystem::path clip_path(const std::string& key) {
        return goemon64::get_asset_path(("tts/" + key + ".wav").c_str());
    }

    bool open_device(const SDL_AudioSpec& wanted) {
        if (device != 0 && device_spec.freq == wanted.freq && device_spec.format == wanted.format && device_spec.channels == wanted.channels) {
            return true;
        }
        if (device != 0) {
            device_id = 0;
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            return false;
        }
        SDL_AudioSpec want = wanted;
        want.callback = nullptr;
        device = SDL_OpenAudioDevice(nullptr, 0, &want, &device_spec, 0);
        device_id = device;
        return device != 0;
    }
}

bool goemon64::tts::available(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    std::lock_guard lock(tts_mutex);
    auto it = exists_cache.find(key);
    if (it == exists_cache.end()) {
        it = exists_cache.emplace(key, std::filesystem::exists(clip_path(key))).first;
    }
    return it->second;
}

void goemon64::tts::play(const std::string& key) {
    std::lock_guard lock(tts_mutex);
    auto it = clips.find(key);
    if (it == clips.end()) {
        Clip clip;
        if (SDL_LoadWAV(clip_path(key).string().c_str(), &clip.spec, &clip.data, &clip.length) == nullptr) {
            std::printf("[tts] failed to load %s: %s\n", key.c_str(), SDL_GetError());
            return;
        }
        // The recordings are quiet next to the game, so they are boosted once when loaded.
        if (clip.spec.format == AUDIO_S16SYS) {
            constexpr float voice_gain = 1.2f;
            Sint16* samples = reinterpret_cast<Sint16*>(clip.data);
            for (Uint32 i = 0; i < clip.length / sizeof(Sint16); i++) {
                samples[i] = static_cast<Sint16>(std::clamp(samples[i] * voice_gain, -32768.0f, 32767.0f));
            }
        }
        it = clips.emplace(key, clip).first;
    }
    if (!open_device(it->second.spec)) {
        std::printf("[tts] failed to open audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_ClearQueuedAudio(device);
    SDL_QueueAudio(device, it->second.data, it->second.length);
    SDL_PauseAudioDevice(device, 0);
}

void goemon64::tts::stop() {
    std::lock_guard lock(tts_mutex);
    if (device != 0) {
        SDL_ClearQueuedAudio(device);
    }
}

float goemon64::tts::duck_gain() {
    constexpr float ducked = 0.40f;
    constexpr float attack = 0.12f;
    constexpr float release = 0.02f;
    static float gain = 1.0f;

    const uint32_t id = device_id.load();
    const bool playing = id != 0 && SDL_GetQueuedAudioSize(id) > 0;
    const float target = playing ? ducked : 1.0f;
    if (gain > target) {
        gain = std::max(target, gain - attack);
    } else {
        gain = std::min(target, gain + release);
    }
    return gain;
}
