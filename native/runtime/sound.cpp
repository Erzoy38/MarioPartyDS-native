#include "sound.h"

#include "hw.h"
#include "memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Tables from the ARM7 program in the player's ROM, found by their contents (see REVERSE.md).
constexpr u32 kArm7DecibelTable = 0xE514;        // s16[128], in 1/10 dB
constexpr u32 kArm7DecibelSquareTable = 0xE614;  // s16[128]
constexpr u32 kArm7SineTable = 0xE4EC;           // u8[33], a quarter wave
constexpr u32 kArm7AttackTable = 0xE72E;         // u8[19]

s16 g_decibel[128], g_decibel_square[128];
u8 g_sine[33], g_attack_table[19];

constexpr s32 kEnvelopeSilent = -92544;  // -723 << 7
constexpr u32 kPitchSteps = 768;         // one octave
u32 g_pitch_ratio[kPitchSteps];          // 2^(i/768) as 16.16, built from integers only

constexpr u32 kPlayers = 16, kTracks = 32, kChannels = 16;

struct Track;

enum class Kind : u8 { Pcm, Psg, Noise };
enum class Phase : u8 { Attack, Decay, Sustain, Release };

struct Channel {
    bool active = false, started = false, auto_sweep = false, timed = false;
    Kind kind = Kind::Pcm;
    Phase phase = Phase::Attack;
    Track* track = nullptr;
    u8 priority = 0, key = 60, base_key = 60, velocity = 127, pan_inst = 64;
    s32 length = 0;
    s32 envelope = kEnvelopeSilent;
    s32 attack = 0, decay = 0, sustain = 0, release = 0;
    s32 sweep_pitch = 0, sweep_length = 0, sweep_counter = 0;
    s32 lfo_counter = 0, lfo_delay_counter = 0;
    u8 lfo_type = 0, lfo_depth = 0, lfo_range = 1, lfo_speed = 16;
    u16 lfo_delay = 0;
    s32 volume = 0, pitch = 0, pan = 0;
    // Wave data.
    u32 data = 0, loop_start = 0, loop_length = 0;
    u8 format = 0;
    bool loop = false;
    u32 rate = 0;
    u8 duty = 0;
    // Playback position in samples, 32.16.
    u64 position = 0;
    u32 step = 0;
    s32 adpcm_sample = 0, adpcm_index = 0, adpcm_at = -1;
    s32 adpcm_loop_sample = 0, adpcm_loop_index = 0;
    u16 noise = 0x7FFF;
    u32 phase_counter = 0;
    s16 current = 0;
    float gain_left = 0, gain_right = 0;
};

struct Track {
    bool active = false, note_wait = true, porta = false, tie = false, compare = true, mute = false;
    u8 player = 0, priority = 0x40, volume = 127, expression = 127, bend_range = 2;
    s8 pan = 0, ext_pan = 0, transpose = 0, bend = 0;
    s16 ext_pitch = 0;
    u16 program = 0;
    s32 fader = 0;
    u32 start = 0, pc = 0, base = 0;
    u32 call_stack[3] = {};
    u8 loop_count[3] = {};
    u8 stack_depth = 0;
    s32 wait = 0;
    u8 porta_key = 60, porta_time = 0;
    s16 sweep_pitch = 0;
    u8 attack = 255, decay = 255, sustain = 255, release = 255;
    u8 lfo_type = 0, lfo_depth = 0, lfo_range = 1, lfo_speed = 16;
    u16 lfo_delay = 0;
    u16 channel_mask = 0;
    Channel* channels[kChannels] = {};
    u8 channel_count = 0;
};

// The first 0x24 bytes are the same fields the game writes by offset, so plain bytes keep it exact.
struct Player {
    u8 bytes[0x24] = {};
    u8 tracks[16];
    bool active = false, prepared = false, paused = false;
    u32 ticks = 0;
    u16 allocatable = 0;

    u8 priority() const { return bytes[4]; }
    u8 volume() const { return bytes[5]; }
    s16 fader() const { s16 v; std::memcpy(&v, bytes + 6, 2); return v; }
    u16 tempo() const { u16 v; std::memcpy(&v, bytes + 0x18, 2); return v; }
    u16 tempo_ratio() const { u16 v; std::memcpy(&v, bytes + 0x1A, 2); return v; }
    s32 counter = 240;
    u32 bank = 0;
    void set_tempo(u16 v) { std::memcpy(bytes + 0x18, &v, 2); }
    void set_ratio(u16 v) { std::memcpy(bytes + 0x1A, &v, 2); }
    void set_volume(u8 v) { bytes[5] = v; }
    void set_priority(u8 v) { bytes[4] = v; }
    void set_fader(s16 v) { std::memcpy(bytes + 6, &v, 2); }
};

Player g_players[kPlayers];
Track g_tracks[kTracks];
Channel g_channels[kChannels];
u16 g_channel_lock = 0;
u32 g_shared = 0;
u32 g_line_counter = 0;
u32 g_random = 0x12345;
SoundStats g_stats;
std::vector<s16> g_samples;
u32 g_sample_rest = 0;

// Sequence data has no alignment, while rd16 rounds an address down as the ARM's halfword read does.
u16 read_u16(u32 address) { return u16(rd8(address) | (rd8(address + 1) << 8)); }

u32 next_random() {
    g_random = g_random * 0x5EEDu + 1;
    return g_random >> 16;
}

s32 sine_at(u32 index) {  // index 0..127 over a full wave; result -127..127
    index &= 127;
    if (index < 32) return g_sine[index];
    if (index < 64) return g_sine[64 - index];
    if (index < 96) return -s32(g_sine[index - 64]);
    return -s32(g_sine[128 - index]);
}

// --- Channels ---

s32 attack_rate(u32 value) {
    if (value >= 109) return g_attack_table[127 - std::min(value, 127u)];
    return s32(255 - value);
}

s32 decay_rate(u32 value) {
    if (value == 127) return 0xFFFF;
    if (value == 126) return 0x3C00;
    if (value < 50) return s32(value * 2 + 1);
    return s32(0x1E00 / (126 - value));
}

void free_channel(Channel& ch) {
    if (ch.track) {
        Track& t = *ch.track;
        for (u32 i = 0; i < t.channel_count; ++i) {
            if (t.channels[i] == &ch) {
                t.channels[i] = t.channels[--t.channel_count];
                break;
            }
        }
    }
    ch.track = nullptr;
    ch.active = false;
}

Channel* allocate_channel(u16 allowed, u8 priority, Kind kind) {
    static constexpr u16 kPsgMask = 0x3F00, kNoiseMask = 0xC000;
    if (kind == Kind::Psg) allowed &= kPsgMask;
    if (kind == Kind::Noise) allowed &= kNoiseMask;
    allowed &= u16(~g_channel_lock);
    Channel* best = nullptr;
    // Channel search order on the hardware driver starts with the channels PCM does not need most.
    static constexpr u8 kOrder[16] = {4, 5, 6, 7, 2, 0, 3, 1, 8, 9, 10, 11, 14, 12, 15, 13};
    for (u8 index : kOrder) {
        if (!(allowed & (1u << index))) continue;
        Channel& ch = g_channels[index];
        if (best == nullptr) {
            best = &ch;
            continue;
        }
        if (ch.priority < best->priority
            || (ch.priority == best->priority && ch.envelope + ch.volume < best->envelope + best->volume)) {
            best = &ch;
        }
    }
    if (best == nullptr) return nullptr;
    if (best->active && best->priority > priority) return nullptr;
    if (best->active) free_channel(*best);
    return best;
}

s32 decode_adpcm_step(u32 nibble, s32& predictor, s32& index) {
    static constexpr s8 kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    static const s16 kStepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
        97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
        4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
        18500, 20350, 22385, 24623, 27086, 29794, 32767};
    const s32 step = kStepTable[index];
    s32 diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    predictor += (nibble & 8) ? -diff : diff;
    predictor = std::clamp(predictor, -32768, 32767);
    index = std::clamp(index + kIndexTable[nibble & 7], 0, 88);
    return predictor;
}

// The sample at the channel's position; false when a one-shot wave has ended.
bool read_sample(Channel& ch, s32& out) {
    if (ch.format == 2) {  // IMA-ADPCM with a 4-byte header
        s32 want = s32(ch.position >> 16);
        const s32 total = s32(ch.loop_start + ch.loop_length);
        if (want >= total) {
            if (!ch.loop) return false;
            want = s32(ch.loop_start) + (want - total) % s32(std::max(1u, ch.loop_length));
            ch.position = (u64(u32(want)) << 16) | (ch.position & 0xFFFF);
            ch.adpcm_at = s32(ch.loop_start);
            ch.adpcm_sample = ch.adpcm_loop_sample;
            ch.adpcm_index = ch.adpcm_loop_index;
        }
        if (ch.adpcm_at < 0) {
            ch.adpcm_sample = s16(read_u16(ch.data));
            ch.adpcm_index = std::clamp(s32(rd8(ch.data + 2)), 0, 88);
            ch.adpcm_at = 0;
        }
        while (ch.adpcm_at < want) {
            const u32 where = u32(ch.adpcm_at);
            const u8 byte = rd8(ch.data + 4 + where / 2);
            const u32 nibble = (where & 1) ? byte >> 4 : byte & 15u;
            decode_adpcm_step(nibble, ch.adpcm_sample, ch.adpcm_index);
            ++ch.adpcm_at;
            if (u32(ch.adpcm_at) == ch.loop_start) {
                ch.adpcm_loop_sample = ch.adpcm_sample;
                ch.adpcm_loop_index = ch.adpcm_index;
            }
        }
        out = ch.adpcm_sample;
        return true;
    }
    const u32 bytes = ch.format == 1 ? 2 : 1;
    u64 total = ch.loop_start + ch.loop_length;
    if ((ch.position >> 16) >= total) {
        if (!ch.loop || ch.loop_length == 0) return false;
        ch.position -= u64(ch.loop_length) << 16;
    }
    const u32 at = ch.data + u32(ch.position >> 16) * bytes;
    out = ch.format == 1 ? s32(s16(read_u16(at))) : s32(s8(rd8(at))) * 256;
    return true;
}

void mix_channel(Channel& ch, float& left, float& right) {
    s32 sample = 0;
    switch (ch.kind) {
        case Kind::Pcm:
            if (!read_sample(ch, sample)) {
                free_channel(ch);
                return;
            }
            break;
        case Kind::Psg:
            sample = (((ch.phase_counter >> 13) & 7) <= ch.duty) ? 32767 : -32767;
            ch.phase_counter += ch.step;
            break;
        case Kind::Noise:
            ch.phase_counter += ch.step;
            while (ch.phase_counter >= 0x10000) {
                ch.phase_counter -= 0x10000;
                const u32 bit = ch.noise & 1;
                ch.noise >>= 1;
                if (bit) ch.noise ^= 0x6000;
            }
            sample = (ch.noise & 1) ? -32767 : 32767;
            break;
    }
    if (ch.kind == Kind::Pcm) ch.position += ch.step;
    left += float(sample) * ch.gain_left;
    right += float(sample) * ch.gain_right;
}

void update_channel(Channel& ch) {
    if (!ch.active) return;
    if (ch.started) {
        ch.started = false;
    }
    // Envelope.
    if (ch.phase == Phase::Attack) {
        ch.envelope = -((-ch.envelope * ch.attack) >> 8);
        if (ch.envelope == 0) ch.phase = Phase::Decay;
    } else if (ch.phase == Phase::Decay) {
        ch.envelope -= ch.decay;
        if (ch.envelope <= ch.sustain) {
            ch.envelope = ch.sustain;
            ch.phase = Phase::Sustain;
        }
    } else if (ch.phase == Phase::Release) {
        ch.envelope -= ch.release;
    }
    if (ch.envelope <= kEnvelopeSilent) {
        free_channel(ch);
        return;
    }
    s32 pitch = (s32(ch.key) - s32(ch.base_key)) * 64 + ch.pitch;
    s32 volume = s32(g_decibel_square[ch.velocity]) + (ch.envelope >> 7) + ch.volume;
    s32 pan = ch.pan;
    // Sweep from a portamento.
    if (ch.sweep_pitch != 0 && ch.sweep_counter < ch.sweep_length) {
        const s64 remaining = s64(ch.sweep_pitch) * (ch.sweep_length - ch.sweep_counter);
        pitch += s32(remaining / ch.sweep_length);
        if (ch.auto_sweep) ++ch.sweep_counter;
    }
    // Modulation.
    if (ch.lfo_depth != 0) {
        if (ch.lfo_delay_counter < ch.lfo_delay) {
            ++ch.lfo_delay_counter;
        } else {
            const s32 value = sine_at(u32(ch.lfo_counter >> 8)) * ch.lfo_depth * ch.lfo_range;
            if (ch.lfo_type == 0) pitch += (value * 60) >> 14;
            else if (ch.lfo_type == 1) volume += (value * 60) >> 14;
            else pan += (value * 64) >> 14;
            ch.lfo_counter = (ch.lfo_counter + (s32(ch.lfo_speed) << 6)) & 0x7FFF;
        }
    }
    // Frequency, in whole numbers only.
    const s32 octaves = pitch >= 0 ? pitch / s32(kPitchSteps) : -((-pitch + s32(kPitchSteps) - 1) / s32(kPitchSteps));
    const u32 fraction = u32(pitch - octaves * s32(kPitchSteps));
    u64 hz = (u64(ch.rate) * g_pitch_ratio[fraction]) >> 16;
    if (octaves >= 0) hz <<= std::min(octaves, 16);
    else hz >>= std::min(-octaves, 31);
    ch.step = u32(std::min<u64>((hz << 16) / kSoundRate, 0x7FFFFFFF));

    const float amp = volume <= -723 ? 0.0f : std::pow(10.0f, float(volume) / 200.0f);
    const s32 final_pan = std::clamp(s32(ch.pan_inst) - 64 + pan + 64, 0, 127);
    ch.gain_left = amp * float(127 - final_pan) / 127.0f;
    ch.gain_right = amp * float(final_pan) / 127.0f;
}

// --- Tracks ---

u32 read_var_length(Track& t) {
    u32 value = 0;
    for (;;) {
        const u8 byte = rd8(t.pc++);
        value = (value << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) return value;
    }
}

s16 read_variable(const Track& t, u32 index) {
    if (g_shared == 0) return 0;
    if (index < 16) return s16(read_u16(g_shared + 0x20 + t.player * 0x24 + index * 2));
    if (index < 32) return s16(read_u16(g_shared + 0x260 + (index - 16) * 2));
    return 0;
}

void write_variable(const Track& t, u32 index, s16 value) {
    if (g_shared == 0) return;
    if (index < 16) wr16(g_shared + 0x20 + t.player * 0x24 + index * 2, u16(value));
    else if (index < 32) wr16(g_shared + 0x260 + (index - 16) * 2, u16(value));
}

void release_track_channels(Track& t, s32 release_override) {
    for (u32 i = 0; i < t.channel_count; ++i) {
        Channel& ch = *t.channels[i];
        ch.phase = Phase::Release;
        ch.priority = 1;
        if (release_override >= 0) ch.release = decay_rate(u32(release_override));
    }
}

void stop_track(Track& t, bool hard) {
    if (hard) {
        while (t.channel_count > 0) free_channel(*t.channels[0]);
    } else {
        release_track_channels(t, -1);
    }
    t.active = false;
}

struct Wave {
    u8 format = 0;
    bool loop = false;
    u32 rate = 0, data = 0, loop_start = 0, loop_length = 0;
};

bool find_wave(const Player& player, u32 swar, u32 swav, Wave& out) {
    const u32 arc = rd32(player.bank + 0x18 + swar * 8);
    if (arc == 0 || (arc >> 24) != 2) return false;
    if (swav >= rd32(arc + 0x38)) return false;
    const u32 offset = rd32(arc + 0x3C + swav * 4);
    if (offset == 0) return false;
    // Waves loaded one by one sit elsewhere; their entry is then the address itself.
    const u32 w = offset >= 0x02000000 ? offset : arc + offset;
    out.format = rd8(w);
    out.loop = rd8(w + 1) != 0;
    out.rate = read_u16(w + 2);
    const u32 loop_words = read_u16(w + 6), length_words = rd32(w + 8);
    out.data = w + 12;
    if (out.format == 2) {
        out.loop_start = loop_words > 0 ? (loop_words - 1) * 8 : 0;
        out.loop_length = length_words * 8;
    } else if (out.format == 1) {
        out.loop_start = loop_words * 2;
        out.loop_length = length_words * 2;
    } else {
        out.loop_start = loop_words * 4;
        out.loop_length = length_words * 4;
    }
    return true;
}

// Finds the instrument for a key: type, and the address of its 10 bytes of parameters.
bool find_instrument(const Player& player, u32 program, u32 key, u32& type, u32& params) {
    if (player.bank == 0 || (player.bank >> 24) != 2) return false;
    if (program >= rd32(player.bank + 0x38)) return false;
    const u32 entry = rd32(player.bank + 0x3C + program * 4);
    type = entry & 0xFF;
    u32 where = player.bank + (entry >> 8);
    if (type == 16) {  // drum set: one instrument per key
        const u32 low = rd8(where), high = rd8(where + 1);
        if (key < low || key > high) return false;
        const u32 item = where + 2 + (key - low) * 12;
        type = read_u16(item);
        where = item + 2;
    } else if (type == 17) {  // key split
        u32 index = 0;
        while (index < 8 && key > rd8(where + index)) ++index;
        if (index >= 8) return false;
        const u32 item = where + 8 + index * 12;
        type = read_u16(item);
        where = item + 2;
    }
    params = where;
    return type != 0;
}

void start_note(Player& player, Track& t, u32 key, u32 velocity, s32 length) {
    u32 type = 0, params = 0;
    Channel* ch = nullptr;
    if (t.tie && t.channel_count > 0) {
        ch = t.channels[0];
        ch->key = u8(key);
        ch->velocity = u8(velocity);
    } else {
        if (!find_instrument(player, t.program, key, type, params)) return;
        const Kind kind = type == 2 ? Kind::Psg : type == 3 ? Kind::Noise : Kind::Pcm;
        const u8 priority = u8(std::clamp(s32(player.priority()) + s32(t.priority), 0, 255));
        ch = allocate_channel(t.channel_mask & player.allocatable, priority, kind);
        if (ch == nullptr) return;
        Wave wave;
        if (type == 1 && !find_wave(player, read_u16(params + 2), read_u16(params), wave)) {
            return;
        }
        *ch = Channel{};
        ch->active = true;
        ch->started = true;
        ch->kind = kind;
        ch->priority = priority;
        ch->track = &t;
        ch->key = u8(key);
        ch->velocity = u8(velocity);
        ch->base_key = rd8(params + 2 + 2);
        ch->attack = attack_rate(t.attack != 255 ? t.attack : rd8(params + 5));
        ch->decay = decay_rate(t.decay != 255 ? t.decay : rd8(params + 6));
        ch->sustain = s32(g_decibel_square[(t.sustain != 255 ? t.sustain : rd8(params + 7)) & 127]) << 7;
        ch->release = decay_rate(t.release != 255 ? t.release : rd8(params + 8));
        ch->pan_inst = rd8(params + 9);
        ch->envelope = kEnvelopeSilent;
        ch->phase = Phase::Attack;
        if (kind == Kind::Pcm) {
            ch->data = wave.data;
            ch->format = wave.format;
            ch->loop = wave.loop;
            ch->rate = wave.rate;
            ch->loop_start = wave.loop_start;
            ch->loop_length = wave.loop_length;
        } else if (kind == Kind::Psg) {
            ch->duty = u8(read_u16(params)) & 7;
            ch->rate = 440;  // base key 69 sounds at 440 Hz
        } else {
            ch->rate = 440 * 8;
        }
        t.channels[t.channel_count++] = ch;
        ++g_stats.notes;
    }
    ch->length = length;
    ch->timed = length > 0;
    ch->lfo_type = t.lfo_type;
    ch->lfo_depth = t.lfo_depth;
    ch->lfo_range = t.lfo_range;
    ch->lfo_speed = t.lfo_speed;
    ch->lfo_delay = t.lfo_delay;
    ch->sweep_pitch = t.sweep_pitch;
    if (t.porta) ch->sweep_pitch += (s32(t.porta_key) - s32(key)) * 64;
    if (t.porta_time == 0) {
        ch->sweep_length = length;
        ch->auto_sweep = false;
    } else {
        const s32 span = s32(t.porta_time) * t.porta_time;
        ch->sweep_length = s32((s64(span) * std::abs(ch->sweep_pitch)) >> 11);
        ch->auto_sweep = true;
    }
    ch->sweep_counter = 0;
}

// Applies the track's settings to its notes, once for each driver tick.
void update_track_channels(Player& player, Track& t) {
    s32 volume = s32(g_decibel_square[t.volume & 127]) + s32(g_decibel_square[t.expression & 127])
                 + s32(g_decibel_square[player.volume() & 127]) + t.fader + player.fader();
    const s32 pitch = (s32(t.bend) * s32(t.bend_range)) * 64 / 128 + t.ext_pitch;
    const s32 pan = t.pan + t.ext_pan;
    for (u32 i = 0; i < t.channel_count; ++i) {
        Channel& ch = *t.channels[i];
        ch.volume = t.mute ? -723 : volume;
        ch.pitch = pitch;
        ch.pan = pan;
        ch.lfo_depth = t.lfo_depth;
        ch.lfo_speed = t.lfo_speed;
        ch.lfo_range = t.lfo_range;
        ch.lfo_type = t.lfo_type;
        // A note given a length ends when that runs out; one without a length holds until stopped.
        if (ch.timed && ch.length == 0 && ch.phase != Phase::Release && !t.tie) {
            ch.phase = Phase::Release;
            ch.priority = 1;
        }
    }
}

Track* allocate_track() {
    for (Track& t : g_tracks) {
        if (!t.active) return &t;
    }
    return nullptr;
}

void init_track(Track& t, u8 player, u32 start) {
    t = Track{};
    t.active = true;
    t.player = player;
    t.start = start;
    t.pc = start;
    t.channel_mask = 0xFFFF;
}

// Runs one tick of a track. Returns true when it has ended.
bool step_track(Player& player, Track& t, u32 track_index) {
    for (u32 i = 0; i < t.channel_count; ++i) {
        Channel& ch = *t.channels[i];
        if (ch.length > 0) --ch.length;
        if (!ch.auto_sweep && ch.sweep_counter < ch.sweep_length) ++ch.sweep_counter;
    }
    if (t.wait == -1) {  // a note without length: wait until it has finished
        if (t.channel_count > 0) return false;
        t.wait = 0;
    } else if (t.wait > 0) {
        --t.wait;
        if (t.wait > 0) return false;
    }

    u32 guard = 0;
    while (t.wait == 0) {
        if (++guard > 20000) return true;  // a track that never waits would hang the driver
        bool run = true;
        u8 cmd = rd8(t.pc++);
        if (cmd == 0xA2) {  // if: the next command only runs when the last compare held
            cmd = rd8(t.pc++);
            run = t.compare;
        }
        s32 arg_mode = 0;   // 0 from the command, 1 random, 2 variable
        if (cmd == 0xA0) { arg_mode = 1; cmd = rd8(t.pc++); }
        else if (cmd == 0xA1) { arg_mode = 2; cmd = rd8(t.pc++); }

        const auto read_arg = [&](s32 normal) -> s32 {
            if (arg_mode == 1) {
                const s32 low = s16(read_u16(t.pc));
                const s32 high = s16(read_u16(t.pc + 2));
                t.pc += 4;
                return low + s32((next_random() * u32(high - low + 1)) >> 16);
            }
            if (arg_mode == 2) return read_variable(t, rd8(t.pc++));
            if (normal == 0) return rd8(t.pc++);
            if (normal == 1) {
                const s32 v = s16(read_u16(t.pc));
                t.pc += 2;
                return v;
            }
            return s32(read_var_length(t));
        };
        const auto read_offset = [&]() {
            const u32 v = read_u16(t.pc) | (u32(rd8(t.pc + 2)) << 16);
            t.pc += 3;
            return t.base + v;
        };

        if (cmd < 0x80) {  // a note
            const u32 velocity = rd8(t.pc++) & 127;
            const s32 length = read_arg(2);
            if (!run) continue;
            const u32 key = u32(std::clamp(s32(cmd) + t.transpose, 0, 127));
            if (!t.mute) start_note(player, t, key, velocity, length);
            t.porta_key = u8(key);
            if (t.note_wait) t.wait = length > 0 ? length : -1;
            continue;
        }
        const u32 high = cmd & 0xF0;
        if (high == 0xB0) {  // variables
            const u32 index = rd8(t.pc++);
            const s32 value = read_arg(1);
            if (!run) continue;
            const s32 current = read_variable(t, index);
            switch (cmd) {
                case 0xB0: write_variable(t, index, s16(value)); break;
                case 0xB1: write_variable(t, index, s16(current + value)); break;
                case 0xB2: write_variable(t, index, s16(current - value)); break;
                case 0xB3: write_variable(t, index, s16(current * value)); break;
                case 0xB4: if (value != 0) write_variable(t, index, s16(current / value)); break;
                case 0xB5: write_variable(t, index, s16(value >= 0 ? current << value : current >> -value)); break;
                case 0xB6: {
                    const u32 r = next_random();
                    write_variable(t, index, s16(value < 0 ? -s32((r * u32(-value + 1)) >> 16)
                                                           : s32((r * u32(value + 1)) >> 16)));
                    break;
                }
                case 0xB8: t.compare = current == value; break;
                case 0xB9: t.compare = current >= value; break;
                case 0xBA: t.compare = current > value; break;
                case 0xBB: t.compare = current <= value; break;
                case 0xBC: t.compare = current < value; break;
                case 0xBD: t.compare = current != value; break;
                default: break;
            }
            continue;
        }
        if (high == 0xE0) {  // 16-bit parameters
            const s32 value = read_arg(1);
            if (!run) continue;
            if (cmd == 0xE0) t.lfo_delay = u16(value);
            else if (cmd == 0xE1) player.set_tempo(u16(value));
            else if (cmd == 0xE3) t.sweep_pitch = s16(value);
            continue;
        }
        if (high == 0xC0 || (cmd >= 0xD0 && cmd <= 0xD6)) {  // 8-bit parameters
            const s32 value = read_arg(0);
            if (!run) continue;
            switch (cmd) {
                case 0xC0: t.pan = s8(value - 64); break;
                case 0xC1: t.volume = u8(value); break;
                case 0xC2: player.set_volume(u8(value)); break;
                case 0xC3: t.transpose = s8(value); break;
                case 0xC4: t.bend = s8(value); break;
                case 0xC5: t.bend_range = u8(value); break;
                case 0xC6: t.priority = u8(value); break;
                case 0xC7: t.note_wait = value & 1; break;
                case 0xC8:
                    t.tie = value & 1;
                    release_track_channels(t, -1);
                    break;
                case 0xC9:
                    t.porta_key = u8(value + t.transpose);
                    t.porta = true;
                    break;
                case 0xCA: t.lfo_depth = u8(value); break;
                case 0xCB: t.lfo_speed = u8(value); break;
                case 0xCC: t.lfo_type = u8(value); break;
                case 0xCD: t.lfo_range = u8(value); break;
                case 0xCE: t.porta = value & 1; break;
                case 0xCF: t.porta_time = u8(value); break;
                case 0xD0: t.attack = u8(value); break;
                case 0xD1: t.decay = u8(value); break;
                case 0xD2: t.sustain = u8(value); break;
                case 0xD3: t.release = u8(value); break;
                case 0xD4:
                    if (t.stack_depth < 3) {
                        t.call_stack[t.stack_depth] = t.pc;
                        t.loop_count[t.stack_depth] = u8(value);
                        ++t.stack_depth;
                    }
                    break;
                case 0xD5: t.expression = u8(value); break;
                default: break;
            }
            continue;
        }
        switch (cmd) {
            case 0x80: { const s32 v = read_arg(2); if (run) t.wait = v; break; }
            case 0x81: { const s32 v = read_arg(2); if (run) t.program = u16(v); break; }
            case 0x93: {
                const u32 index = rd8(t.pc++) & 15;
                const u32 start = read_offset();
                if (run) {
                    const u8 other = player.tracks[index];
                    if (other != 0xFF && g_tracks[other].active) {
                        g_tracks[other].pc = start;
                        g_tracks[other].base = t.base;
                        g_tracks[other].wait = 0;
                    }
                }
                break;
            }
            case 0x94: {
                const u32 target = read_offset();
                if (run) t.pc = target;
                break;
            }
            case 0x95: {
                const u32 target = read_offset();
                if (run && t.stack_depth < 3) {
                    t.call_stack[t.stack_depth] = t.pc;
                    t.loop_count[t.stack_depth] = 0;
                    ++t.stack_depth;
                    t.pc = target;
                }
                break;
            }
            case 0xFC:
                if (run && t.stack_depth > 0) {
                    const u32 top = t.stack_depth - 1u;
                    if (t.loop_count[top] == 0) {  // endless
                        t.pc = t.call_stack[top];
                    } else if (--t.loop_count[top] == 0) {
                        t.stack_depth = u8(top);
                    } else {
                        t.pc = t.call_stack[top];
                    }
                }
                break;
            case 0xFD:
                if (run && t.stack_depth > 0) {
                    --t.stack_depth;
                    t.pc = t.call_stack[t.stack_depth];
                }
                break;
            case 0xFE:
                t.pc += 2;
                break;
            case 0xFF:
                if (run) return true;
                break;
            default:
                break;
        }
    }
    (void)track_index;
    return false;
}

void stop_player(u32 id, bool hard) {
    Player& p = g_players[id];
    for (u8& index : p.tracks) {
        if (index != 0xFF) {
            stop_track(g_tracks[index], hard);
            index = 0xFF;
        }
    }
    p.active = false;
    p.prepared = false;
}

void prepare_player(u32 id, u32 data, u32 offset, u32 bank) {
    Player& p = g_players[id];
    if (p.active || p.prepared) stop_player(id, true);
    std::memset(p.bytes, 0, sizeof p.bytes);
    std::memset(p.tracks, 0xFF, sizeof p.tracks);
    p.bank = bank;
    p.set_tempo(0x78);
    p.set_ratio(0x100);
    p.counter = 240;
    p.set_volume(0x7F);
    p.set_fader(0);
    p.set_priority(0x40);
    p.ticks = 0;
    p.paused = false;
    p.allocatable = 0xFFFF;
    if (g_shared != 0) {
        wr32(g_shared + 0x20 + id * 0x24 + 0x20, 0);
        for (u32 i = 0; i < 16; ++i) wr16(g_shared + 0x20 + id * 0x24 + i * 2, 0xFFFF);
    }
    Track* first = allocate_track();
    if (first == nullptr) return;
    init_track(*first, u8(id), data + offset);
    first->base = data;
    p.tracks[0] = u8(first - g_tracks);
    if (rd8(first->pc) == 0xFE) {
        u32 mask = read_u16(first->pc + 1);
        first->pc += 3;
        for (u32 i = 1; i < 16; ++i) {
            if (!(mask & (1u << i))) continue;
            Track* other = allocate_track();
            if (other == nullptr) break;
            init_track(*other, u8(id), data + offset);
            other->base = data;
            other->wait = 1 << 30;  // waits until an open-track command gives it a start
            p.tracks[i] = u8(other - g_tracks);
        }
    }
    p.prepared = true;
}

void tick_player(u32 id) {
    Player& p = g_players[id];
    if (!p.active || p.paused) return;
    while (p.counter >= 240) {
        p.counter -= 240;
        bool any_active = false;
        for (u32 i = 0; i < 16; ++i) {
            const u8 index = p.tracks[i];
            if (index == 0xFF) continue;
            Track& t = g_tracks[index];
            if (!t.active) continue;
            if (step_track(p, t, index)) {
                stop_track(t, false);
                p.tracks[i] = 0xFF;
            } else {
                any_active = true;
            }
        }
        ++p.ticks;
        if (!any_active) {
            p.active = false;
            return;
        }
    }
    p.counter += (s32(p.tempo()) * s32(p.tempo_ratio())) >> 8;
}

void run_command(u32 id, u32 a0, u32 a1, u32 a2, u32 a3) {
    static bool warned[256];
    switch (id) {
        case 0x00:  // start
            if (a0 < kPlayers) {
                prepare_player(a0, a1, a2, a3);
                g_players[a0].active = g_players[a0].prepared;
            }
            break;
        case 0x01: if (a0 < kPlayers) stop_player(a0, false); break;
        case 0x02: if (a0 < kPlayers) prepare_player(a0, a1, a2, a3); break;
        case 0x03:
            if (a0 < kPlayers && g_players[a0].prepared) g_players[a0].active = true;
            break;
        case 0x04:
            for (u32 i = 0; i < kPlayers; ++i) {
                if (a0 & (1u << i)) g_players[i].paused = a1 != 0;
            }
            break;
        case 0x05:
            if (a0 < kPlayers) {
                Player& p = g_players[a0];
                for (u32 n = 0; n < a1 && p.active; ++n) {
                    p.counter = std::max(p.counter, 240);
                    tick_player(a0);
                }
            }
            break;
        case 0x06:  // writes a player's field by offset, as the driver does
            if (a0 < kPlayers && a1 + a3 <= sizeof g_players[a0].bytes) {
                std::memcpy(g_players[a0].bytes + a1, &a2, a3);
            }
            break;
        case 0x07: {
            // Writes a field of the tracks, by its offset in the driver's own track structure
            // (arm7.bin file offset 0x5AC4 shows the layout).
            const u32 player = a0 & 0xFFFFFF, size = a0 >> 24;
            if (player >= kPlayers) break;
            for (u32 i = 0; i < 16; ++i) {
                const u8 index = g_players[player].tracks[i];
                if (!(a1 & (1u << i)) || index == 0xFF) continue;
                Track& t = g_tracks[index];
                switch (a2) {
                    case 0x02: t.program = u16(a3); break;
                    case 0x04: t.volume = u8(a3); break;
                    case 0x05: t.expression = u8(a3); break;
                    case 0x06: t.bend = s8(a3); break;
                    case 0x07: t.bend_range = u8(a3); break;
                    case 0x08: t.pan = s8(a3); break;
                    case 0x09: t.ext_pan = s8(a3); break;
                    case 0x0A: t.fader = s16(a3); break;
                    case 0x0C: t.ext_pitch = s16(a3); break;
                    case 0x0E: t.attack = u8(a3); break;
                    case 0x0F: t.decay = u8(a3); break;
                    case 0x10: t.sustain = u8(a3); break;
                    case 0x11: t.release = u8(a3); break;
                    case 0x12: t.priority = u8(a3); break;
                    case 0x13: t.transpose = s8(a3); break;
                    case 0x14: t.porta_key = u8(a3); break;
                    case 0x15: t.porta_time = u8(a3); break;
                    case 0x16: t.sweep_pitch = s16(a3); break;
                    case 0x1E: t.channel_mask = u16(a3); break;
                    default:
                        ++g_stats.unknown_commands;
                        if (!warned[7]) {
                            warned[7] = true;
                            std::printf("track field %#x size %u not built\n", a2, size);
                        }
                        break;
                }
            }
            break;
        }
        case 0x08:
            if (a0 < kPlayers) {
                for (u32 i = 0; i < 16; ++i) {
                    const u8 index = g_players[a0].tracks[i];
                    if ((a1 & (1u << i)) && index != 0xFF) g_tracks[index].mute = a2 != 0;
                }
            }
            break;
        case 0x09:
            if (a0 < kPlayers) {
                g_players[a0].allocatable = u16(a2);
            }
            break;
        case 0x0A:
            if (g_shared != 0 && a0 < kPlayers) wr16(g_shared + 0x20 + a0 * 0x24 + a1 * 2, u16(a2));
            break;
        case 0x0B:
            if (g_shared != 0) wr16(g_shared + 0x260 + a0 * 2, u16(a1));
            break;
        case 0x18:  // nothing to do here: the game asks the driver to lock channels, none are locked
        case 0x1D:
            break;
        case 0x1E:
        case 0x1F:
        case 0x20: {
            // The game is about to free this memory range: nothing may play from it any more.
            for (Channel& ch : g_channels) {
                if (ch.active && ch.data >= a0 && ch.data < a1) free_channel(ch);
            }
            for (u32 i = 0; i < kPlayers; ++i) {
                Player& p = g_players[i];
                bool hit = p.bank >= a0 && p.bank < a1;
                for (const u8 index : p.tracks) {
                    if (index != 0xFF && g_tracks[index].pc >= a0 && g_tracks[index].pc < a1) hit = true;
                }
                if (hit && (p.active || p.prepared)) stop_player(i, true);
            }
            break;
        }
        default:
            ++g_stats.unknown_commands;
            if (id < 256 && !warned[id]) {
                warned[id] = true;
                std::printf("sound command %#x not built: %08x %08x %08x %08x\n", id, a0, a1, a2, a3);
            }
            break;
    }
}

void driver_tick() {
    for (u32 id = 0; id < kPlayers; ++id) tick_player(id);
    for (u32 id = 0; id < kPlayers; ++id) {
        Player& p = g_players[id];
        for (const u8 index : p.tracks) {
            if (index != 0xFF && g_tracks[index].active) update_track_channels(p, g_tracks[index]);
        }
    }
    u32 channel_status = 0, active_channels = 0;
    for (u32 i = 0; i < kChannels; ++i) {
        update_channel(g_channels[i]);
        if (g_channels[i].active) {
            channel_status |= 1u << i;
            ++active_channels;
        }
    }
    u32 player_status = 0, active_players = 0;
    for (u32 id = 0; id < kPlayers; ++id) {
        if (g_players[id].active) {
            player_status |= 1u << id;
            ++active_players;
        }
    }
    g_stats.active_channels = active_channels;
    g_stats.active_players = active_players;
    if (g_shared != 0) {
        wr32(g_shared + 4, player_status);
        wr16(g_shared + 8, u16(channel_status));
        for (u32 id = 0; id < kPlayers; ++id) {
            if (g_players[id].active) wr32(g_shared + 0x20 + id * 0x24 + 0x20, g_players[id].ticks);
        }
    }

    // Samples for this tick: 32768 Hz at 192 ticks per second.
    g_sample_rest += kSoundRate;
    const u32 count = g_sample_rest / 192;
    g_sample_rest %= 192;
    constexpr float kMaster = 0.5f;
    for (u32 n = 0; n < count; ++n) {
        float left = 0, right = 0;
        for (Channel& ch : g_channels) {
            if (ch.active) mix_channel(ch, left, right);
        }
        g_samples.push_back(s16(std::clamp(left * kMaster, -32768.0f, 32767.0f)));
        g_samples.push_back(s16(std::clamp(right * kMaster, -32768.0f, 32767.0f)));
    }
    if (g_samples.size() > kSoundRate * 4) {  // nobody is listening
        g_samples.clear();
    }
}

}  // namespace

void sound_init(const std::vector<u8>& rom) {
    u32 arm7_offset;
    std::memcpy(&arm7_offset, &rom[0x30], 4);
    const u8* arm7 = rom.data() + arm7_offset;
    std::memcpy(g_decibel, arm7 + kArm7DecibelTable, sizeof g_decibel);
    std::memcpy(g_decibel_square, arm7 + kArm7DecibelSquareTable, sizeof g_decibel_square);
    std::memcpy(g_sine, arm7 + kArm7SineTable, sizeof g_sine);
    std::memcpy(g_attack_table, arm7 + kArm7AttackTable, sizeof g_attack_table);
    // Each step multiplies by 2^(1/768), in 2.30 fixed point: whole numbers, so the same everywhere.
    constexpr u64 kStep = 0x400ECB37ull;  // 2^(1/768) * 2^30
    u64 value = u64(1) << 30;
    for (u32 i = 0; i < kPitchSteps; ++i) {
        g_pitch_ratio[i] = u32(value >> 14);
        value = (value * kStep) >> 30;
    }
    for (Player& p : g_players) std::memset(p.tracks, 0xFF, sizeof p.tracks);
    for (u32 i = 0; i < kChannels; ++i) g_channels[i] = Channel{};
}

void sound_receive(u32 data) {
    if (data == 0) return;
    for (u32 command = data; command != 0; command = rd32(command)) {
        const u32 id = rd32(command + 4);
        const u32 a0 = rd32(command + 8), a1 = rd32(command + 12);
        const u32 a2 = rd32(command + 16), a3 = rd32(command + 20);
        if (id == 0x1D) g_shared = a0;
        run_command(id, a0, a1, a2, a3);
    }
    if (g_shared == 0) runtime_abort("sound command before the shared block is known", data);
    wr32(g_shared, rd32(g_shared) + 1);
}

void sound_line() {
    // 263 lines at 59.8261 pictures per second: 81.95 lines per driver tick of 1/192 s.
    g_line_counter += 100;
    if (g_line_counter >= 8195) {
        g_line_counter -= 8195;
        driver_tick();
    }
}

void sound_take_samples(std::vector<s16>& out) {
    out.swap(g_samples);
    g_samples.clear();
}

const SoundStats& sound_stats() { return g_stats; }
