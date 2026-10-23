// Exact fast paths for per-tick bookkeeping of moving characters (vanilla-exact: on by default).
//
// Spatial grid exe+0x19070(grid, index): re-files entry `index` (a character) in a quadtree-like grid of cells. It
// computes the cell from a square around the character's position (exe+0x97d90) through a dozen engine math calls
// (Rectangle::Center/Size, Scalar::Max, Vector2 +=/*=), and re-links the entry only if the cell changed - which for
// ~1000 characters a tick is rare. We compute the same cell with the same float operations in the same order (from
// the disassembly) and return at once when it is unchanged; otherwise the original runs and does the re-linking.
// <game>\bperf_verify.txt: every call also runs the original; a "no change" decision the original disagrees with is
// logged as MISMATCH. <game>\bperf_nomovefast.txt: off.
#include "common.h"
#include <cmath>

namespace movefast {

static uint8_t* g_exe;
static float g_half, g_k1, g_k2, g_k3;  // Runtime +0x164804 (Rectangle::Center), exe +0x178814, +0x1787b8, +0x17884c
static bool g_verify;
uint32_t g_gridCalls, g_gridSkipped, g_mismatches, g_pickCalls, g_pickSkipped, g_pickFast;

using GridUpdate_t = void(__fastcall*)(uint8_t* grid, int index);
static GridUpdate_t o_GridUpdate;

static inline uint32_t Spread(uint32_t v) {  // the two halves of the Morton interleave, as the game does them
    v = (v << 8 | v) & 0xff00ff;
    v = (v << 4 | v) & 0xf0f0f0f;
    return (v * 4 | v) & 0x33333333;
}

// The cell exe+0x19070 would file entry `index` in.
static uint32_t Cell(const uint8_t* grid, int index) {
    const uint8_t* entry = *(uint8_t* const*)(grid + 0x10) + (size_t)index * 0x18;
    const uint8_t* comp = *(uint8_t* const*)entry;         // the character's component (exe+0x97d90)
    const uint8_t* model = *(uint8_t* const*)(comp + 0x68);
    float x = *(const float*)(model + 0x158), y = *(const float*)(model + 0x15c);
    float r = *(const float*)(*(uint8_t* const*)(comp + 0x78) + 0x20);
    float r0 = x - r, r1 = y - r, r2 = r + x, r3 = r + y;  // the rectangle
    float cx = (r0 + r2) * g_half, cy = (r3 + r1) * g_half;  // Rectangle::Center
    float a = (r2 - r0) * g_k1, b = (r3 - r1) * g_k1;        // Rectangle::Size * k1
    float m = a > b ? a : b;                                 // Scalar::Max(a, b)
    cx = *(const float*)(grid + 4) + cx;                     // Vector2 += grid offset
    cy = *(const float*)(grid + 8) + cy;
    float worldSize = *(const float*)grid;
    float q = worldSize / (m + g_k2);
    uint32_t qb;
    memcpy(&qb, &q, 4);
    int level = (int)(qb >> 23) - 0x7f;
    int levels = *(const int*)(grid + 0xc);
    if (level >= levels) level = levels - 1;
    if (level < 0) level = 0;
    int cells = 1 << level;
    float s = g_k3 / (worldSize / (float)cells);
    cx = s * cx;  // Vector2 *= (s, s)
    cy = s * cy;
    int ix = (int)cx, iy = (int)cy;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix >= cells) ix = cells - 1;
    if (iy >= cells) iy = cells - 1;
    uint32_t X = Spread((uint32_t)ix), Y = Spread((uint32_t)iy);
    uint32_t y2 = (Y * 2 | Y) * 2;
    uint32_t cell = ((y2 ^ X) & 0x55555555) ^ y2 | (X & 0x2aaaaaaa) * 2;
    return cell + (uint32_t)(*(const int* const*)(grid + 0x40))[level];
}

static void __fastcall h_GridUpdate(uint8_t* grid, int index) {
    g_gridCalls++;
    uint8_t* entry = *(uint8_t**)(grid + 0x10) + (size_t)index * 0x18;
    uint32_t cell = Cell(grid, index);
    bool same = *(uint32_t*)(entry + 0x10) == cell;
    if (g_verify) {
        uint32_t before = *(uint32_t*)(entry + 0x10);
        o_GridUpdate(grid, index);
        entry = *(uint8_t**)(grid + 0x10) + (size_t)index * 0x18;
        uint32_t after = *(uint32_t*)(entry + 0x10);
        if (after != cell && ++g_mismatches <= 20)
            Log("movefast grid MISMATCH entry %d: was %u, game filed %u, mine %u", index, before, after, cell);
        if (same) g_gridSkipped++;
        return;
    }
    if (same) {
        g_gridSkipped++;
        return;
    }
    o_GridUpdate(grid, index);
}

// Picking::Scene::SetTransform(scene, object, transform): stores the object's first instance transform (7 floats:
// quaternion x y z w, position x y z) and re-files the object in the scene's grid (rt+0x72fa0): the x/y box of all its
// mesh parts under their transforms (rt+0x128450 -> 0x1fd50, matrix from Transform::AsMatrix) times the grid's
// cells-per-unit (scene+4), truncated -> cell rectangle; the grid lists change only if the rectangle does. Walking
// characters move a little every tick but rarely leave their cells.
// We compute the box in double precision. The engine's float result differs from it by far less than 0.01 of a cell,
// so wherever all four edges are at least 0.01 cell away from a cell boundary both truncate to the same cells -
// then, if that rectangle is the one on file, the re-file would do nothing and we only store the transform. Anything
// else (near a boundary, new cells, no parts) runs the original. Exact by construction; verify mode checks it.
using SetTransform_t = void(__fastcall*)(uint8_t* scene, uint32_t object, const float* t);
static SetTransform_t o_SetTransform;
static float g_boxHalf;  // Runtime +0x164804 (used by the box centre/extent)

// The cell rectangle (x0, y0, x1, y1 before the width/height step) the re-file would compute, or false if unsure.
static bool SafeRect(const uint8_t* scene, const uint8_t* obj, const float* t0, int out[4]) {
    const uint8_t* mesh = *(uint8_t* const*)(obj + 0x18);
    const float* xf = *(float* const*)(obj + 0x20);
    int parts = *(const int*)(obj + 0x28);
    if (!mesh || parts <= 0) return false;
    const uint8_t* boxes = *(uint8_t* const*)(mesh + 0x38);
    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    for (int i = 0; i < parts; i++) {
        const float* t = i == 0 ? t0 : xf + i * 7;
        double qx = t[0], qy = t[1], qz = t[2], qw = t[3];
        // Transform::AsMatrix (row vectors): rows m0..m2, m4..m6, m8..m10, translation m12..m14
        double m0 = 1 - 2 * (qz * qz + qy * qy), m1 = 2 * (qx * qy - qw * qz), m4 = 2 * (qw * qz + qx * qy);
        double m5 = 1 - 2 * (qz * qz + qx * qx), m8 = 2 * (qx * qz - qw * qy), m9 = 2 * (qw * qx + qz * qy);
        const float* b = (const float*)(boxes + (size_t)i * 0x80 + 0x28);  // min xyz, max xyz
        double cx = ((double)b[3] + b[0]) * 0.5, cy = ((double)b[4] + b[1]) * 0.5, cz = ((double)b[5] + b[2]) * 0.5;
        double ex = ((double)b[3] - b[0]) * 0.5, ey = ((double)b[4] - b[1]) * 0.5, ez = ((double)b[5] - b[2]) * 0.5;
        double wx = m4 * cy + m0 * cx + m8 * cz + t[4];
        double wy = m5 * cy + m1 * cx + m9 * cz + t[5];
        double hx = fabs(m0) * ex + fabs(m4) * ey + fabs(m8) * ez;
        double hy = fabs(m1) * ex + fabs(m5) * ey + fabs(m9) * ez;
        if (wx - hx < minx) minx = wx - hx;
        if (wy - hy < miny) miny = wy - hy;
        if (hx + wx > maxx) maxx = hx + wx;
        if (hy + wy > maxy) maxy = hy + wy;
    }
    double inv = *(const float*)(scene + 4);
    double v[4] = {inv * minx, inv * miny, inv * maxx, inv * maxy};
    for (int i = 0; i < 4; i++) {
        if (!(fabs(v[i]) < 1e8)) return false;
        double frac = v[i] - floor(v[i]);
        if (frac < 0.01 || frac > 0.99) return false;  // near a cell boundary (or 0): let the engine decide
        out[i] = (int)v[i];
    }
    return true;
}

static void __fastcall h_SetTransform(uint8_t* scene, uint32_t object, const float* t) {
    g_pickCalls++;
    uint8_t* obj = *(uint8_t**)(scene + 0x48) + (size_t)object * 0x48;
    float* cur = *(float**)(obj + 0x20);
    const uint32_t* cw = (const uint32_t*)cur;
    const uint32_t* tw = (const uint32_t*)t;
    if (((cw[0] ^ tw[0]) | (cw[1] ^ tw[1]) | (cw[2] ^ tw[2]) | (cw[3] ^ tw[3]) | (cw[4] ^ tw[4]) | (cw[5] ^ tw[5]) |
         (cw[6] ^ tw[6])) == 0) {  // the same transform: nothing would change
        g_pickSkipped++;
        if (!g_verify) return;
    }
    int r[4];
    bool safe = SafeRect(scene, obj, t, r);
    bool same = safe && *(int*)(obj + 8) == r[0] && *(int*)(obj + 0xc) == r[1] &&
                *(uint16_t*)(obj + 0x10) == (uint16_t)((uint16_t)r[2] - (uint16_t)r[0] + 1) &&
                *(uint16_t*)(obj + 0x12) == (uint16_t)((uint16_t)r[3] - (uint16_t)r[1] + 1);
    if (g_verify) {
        uint8_t before[12];
        memcpy(before, obj + 8, 12);
        o_SetTransform(scene, object, t);
        obj = *(uint8_t**)(scene + 0x48) + (size_t)object * 0x48;
        if (same && memcmp(before, obj + 8, 12) != 0 && ++g_mismatches <= 20)
            Log("movefast picking MISMATCH object %u: I said unchanged, the game re-filed it", object);
        if (same) g_pickFast++;
        return;
    }
    if (same) {
        memcpy(cur, t, 7 * sizeof(float));
        g_pickFast++;
        return;
    }
    o_SetTransform(scene, object, t);
}

void Install() {
    g_exe = (uint8_t*)GetModuleHandleW(nullptr);
    HMODULE rt = GetModuleHandleW(L"Runtime-steam-x64.dll");
    if (!rt || GameFileExists(L"bperf_nomovefast.txt")) {
        Log("movefast: off");
        return;
    }
    g_half = *(float*)((uint8_t*)rt + 0x164804);
    g_k1 = *(float*)(g_exe + 0x178814);
    g_k2 = *(float*)(g_exe + 0x1787b8);
    g_k3 = *(float*)(g_exe + 0x17884c);
    g_verify = GameFileExists(L"bperf_verify.txt");
    HookAt("SpatialGrid::Update", g_exe + 0x19070, (void*)h_GridUpdate, (void**)&o_GridUpdate);
    HookAt("Picking::Scene::SetTransform",
               (void*)GetProcAddress(rt, "?SetTransform@Scene@Picking@@QEAAXIAEBVTransform@Math@@@Z"),
               (void*)h_SetTransform, (void**)&o_SetTransform);
    Log("movefast: on%s", g_verify ? ", VERIFY mode" : "");
}

}  // namespace movefast
