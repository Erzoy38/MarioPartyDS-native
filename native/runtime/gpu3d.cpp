#define _CRT_SECURE_NO_WARNINGS  // getenv, only for a diagnostic switch
#include "gpu3d.h"

#include "hw.h"
#include "memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kWidth = 256, kHeight = 192;
constexpr u32 kMaxPolygons = 2048, kMaxVertices = 6144;

// Matrices are row-major, 20.12 fixed point, used as v' = v * M.
struct Matrix {
    s32 m[4][4];
};

Matrix identity() {
    Matrix out{};
    for (int i = 0; i < 4; ++i) out.m[i][i] = 0x1000;
    return out;
}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            s64 sum = 0;
            for (int k = 0; k < 4; ++k) sum += s64(a.m[r][k]) * b.m[k][c];
            out.m[r][c] = s32(sum >> 12);
        }
    }
    return out;
}

struct Vertex {
    float x, y, z, w;  // clip space, as 20.12 values
    float r, g, b;     // 0 to 31
    float s, t;        // texels
};

struct Polygon {
    std::vector<Vertex> vertices;
    u32 attr, tex_param, pal_base;
    bool translucent;
    float y_min, y_max;
    bool w_buffer;
};

// Geometry state.
u32 g_matrix_mode = 0;
Matrix g_projection = identity(), g_position = identity(), g_vector = identity(), g_texture = identity();
Matrix g_clip = identity();
Matrix g_projection_stack, g_position_stack[32], g_vector_stack[32], g_texture_stack;
u32 g_stack_level = 0, g_projection_level = 0;
bool g_stack_error = false;

u32 g_poly_attr_next = 0, g_poly_attr = 0;
u32 g_tex_param = 0, g_pal_base = 0;
u32 g_vertex_color = 0x7FFF;
u32 g_diffuse = 0, g_ambient = 0, g_specular = 0, g_emission = 0;
bool g_use_shininess = false;
u8 g_shininess[128] = {};
s32 g_light_dir[4][3] = {};
u32 g_light_color[4] = {};
s32 g_raw_s = 0, g_raw_t = 0, g_tex_s = 0, g_tex_t = 0;
s32 g_cur[3] = {};
u32 g_viewport = 0x00BFFF00;  // x1, y1, x2, y2 in bytes: 0, 0, 255, 191

u32 g_prim_type = 0;
bool g_in_primitive = false;
std::vector<Vertex> g_strip;
u32 g_strip_count = 0;

u32 g_vertices_written = 0;
std::vector<Polygon> g_building;
std::vector<Polygon> g_ready;
bool g_swap_pending = false;
u32 g_swap_param = 0;
bool g_box_result = false;
s32 g_pos_result[4] = {};
s16 g_vec_result[3] = {};

struct FifoEntry {
    u8 cmd;
    u32 param;
};
std::deque<FifoEntry> g_fifo;
u32 g_packed = 0;          // command bytes waiting in a packed word
u32 g_packed_params = 0;   // parameters still due for the current packed command
u8 g_packed_cmd = 0;
u32 g_params[32];
u32 g_param_count = 0;

u32 g_status_irq_mode = 0;
Gpu3dStats g_stats;

u32 g_picture[kHeight][kWidth];

// Number of parameters per command. -1 marks commands that do not exist.
int params_for(u32 cmd) {
    switch (cmd) {
        case 0x00: return 0;
        case 0x10: case 0x12: case 0x13: case 0x14: return 1;
        case 0x11: case 0x15: return 0;
        case 0x16: case 0x18: return 16;
        case 0x17: case 0x19: return 12;
        case 0x1A: return 9;
        case 0x1B: case 0x1C: return 3;
        case 0x20: case 0x21: case 0x22: return 1;
        case 0x23: return 2;
        case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B:
            return 1;
        case 0x30: case 0x31: case 0x32: case 0x33: return 1;
        case 0x34: return 32;
        case 0x40: return 1;
        case 0x41: return 0;
        case 0x50: case 0x60: return 1;
        case 0x70: return 3;
        case 0x71: return 2;
        case 0x72: return 1;
        default: return -1;
    }
}

void update_clip() { g_clip = multiply(g_position, g_projection); }

// Applies a new matrix to whatever the current mode selects. `both` also changes the vector matrix.
void apply(const Matrix& next, bool both) {
    switch (g_matrix_mode) {
        case 0: g_projection = multiply(next, g_projection); update_clip(); break;
        case 1: g_position = multiply(next, g_position); update_clip(); break;
        case 2:
            g_position = multiply(next, g_position);
            if (both) g_vector = multiply(next, g_vector);
            update_clip();
            break;
        default: g_texture = multiply(next, g_texture); break;
    }
}

void load(const Matrix& next) {
    switch (g_matrix_mode) {
        case 0: g_projection = next; update_clip(); break;
        case 1: g_position = next; update_clip(); break;
        case 2: g_position = next; g_vector = next; update_clip(); break;
        default: g_texture = next; break;
    }
}

Matrix read_matrix(int rows, int columns) {
    Matrix out = identity();
    int index = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) out.m[r][c] = s32(g_params[index++]);
    }
    return out;
}

s32 sign_extend(u32 value, int bits) { return s32(value << (32 - bits)) >> (32 - bits); }

void compute_lighting(const s32 normal_raw[3]) {
    s32 n[3];
    for (int i = 0; i < 3; ++i) {
        n[i] = s32((s64(normal_raw[0]) * g_vector.m[0][i] + s64(normal_raw[1]) * g_vector.m[1][i]
                    + s64(normal_raw[2]) * g_vector.m[2][i]) >> 12);
    }
    const auto channel = [](u32 color, int shift) { return s32((color >> shift) & 31); };
    s32 rgb[3] = {channel(g_emission, 0), channel(g_emission, 5), channel(g_emission, 10)};
    for (int light = 0; light < 4; ++light) {
        if (!(g_poly_attr_next & (1u << light))) continue;
        const s32* l = g_light_dir[light];
        s32 diffuse = -(l[0] * n[0] + l[1] * n[1] + l[2] * n[2]) >> 10;
        diffuse = std::clamp(diffuse, 0, 255);
        s32 shine = -((l[0] >> 1) * n[0] + (l[1] >> 1) * n[1] + ((l[2] - 0x200) >> 1) * n[2]) >> 10;
        if (shine < 0) {
            shine = 0;
        } else {
            shine = std::min(shine, 255);
            shine = ((shine * shine) >> 7) - 0x100;
            shine = std::max(shine, 0) >> 1;
            shine = std::min(shine, 127);
            if (g_use_shininess) shine = g_shininess[shine];
            else shine *= 2;
        }
        for (int c = 0; c < 3; ++c) {
            const int shift = c * 5;
            const s32 light_c = channel(g_light_color[light], shift);
            rgb[c] += (channel(g_specular, shift) * light_c * shine) >> 13;
            rgb[c] += (channel(g_diffuse, shift) * light_c * diffuse) >> 13;
            rgb[c] += (channel(g_ambient, shift) * light_c) >> 5;
        }
    }
    g_vertex_color = u32(std::min(rgb[0], 31)) | (u32(std::min(rgb[1], 31)) << 5)
                     | (u32(std::min(rgb[2], 31)) << 10);
}

Vertex lerp(const Vertex& a, const Vertex& b, float k) {
    return {a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k,
            a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k,
            a.s + (b.s - a.s) * k, a.t + (b.t - a.t) * k};
}

// Keeps what lies inside one clip plane: sign * coordinate <= w.
std::vector<Vertex> clip_plane(const std::vector<Vertex>& in, int axis, float sign) {
    std::vector<Vertex> out;
    const auto distance = [&](const Vertex& v) {
        const float coordinate = axis == 0 ? v.x : axis == 1 ? v.y : v.z;
        return v.w - sign * coordinate;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        const Vertex& a = in[i];
        const Vertex& b = in[(i + 1) % in.size()];
        const float da = distance(a), db = distance(b);
        if (da >= 0) out.push_back(a);
        if ((da >= 0) != (db >= 0)) out.push_back(lerp(a, b, da / (da - db)));
    }
    return out;
}

struct DebugCounts {
    u32 submitted = 0, far_dropped = 0, culled = 0;
    u32 per_format[8] = {};
    u32 per_mode[4] = {};
    u32 translucent = 0;
    u32 large = 0;  // covers more than a quarter of the picture
    u32 sample_attr[4] = {}, sample_tex[4] = {}, sample_pal[4] = {};
    int samples = 0;
} g_debug, g_last_debug;

void add_polygon(std::vector<Vertex> vertices) {
    if (g_building.size() >= kMaxPolygons) return;
    const u32 attr = g_poly_attr;
    // A polygon that reaches past the far plane is dropped unless it asks to be clipped there.
    ++g_debug.submitted;
    if (!(attr & 0x1000)) {
        for (const Vertex& v : vertices) {
            if (v.z > v.w) {
                ++g_debug.far_dropped;
                return;
            }
        }
    }
    for (int axis = 0; axis < 3 && !vertices.empty(); ++axis) {
        vertices = clip_plane(vertices, axis, 1.0f);
        if (!vertices.empty()) vertices = clip_plane(vertices, axis, -1.0f);
    }
    if (vertices.size() < 3) return;
    // Vertex memory only holds what survives clipping and culling; see below for culling.
    if (g_vertices_written + vertices.size() > kMaxVertices) return;

    // To screen space: x and y in pixels, z in 0 to 1, w kept for depth and perspective.
    const float x1 = float(g_viewport & 0xFF), y1 = float((g_viewport >> 8) & 0xFF);
    const float x2 = float((g_viewport >> 16) & 0xFF), y2 = float(g_viewport >> 24);
    const float vw = x2 - x1 + 1, vh = y2 - y1 + 1;
    float y_min = 1e9f, y_max = -1e9f;
    for (Vertex& v : vertices) {
        const float w = v.w <= 0 ? 1.0f : v.w;
        // The hardware keeps whole pixels here, and samples at those positions without an offset.
        v.x = std::floor(x1 + (v.x / w + 1) * vw / 2);
        v.y = std::floor((kHeight - 1 - y2) + (1 - v.y / w) * vh / 2);
        v.z = (v.z / w + 1) / 2;
        y_min = std::min(y_min, v.y);
        y_max = std::max(y_max, v.y);
    }

    // Counter-clockwise on screen is the front: seen on the board, where floors only render their
    // front and go missing the other way round. `area` is positive for clockwise, as y runs down.
    float area = 0;
    for (std::size_t i = 1; i + 1 < vertices.size(); ++i) {
        const Vertex& a = vertices[0];
        const Vertex& b = vertices[i];
        const Vertex& c = vertices[i + 1];
        area += (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    }
    const bool front = area < 0;
    if (front ? !(attr & 0x80) : !(attr & 0x40)) {
        ++g_debug.culled;
        return;
    }

    const u32 alpha = (attr >> 16) & 31;
    const u32 format = (g_tex_param >> 26) & 7;
    Polygon polygon;
    polygon.vertices = std::move(vertices);
    polygon.attr = attr;
    polygon.tex_param = g_tex_param;
    polygon.pal_base = g_pal_base;
    polygon.translucent = (alpha != 31 && alpha != 0) || format == 1 || format == 6;
    polygon.y_min = y_min;
    polygon.y_max = y_max;
    ++g_debug.per_format[format];
    ++g_debug.per_mode[(attr >> 4) & 3];
    if (polygon.translucent) ++g_debug.translucent;
    if (std::fabs(area) > kWidth * kHeight / 2.0f) {
        ++g_debug.large;
        if (g_debug.samples < 4) {
            g_debug.sample_attr[g_debug.samples] = attr;
            g_debug.sample_tex[g_debug.samples] = g_tex_param;
            g_debug.sample_pal[g_debug.samples] = g_pal_base;
            ++g_debug.samples;
        }
    }
    g_vertices_written += u32(polygon.vertices.size());
    g_building.push_back(std::move(polygon));
}

void add_vertex() {
    if (!g_in_primitive) return;

    const s64 x = g_cur[0], y = g_cur[1], z = g_cur[2];
    const Matrix& m = g_clip;
    Vertex v;
    v.x = float((x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + s64(0x1000) * m.m[3][0]) >> 12);
    v.y = float((x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + s64(0x1000) * m.m[3][1]) >> 12);
    v.z = float((x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + s64(0x1000) * m.m[3][2]) >> 12);
    v.w = float((x * m.m[0][3] + y * m.m[1][3] + z * m.m[2][3] + s64(0x1000) * m.m[3][3]) >> 12);
    v.r = float(g_vertex_color & 31);
    v.g = float((g_vertex_color >> 5) & 31);
    v.b = float((g_vertex_color >> 10) & 31);

    s32 tex_s = g_tex_s, tex_t = g_tex_t;
    if ((g_tex_param >> 30) == 3) {  // position transform
        const Matrix& t = g_texture;
        tex_s = s32(((x * t.m[0][0] + y * t.m[1][0] + z * t.m[2][0]) >> 24) + g_raw_s);
        tex_t = s32(((x * t.m[0][1] + y * t.m[1][1] + z * t.m[2][1]) >> 24) + g_raw_t);
    }
    v.s = float(tex_s) / 16.0f;
    v.t = float(tex_t) / 16.0f;

    g_strip.push_back(v);
    ++g_strip_count;
    switch (g_prim_type) {
        case 0:  // triangles
            if (g_strip.size() == 3) {
                add_polygon(g_strip);
                g_strip.clear();
            }
            break;
        case 1:  // quads
            if (g_strip.size() == 4) {
                add_polygon(g_strip);
                g_strip.clear();
            }
            break;
        case 2:  // triangle strip: every second triangle runs the other way round
            if (g_strip.size() == 3) {
                if (g_strip_count & 1) add_polygon(g_strip);
                else add_polygon({g_strip[1], g_strip[0], g_strip[2]});
                g_strip.erase(g_strip.begin());
            }
            break;
        default:  // quad strip: the new pair comes second to last in outline order
            if (g_strip.size() == 4) {
                add_polygon({g_strip[0], g_strip[1], g_strip[3], g_strip[2]});
                g_strip.erase(g_strip.begin(), g_strip.begin() + 2);
            }
            break;
    }
}

// --- Rasterizer ---


// Runs body(first, last) over [0, count) in bands, on all cores; the bands never overlap.
template <typename Body>
void parallel_rows(int count, Body body) {
    static const int kThreads = std::max(1, std::min(16, int(std::thread::hardware_concurrency())));
    if (count < 64 || kThreads == 1) {
        body(0, count);
        return;
    }
    std::vector<std::thread> threads;
    const int step = (count + kThreads - 1) / kThreads;
    for (int first = step; first < count; first += step) threads.emplace_back(body, first, std::min(count, first + step));
    body(0, std::min(count, step));
    for (std::thread& t : threads) t.join();
}

// A picture being drawn: the game's own at 256 x 192, or the one for the screen at `scale` times
// that. Index y * w + x.
struct Target {
    int scale = 1, w = kWidth, h = kHeight;
    // Any other size (for DLSS): pixel centres spread evenly, the sample point moved by `jitter`
    // pixels of this picture. `free_size` is false for the game's own and the plain scales.
    bool free_size = false;
    float jitter_x = 0, jitter_y = 0;
    std::vector<float> depth;
    std::vector<u32> color;  // 6 bits per channel in 0-17, alpha in 18-22
    std::vector<u8> id;      // polygon id of the opaque pixel
    std::vector<s16> translucent_id;
    std::vector<u8> fog;
    std::vector<u8> stencil;

    void resize(int s) {
        scale = s;
        free_size = false;
        allocate(kWidth * s, kHeight * s);
    }
    void resize_free(int width, int height) {
        scale = 0;
        free_size = true;
        allocate(width, height);
    }
    void allocate(int width, int height) {
        w = width;
        h = height;
        const size_t n = size_t(w) * size_t(h);
        depth.assign(n, 0);
        color.assign(n, 0);
        id.assign(n, 0);
        translucent_id.assign(n, -1);
        fog.assign(n, 0);
        stencil.assign(n, 0);
    }
};
Target g_native = [] {
    Target t;
    t.resize(1);
    return t;
}();

// A texture pack's picture for one of the game's textures, in RGBA (r in the low byte).
struct Replacement {
    int w = 0, h = 0;
    float kx = 1, ky = 1;  // replacement texels per game texel
    std::vector<u32> rgba;
};

// --- The picture for the screen: higher resolution, replacement textures ---
int g_display_scale = 1;
bool g_display_replace = false, g_display_dump = false;
Gpu3dTextureHooks g_hooks{};
Target g_display;
int g_display_free_w = 0, g_display_free_h = 0;  // 0: 256 * scale x 192 * scale
float g_next_jitter_x = 0, g_next_jitter_y = 0;
std::vector<u32> g_display_picture[2];
std::vector<float> g_display_depth[2];  // only with a free size
Gpu3dDisplayInfo g_display_info[2];
int g_display_index = 0;
bool g_display_drawn = false;
std::unordered_map<u64, std::shared_ptr<Replacement>> g_replacements;  // null: no file for it
std::unordered_set<u64> g_dumped;

u16 io16_at(u32 offset) {
    u16 value;
    std::memcpy(&value, &g_machine.io[offset], 2);
    return value;
}

u32 io32_at(u32 offset) {
    u32 value;
    std::memcpy(&value, &g_machine.io[offset], 4);
    return value;
}

struct TexelRead {
    u32 color;
    u32 alpha;
};

u8 tex8(u32 address) {
    const u8* slot = vram_texture_slot(address >> 17);
    return slot ? slot[address & 0x1FFFF] : 0;
}

u16 tex16(u32 address) { return u16(tex8(address) | (tex8(address + 1) << 8)); }

u16 pal16(u32 address) {
    const u8* slot = vram_texture_palette_slot(address >> 14);
    if (slot == nullptr) return 0;
    const u32 offset = address & 0x3FFF;
    u16 value;
    std::memcpy(&value, slot + (offset & ~1u), 2);
    return value;
}

u32 average(u16 a, u16 b, u32 weight_a, u32 weight_b) {
    u32 out = 0;
    for (int shift = 0; shift < 15; shift += 5) {
        const u32 ca = (a >> shift) & 31, cb = (b >> shift) & 31;
        out |= ((ca * weight_a + cb * weight_b) / (weight_a + weight_b)) << shift;
    }
    return out;
}

TexelRead sample_texture(u32 param, u32 pal_base, s32 s, s32 t);
TexelRead sample(const Polygon& p, s32 s, s32 t) { return sample_texture(p.tex_param, p.pal_base, s, t); }

TexelRead sample_texture(u32 param, u32 pal_base, s32 s, s32 t) {
    const u32 format = (param >> 26) & 7;
    const s32 width = 8 << ((param >> 20) & 7), height = 8 << ((param >> 23) & 7);
    const auto wrap = [](s32 v, s32 size, bool repeat, bool flip) {
        if (!repeat) return std::clamp(v, 0, size - 1);
        if (flip) {
            const s32 period = size * 2;
            s32 m = v & (period - 1);
            return m >= size ? period - 1 - m : m;
        }
        return v & (size - 1);
    };
    const u32 x = u32(wrap(s, width, param & (1u << 16), param & (1u << 18)));
    const u32 y = u32(wrap(t, height, param & (1u << 17), param & (1u << 19)));
    const u32 base = (param & 0xFFFF) * 8;
    const bool transparent0 = param & (1u << 29);
    const u32 pal = pal_base * 16;
    switch (format) {
        case 1: {  // A3I5
            const u8 v = tex8(base + y * u32(width) + x);
            const u32 a = v >> 5;
            return {pal16(pal + (v & 31u) * 2) & 0x7FFFu, (a << 2) | (a >> 1)};
        }
        case 2: {  // 4 colours
            const u8 byte = tex8(base + (y * u32(width) + x) / 4);
            const u32 i = (byte >> (((y * u32(width) + x) & 3) * 2)) & 3;
            if (i == 0 && transparent0) return {0, 0};
            return {pal16(pal_base * 8 + i * 2) & 0x7FFFu, 31};
        }
        case 3: {  // 16 colours
            const u32 where = y * u32(width) + x;
            const u8 byte = tex8(base + where / 2);
            const u32 i = (where & 1) ? byte >> 4 : byte & 15u;
            if (i == 0 && transparent0) return {0, 0};
            return {pal16(pal + i * 2) & 0x7FFFu, 31};
        }
        case 4: {  // 256 colours
            const u8 i = tex8(base + y * u32(width) + x);
            if (i == 0 && transparent0) return {0, 0};
            return {pal16(pal + u32(i) * 2) & 0x7FFFu, 31};
        }
        case 5: {  // 4x4 compressed
            const u32 word = base + (y & ~3u) * (u32(width) / 4) + (x & ~3u) / 4 * 4;
            const u8 row = tex8(word + (y & 3));
            const u32 i = (row >> ((x & 3) * 2)) & 3;
            const u32 slot = word >> 17;
            const u32 info_at = 0x20000 + ((word & 0x1FFFF) >> 1) + (slot == 2 ? 0x10000 : 0);
            const u16 info = tex16(info_at);
            const u32 pal4 = pal + (info & 0x3FFFu) * 4;
            const u32 mode = info >> 14;
            const u16 c0 = pal16(pal4), c1 = pal16(pal4 + 2);
            switch (mode) {
                case 0:
                    if (i == 3) return {0, 0};
                    return {pal16(pal4 + i * 2) & 0x7FFFu, 31};
                case 1:
                    if (i == 3) return {0, 0};
                    if (i == 2) return {average(c0, c1, 1, 1), 31};
                    return {pal16(pal4 + i * 2) & 0x7FFFu, 31};
                case 2:
                    return {pal16(pal4 + i * 2) & 0x7FFFu, 31};
                default:
                    if (i == 2) return {average(c0, c1, 5, 3), 31};
                    if (i == 3) return {average(c0, c1, 3, 5), 31};
                    return {pal16(pal4 + i * 2) & 0x7FFFu, 31};
            }
        }
        case 6: {  // A5I3
            const u8 v = tex8(base + y * u32(width) + x);
            return {pal16(pal + (v & 7u) * 2) & 0x7FFFu, u32(v >> 3)};
        }
        case 7: {  // direct colour
            const u16 v = tex16(base + (y * u32(width) + x) * 2);
            return {v & 0x7FFFu, (v & 0x8000) ? 31u : 0u};
        }
        default:
            return {0x7FFF, 31};
    }
}

// Colours are mixed in 6 bits per channel, as the hardware does: r | g << 6 | b << 12.
u32 expand6(u32 c5) { return c5 * 2 + (c5 ? 1 : 0); }

u32 color15_to_18(u32 c) {
    return expand6(c & 31) | (expand6((c >> 5) & 31) << 6) | (expand6((c >> 10) & 31) << 12);
}

u32 mix18(u32 src, u32 dst, u32 alpha) {
    u32 out = 0;
    for (int shift = 0; shift < 18; shift += 6) {
        const u32 cs = (src >> shift) & 63, cd = (dst >> shift) & 63;
        out |= ((cs * (alpha + 1) + cd * (31 - alpha)) >> 5) << shift;
    }
    return out;
}

// One polygon's colour at a pixel, from the interpolated vertex colour and the texture.
void shade(const Polygon& p, float r, float g, float b, float s, float t, u32 poly_alpha, u32 disp,
           const Replacement* replacement, u32& out_color, u32& out_alpha) {
    // Vertex colours in 6 bits.
    u32 vr = u32(std::clamp(r * 2.0f + 1.0f, 0.0f, 63.0f));
    u32 vg = u32(std::clamp(g * 2.0f + 1.0f, 0.0f, 63.0f));
    u32 vb = u32(std::clamp(b * 2.0f + 1.0f, 0.0f, 63.0f));
    if (r <= 0) vr = 0;
    if (g <= 0) vg = 0;
    if (b <= 0) vb = 0;
    const u32 mode = (p.attr >> 4) & 3;
    u32 toon_add = 0;
    bool highlight = false;
    if (mode == 2) {
        const u16 toon = io16_at(0x380 + (vr >> 1) * 2);
        const u32 toon18 = color15_to_18(toon);
        vr = toon18 & 63;
        vg = (toon18 >> 6) & 63;
        vb = (toon18 >> 12) & 63;
        if (disp & 2) {  // highlight shading adds the same colour again afterwards
            toon_add = toon18;
            highlight = true;
        }
    }
    const u32 format = (p.tex_param >> 26) & 7;
    u32 alpha = poly_alpha == 0 ? 31 : poly_alpha;
    if (format != 0 && (disp & 1)) {
        TexelRead texel;
        u32 tr, tg, tb;
        if (replacement != nullptr) {
            // The replacement's texel at the same place, with the game's wrapping rules.
            const u32 param = p.tex_param;
            const auto wrap = [](s32 v, s32 size, bool repeat, bool flip) {
                if (!repeat) return std::clamp(v, 0, size - 1);
                if (flip) {
                    const s32 period = size * 2;
                    s32 m = v % period;
                    if (m < 0) m += period;
                    return m >= size ? period - 1 - m : m;
                }
                s32 m = v % size;
                return m < 0 ? m + size : m;
            };
            const s32 x = wrap(s32(std::floor(s * replacement->kx)), replacement->w, param & (1u << 16), param & (1u << 18));
            const s32 y = wrap(s32(std::floor(t * replacement->ky)), replacement->h, param & (1u << 17), param & (1u << 19));
            const u32 c = replacement->rgba[size_t(y) * size_t(replacement->w) + size_t(x)];
            tr = (c & 0xFF) >> 2;
            tg = ((c >> 8) & 0xFF) >> 2;
            tb = ((c >> 16) & 0xFF) >> 2;
            texel = {0, (c >> 24) >> 3};
        } else {
            texel = sample(p, s32(std::floor(s)), s32(std::floor(t)));
            tr = expand6(texel.color & 31);
            tg = expand6((texel.color >> 5) & 31);
            tb = expand6((texel.color >> 10) & 31);
        }
        if (mode == 1) {  // decal
            vr = (tr * texel.alpha + vr * (31 - texel.alpha)) >> 5;
            vg = (tg * texel.alpha + vg * (31 - texel.alpha)) >> 5;
            vb = (tb * texel.alpha + vb * (31 - texel.alpha)) >> 5;
        } else {  // modulation, also for toon
            vr = ((tr + 1) * (vr + 1) - 1) >> 6;
            vg = ((tg + 1) * (vg + 1) - 1) >> 6;
            vb = ((tb + 1) * (vb + 1) - 1) >> 6;
            alpha = ((texel.alpha + 1) * (alpha + 1) - 1) >> 5;
        }
    }
    if (highlight) {
        vr = std::min(63u, vr + (toon_add & 63));
        vg = std::min(63u, vg + ((toon_add >> 6) & 63));
        vb = std::min(63u, vb + ((toon_add >> 12) & 63));
    }
    out_color = vr | (vg << 6) | (vb << 12);
    out_alpha = alpha;
}

void draw_triangle(const Polygon& p, Vertex a, Vertex b, Vertex c, u32 disp, bool w_mode, Target& target,
                   const Replacement* replacement, int row_first, int row_last) {
    if (target.free_size) {
        // Pixel i of this picture samples the game's picture at ((i + 0.5 + jitter) / w) of its
        // width, so the centres are even across the whole picture, as DLSS expects them.
        const float kx = float(target.w) / float(kWidth), ky = float(target.h) / float(kHeight);
        for (Vertex* v : {&a, &b, &c}) {
            v->x = (v->x + 0.5f) * kx - 0.5f - target.jitter_x;
            v->y = (v->y + 0.5f) * ky - 0.5f - target.jitter_y;
        }
    } else if (target.scale != 1) {
        const float k = float(target.scale);
        // Pixel centres stay where the game's picture has them: pixel x of the game is pixel x * k.
        for (Vertex* v : {&a, &b, &c}) {
            v->x *= k;
            v->y *= k;
        }
    }
    const int tw = target.w, th = target.h;
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    const float inv_area = 1.0f / area;
    const int x_start = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int x_end = std::min(tw - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y_start = std::max(row_first, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int y_end = std::min({th - 1, row_last - 1, int(std::ceil(std::max({a.y, b.y, c.y})))});

    const u32 poly_alpha = (p.attr >> 16) & 31;
    const u32 id = (p.attr >> 24) & 63;
    const u32 mode = (p.attr >> 4) & 3;
    const bool shadow = mode == 3;
    const bool depth_equal = p.attr & (1u << 14);
    const bool update_depth_translucent = p.attr & (1u << 11);
    const bool fog = p.attr & (1u << 15);
    const bool alpha_test = disp & 4;
    const bool blend = disp & 8;
    const u32 alpha_ref = g_machine.io[0x340] & 31;

    const float wa = a.w <= 0 ? 1.0f : 1.0f / a.w;
    const float wb = b.w <= 0 ? 1.0f : 1.0f / b.w;
    const float wc = c.w <= 0 ? 1.0f : 1.0f / c.w;

    // Which side of an edge counts as inside: the sign keeps the shared-edge rule the same for
    // triangles of either winding.
    const float sign = area > 0 ? 1.0f : -1.0f;
    const auto top_left = [&](const Vertex& from, const Vertex& to) {
        const float dx = (to.x - from.x) * sign, dy = (to.y - from.y) * sign;
        return (dy == 0 && dx > 0) || dy < 0;
    };
    const bool tl0 = top_left(b, c), tl1 = top_left(c, a), tl2 = top_left(a, b);

    for (int y = y_start; y <= y_end; ++y) {
        const float py = float(y);
        for (int x = x_start; x <= x_end; ++x) {
            const float px = float(x);
            const size_t at = size_t(y) * size_t(tw) + size_t(x);
            const float e0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * sign;
            const float e1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * sign;
            const float e2 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * sign;
            if (e0 < 0 || e1 < 0 || e2 < 0) continue;
            if ((e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2)) continue;
            const float l0 = e0 * inv_area * sign, l1 = e1 * inv_area * sign, l2 = e2 * inv_area * sign;

            const float one_over_w = l0 * wa + l1 * wb + l2 * wc;
            const float w = 1.0f / one_over_w;
            const float screen_z = l0 * a.z + l1 * b.z + l2 * c.z;
            const float depth = w_mode ? std::min(w / 4096.0f, 4095.0f) / 4095.0f : screen_z;

            float& buffered = target.depth[at];
            const bool depth_pass = depth_equal ? std::fabs(depth - buffered) <= 0.0002f : depth < buffered;

            if (shadow && id == 0) {
                // A shadow mask marks where the volume is hidden behind the scene.
                if (!depth_pass) target.stencil[at] = true;
                continue;
            }
            if (!depth_pass) continue;
            if (shadow) {
                const bool marked = target.stencil[at];
                target.stencil[at] = false;
                if (!marked || target.id[at] == id) continue;
            }

            const float pr = (l0 * a.r * wa + l1 * b.r * wb + l2 * c.r * wc) * w;
            const float pg = (l0 * a.g * wa + l1 * b.g * wb + l2 * c.g * wc) * w;
            const float pb = (l0 * a.b * wa + l1 * b.b * wb + l2 * c.b * wc) * w;
            const float ps = (l0 * a.s * wa + l1 * b.s * wb + l2 * c.s * wc) * w;
            const float pt = (l0 * a.t * wa + l1 * b.t * wb + l2 * c.t * wc) * w;
            u32 color, alpha;
            shade(p, pr, pg, pb, ps, pt, poly_alpha, disp, replacement, color, alpha);
            if (alpha == 0) continue;
            if (alpha_test && alpha <= alpha_ref) continue;

            if (alpha == 31) {
                target.color[at] = color | (31u << 18);
                buffered = depth;
                target.id[at] = u8(id);
                target.fog[at] = fog;
            } else {
                if (target.translucent_id[at] == s16(id)) continue;  // same id does not overlap
                target.translucent_id[at] = s16(id);
                const u32 under = target.color[at];
                const u32 under_alpha = (under >> 18) & 31;
                if (blend && under_alpha != 0) {
                    target.color[at] = mix18(color, under & 0x3FFFF, alpha) | (std::max(alpha, under_alpha) << 18);
                } else {
                    target.color[at] = color | (alpha << 18);
                }
                if (update_depth_translucent) buffered = depth;
                target.fog[at] = target.fog[at] && fog;
            }
        }
    }
}

void apply_edges_and_fog(u32 disp, Target& t, bool parallel) {
    const int w = t.w, h = t.h;
    if (disp & 0x20) {  // edge marking: outlines between different polygon ids
        std::vector<u32> marked = t.color;
        const auto rows = [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t at = size_t(y) * size_t(w) + size_t(x);
                const u8 id = t.id[at];
                const float depth = t.depth[at];
                const auto differs = [&](int nx, int ny) {
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return false;
                    const size_t n = size_t(ny) * size_t(w) + size_t(nx);
                    return t.id[n] != id && depth < t.depth[n];
                };
                if (differs(x - 1, y) || differs(x + 1, y) || differs(x, y - 1) || differs(x, y + 1)) {
                    const u32 alpha = (t.color[at] >> 18) & 31;
                    marked[at] = color15_to_18(io16_at(0x330 + (id >> 3) * 2) & 0x7FFF) | (alpha << 18);
                }
            }
        }
        };
        if (parallel) parallel_rows(h, rows);
        else rows(0, h);
        t.color.swap(marked);
    }
    if (disp & 0x80) {  // fog from a table over the depth range
        const u32 fog_raw = io32_at(0x358);
        const u32 fog_color = color15_to_18(fog_raw & 0x7FFF);
        const u32 fog_alpha_value = (fog_raw >> 16) & 31;
        const u32 offset = io16_at(0x35C) & 0x7FFF;
        const u32 shift = (disp >> 8) & 15;
        const bool alpha_only = disp & 0x40;
        const auto rows = [&](int first, int last) {
        for (size_t at = size_t(first) * size_t(w); at < size_t(last) * size_t(w); ++at) {
            if (!t.fog[at]) continue;
            const u32 depth15 = u32(std::clamp(t.depth[at], 0.0f, 1.0f) * 32767.0f);
            u32 density;
            if (depth15 < offset) {
                density = g_machine.io[0x360] & 127;
            } else {
                const u32 step = 0x400u >> shift;
                const u32 index = step ? (depth15 - offset) / step : 31;
                density = index >= 31 ? g_machine.io[0x360 + 31] & 127 : g_machine.io[0x360 + index] & 127;
            }
            if (density >= 127) density = 128;
            const u32 pixel = t.color[at];
            u32 out = 0;
            for (int s = 0; s < 18; s += 6) {
                const u32 c = (pixel >> s) & 63, f = (fog_color >> s) & 63;
                out |= alpha_only ? (c << s) : (((f * density + c * (128 - density)) >> 7) << s);
            }
            const u32 alpha = (pixel >> 18) & 31;
            const u32 new_alpha = (fog_alpha_value * density + alpha * (128 - density)) >> 7;
            t.color[at] = out | (new_alpha << 18);
        }
        };
        if (parallel) parallel_rows(h, rows);
        else rows(0, h);
    }
}

// FNV-1a over the decoded texture: the same picture always gets the same name.
u64 texture_key(int w, int h, const std::vector<u32>& rgba) {
    u64 hash = 0xCBF29CE484222325ull;
    const auto add = [&](u32 v) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (v >> (i * 8)) & 0xFF;
            hash *= 0x100000001B3ull;
        }
    };
    add(u32(w));
    add(u32(h));
    for (u32 v : rgba) add(v);
    return hash;
}

// The texture of these fields as RGBA, the way the game shows it.
void decode_texture(u32 param, u32 pal_base, int w, int h, std::vector<u32>& rgba) {
    rgba.resize(size_t(w) * size_t(h));
    const u32 plain = param & ~((15u << 16) | (3u << 30));  // without wrapping and coordinate modes
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const TexelRead t = sample_texture(plain, pal_base, x, y);
            const u32 r = expand6(t.color & 31) << 2, g = expand6((t.color >> 5) & 31) << 2;
            const u32 b = expand6((t.color >> 10) & 31) << 2;
            const u32 a = t.alpha * 255 / 31;
            rgba[size_t(y) * size_t(w) + size_t(x)] = (r | (r >> 6)) | ((g | (g >> 6)) << 8) | ((b | (b >> 6)) << 16) | (a << 24);
        }
    }
}

// For each texture of the list: its replacement (or none), and the dump of new ones.
std::unordered_map<u64, const Replacement*> prepare_textures(u32 disp) {
    std::unordered_map<u64, const Replacement*> found;
    if (!(disp & 1) || (!g_display_replace && !g_display_dump)) return found;
    std::vector<u32> rgba;
    for (const Polygon& p : g_ready) {
        const u32 format = (p.tex_param >> 26) & 7;
        if (format == 0) continue;
        const u64 fields = (u64(p.tex_param & ~((15u << 16) | (3u << 30))) << 32) | p.pal_base;
        if (found.count(fields)) continue;
        const int w = 8 << ((p.tex_param >> 20) & 7), h = 8 << ((p.tex_param >> 23) & 7);
        decode_texture(p.tex_param, p.pal_base, w, h, rgba);
        const u64 key = texture_key(w, h, rgba);
        if (g_display_dump && g_hooks.dump != nullptr && g_dumped.insert(key).second) g_hooks.dump(key, w, h, rgba);
        const Replacement* replacement = nullptr;
        if (g_display_replace && g_hooks.load != nullptr) {
            auto cached = g_replacements.find(key);
            if (cached == g_replacements.end()) {
                std::shared_ptr<Replacement> loaded;
                int rw = 0, rh = 0;
                std::vector<u32> pixels;
                if (g_hooks.load(key, rw, rh, pixels) && rw > 0 && rh > 0 && pixels.size() == size_t(rw) * size_t(rh)) {
                    loaded = std::make_shared<Replacement>();
                    loaded->w = rw;
                    loaded->h = rh;
                    loaded->kx = float(rw) / float(w);
                    loaded->ky = float(rh) / float(h);
                    loaded->rgba = std::move(pixels);
                    ++g_stats.textures_replaced;
                }
                cached = g_replacements.emplace(key, loaded).first;
            }
            replacement = cached->second.get();
        }
        found.emplace(fields, replacement);
    }
    return found;
}

void clear_target(Target& t) {
    const u32 clear = io32_at(0x350);
    const u32 clear_color = color15_to_18(clear & 0x7FFF) | (((clear >> 16) & 31) << 18);
    const u8 clear_id = u8((clear >> 24) & 63);
    const bool clear_fog = clear & 0x8000;
    const u32 clear_depth15 = io16_at(0x354) & 0x7FFF;
    const float clear_depth = float(clear_depth15 * 0x200u + 0x1FFu) / float(0xFFFFFF);
    std::fill(t.depth.begin(), t.depth.end(), clear_depth);
    std::fill(t.color.begin(), t.color.end(), clear_color);
    std::fill(t.id.begin(), t.id.end(), clear_id);
    std::fill(t.translucent_id.begin(), t.translucent_id.end(), s16(-1));
    std::fill(t.fog.begin(), t.fog.end(), u8(clear_fog));
    std::fill(t.stencil.begin(), t.stencil.end(), u8(0));
}

// Opaque first; translucent afterwards, top of the screen first unless the game keeps order.
std::vector<const Polygon*> drawing_order(bool manual_sort) {
    std::vector<const Polygon*> order;
    order.reserve(g_ready.size());
    for (const Polygon& p : g_ready) {
        if (!p.translucent) order.push_back(&p);
    }
    const std::size_t opaque_count = order.size();
    for (const Polygon& p : g_ready) {
        if (p.translucent) order.push_back(&p);
    }
    if (!manual_sort) {
        std::stable_sort(order.begin() + std::ptrdiff_t(opaque_count), order.end(),
                         [](const Polygon* a, const Polygon* b) {
                             return a->y_max != b->y_max ? a->y_max < b->y_max : a->y_min < b->y_min;
                         });
    }
    return order;
}

// The game's own picture is drawn on one core; the screen's in bands of rows on all of them. Each
// band sees every polygon in the same order, so the result is the same as on one core.
void draw_into(Target& t, const std::vector<const Polygon*>& order, u32 disp, bool w_mode,
               const std::unordered_map<u64, const Replacement*>* textures, bool parallel) {
    clear_target(t);
    std::vector<const Replacement*> replacements(order.size(), nullptr);
    if (textures != nullptr && !textures->empty()) {
        for (size_t k = 0; k < order.size(); ++k) {
            const Polygon* p = order[k];
            const u64 fields = (u64(p->tex_param & ~((15u << 16) | (3u << 30))) << 32) | p->pal_base;
            const auto found = textures->find(fields);
            if (found != textures->end()) replacements[k] = found->second;
        }
    }
    const auto rows = [&](int first, int last) {
        for (size_t k = 0; k < order.size(); ++k) {
            const Polygon* p = order[k];
            for (std::size_t i = 1; i + 1 < p->vertices.size(); ++i) {
                draw_triangle(*p, p->vertices[0], p->vertices[i], p->vertices[i + 1], disp, w_mode, t, replacements[k],
                              first, last);
            }
        }
    };
    if (parallel) parallel_rows(t.h, rows);
    else rows(0, t.h);
    apply_edges_and_fog(disp, t, parallel);
}

void draw_list() {
    u32 disp = io16_at(0x60);
    static const bool no_textures = std::getenv("MPDS_NO_TEXTURES") != nullptr;
    if (no_textures) disp &= ~1u;
    const bool w_mode = g_swap_param & 2;
    const std::vector<const Polygon*> order = drawing_order(g_swap_param & 1);

    // The game's own picture: the 2D engines and the display capture read it.
    draw_into(g_native, order, disp, w_mode, nullptr, false);
    std::memcpy(g_picture, g_native.color.data(), sizeof g_picture);
    ++g_stats.lists_drawn;

    // The screen's picture, only for showing; it never goes back into the game.
    const bool free_size = g_display_free_w > 0 && g_display_free_h > 0;
    const bool display = free_size || g_display_scale > 1 || g_display_replace;
    const auto textures = prepare_textures(disp);
    if (!display) return;
    if (free_size) {
        if (!g_display.free_size || g_display.w != g_display_free_w || g_display.h != g_display_free_h) {
            g_display.resize_free(g_display_free_w, g_display_free_h);
        }
        g_display.jitter_x = g_next_jitter_x;
        g_display.jitter_y = g_next_jitter_y;
    } else if (g_display.free_size || g_display.scale != g_display_scale || g_display.color.empty()) {
        g_display.resize(g_display_scale);
    }
    draw_into(g_display, order, disp, w_mode, &textures, true);
    const int next = g_display_index ^ 1;
    g_display_picture[next] = g_display.color;
    if (free_size) g_display_depth[next] = g_display.depth;
    else g_display_depth[next].clear();
    Gpu3dDisplayInfo& info = g_display_info[next];
    info.w = g_display.w;
    info.h = g_display.h;
    info.jitter_x = free_size ? g_display.jitter_x : 0.0f;
    info.jitter_y = free_size ? g_display.jitter_y : 0.0f;
    info.depth = g_display_depth[next].empty() ? nullptr : g_display_depth[next].data();
    g_display_index = next;
    g_display_drawn = true;
}

// --- Commands ---

void run_command(u8 cmd) {
    switch (cmd) {
        case 0x10: g_matrix_mode = g_params[0] & 3; break;
        case 0x11:  // push
            if (g_matrix_mode == 0) {
                g_projection_stack = g_projection;
                if (g_projection_level > 0) g_stack_error = true;
                g_projection_level = 1;
            } else if (g_matrix_mode == 3) {
                g_texture_stack = g_texture;
            } else {
                if (g_stack_level >= 31) g_stack_error = true;
                g_position_stack[g_stack_level & 31] = g_position;
                g_vector_stack[g_stack_level & 31] = g_vector;
                g_stack_level = (g_stack_level + 1) & 63;
            }
            break;
        case 0x12: {  // pop
            if (g_matrix_mode == 0) {
                if (g_projection_level == 0) g_stack_error = true;
                g_projection_level = 0;
                g_projection = g_projection_stack;
                update_clip();
            } else if (g_matrix_mode == 3) {
                g_texture = g_texture_stack;
            } else {
                const s32 level = s32(g_stack_level) - sign_extend(g_params[0], 6);
                if (level < 0 || level > 31) g_stack_error = true;
                g_stack_level = u32(level) & 63;
                g_position = g_position_stack[g_stack_level & 31];
                g_vector = g_vector_stack[g_stack_level & 31];
                update_clip();
            }
            break;
        }
        case 0x13: {  // store
            const u32 index = g_params[0] & 31;
            if (g_matrix_mode == 0) g_projection_stack = g_projection;
            else if (g_matrix_mode == 3) g_texture_stack = g_texture;
            else {
                if (index == 31) g_stack_error = true;
                g_position_stack[index] = g_position;
                g_vector_stack[index] = g_vector;
            }
            break;
        }
        case 0x14: {  // restore
            const u32 index = g_params[0] & 31;
            if (g_matrix_mode == 0) { g_projection = g_projection_stack; update_clip(); }
            else if (g_matrix_mode == 3) g_texture = g_texture_stack;
            else {
                if (index == 31) g_stack_error = true;
                g_position = g_position_stack[index];
                g_vector = g_vector_stack[index];
                update_clip();
            }
            break;
        }
        case 0x15: load(identity()); break;
        case 0x16: load(read_matrix(4, 4)); break;
        case 0x17: load(read_matrix(4, 3)); break;
        case 0x18: apply(read_matrix(4, 4), true); break;
        case 0x19: apply(read_matrix(4, 3), true); break;
        case 0x1A: apply(read_matrix(3, 3), true); break;
        case 0x1B: {  // scale: leaves the vector matrix alone
            Matrix scale = identity();
            for (int i = 0; i < 3; ++i) scale.m[i][i] = s32(g_params[i]);
            apply(scale, false);
            break;
        }
        case 0x1C: {  // translate
            Matrix move = identity();
            for (int i = 0; i < 3; ++i) move.m[3][i] = s32(g_params[i]);
            apply(move, true);
            break;
        }
        case 0x20: g_vertex_color = g_params[0] & 0x7FFF; break;
        case 0x21: {  // normal
            const s32 n[3] = {sign_extend(g_params[0], 10), sign_extend(g_params[0] >> 10, 10),
                              sign_extend(g_params[0] >> 20, 10)};
            if ((g_tex_param >> 30) == 2) {
                const Matrix& t = g_texture;
                g_tex_s = s32(((s64(n[0]) * t.m[0][0] + s64(n[1]) * t.m[1][0] + s64(n[2]) * t.m[2][0]) >> 21) + g_raw_s);
                g_tex_t = s32(((s64(n[0]) * t.m[0][1] + s64(n[1]) * t.m[1][1] + s64(n[2]) * t.m[2][1]) >> 21) + g_raw_t);
            }
            compute_lighting(n);
            break;
        }
        case 0x22: {  // texture coordinate
            g_raw_s = s16(g_params[0] & 0xFFFF);
            g_raw_t = s16(g_params[0] >> 16);
            g_tex_s = g_raw_s;
            g_tex_t = g_raw_t;
            if ((g_tex_param >> 30) == 1) {
                const Matrix& t = g_texture;
                g_tex_s = s32((s64(g_raw_s) * t.m[0][0] + s64(g_raw_t) * t.m[1][0] + t.m[2][0] + t.m[3][0]) >> 12);
                g_tex_t = s32((s64(g_raw_s) * t.m[0][1] + s64(g_raw_t) * t.m[1][1] + t.m[2][1] + t.m[3][1]) >> 12);
            }
            break;
        }
        case 0x23:
            g_cur[0] = s16(g_params[0] & 0xFFFF);
            g_cur[1] = s16(g_params[0] >> 16);
            g_cur[2] = s16(g_params[1] & 0xFFFF);
            add_vertex();
            break;
        case 0x24:
            g_cur[0] = sign_extend(g_params[0], 10) << 6;
            g_cur[1] = sign_extend(g_params[0] >> 10, 10) << 6;
            g_cur[2] = sign_extend(g_params[0] >> 20, 10) << 6;
            add_vertex();
            break;
        case 0x25:
            g_cur[0] = s16(g_params[0] & 0xFFFF);
            g_cur[1] = s16(g_params[0] >> 16);
            add_vertex();
            break;
        case 0x26:
            g_cur[0] = s16(g_params[0] & 0xFFFF);
            g_cur[2] = s16(g_params[0] >> 16);
            add_vertex();
            break;
        case 0x27:
            g_cur[1] = s16(g_params[0] & 0xFFFF);
            g_cur[2] = s16(g_params[0] >> 16);
            add_vertex();
            break;
        case 0x28:
            g_cur[0] += sign_extend(g_params[0], 10);
            g_cur[1] += sign_extend(g_params[0] >> 10, 10);
            g_cur[2] += sign_extend(g_params[0] >> 20, 10);
            add_vertex();
            break;
        case 0x29: g_poly_attr_next = g_params[0]; break;
        case 0x2A: g_tex_param = g_params[0]; break;
        case 0x2B: g_pal_base = g_params[0] & 0x1FFF; break;
        case 0x30:
            g_diffuse = g_params[0] & 0x7FFF;
            g_ambient = (g_params[0] >> 16) & 0x7FFF;
            if (g_params[0] & 0x8000) g_vertex_color = g_diffuse;
            break;
        case 0x31:
            g_specular = g_params[0] & 0x7FFF;
            g_emission = (g_params[0] >> 16) & 0x7FFF;
            g_use_shininess = g_params[0] & 0x8000;
            break;
        case 0x32: {  // light direction, turned by the vector matrix
            const u32 light = g_params[0] >> 30;
            const s64 x = sign_extend(g_params[0], 10), y = sign_extend(g_params[0] >> 10, 10);
            const s64 z = sign_extend(g_params[0] >> 20, 10);
            for (int i = 0; i < 3; ++i) {
                g_light_dir[light][i] = s32((x * g_vector.m[0][i] + y * g_vector.m[1][i] + z * g_vector.m[2][i]) >> 12);
            }
            break;
        }
        case 0x33: g_light_color[g_params[0] >> 30] = g_params[0] & 0x7FFF; break;
        case 0x34:
            for (int i = 0; i < 32; ++i) {
                for (int b = 0; b < 4; ++b) g_shininess[i * 4 + b] = u8(g_params[i] >> (b * 8));
            }
            break;
        case 0x40:
            g_prim_type = g_params[0] & 3;
            g_poly_attr = g_poly_attr_next;
            g_in_primitive = true;
            g_strip.clear();
            g_strip_count = 0;
            break;
        case 0x41: g_in_primitive = false; break;
        case 0x50:
            g_swap_param = g_params[0] & 3;
            g_swap_pending = true;
            break;
        case 0x60: g_viewport = g_params[0]; break;
        case 0x70: {  // box test: is any of the box inside the view?
            const s32 x = s16(g_params[0] & 0xFFFF), y = s16(g_params[0] >> 16);
            const s32 z = s16(g_params[1] & 0xFFFF), w = s16(g_params[1] >> 16);
            const s32 h = s16(g_params[2] & 0xFFFF), d = s16(g_params[2] >> 16);
            u32 all_outside = 0x3F;
            for (int corner = 0; corner < 8; ++corner) {
                const s64 cx = x + ((corner & 1) ? w : 0), cy = y + ((corner & 2) ? h : 0);
                const s64 cz = z + ((corner & 4) ? d : 0);
                s64 v[4];
                for (int i = 0; i < 4; ++i) {
                    v[i] = (cx * g_clip.m[0][i] + cy * g_clip.m[1][i] + cz * g_clip.m[2][i] + s64(0x1000) * g_clip.m[3][i]) >> 12;
                }
                u32 outside = 0;
                for (int i = 0; i < 3; ++i) {
                    if (v[i] > v[3]) outside |= 1u << (i * 2);
                    if (v[i] < -v[3]) outside |= 2u << (i * 2);
                }
                all_outside &= outside;
            }
            g_box_result = all_outside == 0;
            break;
        }
        case 0x71: {  // position test
            g_cur[0] = s16(g_params[0] & 0xFFFF);
            g_cur[1] = s16(g_params[0] >> 16);
            g_cur[2] = s16(g_params[1] & 0xFFFF);
            for (int i = 0; i < 4; ++i) {
                g_pos_result[i] = s32((s64(g_cur[0]) * g_clip.m[0][i] + s64(g_cur[1]) * g_clip.m[1][i]
                                       + s64(g_cur[2]) * g_clip.m[2][i] + s64(0x1000) * g_clip.m[3][i]) >> 12);
            }
            break;
        }
        case 0x72: {  // vector test
            const s64 x = sign_extend(g_params[0], 10) << 3, y = sign_extend(g_params[0] >> 10, 10) << 3;
            const s64 z = sign_extend(g_params[0] >> 20, 10) << 3;
            for (int i = 0; i < 3; ++i) {
                g_vec_result[i] = s16((x * g_vector.m[0][i] + y * g_vector.m[1][i] + z * g_vector.m[2][i]) >> 12);
            }
            break;
        }
        default: break;
    }
}

// Runs what is queued, unless the geometry engine waits for the next VBlank after a swap.
void run_queue() {
    while (!g_fifo.empty() && !g_swap_pending) {
        const FifoEntry entry = g_fifo.front();
        g_fifo.pop_front();
        const int needed = params_for(entry.cmd);
        if (needed < 0) {
            ++g_stats.unknown_commands;
            continue;
        }
        if (needed == 0) {
            run_command(entry.cmd);
            continue;
        }
        g_params[g_param_count++] = entry.param;
        if (g_param_count >= u32(needed)) {
            g_param_count = 0;
            run_command(entry.cmd);
        }
    }
    if (g_status_irq_mode == 2 ? g_fifo.empty() : (g_status_irq_mode == 1 && g_fifo.size() < 128)) {
        hw_raise(1u << 21);
    }
}

void start_next_packed() {
    // Takes commands from the packed word until one needs parameters.
    while (g_packed != 0 && g_packed_params == 0) {
        const u8 cmd = u8(g_packed & 0xFF);
        g_packed >>= 8;
        if (cmd == 0) continue;
        const int needed = params_for(cmd);
        if (needed <= 0) {
            g_fifo.push_back({cmd, 0});
        } else {
            g_packed_cmd = cmd;
            g_packed_params = u32(needed);
        }
    }
}

void fifo_write(u32 value) {
    if (g_packed_params > 0) {
        g_fifo.push_back({g_packed_cmd, value});
        --g_packed_params;
        if (g_packed_params == 0) start_next_packed();
        return;
    }
    g_packed = value;
    start_next_packed();
}

}  // namespace

void gpu3d_write(u32 addr, u32 size, u32 value) {
    if (size != 4) {
        // The ports take 32-bit writes; anything else has not been seen from this game.
        ++g_stats.unknown_commands;
        return;
    }
    if (addr < 0x04000440) {
        fifo_write(value);
    } else {
        g_fifo.push_back({u8((addr - 0x04000400) >> 2), value});
    }
    run_queue();
}

u32 gpu3d_read(u32 addr) {
    const u32 aligned = addr & ~3u;
    u32 value = 0;
    if (aligned == 0x04000600) {
        const u32 count = u32(std::min<std::size_t>(g_fifo.size(), 256));
        value = (g_box_result ? 2u : 0u) | ((g_stack_level & 31) << 8) | ((g_projection_level & 1) << 13)
                | (g_stack_error ? 0x8000u : 0u) | (count << 16) | (count >= 256 ? (1u << 24) : 0u)
                | (count < 128 ? (1u << 25) : 0u) | (count == 0 ? (1u << 26) : 0u)
                | (g_swap_pending || !g_fifo.empty() ? (1u << 27) : 0u) | (g_status_irq_mode << 30);
    } else if (aligned == 0x04000604) {
        value = u32(g_building.size()) | (g_vertices_written << 16);
    } else if (aligned >= 0x04000620 && aligned < 0x04000630) {
        value = u32(g_pos_result[(aligned - 0x04000620) / 4]);
    } else if (aligned == 0x04000630 || aligned == 0x04000634) {
        const u32 index = (addr - 0x04000630) / 2;
        return u16(g_vec_result[std::min(index, 2u)]);
    } else if (aligned >= 0x04000640 && aligned < 0x04000680) {
        const u32 index = (aligned - 0x04000640) / 4;
        value = u32(g_clip.m[index / 4][index % 4]);
    } else if (aligned >= 0x04000680 && aligned < 0x040006A4) {
        const u32 index = (aligned - 0x04000680) / 4;
        value = u32(g_vector.m[index / 3][index % 3]);
    }
    return value >> ((addr & 3) * 8);
}

void gpu3d_write_status(u32 value) {
    if (value & 0x8000) g_stack_error = false;
    g_status_irq_mode = value >> 30;
}

void gpu3d_vblank() {
    if (!g_swap_pending) return;
    g_ready.swap(g_building);
    g_building.clear();
    g_stats.last_polygons = u32(g_ready.size());
    g_stats.last_vertices = g_vertices_written;
    g_vertices_written = 0;
    draw_list();
    g_last_debug = g_debug;
    g_debug = DebugCounts{};
    g_swap_pending = false;
    run_queue();
}

const u32* gpu3d_line(u32 line) { return g_picture[line]; }

void gpu3d_set_display(int scale, bool replace_textures, bool dump_textures, const Gpu3dTextureHooks& hooks) {
    g_display_scale = std::clamp(scale, 1, 12);
    g_display_replace = replace_textures;
    g_display_dump = dump_textures;
    g_hooks = hooks;
    g_display_drawn = false;
    g_replacements.clear();
}

int gpu3d_display_scale() { return g_display_scale; }

int gpu3d_display_index() { return g_display_drawn ? g_display_index : -1; }

const u32* gpu3d_display_picture(int index) {
    if (index < 0 || index > 1 || g_display_picture[index].empty()) return nullptr;
    return g_display_picture[index].data();
}

void gpu3d_set_display_size(int w, int h) {
    g_display_free_w = std::max(0, w);
    g_display_free_h = std::max(0, h);
    g_display_drawn = false;
}

void gpu3d_set_display_jitter(float x, float y) {
    g_next_jitter_x = x;
    g_next_jitter_y = y;
}

bool gpu3d_display_info(int index, Gpu3dDisplayInfo& info) {
    if (index < 0 || index > 1 || g_display_picture[index].empty()) return false;
    info = g_display_info[index];
    return true;
}

const Gpu3dStats& gpu3d_stats() { return g_stats; }

void gpu3d_print_last_list() {
    const DebugCounts& d = g_last_debug;
    std::printf("3D list: submitted %u, far dropped %u, culled %u, translucent %u, large %u\n",
                d.submitted, d.far_dropped, d.culled, d.translucent, d.large);
    std::printf("  formats:");
    for (int i = 0; i < 8; ++i) std::printf(" %u", d.per_format[i]);
    std::printf("  modes:");
    for (int i = 0; i < 4; ++i) std::printf(" %u", d.per_mode[i]);
    std::printf("\n  disp3d %04x clear %08x depth %04x\n", io16_at(0x60), io32_at(0x350), io16_at(0x354));
    for (int i = 0; i < d.samples; ++i) {
        std::printf("  large polygon: attr %08x tex %08x pal %04x\n", d.sample_attr[i], d.sample_tex[i], d.sample_pal[i]);
    }
    std::printf("  texture slots:");
    for (u32 i = 0; i < 4; ++i) std::printf(" %d", vram_texture_slot(i) != nullptr);
    std::printf("  palette slots:");
    for (u32 i = 0; i < 6; ++i) std::printf(" %d", vram_texture_palette_slot(i) != nullptr);
    std::printf("\n");
}
