// s3eSound (24 PCM channels, guest GEN_AUDIO callbacks run on a second guest
// CPU in the SDL audio thread) and s3eAudio (MP3 music via minimp3).
#include <algorithm>
#include <SDL.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT_DISABLED
#include "minimp3_ex.h"

#include "config.h"
#include "hle.h"
#include "runtime.h"

namespace {
using namespace runtime;

constexpr int kChannels = 24;
constexpr int kOutFreq = 44100;
constexpr int kMaxFrames = 4096;

enum { CB_END_SAMPLE = 0, CB_GEN_AUDIO = 1, CB_STOP_AUDIO = 2, CB_GEN_AUDIO_STEREO = 3 };

struct Channel {
    bool active = false;
    bool paused = false;
    bool stop_pending = false;
    addr_t start = 0, loop_start = 0, end = 0;  // guest int16 sample pointers
    uint64_t pos = 0;                            // position in samples, 32.32 fixed point from `loop_base`
    addr_t base = 0;                             // pointer `pos` is relative to
    int32_t repeat = 0;                          // remaining plays, 0 = forever
    int32_t rate = 11000;
    int32_t volume = 0x100;
    int32_t uservar = 0;
    addr_t cb_fn[4] = {};
    addr_t cb_user[4] = {};
};

std::recursive_mutex g_mutex;
Channel g_ch[kChannels];
int32_t g_master = 0x100;
int32_t g_default_freq = 11000;
addr_t g_gen_audio_seen = 0;
SDL_AudioDeviceID g_dev = 0;
std::atomic<bool> g_paused{false};

// Guest-visible scratch memory for the audio thread.
Cpu *g_audio_cpu;
addr_t g_mix_buf;   // int16 mono
addr_t g_info_buf;  // s3eSoundGenAudioInfo / s3eSoundEndSampleInfo

// --- music ---------------------------------------------------------------------
struct Music {
    std::mutex mutex;
    std::unique_ptr<mp3dec_ex_t> dec;
    bool playing = false;
    bool paused = false;
    int32_t repeat = 0;  // total plays, 0 = forever
    int32_t plays = 0;
    int32_t volume = 0x100;
    uint64_t frames_played = 0;
    int hz = 44100, channels = 2;
    double frac = 0;
    int16_t last[2] = {0, 0}, next[2] = {0, 0};
    bool have_next = false;
    std::string path;
} g_music;

bool music_frame(int16_t out[2]) {
    Music &m = g_music;
    for (int attempt = 0; attempt < 2; ++attempt) {
        int16_t buf[2];
        size_t n = mp3dec_ex_read(m.dec.get(), buf, m.channels);
        if (n == static_cast<size_t>(m.channels)) {
            out[0] = buf[0];
            out[1] = m.channels > 1 ? buf[1] : buf[0];
            return true;
        }
        ++m.plays;
        if (m.repeat == 0 || m.plays < m.repeat) {
            mp3dec_ex_seek(m.dec.get(), 0);
        } else {
            m.playing = false;
            return false;
        }
    }
    m.playing = false;
    return false;
}

void mix_music(int16_t *stereo, int frames) {
    Music &m = g_music;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.playing || m.paused || !m.dec) return;
    int eff = m.volume * config::get_int("s3e", "AudioVolScale", 100) / 100;
    double step = double(m.hz) / kOutFreq;
    for (int i = 0; i < frames && m.playing; ++i) {
        m.frac += step;
        while (m.frac >= 1.0) {
            m.last[0] = m.next[0];
            m.last[1] = m.next[1];
            if (!music_frame(m.next)) break;
            m.frac -= 1.0;
        }
        double t = m.frac;
        for (int c = 0; c < 2; ++c) {
            int v = static_cast<int>(m.last[c] + (m.next[c] - m.last[c]) * t) * eff >> 8;
            int s = stereo[i * 2 + c] + v;
            stereo[i * 2 + c] = static_cast<int16_t>(std::clamp(s, -32768, 32767));
        }
    }
    m.frames_played += frames;
}

// --- channels -------------------------------------------------------------------
Cpu &audio_cpu() {
    if (!g_audio_cpu) g_audio_cpu = new Cpu("audio");
    return *g_audio_cpu;
}

void fire_stop(Cpu &cpu, int ch) {
    Channel &c = g_ch[ch];
    c.active = false;
    c.stop_pending = false;
    if (c.cb_fn[CB_STOP_AUDIO]) {
        uint32_t info[4] = {static_cast<uint32_t>(ch), 0, 0, 0};
        memcpy(gptr(g_info_buf), info, sizeof(info));
        cpu.call(c.cb_fn[CB_STOP_AUDIO], {g_info_buf, c.cb_user[CB_STOP_AUDIO]});
    }
}

// End of sample reached: returns false if the channel stopped.
bool end_of_sample(Cpu &cpu, int ch) {
    Channel &c = g_ch[ch];
    if (c.cb_fn[CB_END_SAMPLE]) {
        int32_t reps = c.repeat == 0 ? -1 : c.repeat - 1;
        uint32_t info[4] = {static_cast<uint32_t>(ch), static_cast<uint32_t>(reps), 0, 0};
        memcpy(gptr(g_info_buf), info, sizeof(info));
        uint32_t keep = cpu.call(c.cb_fn[CB_END_SAMPLE], {g_info_buf, c.cb_user[CB_END_SAMPLE]});
        if (!keep) {
            fire_stop(cpu, ch);
            return false;
        }
        addr_t new_data = guest::read32(g_info_buf + 8);
        uint32_t new_len = guest::read32(g_info_buf + 12);
        if (new_data) {
            c.start = c.loop_start = new_data;
            c.end = new_data + new_len * 2;
        }
        c.base = c.loop_start;
        c.pos = 0;
        if (c.repeat > 0) --c.repeat;
        return true;
    }
    if (c.repeat == 1) {
        fire_stop(cpu, ch);
        return false;
    }
    if (c.repeat > 1) --c.repeat;
    c.base = c.loop_start;
    c.pos = 0;
    return true;
}

void mix_channel(Cpu &cpu, int ch, int16_t *mix, int frames, bool &mixed) {
    Channel &c = g_ch[ch];
    if (c.cb_fn[CB_GEN_AUDIO]) {
        int16_t *target = mix;
        uint32_t remaining = frames;
        for (int guard = 0; remaining && c.active && guard < 64; ++guard) {
            struct {
                int32_t channel;
                uint32_t target;
                uint32_t num_samples;
                uint32_t mix;
                uint32_t orig_start;
                uint32_t orig_num_samples;
                int32_t orig_repeat;
                uint8_t end_sample;
                uint8_t stereo;
                uint8_t pad[2];
            } info = {ch,
                      gaddr(target),
                      remaining,
                      mixed ? 1u : 0u,
                      c.base,
                      c.end > c.base ? (c.end - c.base) / 2 : 0,
                      c.repeat,
                      0,
                      0,
                      {0, 0}};
            memcpy(gptr(g_info_buf), &info, sizeof(info));
            uint32_t ret = cpu.call(c.cb_fn[CB_GEN_AUDIO], {g_info_buf, c.cb_user[CB_GEN_AUDIO]});
            bool end = gptr_t<uint8_t>(g_info_buf)[0x1c] != 0;
            ret = std::min(ret, remaining);
            if (ret) mixed = true;
            target += ret;
            remaining -= ret;
            if (end) {
                if (!end_of_sample(cpu, ch)) break;
            } else if (!ret) {
                break;
            }
        }
        return;
    }
    // Plain PCM with linear interpolation.
    const int16_t *samples = gptr_t<int16_t>(c.base);
    uint64_t step = (static_cast<uint64_t>(c.rate) << 32) / kOutFreq;
    for (int i = 0; i < frames && c.active; ++i) {
        uint32_t idx = static_cast<uint32_t>(c.pos >> 32);
        uint32_t len = (c.end - c.base) / 2;
        if (idx >= len) {
            uint32_t overflow = idx - len;
            if (!end_of_sample(cpu, ch)) break;
            samples = gptr_t<int16_t>(c.base);
            c.pos += static_cast<uint64_t>(overflow) << 32;
            len = (c.end - c.base) / 2;
            idx = static_cast<uint32_t>(c.pos >> 32);
            if (idx >= len) continue;
        }
        int32_t a = samples[idx];
        int32_t b = idx + 1 < len ? samples[idx + 1] : a;
        int32_t frac = static_cast<int32_t>((c.pos >> 20) & 0xfff);
        int32_t v = (a + ((b - a) * frac >> 12)) * c.volume >> 8;
        mix[i] = static_cast<int16_t>(std::clamp(mix[i] + v, -32768, 32767));
        mixed = true;
        c.pos += step;
    }
}

void audio_callback(void *, Uint8 *stream, int len) {
    int frames = len / 4;
    memset(stream, 0, len);
    auto *out = reinterpret_cast<int16_t *>(stream);
    Cpu &cpu = audio_cpu();
    while (frames > 0) {
        int n = std::min(frames, kMaxFrames);
        int16_t *mix = gptr_t<int16_t>(g_mix_buf);
        memset(mix, 0, n * sizeof(int16_t));
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            for (int ch = 0; ch < kChannels; ++ch)
                if (g_ch[ch].stop_pending) fire_stop(cpu, ch);
            bool mixed = false;
            for (int ch = kChannels - 1; ch >= 0; --ch)
                if (g_ch[ch].active && !g_ch[ch].paused) mix_channel(cpu, ch, mix, n, mixed);
            for (int i = 0; i < n; ++i) {
                int16_t v = static_cast<int16_t>(mix[i] * g_master >> 8);
                out[i * 2] = out[i * 2 + 1] = v;
            }
        }
        mix_music(out, n);
        out += n * 2;
        frames -= n;
    }
}

Channel *channel(int32_t ch) {
    if (ch < 0 || ch >= kChannels) {
        set_error(DEV_SOUND, 1);
        return nullptr;
    }
    return &g_ch[ch];
}

// --- s3eSound API ------------------------------------------------------------------
int32_t s3eSoundGetInt(int32_t prop) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    switch (prop) {
    case 0: return g_master;
    case 1: return kOutFreq;
    case 2: return g_default_freq;
    case 3: return kChannels;
    case 4: {
        uint32_t used = 0;
        for (int i = 0; i < kChannels; ++i)
            if (g_ch[i].active || g_ch[i].stop_pending) used |= 1u << i;
        return static_cast<int32_t>(used | 0xff000000u);
    }
    case 5: return 1;
    case 6: return config::get_int("s3e", "SoundVolDefault", 0x100);
    case 7: return 0;
    default: set_error(DEV_SOUND, 1); return -1;
    }
}
HLE_REGISTER(s3eSoundGetInt);

int32_t s3eSoundSetInt(int32_t prop, int32_t value) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (prop == 0) {
        g_master = std::clamp(value, 0, 0x100);
        return S3E_RESULT_SUCCESS;
    }
    if (prop == 2 && value <= 0x40000) {
        g_default_freq = value;
        for (Channel &c : g_ch)
            if (!c.active) c.rate = value;
        return S3E_RESULT_SUCCESS;
    }
    set_error(DEV_SOUND, 1);
    return S3E_RESULT_ERROR;
}
HLE_REGISTER(s3eSoundSetInt);

int32_t s3eSoundGetFreeChannel() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (int i = 0; i < kChannels; ++i)
        if (!g_ch[i].active && !g_ch[i].stop_pending) return i;
    return -1;
}
HLE_REGISTER(s3eSoundGetFreeChannel);

void s3eSoundStopAllChannels() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (Channel &c : g_ch) c.stop_pending = true;
}
HLE_REGISTER(s3eSoundStopAllChannels);

int32_t s3eSoundChannelPlay(int32_t ch, addr_t samples, uint32_t num, int32_t repeat, int32_t loop_from) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Channel *c = channel(ch);
    if (!c) return S3E_RESULT_ERROR;
    if (!samples || !num) {
        set_error(DEV_SOUND, 1);
        return S3E_RESULT_ERROR;
    }
    c->start = c->base = samples;
    c->end = samples + num * 2;
    c->loop_start = samples + std::clamp<uint32_t>(loop_from, 0, num) * 2;
    c->pos = 0;
    c->repeat = repeat < 0 ? 0 : repeat;
    c->active = true;
    c->stop_pending = false;
    c->paused = false;
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eSoundChannelPlay);

int32_t s3eSoundChannelStop(int32_t ch) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Channel *c = channel(ch);
    if (!c) return S3E_RESULT_ERROR;
    c->stop_pending = true;
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eSoundChannelStop);

int32_t s3eSoundChannelRegister(int32_t ch, int32_t cbid, addr_t fn, addr_t user) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Channel *c = channel(ch);
    if (!c) return S3E_RESULT_ERROR;
    if (!fn || cbid < 0 || cbid > 3) {
        set_error(DEV_SOUND, 1);
        return S3E_RESULT_ERROR;
    }
    if (cbid == CB_GEN_AUDIO) g_gen_audio_seen = fn;
    if (cbid == CB_GEN_AUDIO_STEREO) return S3E_RESULT_SUCCESS;  // mono output only
    c->cb_fn[cbid] = fn;
    c->cb_user[cbid] = user;
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eSoundChannelRegister);

int32_t s3eSoundChannelGetInt(int32_t ch, int32_t prop) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Channel *c = channel(ch);
    if (!c) return -1;
    switch (prop) {
    case 0: return static_cast<int32_t>((static_cast<int64_t>(c->rate) << 16) / std::max(g_default_freq, 1));
    case 1: return c->rate;
    case 2: return c->uservar;
    case 3: return c->volume;
    case 4: return c->active || c->stop_pending ? 1 : 0;
    case 5: return c->paused ? 1 : 0;
    default: set_error(DEV_SOUND, 1); return -1;
    }
}
HLE_REGISTER(s3eSoundChannelGetInt);

int32_t s3eSoundChannelSetInt(int32_t ch, int32_t prop, int32_t value) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Channel *c = channel(ch);
    if (!c) return S3E_RESULT_ERROR;
    switch (prop) {
    case 0: c->rate = std::min<int32_t>(static_cast<int32_t>((int64_t(value >> 8) * g_default_freq) / 256), 0x40000); break;
    case 1: c->rate = std::min(value, 0x40000); break;
    case 2: c->uservar = value; break;
    case 3: c->volume = std::clamp(value, 0, 0x100); break;
    default: set_error(DEV_SOUND, 1); return S3E_RESULT_ERROR;
    }
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eSoundChannelSetInt);

// --- s3eAudio API ------------------------------------------------------------------
int32_t s3eAudioPlay(const char *filename, uint32_t repeat) {
    if (!filename) {
        set_error(DEV_AUDIO, 1);
        return S3E_RESULT_ERROR;
    }
    std::string host;
    if (!files::host_path_for_read(filename, host)) {
        set_error(DEV_AUDIO, 6);
        return S3E_RESULT_ERROR;
    }
    auto dec = std::make_unique<mp3dec_ex_t>();
    if (mp3dec_ex_open(dec.get(), host.c_str(), MP3D_SEEK_TO_SAMPLE) != 0) {
        set_error(DEV_AUDIO, 1000);
        return S3E_RESULT_ERROR;
    }
    Music &m = g_music;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.dec) mp3dec_ex_close(m.dec.get());
    m.hz = dec->info.hz ? dec->info.hz : 44100;
    m.channels = dec->info.channels ? dec->info.channels : 2;
    m.dec = std::move(dec);
    m.repeat = static_cast<int32_t>(repeat);
    m.plays = 0;
    m.frames_played = 0;
    m.frac = 0;
    m.have_next = false;
    m.last[0] = m.last[1] = m.next[0] = m.next[1] = 0;
    m.playing = true;
    m.paused = false;
    m.path = host;
    logf("[audio] playing %s (%d Hz, %d ch, repeat %u)", filename, m.hz, m.channels, repeat);
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eAudioPlay);

void s3eAudioStop() {
    Music &m = g_music;
    std::lock_guard<std::mutex> lock(m.mutex);
    m.playing = false;
    if (m.dec) {
        mp3dec_ex_close(m.dec.get());
        m.dec.reset();
    }
}
HLE_REGISTER(s3eAudioStop);

uint8_t s3eAudioIsPlaying() {
    std::lock_guard<std::mutex> lock(g_music.mutex);
    return g_music.playing && !g_music.paused ? 1 : 0;
}
HLE_REGISTER(s3eAudioIsPlaying);

int32_t s3eAudioGetInt(int32_t prop) {
    Music &m = g_music;
    std::lock_guard<std::mutex> lock(m.mutex);
    switch (prop) {
    case 0: return m.volume;
    case 1: return m.playing ? (m.paused ? 2 : 1) : 0;
    case 2: return static_cast<int32_t>(m.frames_played * 1000 / kOutFreq);
    case 3: return config::get_int("s3e", "AudioVolDefault", 0x100);
    case 4: return 0;
    case 5: return 1;
    case 6: return 1;
    case 7: return 0;
    default: set_error(DEV_AUDIO, 1); return -1;
    }
}
HLE_REGISTER(s3eAudioGetInt);

int32_t s3eAudioSetInt(int32_t prop, int32_t value) {
    Music &m = g_music;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (prop == 0) {
        m.volume = std::clamp(value, 0, 0x100);
        return S3E_RESULT_SUCCESS;
    }
    if (prop == 4 && value == 0) return S3E_RESULT_SUCCESS;
    set_error(DEV_AUDIO, 1);
    return S3E_RESULT_ERROR;
}
HLE_REGISTER(s3eAudioSetInt);

uint8_t s3eAudioIsCodecSupported(int32_t codec) { return codec == 2 ? 1 : 0; }
HLE_REGISTER(s3eAudioIsCodecSupported);

int32_t s3eAudioGetError() { return take_error(DEV_AUDIO); }
HLE_REGISTER(s3eAudioGetError);
}  // namespace

namespace sound {
void init() {
    g_mix_buf = guest::alloc(kMaxFrames * sizeof(int16_t));
    g_info_buf = guest::alloc(64);
    SDL_AudioSpec want{}, have{};
    want.freq = kOutFreq;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        logf("[sound] cannot open audio device: %s", SDL_GetError());
        return;
    }
    const char *drv = SDL_GetCurrentAudioDriver();
    logf("[sound] %s: output %d Hz, %d channels, %d frames", drv ? drv : "?", have.freq, have.channels,
         have.samples);
    SDL_PauseAudioDevice(g_dev, 0);
}

void shutdown() {
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

void set_paused(bool paused) {
    g_paused = paused;
    if (g_dev) SDL_PauseAudioDevice(g_dev, paused ? 1 : 0);
    std::lock_guard<std::mutex> lock(g_music.mutex);
    g_music.paused = paused;
}

void pump() {
    // With the output paused the mixer does not run; process stop requests
    // here so the game's "wait until all channels stopped" loop finishes.
    if (!g_paused && g_dev) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (int ch = 0; ch < kChannels; ++ch)
        if (g_ch[ch].stop_pending) fire_stop(Cpu::current(), ch);
}
}  // namespace sound
