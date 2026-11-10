#include "upscale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr int kW = 256, kH = 192;  // one DS screen

// Workers that stay alive: starting threads anew for every picture costs more than the work.
class Pool {
public:
    Pool() {
        const int n = std::max(1, std::min(16, int(std::thread::hardware_concurrency()))) - 1;
        for (int i = 0; i < n; ++i) workers_.emplace_back([this] { work(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : workers_) t.join();
    }
    // Runs job(i) for i in [0, count) on all workers and this thread; returns when all are done.
    void run(int count, const std::function<void(int)>& job) {
        std::lock_guard<std::mutex> one_caller(run_mutex_);  // a second caller waits its turn
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            next_ = 0;
            count_ = count;
            busy_ = 0;
            ++generation_;
        }
        wake_.notify_all();
        take_jobs();
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return next_ >= count_ && busy_ == 0; });
        job_ = nullptr;
    }
    int threads() const { return int(workers_.size()) + 1; }

private:
    void take_jobs() {
        for (;;) {
            int i;
            const std::function<void(int)>* job;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job_ == nullptr || next_ >= count_) return;
                i = next_++;
                ++busy_;
                job = job_;
            }
            (*job)(i);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --busy_;
            }
            done_.notify_all();
        }
    }
    void work() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
            }
            take_jobs();
        }
    }
    std::vector<std::thread> workers_;
    std::mutex mutex_, run_mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(int)>* job_ = nullptr;
    int next_ = 0, count_ = 0, busy_ = 0;
    std::uint64_t generation_ = 0;
    bool quit_ = false;
};

Pool& pool() {
    static Pool p;
    return p;
}

// Runs body(first, last) over rows [0, count) on all cores, in bands.
template <typename Body>
void parallel(int count, Body body) {
    const int bands = std::min(count, pool().threads() * 2);
    if (bands <= 1) {
        body(0, count);
        return;
    }
    const int step = (count + bands - 1) / bands;
    const std::function<void(int)> job = [&](int i) {
        const int first = i * step;
        if (first < count) body(first, std::min(count, first + step));
    };
    pool().run(bands, job);
}

// --- Scale2x / Scale3x ---

void scale2x(const u32* in, int w, int h, u32* out) {
    const int ow = w * 2;
    parallel(h, [&](int first, int last) {
    for (int y = first; y < last; ++y) {
        for (int x = 0; x < w; ++x) {
            const u32 e = in[y * w + x];
            const u32 b = in[(y > 0 ? y - 1 : y) * w + x];
            const u32 h_ = in[(y < h - 1 ? y + 1 : y) * w + x];
            const u32 d = in[y * w + (x > 0 ? x - 1 : x)];
            const u32 f = in[y * w + (x < w - 1 ? x + 1 : x)];
            u32 e0 = e, e1 = e, e2 = e, e3 = e;
            if (b != h_ && d != f) {
                e0 = d == b ? d : e;
                e1 = b == f ? f : e;
                e2 = d == h_ ? d : e;
                e3 = h_ == f ? f : e;
            }
            u32* o = out + size_t(y * 2) * size_t(ow) + size_t(x * 2);
            o[0] = e0;
            o[1] = e1;
            o[ow] = e2;
            o[ow + 1] = e3;
        }
    }
    });
}

void scale3x(const u32* in, int w, int h, u32* out) {
    const int ow = w * 3;
    const auto px = [&](int x, int y) {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return in[y * w + x];
    };
    parallel(h, [&](int first, int last) {
    for (int y = first; y < last; ++y) {
        for (int x = 0; x < w; ++x) {
            const u32 a = px(x - 1, y - 1), b = px(x, y - 1), c = px(x + 1, y - 1);
            const u32 d = px(x - 1, y), e = px(x, y), f = px(x + 1, y);
            const u32 g = px(x - 1, y + 1), h_ = px(x, y + 1), i = px(x + 1, y + 1);
            u32 r[9] = {e, e, e, e, e, e, e, e, e};
            if (b != h_ && d != f) {
                r[0] = d == b ? d : e;
                r[1] = (d == b && e != c) || (b == f && e != a) ? b : e;
                r[2] = b == f ? f : e;
                r[3] = (d == b && e != g) || (d == h_ && e != a) ? d : e;
                r[5] = (b == f && e != i) || (h_ == f && e != c) ? f : e;
                r[6] = d == h_ ? d : e;
                r[7] = (d == h_ && e != i) || (h_ == f && e != g) ? h_ : e;
                r[8] = h_ == f ? f : e;
            }
            u32* o = out + size_t(y * 3) * size_t(ow) + size_t(x * 3);
            for (int k = 0; k < 9; ++k) o[size_t(k / 3) * size_t(ow) + size_t(k % 3)] = r[k];
        }
    }
    });
}

// One screen to `factor` times: chains of Scale2x and Scale3x where the factor allows, otherwise the
// nearest such size and then plain enlarging to the rest.
void edges_screen(const u32* in, int factor, u32* out) {
    std::vector<u32> a(in, in + size_t(kW) * kH), b;
    int scale = 1;
    const auto step = [&](int k) {
        b.assign(size_t(kW * scale * k) * size_t(kH * scale * k), 0);
        if (k == 2) scale2x(a.data(), kW * scale, kH * scale, b.data());
        else scale3x(a.data(), kW * scale, kH * scale, b.data());
        a.swap(b);
        scale *= k;
    };
    int rest = factor;
    while (rest % 2 == 0) { step(2); rest /= 2; }
    while (rest % 3 == 0) { step(3); rest /= 3; }
    if (rest != 1) {  // 5, 7, 11: from Scale2x once more, then nearest
        if (scale == 1) step(2);
    }
    const int ow = kW * factor, oh = kH * factor, sw = kW * scale;
    parallel(oh, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const int sy = y * scale / factor;
            for (int x = 0; x < ow; ++x) out[size_t(y) * ow + x] = a[size_t(sy) * sw + size_t(x * scale / factor)];
        }
    });
}

// --- FSR 1: EASU and RCAS, after AMD's ffx_fsr1.h ---

struct Rgb {
    float r, g, b;
};

Rgb load(const u32* in, int x, int y) {
    x = std::clamp(x, 0, kW - 1);
    y = std::clamp(y, 0, kH - 1);
    const u32 p = in[y * kW + x];
    return {(p & 0xFF) / 255.0f, ((p >> 8) & 0xFF) / 255.0f, ((p >> 16) & 0xFF) / 255.0f};
}

float luma(const Rgb& c) { return c.b * 0.5f + (c.r * 0.5f + c.g); }

// Accumulates direction and length from one of the four bilinear corners (FsrEasuSetF).
void easu_set(float& dx, float& dy, float& len, float w, float la, float lb, float lc, float ld, float le) {
    const float dc = ld - lc, cb = lc - lb;
    float lenx = std::max(std::fabs(dc), std::fabs(cb));
    lenx = lenx > 0 ? 1.0f / lenx : 0.0f;
    const float dirx = ld - lb;
    dx += dirx * w;
    lenx = std::clamp(std::fabs(dirx) * lenx, 0.0f, 1.0f);
    len += lenx * lenx * w;
    const float ec = le - lc, ca = lc - la;
    float leny = std::max(std::fabs(ec), std::fabs(ca));
    leny = leny > 0 ? 1.0f / leny : 0.0f;
    const float diry = le - la;
    dy += diry * w;
    leny = std::clamp(std::fabs(diry) * leny, 0.0f, 1.0f);
    len += leny * leny * w;
}

// The source screen as floats with luma, one pixel of border on each side so taps need no clamping.
struct Source {
    static constexpr int kPad = 2;
    int w = kW + 2 * kPad, h = kH + 2 * kPad;
    std::vector<Rgb> c;
    std::vector<float> l;
    explicit Source(const u32* in) : c(size_t(w) * h), l(size_t(w) * h) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const Rgb v = load(in, x - kPad, y - kPad);
                c[size_t(y) * w + x] = v;
                l[size_t(y) * w + x] = luma(v);
            }
        }
    }
    size_t at(int x, int y) const { return size_t(y + kPad) * size_t(w) + size_t(x + kPad); }
};

// The 12 taps around f:   b c / e f g h / i j k l / n o
constexpr int kTaps[12][2] = {{0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {2, 0},
                              {-1, 1}, {0, 1},  {1, 1},  {2, 1}, {0, 2}, {1, 2}};
enum { B, C, E, F, G, H, I, J, K, L, N, O };

// FsrEasuSetF without the weight: what one bilinear corner adds, scaled by its weight later.
struct Corner {
    float dx, dy, len;
};
Corner easu_corner(float la, float lb, float lc, float ld, float le) {
    Corner k{0, 0, 0};
    easu_set(k.dx, k.dy, k.len, 1.0f, la, lb, lc, ld, le);
    return k;
}

// All output pixels whose source position falls into the cell of source pixel (x, y).
void easu_cell(const Source& s, int x, int y, int factor, std::vector<Rgb>& up, int ow) {
    Rgb c[12];
    float l[12];
    for (int i = 0; i < 12; ++i) {
        const size_t at = s.at(x + kTaps[i][0], y + kTaps[i][1]);
        c[i] = s.c[at];
        l[i] = s.l[at];
    }
    const Corner k0 = easu_corner(l[B], l[E], l[F], l[G], l[J]);
    const Corner k1 = easu_corner(l[C], l[F], l[G], l[H], l[K]);
    const Corner k2 = easu_corner(l[F], l[I], l[J], l[K], l[N]);
    const Corner k3 = easu_corner(l[G], l[J], l[K], l[L], l[O]);
    const float min_r = std::min({c[F].r, c[G].r, c[J].r, c[K].r}), max_r = std::max({c[F].r, c[G].r, c[J].r, c[K].r});
    const float min_g = std::min({c[F].g, c[G].g, c[J].g, c[K].g}), max_g = std::max({c[F].g, c[G].g, c[J].g, c[K].g});
    const float min_b = std::min({c[F].b, c[G].b, c[J].b, c[K].b}), max_b = std::max({c[F].b, c[G].b, c[J].b, c[K].b});
    const float scale = 1.0f / float(factor);
    // Output pixels with floor(source position) == (x, y): sx = (ox + 0.5) / factor - 0.5.
    const auto range = [&](int v, int size, int& first, int& last) {
        first = std::max(0, int(std::ceil((float(v) + 0.5f) * float(factor) - 0.5f - 1e-4f)));
        last = std::min(size * factor, int(std::ceil((float(v) + 1.5f) * float(factor) - 0.5f - 1e-4f)));
    };
    int x0, x1, y0, y1;
    range(x, kW, x0, x1);
    range(y, kH, y0, y1);
    for (int oy = y0; oy < y1; ++oy) {
        const float ppy = (float(oy) + 0.5f) * scale - 0.5f - float(y);
        for (int ox = x0; ox < x1; ++ox) {
            const float ppx = (float(ox) + 0.5f) * scale - 0.5f - float(x);
            const float w0 = (1 - ppx) * (1 - ppy), w1 = ppx * (1 - ppy), w2 = (1 - ppx) * ppy, w3 = ppx * ppy;
            float dx = k0.dx * w0 + k1.dx * w1 + k2.dx * w2 + k3.dx * w3;
            float dy = k0.dy * w0 + k1.dy * w1 + k2.dy * w2 + k3.dy * w3;
            float len = k0.len * w0 + k1.len * w1 + k2.len * w2 + k3.len * w3;
            const float dir2 = dx * dx + dy * dy;
            if (dir2 < 1.0f / 32768.0f) {
                dx = 1;
                dy = 0;
            } else {
                const float r = 1.0f / std::sqrt(dir2);
                dx *= r;
                dy *= r;
            }
            len = len * 0.5f;
            len *= len;
            const float stretch = (dx * dx + dy * dy) / std::max(std::fabs(dx), std::fabs(dy));
            const float len2x = 1.0f + (stretch - 1.0f) * len, len2y = 1.0f - 0.5f * len;
            const float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
            const float clp = 1.0f / lob;
            float sr = 0, sg = 0, sb = 0, weight = 0;
            for (int i = 0; i < 12; ++i) {
                const float tx = float(kTaps[i][0]) - ppx, ty = float(kTaps[i][1]) - ppy;
                const float vx = (tx * dx + ty * dy) * len2x, vy = (tx * -dy + ty * dx) * len2y;
                const float d2 = std::min(vx * vx + vy * vy, clp);
                float wb = 2.0f / 5.0f * d2 - 1.0f, wa = lob * d2 - 1.0f;
                wb *= wb;
                wa *= wa;
                const float w = (25.0f / 16.0f * wb - (25.0f / 16.0f - 1.0f)) * wa;
                sr += c[i].r * w;
                sg += c[i].g * w;
                sb += c[i].b * w;
                weight += w;
            }
            const float inv = weight != 0 ? 1.0f / weight : 0.0f;
            up[size_t(oy) * ow + ox] = {std::clamp(sr * inv, min_r, max_r), std::clamp(sg * inv, min_g, max_g),
                                        std::clamp(sb * inv, min_b, max_b)};
        }
    }
}

u32 pack(float r, float g, float b) {
    const auto q = [](float v) { return u32(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | 0xFF000000u;
}

// RCAS: sharpening of the upscaled picture, limited so it cannot overshoot.
void rcas(const std::vector<Rgb>& in, int w, int h, u32* out, float sharpness) {
    parallel(h, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            for (int x = 0; x < w; ++x) {
                const auto at = [&](int xx, int yy) {
                    return in[size_t(std::clamp(yy, 0, h - 1)) * w + size_t(std::clamp(xx, 0, w - 1))];
                };
                const Rgb b = at(x, y - 1), d = at(x - 1, y), e = at(x, y), f = at(x + 1, y), hh = at(x, y + 1);
                float lobe = 0;
                bool first_channel = true;
                for (int ch = 0; ch < 3; ++ch) {
                    const auto get = [ch](const Rgb& c) { return ch == 0 ? c.r : ch == 1 ? c.g : c.b; };
                    const float mn = std::min({get(b), get(d), get(f), get(hh)});
                    const float mx = std::max({get(b), get(d), get(f), get(hh)});
                    const float hit_min = mx > 0 ? mn / (4.0f * mx) : 0.0f;
                    const float hit_max = mn < 1 ? (1.0f - mx) / (4.0f * mn - 4.0f) : 0.0f;
                    const float l = std::max(-hit_min, hit_max);
                    lobe = first_channel ? l : std::max(lobe, l);
                    first_channel = false;
                }
                lobe = std::max(-(0.25f - 1.0f / 16.0f), std::min(lobe, 0.0f)) * sharpness;
                const float rcp = 1.0f / (4.0f * lobe + 1.0f);
                out[size_t(y) * w + x] = pack((lobe * (b.r + d.r + hh.r + f.r) + e.r) * rcp,
                                              (lobe * (b.g + d.g + hh.g + f.g) + e.g) * rcp,
                                              (lobe * (b.b + d.b + hh.b + f.b) + e.b) * rcp);
            }
        }
    });
}

// FSR up to 2 times; beyond that the result is enlarged the rest of the way (the DS's 2D pictures
// gain nothing from more, and the processor stays fast enough).
void fsr_screen(const u32* in, int factor, u32* out) {
    const int f = std::min(factor, 2);
    const int ow = kW * f, oh = kH * f;
    const Source source(in);
    std::vector<Rgb> up(size_t(ow) * oh);
    // Cells start at source row -1 so the output's first half pixel is covered too.
    parallel(kH + 1, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            for (int x = -1; x < kW; ++x) easu_cell(source, x, y - 1, f, up, ow);
        }
    });
    if (f == factor) {
        rcas(up, ow, oh, out, std::exp2(-0.2f));  // AMD's usual 0.2 stops
        return;
    }
    std::vector<u32> sharp(size_t(ow) * oh);
    rcas(up, ow, oh, sharp.data(), std::exp2(-0.2f));
    const int fw = kW * factor, fh = kH * factor;
    parallel(fh, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const int sy = y * f / factor;
            for (int x = 0; x < fw; ++x) out[size_t(y) * fw + x] = sharp[size_t(sy) * ow + size_t(x * f / factor)];
        }
    });
}

}  // namespace

void upscale_picture(const u8* picture, int factor, int mode, std::vector<u8>& out) {
    factor = std::clamp(factor, 2, 12);
    out.resize(size_t(kW * factor) * size_t(kH * 2 * factor) * 4);
    std::vector<u32> screen(size_t(kW) * kH);
    for (int s = 0; s < 2; ++s) {
        std::memcpy(screen.data(), picture + size_t(s) * kW * kH * 4, screen.size() * 4);
        u32* target = reinterpret_cast<u32*>(out.data()) + size_t(s) * size_t(kW * factor) * size_t(kH * factor);
        if (mode == kUpscaleFsr) fsr_screen(screen.data(), factor, target);
        else edges_screen(screen.data(), factor, target);
    }
}
