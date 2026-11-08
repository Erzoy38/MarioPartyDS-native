// A real microphone for the game's: a recording device of Windows (the PC's, a headset's, a
// controller's). The game gets how loud it is, the way the DS microphone would hear it.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

// Settings value for "no microphone, keys only".
inline constexpr const char* kMicOff = "off";

// What the game's own microphone test shows, as a level for the game (0 to 1), measured with
// --mic-level: below kMicHeard nothing, then too quiet, from kMicGood just right, from kMicLoud
// too loud.
inline constexpr float kMicHeard = 0.25f, kMicGood = 0.52f, kMicLoud = 0.83f;

class Microphone {
public:
    ~Microphone() { close(); }

    // `device`: a name from devices(), "" for Windows' default, kMicOff for none.
    void open(const std::string& device);
    void close();
    bool is_open() const { return stream_ != nullptr; }
    const std::string& device() const { return device_; }

    // Reads what came in since the last call: the loudest sample, falling back slowly.
    void update();
    // The level for the game at this sensitivity (10 to 100 percent), 0 to 1.
    float game_level(int sensitivity) const;
    static float gain(int sensitivity);

    // The names of the recording devices.
    static std::vector<std::string> devices();

private:
    SDL_AudioStream* stream_ = nullptr;
    std::string device_ = kMicOff;
    float peak_ = 0;
    std::vector<float> buffer_;
};
