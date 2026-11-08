#include "microphone.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

std::vector<std::string> Microphone::devices() {
    std::vector<std::string> names;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        if (name != nullptr && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    SDL_free(ids);
    return names;
}

void Microphone::open(const std::string& device) {
    close();
    device_ = device;
    if (device == kMicOff) return;
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!device.empty()) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
        bool found = false;
        for (int i = 0; ids != nullptr && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name != nullptr && device == name) {
                id = ids[i];
                found = true;
                break;
            }
        }
        SDL_free(ids);
        if (!found) std::printf("microphone %s not connected, using the default\n", device.c_str());
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 16000};
    stream_ = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (stream_ == nullptr) {
        std::printf("microphone: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    std::printf("microphone: %s\n", device.empty() ? "default device" : device.c_str());
}

void Microphone::close() {
    if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    peak_ = 0;
}

void Microphone::update() {
    peak_ *= 0.8f;  // falls back over a few pictures, like the DS's meter
    if (stream_ == nullptr) return;
    const int available = SDL_GetAudioStreamAvailable(stream_);
    if (available <= 0) return;
    buffer_.resize(size_t(available) / sizeof(float) + 1);
    const int got = SDL_GetAudioStreamData(stream_, buffer_.data(), int(buffer_.size() * sizeof(float)));
    const size_t n = got > 0 ? size_t(got) / sizeof(float) : 0;
    float loudest = 0;
    for (size_t i = 0; i < n; ++i) loudest = std::max(loudest, std::fabs(buffer_[i]));
    peak_ = std::max(peak_, std::min(1.0f, loudest));
}

// 50 % is twice as loud as the device delivers; every 25 % doubles or halves it (10 %: 0.66, 100 %: 8).
float Microphone::gain(int sensitivity) {
    return 2.0f * std::pow(2.0f, float(std::clamp(sensitivity, 10, 100) - 50) / 25.0f);
}

float Microphone::game_level(int sensitivity) const { return std::min(1.0f, peak_ * gain(sensitivity)); }