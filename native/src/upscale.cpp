#include "upscale.h"

#include <cstring>

namespace {

using u32 = std::uint32_t;

// One screen of w x h pixels into (w*2) x (h*2), Scale2x.
void scale2x(const u32* in, int w, int h, u32* out) {
    const int ow = w * 2;
    for (int y = 0; y < h; ++y) {
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
}

// One screen of w x h pixels into (w*3) x (h*3), Scale3x.
void scale3x(const u32* in, int w, int h, u32* out) {
    const int ow = w * 3;
    const auto px = [&](int x, int y) {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return in[y * w + x];
    };
    for (int y = 0; y < h; ++y) {
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
}

}  // namespace

void upscale_picture(const std::uint8_t* picture, int factor, std::vector<std::uint8_t>& out) {
    constexpr int kW = 256, kH = 192;
    out.resize(size_t(kW * factor) * size_t(kH * 2 * factor) * 4);
    std::vector<u32> screen(size_t(kW) * kH), twice(size_t(kW * 2) * (kH * 2));
    for (int s = 0; s < 2; ++s) {
        std::memcpy(screen.data(), picture + size_t(s) * kW * kH * 4, screen.size() * 4);
        u32* target = reinterpret_cast<u32*>(out.data()) + size_t(s) * size_t(kW * factor) * size_t(kH * factor);
        if (factor == 2) {
            scale2x(screen.data(), kW, kH, target);
        } else if (factor == 3) {
            scale3x(screen.data(), kW, kH, target);
        } else {  // 4: twice Scale2x
            scale2x(screen.data(), kW, kH, twice.data());
            scale2x(twice.data(), kW * 2, kH * 2, target);
        }
    }
}
