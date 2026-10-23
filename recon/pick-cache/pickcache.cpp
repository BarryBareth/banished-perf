// Exact cache for Picking::Scene::Intersect (engine ray casts).
//
// Every villager's ground height (exe+0xa48b0) on a cell with walkable geometry ray-casts straight down through the
// map's picking scene: a hashed grid of cells (integer cell = (int)(coordinate * scene+4)), each object listed in the
// cells its box covers, and for every object in the ray's cells a brute-force test of all its triangles. ~5% of a big
// town's frame, and most of the rays repeat exactly: villagers standing still at work ask the same question every tick.
//
// A result depends only on the ray and on the objects listed in the ray's cells (their transforms, meshes and list
// order). Objects change only through four engine functions, which we hook to bump a version per cell they touch:
//   SetTransform (old + new cells, only when the transform or the cells really change), AddMesh (new cells),
//   RemoveMesh (old cells); constructing / destroying / assigning a scene clears the whole cache.
// A cached answer is reused only while every cell under the ray has the version it had when the answer was computed.
// The one side effect of a call other than its outputs - the scene's query counter (+0x5c), which numbers queries so
// that objects spanning several cells are tested once - is advanced on a hit just like a real call.
// <game>\bperf_verify.txt: on every hit also run the real ray cast and compare everything (logged as MISMATCH).
#include "common.h"
#include <unordered_map>
#include <vector>

namespace pickcache {

struct V3 { float x, y, z; };

using Intersect_t = int(__fastcall*)(uint8_t* scene, const V3* o, const V3* d, const float* maxDist, V3* pos, V3* nrm);
using SetTransform_t = void(__fastcall*)(uint8_t* scene, uint32_t idx, const float* t);
using AddMesh_t = uint32_t(__fastcall*)(uint8_t*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using RemoveMesh_t = void(__fastcall*)(uint8_t* scene, uint32_t idx);
using SceneFn_t = void*(__fastcall*)(uint8_t* scene, void* other);
static Intersect_t o_Intersect;
static SetTransform_t o_SetTransform;
static AddMesh_t o_AddMesh;
static RemoveMesh_t o_RemoveMesh;
static SceneFn_t o_Ctor, o_CopyCtor, o_Dtor, o_Assign;

static bool g_verify;
static float g_c1, g_c2;  // the engine's clamp constants for the ray's box (Runtime +0x1648ec / +0x1648b0)
static SRWLOCK g_lock = SRWLOCK_INIT;
uint32_t g_calls, g_hits, g_mismatches;

// ---- cell versions ------------------------------------------------------------------------------------------------
// Walking villagers move their picking meshes every tick, so bumps are frequent: a flat array per scene for cells in
// [-kHalf, kHalf) (a large map is +-256 units), a hash map for anything outside.
static const int kHalf = 1024, kSide = 2 * kHalf;
struct CellKey {
    const uint8_t* scene;
    int x, y;
    bool operator==(const CellKey& o) const { return scene == o.scene && x == o.x && y == o.y; }
};
struct CellHash {
    size_t operator()(const CellKey& k) const {
        return (size_t)k.scene * 31 ^ (size_t)(uint32_t)k.x * 0x9E3779B1u ^ (size_t)(uint32_t)k.y * 0x85EBCA77u;
    }
};
static std::unordered_map<CellKey, uint32_t, CellHash> g_far;
struct Dense { const uint8_t* scene; uint32_t* v; };
static Dense g_dense[4];  // the few scenes that exist at a time (map, menu scene, ...)
static uint32_t g_epoch = 1;  // bumped when a scene is created / destroyed / assigned: every cached answer is void

static uint32_t* DenseOf(const uint8_t* scene, bool create) {
    for (Dense& d : g_dense)
        if (d.scene == scene) return d.v;
    if (!create) return nullptr;
    for (Dense& d : g_dense)
        if (!d.scene) {
            d.v = (uint32_t*)VirtualAlloc(nullptr, (size_t)kSide * kSide * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!d.v) return nullptr;
            d.scene = scene;
            return d.v;
        }
    return nullptr;  // more scenes than slots: they use the hash map
}
static inline bool InDense(int x, int y) { return (unsigned)(x + kHalf) < (unsigned)kSide && (unsigned)(y + kHalf) < (unsigned)kSide; }

static uint32_t Version(const uint8_t* scene, int x, int y) {
    if (InDense(x, y))
        if (uint32_t* v = DenseOf(scene, false)) return v[(size_t)(y + kHalf) * kSide + (x + kHalf)];
    auto it = g_far.find({scene, x, y});
    return it == g_far.end() ? 0 : it->second;
}
static void BumpCell(const uint8_t* scene, int x, int y) {
    if (InDense(x, y))
        if (uint32_t* v = DenseOf(scene, true)) {
            v[(size_t)(y + kHalf) * kSide + (x + kHalf)]++;
            return;
        }
    g_far[{scene, x, y}]++;
}
// A scene going away: forget its cells (a new scene at the same address must not see them as current - the epoch
// already voids every cached answer, this just keeps the tables from mixing two scenes' counts).
static void DropScene(const uint8_t* scene) {
    for (Dense& d : g_dense)
        if (d.scene == scene) {
            VirtualFree(d.v, 0, MEM_RELEASE);
            d = {};
        }
    for (auto it = g_far.begin(); it != g_far.end();) it = it->first.scene == scene ? g_far.erase(it) : std::next(it);
}

// object idx: +8 int x, +0xc int y, +0x10 u16 w, +0x12 u16 h (cells [x, x+w) x [y, y+h))
struct Rect { int x, y; uint16_t w, h; };
static Rect ObjRect(const uint8_t* scene, uint32_t idx) {
    const uint8_t* o = *(uint8_t* const*)(scene + 0x48) + (size_t)idx * 0x48;
    return {*(const int*)(o + 8), *(const int*)(o + 0xc), *(const uint16_t*)(o + 0x10), *(const uint16_t*)(o + 0x12)};
}
static void Bump(const uint8_t* scene, const Rect& r) {
    for (int y = r.y; y < r.y + (int)r.h; y++)
        for (int x = r.x; x < r.x + (int)r.w; x++) BumpCell(scene, x, y);
}

// ---- the cache ------------------------------------------------------------------------------------------------------
struct Entry {
    const uint8_t* scene;
    V3 o, d;
    float maxDist;
    uint32_t epoch;
    int x0, y0, x1, y1;
    uint32_t ver[4];
    int ret;
    V3 pos, nrm;
    bool valid;
};
static const size_t kSlots = 1 << 16;
static Entry* g_cache;

// The cells Intersect visits: the ray's box, exactly as the engine computes it.
static void RayCells(const uint8_t* scene, const V3* o, const V3* d, float L, int* x0, int* y0, int* x1, int* y1) {
    float maxY = o->y < g_c1 ? g_c1 : o->y, maxX = g_c1 <= o->x ? o->x : g_c1;
    float minY = g_c2 < o->y ? g_c2 : o->y, minX = o->x <= g_c2 ? o->x : g_c2;
    float ey = o->y + L * d->y, ex = o->x + L * d->x;
    if (maxY <= ey) maxY = ey;
    if (maxX <= ex) maxX = ex;
    if (ey <= minY) minY = ey;
    if (ex <= minX) minX = ex;
    float inv = *(const float*)(scene + 4);
    *x0 = (int)(inv * minX);
    *y0 = (int)(inv * minY);
    *x1 = (int)(inv * maxX);
    *y1 = (int)(inv * maxY);
}

static inline bool Same(const V3& a, const V3* b) { return memcmp(&a, b, sizeof(V3)) == 0; }
static size_t Slot(const uint8_t* scene, const V3* o, const V3* d, float L) {
    uint32_t w[8];
    memcpy(w, o, 12);
    memcpy(w + 3, d, 12);
    memcpy(w + 6, &L, 4);
    uint64_t h = (uint64_t)scene;
    for (int i = 0; i < 7; i++) h = (h ^ w[i]) * 0x100000001B3ull;
    return (size_t)(h ^ (h >> 29)) & (kSlots - 1);
}

static int __fastcall h_Intersect(uint8_t* scene, const V3* o, const V3* d, const float* maxDist, V3* pos, V3* nrm) {
    if (GetCurrentThreadId() != g_mainTid) return o_Intersect(scene, o, d, maxDist, pos, nrm);
    g_calls++;
    const float L = *maxDist;
    int x0, y0, x1, y1;
    RayCells(scene, o, d, L, &x0, &y0, &x1, &y1);
    bool cacheable = x1 >= x0 && y1 >= y0 && (int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) <= 4;
    if (!cacheable) return o_Intersect(scene, o, d, maxDist, pos, nrm);
    Entry& e = g_cache[Slot(scene, o, d, L)];
    AcquireSRWLockShared(&g_lock);
    bool hit = e.valid && e.scene == scene && e.epoch == g_epoch && Same(e.o, o) && Same(e.d, d) &&
               memcmp(&e.maxDist, &L, 4) == 0;
    if (hit) {
        int i = 0;
        for (int y = y0; y <= y1 && hit; y++)
            for (int x = x0; x <= x1; x++, i++)
                if (Version(scene, x, y) != e.ver[i]) { hit = false; break; }
    }
    ReleaseSRWLockShared(&g_lock);
    if (hit) {
        g_hits++;
        if (g_verify) {
            V3 p = *pos, n = *nrm;
            int r = o_Intersect(scene, o, d, maxDist, &p, &n);
            V3 cp = *pos, cn = *nrm;
            if (e.ret >= 0) cp = e.pos, cn = e.nrm;
            if (r != e.ret || memcmp(&p, &cp, 12) || memcmp(&n, &cn, 12)) {
                if (++g_mismatches <= 20)
                    Log("pickcache MISMATCH at (%a,%a,%a): real %d z %a, cached %d z %a", o->x, o->y, o->z, r, p.z,
                        e.ret, cp.z);
            }
            *pos = p;
            *nrm = n;
            return r;
        }
        *(int*)(scene + 0x5c) += 1;  // the query counter, as a real call would
        if (e.ret >= 0) *pos = e.pos, *nrm = e.nrm;
        return e.ret;
    }
    e.valid = false;
    // versions first: a change on another thread during the ray cast then makes this answer stale, never wrong
    AcquireSRWLockShared(&g_lock);
    e.epoch = g_epoch;
    int i = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) e.ver[i++] = Version(scene, x, y);
    ReleaseSRWLockShared(&g_lock);
    V3 p0 = *pos, n0 = *nrm;
    int r = o_Intersect(scene, o, d, maxDist, pos, nrm);
    e.scene = scene;
    e.o = *o;
    e.d = *d;
    e.maxDist = L;
    e.x0 = x0, e.y0 = y0, e.x1 = x1, e.y1 = y1;
    e.ret = r;
    e.pos = r >= 0 ? *pos : p0;
    e.nrm = r >= 0 ? *nrm : n0;
    e.valid = true;
    return r;
}

// ---- invalidation -----------------------------------------------------------------------------------------------------
static void __fastcall h_SetTransform(uint8_t* scene, uint32_t idx, const float* t) {
    const float* cur = *(float* const*)(*(uint8_t* const*)(scene + 0x48) + (size_t)idx * 0x48 + 0x20);
    Rect before = ObjRect(scene, idx);
    bool sameTransform = memcmp(cur, t, 7 * sizeof(float)) == 0;
    o_SetTransform(scene, idx, t);
    Rect after = ObjRect(scene, idx);
    if (sameTransform && memcmp(&before, &after, sizeof(Rect)) == 0) return;  // nothing changed
    AcquireSRWLockExclusive(&g_lock);
    Bump(scene, before);
    Bump(scene, after);
    ReleaseSRWLockExclusive(&g_lock);
}
static uint32_t __fastcall h_AddMesh(uint8_t* scene, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    uint32_t idx = o_AddMesh(scene, a, b, c, d, e, f);
    AcquireSRWLockExclusive(&g_lock);
    Bump(scene, ObjRect(scene, idx));
    ReleaseSRWLockExclusive(&g_lock);
    return idx;
}
static void __fastcall h_RemoveMesh(uint8_t* scene, uint32_t idx) {
    AcquireSRWLockExclusive(&g_lock);
    Bump(scene, ObjRect(scene, idx));
    ReleaseSRWLockExclusive(&g_lock);
    o_RemoveMesh(scene, idx);
}
static void NewEpoch(const uint8_t* scene) {
    AcquireSRWLockExclusive(&g_lock);
    g_epoch++;
    DropScene(scene);
    ReleaseSRWLockExclusive(&g_lock);
}
static void* __fastcall h_Ctor(uint8_t* s, void* o) { NewEpoch(s); return o_Ctor(s, o); }
static void* __fastcall h_CopyCtor(uint8_t* s, void* o) { NewEpoch(s); return o_CopyCtor(s, o); }
static void* __fastcall h_Dtor(uint8_t* s, void* o) { NewEpoch(s); return o_Dtor(s, o); }
static void* __fastcall h_Assign(uint8_t* s, void* o) { NewEpoch(s); return o_Assign(s, o); }

// Called from DllMain, before the multiplayer mod installs its own hooks: its Intersect hook (it hashes every result
// for desync detection) must wrap this one, so that it sees cached answers too.
void Install() {
    if (GameFileExists(L"bperf_nopick.txt")) {
        Log("pickcache: off (bperf_nopick.txt)");
        return;
    }
    HMODULE rt = GetModuleHandleW(L"Runtime-steam-x64.dll");
    if (!rt) return;
    g_c1 = *(const float*)((uint8_t*)rt + 0x1648ec);
    g_c2 = *(const float*)((uint8_t*)rt + 0x1648b0);
    g_cache = (Entry*)VirtualAlloc(nullptr, kSlots * sizeof(Entry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_cache) return;
    g_verify = GameFileExists(L"bperf_verify.txt");
    auto ex = [rt](const char* n) { return (void*)GetProcAddress(rt, n); };
    HookAt("Picking::Scene::SetTransform", ex("?SetTransform@Scene@Picking@@QEAAXIAEBVTransform@Math@@@Z"),
           (void*)h_SetTransform, (void**)&o_SetTransform);
    HookAt("Picking::Scene::AddMesh",
           ex("?AddMesh@Scene@Picking@@QEAAIV?$ExternalPtr@VPickingMesh@Picking@@@Object@@AEBVTransform@Math@@PEAXHHP6AX2HHAEBVVector3@6@33@Z@Z"),
           (void*)h_AddMesh, (void**)&o_AddMesh);
    HookAt("Picking::Scene::RemoveMesh", ex("?RemoveMesh@Scene@Picking@@QEAAXI@Z"), (void*)h_RemoveMesh, (void**)&o_RemoveMesh);
    HookAt("Picking::Scene::Scene", ex("??0Scene@Picking@@QEAA@XZ"), (void*)h_Ctor, (void**)&o_Ctor);
    HookAt("Picking::Scene::Scene(copy)", ex("??0Scene@Picking@@QEAA@AEBV01@@Z"), (void*)h_CopyCtor, (void**)&o_CopyCtor);
    HookAt("Picking::Scene::~Scene", ex("??1Scene@Picking@@QEAA@XZ"), (void*)h_Dtor, (void**)&o_Dtor);
    HookAt("Picking::Scene::operator=", ex("??4Scene@Picking@@QEAAAEAV01@AEBV01@@Z"), (void*)h_Assign, (void**)&o_Assign);
    HookAt("Picking::Scene::Intersect", ex("?Intersect@Scene@Picking@@QEBAHAEBVVector3@Math@@0AEBVScalar@4@AEAV34@2@Z"),
           (void*)h_Intersect, (void**)&o_Intersect);
    Log("pickcache: on%s", g_verify ? ", VERIFY mode (real ray cast on every hit)" : "");
}

}  // namespace pickcache
