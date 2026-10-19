#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include <unordered_map>

#include "SDL.h"

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

#include "goemon_support.h"
#include "goemon_tts.h"

namespace {
    struct Clip {
        std::vector<Sint16> samples;
        SDL_AudioSpec spec{};
    };

    std::mutex tts_mutex;
    std::unordered_map<std::string, bool> exists_cache;
    std::unordered_map<std::string, Clip> clips;
    SDL_AudioDeviceID device = 0;
    std::atomic<uint32_t> device_id{ 0 }; // copy of device that the audio thread can read without the lock
    SDL_AudioSpec device_spec{};

    std::filesystem::path clip_path(const std::string& key) {
        return goemon64::get_asset_path(("tts/" + key + ".ogg").c_str());
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
        std::ifstream file(clip_path(key), std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int channels = 0, rate = 0;
        short* decoded = nullptr;
        const int frames = bytes.empty() ? -1 : stb_vorbis_decode_memory(bytes.data(), (int)bytes.size(), &channels, &rate, &decoded);
        if (frames <= 0 || decoded == nullptr) {
            std::printf("[tts] failed to decode %s\n", key.c_str());
            return;
        }
        Clip clip;
        clip.samples.assign(decoded, decoded + (size_t)frames * channels);
        std::free(decoded);
        clip.spec.freq = rate;
        clip.spec.format = AUDIO_S16SYS;
        clip.spec.channels = (Uint8)channels;
        clip.spec.samples = 1024;
        // The recordings are quiet next to the game, so they are boosted once when loaded.
        constexpr float voice_gain = 1.2f;
        for (Sint16& sample : clip.samples) {
            sample = static_cast<Sint16>(std::clamp(sample * voice_gain, -32768.0f, 32767.0f));
        }
        it = clips.emplace(key, std::move(clip)).first;
    }
    if (!open_device(it->second.spec)) {
        std::printf("[tts] failed to open audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_ClearQueuedAudio(device);
    SDL_QueueAudio(device, it->second.samples.data(), (Uint32)(it->second.samples.size() * sizeof(Sint16)));
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
