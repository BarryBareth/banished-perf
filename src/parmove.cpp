// Parallel villager movement (<game>\bperf_parallelmove.txt). Not vanilla-exact - deterministic; every multiplayer
// player needs the same setting (it is part of BperfSimSignature).
//
// Each tick the entity manager (exe+0xc9560) updates entities one by one; a character's update
// (CharacterComponent::vf8, exe+0x10e990) is its AI step (exe+0xca120, if it has an action) followed by its movement:
// new x/y from the animation's root motion, the ground height there (exe+0xa48b0: terrain heightfield + a ray down
// through the map's picking scene), then the model transform, the render geometry, the selection picking scene and the
// spatial grid. Ground height alone is ~5% of a big town's frame and only reads state.
//
// This mode moves every character's movement out of the entity loop into a phase run before the manager destroys
// anything that tick (first exe+0xc8db0 call, or the end of the update):
//   loop     : the AI step only; the character is queued (in update order)
//   phase 1  : worker threads compute everything the movement stores - heading, position, ground height, rotation -
//              with the same float operations as vf8 (Predict; read-only work)
//   phase 2  : in queue order: store transform + heading, then the shared structures in vf8's order (render geometry,
//              selection picking scene, spatial grid). With bperf_verify.txt: the game's own vf8 runs instead (AI step
//              switched off via +0x60 = -1, its ground-height call answered from phase 1) and what it stores is
//              compared with the prediction
// The difference from vanilla: an entity updated later in the loop sees a character's position from before its move
// this tick instead of after. Results depend only on game state - never on thread timing or core count.
//
// Phase 1 calls the game's code underneath the multiplayer mod's hooks (pass-through hooks installed from DllMain, so
// the multiplayer DLL's hooks wrap them): its visual-scope stub on exe+0xb6460 and its probes on Intersect /
// RootAnimationOffset keep global, single-threaded state. Ray casts from workers run on a private copy of the picking
// scene's header whose query counter (+0x5c, which numbers queries for the per-object "visited" stamps) counts in a
// range of negative values per worker: never equal to the scene's own counter or another worker's.
#include "common.h"
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <vector>

namespace parmove {

static uint8_t* g_exe;
static bool g_on;
uint32_t g_jobs, g_served, g_computed, g_fast, g_mismatches;
static bool g_verify;
using GeomSetTransform_t = void(__fastcall*)(void* geometry, const void* transform);
using PickSetTransform_t = void(__fastcall*)(uint8_t* scene, uint32_t object, const float* t);
static GeomSetTransform_t g_geomSetTransform;
static PickSetTransform_t g_pickSetTransform;

struct V3 { float x, y, z; };

// ---- trampolines beneath the multiplayer mod (installed from DllMain) ----------------------------------------------
using GroundPoint_t = float*(__fastcall*)(int* map, float* out, const float* in, char noPick, float* normal);
using TerrainHeight_t = float(__fastcall*)(void* terrain, const float* in, float* normal);
using Intersect_t = int(__fastcall*)(uint8_t* scene, const V3* o, const V3* d, const float* maxDist, V3* pos, V3* nrm);
using RootOffset_t = V3*(__fastcall*)(void* meshInstance, V3* out);
static GroundPoint_t o_GroundPoint;
static TerrainHeight_t o_TerrainHeight;
static Intersect_t o_Intersect;
static RootOffset_t o_RootOffset;
static float g_originZ, g_dirZ, g_maxDist;  // a48b0's ray: from (x, y, exe+0x1788e8) along (0, 0, exe+0x178984)

struct Job {
    uint8_t* comp;
    uint8_t* entity;
    const void* dt;  // the tick's time step (what the entity loop passed to vf8)
    bool deferAI;    // parallel AI: this character's AI step was left for phase 1 (it looked like steady walking)
    bool serial;     // ... and phase 1 could not take it: phase 2 runs the game's whole vf8 (AI step + movement)
    bool valid;      // inX/inY/out: the height question vf8 will ask, and its answer
    bool full;       // t/heading: everything vf8's movement stores (phase 2 then only stores and re-files)
    bool pick;       // vf8 would update the selection picking scene (has a picking object, not a root-only update)
    float inX, inY;
    float out[3];
    float t[7];      // the model transform: quaternion (0, 0, -sin a, cos a), position
    float heading;   // model +0x168
    const void* pre[6];  // phase 1 -> phase 2: the cache lines phase 2's stores will touch (prefetched a few jobs ahead)
    int npre;
};
static std::vector<Job> g_queue;
static bool g_collect;          // inside the entity manager's update
static uint8_t* g_curEntity;    // the entity whose components are updating (exe+0xc82b0)
static Job* g_armed;            // phase 2: the job whose vf8 is running

static int __fastcall h_IntersectPassthrough(uint8_t* s, const V3* o, const V3* d, const float* m, V3* p, V3* n) {
    return o_Intersect(s, o, d, m, p, n);
}
static V3* __fastcall h_RootOffsetPassthrough(void* mi, V3* out) { return o_RootOffset(mi, out); }
static float __fastcall h_TerrainHeight(void* t, const float* in, float* n) { return o_TerrainHeight(t, in, n); }

// exe+0xa48b0, phase 2: the character's ground height from phase 1 when the question is the same
static float* __fastcall h_GroundPoint(int* map, float* out, const float* in, char noPick, float* normal) {
    Job* j = g_armed;
    if (j && j->valid && !noPick && !normal && memcmp(&in[0], &j->inX, 4) == 0 && memcmp(&in[1], &j->inY, 4) == 0) {
        out[0] = j->out[0], out[1] = j->out[1], out[2] = j->out[2];
        g_served++;
        return out;
    }
    if (j) g_computed++;
    return o_GroundPoint(map, out, in, noPick, normal);
}

// ---- phase 1 ----------------------------------------------------------------------------------------------------------
static thread_local uint8_t t_scene[0x80];  // private copy of the map picking scene's header
// Query numbers for phase-1 ray casts: one counter for all threads, in the negative range (the scene's own counter
// counts up from 0), so no two queries ever share a number whichever thread runs them.
static std::atomic<int32_t> g_rayCounter{(int32_t)0x80000000u};

// exe+0xa48b0 for noPick = 0, normal = null, computed with the trampolines and a private scene header
static void GroundPoint(int* map, float x, float y, float* out) {
    uint8_t* gl = *(uint8_t**)(g_exe + 0x1cbdd8);
    float in[3] = {x, y, g_originZ};
    float h = o_TerrainHeight(*(void**)(gl + 0x110), in, nullptr);
    int n = map[0];
    int cx = (int)(x + (float)(n >> 1)), cy = (int)(y + (float)(n >> 1));
    if (cx < 0) cx = 0;
    if (n <= cx) cx = n - 1;
    if (cy < 0) cy = 0;
    if (n <= cy) cy = n - 1;
    uint8_t* cells = *(uint8_t**)((uint8_t*)map + 0x80);
    if ((int8_t)cells[((int64_t)n * cy + cx) * 0x30 + 0x2d] > 0) {
        memcpy(t_scene, (uint8_t*)map + 8, sizeof t_scene);
        *(int32_t*)(t_scene + 0x5c) = g_rayCounter.fetch_add(1);  // Intersect adds 1 and uses that
        V3 o{x, y, g_originZ}, d{0, 0, g_dirZ}, pos{0, 0, 0}, nrm{0, 0, 0};
        int r = o_Intersect(t_scene, &o, &d, &g_maxDist, &pos, &nrm);
        if (r >= 0 && h < pos.z) {
            out[0] = pos.x, out[1] = pos.y, out[2] = pos.z;
            return;
        }
    }
    out[0] = x, out[1] = y, out[2] = h;
}

// ---- parallel AI: steady walking -----------------------------------------------------------------------------------
// A character's AI step (exe+0xca120 on its action queue c+0x48: [0] slots, [3] current index) for a villager in
// the middle of a walk is one call of MoveTo::vf5 (exe+0xcda40) on its started, unchained MoveTo, returning 1. That
// call reads the map, the villager's own position and its own path, and writes only the villager's steering (speed
// +0x38, direction +0x2c, heading target +0x3c) - except in three branches: entering the next path cell (exe+0xce1f0
// asks the ground height: the multiplayer mod's non-thread-safe hooks), the end of the path (animation change through
// the shared AnimationManager) and a path still being computed. Those are decided by the same integer cell arithmetic
// vf5 uses, so a worker can tell beforehand; it then runs the game's own vf5 through a trampoline beneath the
// multiplayer mod's action probe. A 0 ("arrived") comes from a branch that writes nothing; the character then goes to
// phase 2, where the game's whole vf8 runs (and gets the same 0 and finishes the action), like every other case.
static bool g_parallelAI;
static uintptr_t g_moveToVt;
using MoveToVf5_t = int(__fastcall*)(uint8_t* action, void* queue, const void* dt);
static MoveToVf5_t o_MoveToVf5;
static int __fastcall h_MoveToVf5Passthrough(uint8_t* a, void* q, const void* dt) { return o_MoveToVf5(a, q, dt); }
uint32_t g_aiDeferred, g_aiParallel;

// Main thread, in the entity loop: does this character's AI step look like steady walking? (cheap, own state only)
static bool LooksLikeSteadyWalk(uint8_t* c) {
    int64_t* q = (int64_t*)(c + 0x48);
    int idx = (int)q[3];
    if (idx < 0 || !q[0]) return false;
    uint8_t* a = ((uint8_t**)q[0])[idx];
    if (!a || !a[0x18] || *(void**)(a + 0x10) || *(uintptr_t*)a != g_moveToVt) return false;
    int wp = *(int*)(a + 0x60), count = *(int*)(a + 0x64);
    return wp != -1 && (count < 2 || wp < count);
}

// Worker: run the AI step if it is one of vf5's thread-safe branches; false = leave it to phase 2.
static bool Steer(Job& j) {
    uint8_t* c = j.comp;
    // later entities in the loop may have changed the queue since (a cancel sets its index to -1): check again
    if (!LooksLikeSteadyWalk(c)) return false;
    int64_t* q = (int64_t*)(c + 0x48);
    uint8_t* a = ((uint8_t**)q[0])[(int)q[3]];
    uint8_t* model = *(uint8_t**)(*(uint8_t**)(a + 0x28) + 0x68);
    int* map = *(int**)(*(uint8_t**)(g_exe + 0x1cbdd8) + 0xf8);
    int n = map[0];
    float half = (float)(n >> 1);
    auto cell = [n, half](float x, float y) {
        int ix = (int)(x + half), iy = (int)(y + half);
        if (ix < 0) ix = 0;
        if (n <= ix) ix = n - 1;
        if (iy < 0) iy = 0;
        if (n <= iy) iy = n - 1;
        return n * iy + ix;
    };
    int dest = cell(*(float*)(a + 0x38), *(float*)(a + 0x3c));
    int cur = cell(*(float*)(model + 0x158), *(float*)(model + 0x15c));
    int wp = *(int*)(a + 0x60), count = *(int*)(a + 0x64);
    if (dest != cur && count >= 2) {
        if (wp >= count) return false;                          // end of the path: animation change
        if (cur == *(int*)(a + 0x68 + (size_t)wp * 4)) return false;  // entering the next path cell: exe+0xce1f0
    }
    if (o_MoveToVf5(a, q, j.dt) != 1) return false;  // arrived: nothing written; phase 2 redoes it in full
    g_aiParallel++;
    return true;
}

// Everything vf8's movement computes (charvf8 + its disassembly, exe+0x10e990), in the same float operations.
static float g_pi, g_twoPi, g_eps, g_one, g_negPi, g_angK1, g_angK2;
using Trig_t = float(__fastcall*)(float);
static Trig_t g_sin, g_cos;  // the game's own (exe+0x137160 / exe+0x136560)
static void Predict(Job& j) {
    j.valid = j.full = j.pick = false;
    j.npre = 0;
    uint8_t* c = j.comp;
    if (j.entity && ((*(uint32_t*)(j.entity + 0x40) >> 10) & 1)) return;  // died this tick: phase 2 skips it
    if (j.deferAI && !Steer(j)) {
        j.serial = true;
        return;
    }
    uint8_t* model = *(uint8_t**)(c + 0x68);
    uint8_t* move = *(uint8_t**)(c + 0x78);
    if (!model || !move || *(int*)(c + 0x40) < 0) return;
    bool rootOnly = **(uint8_t**)(g_exe + 0x158028) != 0;  // MeshInstance::_rootOnlyUpdates
    // heading: towards the target (+0x3c), smoothed by move+0x28, angles wrapped to (-pi, pi]
    float target = *(float*)(c + 0x3c);
    float cur = *(float*)(model + 0x168) - *(float*)(move + 0x24);
    float d = target - cur;
    if (!(g_pi > fabsf(d))) d = d > 0.0f ? -(g_twoPi - d) : d + g_twoPi;
    float heading;
    if (fabsf(d) > g_eps) {
        float k = *(float*)(move + 0x28);
        heading = (d + cur) * (g_one - k) + k * cur;
        if (heading > g_pi) heading -= g_twoPi;
        else if (g_negPi > heading) heading += g_twoPi;
    } else {
        heading = target;
    }
    // position: root motion x speed along the direction vector
    float x = *(float*)(model + 0x158), y = *(float*)(model + 0x15c), z = *(float*)(model + 0x160);
    V3 r;
    o_RootOffset(model + 0x28, &r);
    float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z) * *(float*)(c + 0x38);
    if (len > 0.0f) {
        float dx = len * *(float*)(c + 0x2c), dy = len * *(float*)(c + 0x30), dz = len * *(float*)(c + 0x34);
        x = x + dx, y = y + dy, z = z + dz;
    }
    int* map = *(int**)(*(uint8_t**)(g_exe + 0x1cbdd8) + 0xf8);
    bool water = false;
    if (!rootOnly) {
        int n = map[0];
        int cx = (int)(x + (float)(n >> 1)), cy = (int)(y + (float)(n >> 1));
        if (cx < 0) cx = 0;
        if (n <= cx) cx = n - 1;
        if (cy < 0) cy = 0;
        if (n <= cy) cy = n - 1;
        water = (*(uint8_t**)((uint8_t*)map + 0x80))[((int64_t)n * cy + cx) * 0x30 + 0x29] == 0x0b;
    }
    if (water) {
        z = 0.0f;
    } else if (move[0x3c]) {  // fixed height (flying)
        z = *(float*)(move + 0x40);
    } else if (!rootOnly) {   // on the ground
        j.inX = x, j.inY = y;
        GroundPoint(map, x, y, j.out);
        j.valid = true;
        x = j.out[0], y = j.out[1];
        float floorZ = *(float*)(move + 0x44);
        z = j.out[2] > floorZ ? j.out[2] : floorZ;  // Scalar::Max
    }
    heading = heading + *(float*)(move + 0x24);
    float ang = heading * g_angK1 * g_angK2;
    float sn = g_sin(ang), cs = g_cos(ang);
    j.t[0] = 0.0f, j.t[1] = 0.0f, j.t[2] = -sn, j.t[3] = cs;
    j.t[4] = x, j.t[5] = y, j.t[6] = z;
    j.heading = heading;
    j.pick = *(void**)(c + 0x70) && !rootOnly;
    j.full = true;
    // what phase 2 will touch, looked up here where misses cost nothing on the main thread: the model's transform and
    // render geometry, the picking object and its stored transform, the spatial-grid entry (read only; a stale address
    // would only waste a prefetch)
    uint8_t* gl = *(uint8_t**)(g_exe + 0x1cbdd8);
    j.pre[j.npre++] = model + 0x140;
    j.pre[j.npre++] = model + 0x80;
    if (j.pick) {
        int idx = *(int*)(*(uint8_t**)(c + 0x70) + 0x28);
        uint8_t* objs = *(uint8_t**)(*(uint8_t**)(gl + 0x70) + 0x48);
        if (idx >= 0 && objs) {
            uint8_t* obj = objs + (size_t)idx * 0x48;
            j.pre[j.npre++] = obj;
            j.pre[j.npre++] = *(uint8_t**)(obj + 0x20);
        }
    }
    if (uint8_t* entries = *(uint8_t**)(*(uint8_t**)(gl + 0x88) + 0x10))
        j.pre[j.npre++] = entries + (size_t)*(int*)(c + 0x40) * 0x18;
}

// workers: a parallel-for over g_queue per phase (each worker has its own wake-up event)
static int g_workers;
static HANDLE g_wake[8];
static std::atomic<size_t> g_next;
static std::atomic<int> g_busy;
// ---- parallel AI: crowding checks ----------------------------------------------------------------------------------
// MoveTo::vf6 (exe+0xcde40), from the time-sliced CharacterComponent::vf9 -> exe+0xca450: the nearest other character
// within 0.5 of the walker (exe+0x97730: a read-only walk of the spatial grid), and if it has the higher address and
// heads the same way, the walker's "slow down" flag (action +0x474, read by its next vf5) is set, else cleared. In
// parallel-AI mode the calls are collected during the entity update and run on the workers after phase 1's steering
// and movement (so every vf5 of the tick has read the flag before it changes), with the grid and positions as they
// stand then (movement is stored in phase 2). An action cancelled and freed meanwhile is skipped: the flag would only
// have been written to it.
using MoveToVf6_t = void(__fastcall*)(uint8_t* action, void* a2, void* a3);
static MoveToVf6_t o_MoveToVf6;
static std::vector<uint8_t*> g_crowd;  // actions whose vf6 waits for the phase (in call order)
uint32_t g_crowdChecks;
static void __fastcall h_MoveToVf6(uint8_t* a, void* a2, void* a3) {
    if (g_collect && GetCurrentThreadId() == g_mainTid) {
        g_crowd.push_back(a);
        return;
    }
    o_MoveToVf6(a, a2, a3);
}
static void CrowdCheck(uint8_t* a) {
    uint8_t* c = *(uint8_t**)(a + 0x28);  // the walker's CharacterComponent
    int64_t* q = (int64_t*)(c + 0x48);
    int idx = (int)q[3];
    if (idx < 0 || !q[0]) return;
    for (uint8_t* x = ((uint8_t**)q[0])[idx]; x; x = *(uint8_t**)(x + 0x10))
        if (x == a) {  // still in the walker's current slot: alive
            o_MoveToVf6(a, nullptr, nullptr);
            return;
        }
}

// what the workers run in the current sub-phase
static int g_task;  // 0: Predict over g_queue, 1: CrowdCheck over g_crowd
static void RunSome() {
    if (g_task == 0)
        for (size_t i; (i = g_next.fetch_add(1)) < g_queue.size();) Predict(g_queue[i]);
    else
        for (size_t i; (i = g_next.fetch_add(1)) < g_crowd.size();) CrowdCheck(g_crowd[i]);
}
static DWORD WINAPI Worker(LPVOID p) {
    HANDLE wake = (HANDLE)p;
    for (;;) {
        WaitForSingleObject(wake, INFINITE);
        RunSome();
        g_busy--;
    }
}

using Vf8_t = void(__fastcall*)(uint8_t*, const void* dt);
static Vf8_t o_Vf8;

static void ParallelFor(int task, size_t n) {
    g_task = task;
    g_next = 0;
    if (g_workers && n >= 16) {
        g_busy = g_workers;
        for (int i = 0; i < g_workers; i++) SetEvent(g_wake[i]);
        RunSome();
        while (g_busy > 0) YieldProcessor();  // every worker has finished its last job (and won't touch the lists)
    } else {
        RunSome();
    }
}

static void Phase() {
    if (g_queue.empty() && g_crowd.empty()) return;
    g_collect = false;
    g_jobs += (uint32_t)g_queue.size();
    // phase 1: steering (parallel AI) and movement, then the crowding checks
    ParallelFor(0, g_queue.size());
    if (!g_crowd.empty()) {
        g_crowdChecks += (uint32_t)g_crowd.size();
        ParallelFor(1, g_crowd.size());
        g_crowd.clear();
    }
    // phase 2, in update order: store what phase 1 computed and re-file (render geometry, selection picking scene,
    // spatial grid - the shared structures). Verify mode instead runs the game's movement and compares.
    uint8_t* gl = *(uint8_t**)(g_exe + 0x1cbdd8);
    const size_t kAhead = 4;
    for (size_t qi = 0; qi < g_queue.size(); qi++) {
        if (qi + kAhead < g_queue.size() && loopfast::On()) {  // (bperf_noprefetch.txt turns this off too)
            const Job& a = g_queue[qi + kAhead];
            for (int k = 0; k < a.npre; k++) _mm_prefetch((const char*)a.pre[k], _MM_HINT_T0);
        }
        Job& j = g_queue[qi];
        if (j.entity && ((*(uint32_t*)(j.entity + 0x40) >> 10) & 1)) continue;
        uint8_t* c = j.comp;
        if (j.serial) {  // parallel AI could not take it: the game's whole update, AI step included
            o_Vf8(c, j.dt);
            continue;
        }
        if (j.full && !g_verify) {
            uint8_t* model = *(uint8_t**)(c + 0x68);
            memcpy(model + 0x148, j.t, sizeof j.t);  // Transform::operator=
            *(float*)(model + 0x168) = j.heading;
            g_geomSetTransform(model + 0x28, model + 0x148);
            if (j.pick) {
                int obj = *(int*)(*(uint8_t**)(c + 0x70) + 0x28);
                if (obj >= 0) {
                    float copy[7];
                    memcpy(copy, model + 0x148, sizeof copy);
                    g_pickSetTransform(*(uint8_t**)(gl + 0x70), (uint32_t)obj, copy);
                }
            }
            ((void(__fastcall*)(void*, int))(g_exe + 0x19070))(*(void**)(gl + 0x88), *(int*)(c + 0x40));
            g_fast++;
            continue;
        }
        int action = *(int*)(c + 0x60);
        *(int*)(c + 0x60) = -1;
        g_armed = &j;
        o_Vf8(c, j.dt);
        g_armed = nullptr;
        *(int*)(c + 0x60) = action;
        if (g_verify && j.full) {
            uint8_t* model = *(uint8_t**)(c + 0x68);
            if ((memcmp(model + 0x148, j.t, sizeof j.t) || memcmp(model + 0x168, &j.heading, 4)) && ++g_mismatches <= 20)
                Log("parmove MISMATCH: game stored pos (%a %a %a) q (%a %a) h %a, predicted (%a %a %a) q (%a %a) h %a",
                    *(float*)(model + 0x15c - 4), *(float*)(model + 0x15c), *(float*)(model + 0x160),
                    *(float*)(model + 0x150), *(float*)(model + 0x154), *(float*)(model + 0x168), j.t[4], j.t[5],
                    j.t[6], j.t[2], j.t[3], j.heading);
        }
    }
    g_queue.clear();
}

// ---- the entity loop --------------------------------------------------------------------------------------------------
static void __fastcall h_Vf8(uint8_t* c, const void* dt) {
    if (!g_collect) return o_Vf8(c, dt);
    if (g_parallelAI && LooksLikeSteadyWalk(c)) {  // the AI step too goes to phase 1
        Job j{c, g_curEntity, dt};
        j.deferAI = true;
        g_queue.push_back(j);
        g_aiDeferred++;
        return;
    }
    if (*(int*)(c + 0x60) >= 0)  // the AI step (exe+0xca120 on the action queue, with the tick's time step)
        ((void(__fastcall*)(uint8_t*, const void*))(g_exe + 0xca120))(c + 0x48, dt);
    if (*(int*)(c + 0x40) < 0) return;
    g_queue.push_back({c, g_curEntity, dt});
}
using EntityUpdate_t = void(__fastcall*)(uint8_t* entity, void* dt);
static EntityUpdate_t o_EntityUpdate;
static void __fastcall h_EntityUpdate(uint8_t* entity, void* dt) {  // exe+0xc82b0: one entity's per-tick components
    loopfast::Ahead(entity);
    if (!g_on) return o_EntityUpdate(entity, dt);
    uint8_t* prev = g_curEntity;
    g_curEntity = entity;
    o_EntityUpdate(entity, dt);
    g_curEntity = prev;
}
using MgrUpdate_t = void(__fastcall*)(void* mgr, void* dt);
static MgrUpdate_t o_MgrUpdate;
static void __fastcall h_MgrUpdate(void* mgr, void* dt) {  // exe+0xc9560
    if (!g_on) {
        loopfast::Begin(mgr);
        o_MgrUpdate(mgr, dt);
        loopfast::End();
        return;
    }
    bool outer = !g_collect;
    if (outer) loopfast::Begin(mgr);
    g_collect = true;
    o_MgrUpdate(mgr, dt);
    if (outer) loopfast::End(), Phase();
    g_collect = false;
}
using Destroy_t = void(__fastcall*)(void* mgr, void* entityRef);
static Destroy_t o_Destroy;
static void __fastcall h_Destroy(void* mgr, void* ref) {  // exe+0xc8db0: the manager destroys a dead entity
    if (g_collect) Phase();
    o_Destroy(mgr, ref);
}

bool On() { return g_on; }
bool ParallelAI() { return g_on && g_parallelAI; }

// From DllMain (before the multiplayer mod installs its hooks): the hooks whose trampolines must bypass it.
void InstallEarly() {
    g_exe = (uint8_t*)GetModuleHandleW(nullptr);
    if (!GameFileExists(L"bperf_parallelmove.txt")) return;
    HMODULE rt = GetModuleHandleW(L"Runtime-steam-x64.dll");
    if (!rt) return;
    g_originZ = *(float*)(g_exe + 0x1788e8);
    g_dirZ = *(float*)(g_exe + 0x178984);
    g_maxDist = *(float*)(g_exe + 0x178908);
    bool ok = HookAt("ground point", g_exe + 0xa48b0, (void*)h_GroundPoint, (void**)&o_GroundPoint);
    ok &= HookAt("terrain height", g_exe + 0xb6460, (void*)h_TerrainHeight, (void**)&o_TerrainHeight);
    ok &= HookAt("Intersect (pass-through)",
                 (void*)GetProcAddress(rt, "?Intersect@Scene@Picking@@QEBAHAEBVVector3@Math@@0AEBVScalar@4@AEAV34@2@Z"),
                 (void*)h_IntersectPassthrough, (void**)&o_Intersect);
    ok &= HookAt("RootAnimationOffset (pass-through)",
                 (void*)GetProcAddress(rt, "?RootAnimationOffset@MeshInstance@Graphics@@QEBA?AVVector3@Math@@XZ"),
                 (void*)h_RootOffsetPassthrough, (void**)&o_RootOffset);
    g_parallelAI = ok && GameFileExists(L"bperf_parallelai.txt");
    if (g_parallelAI) {
        g_moveToVt = (uintptr_t)(g_exe + 0x166260);
        g_parallelAI = HookAt("MoveTo::vf5 (pass-through)", g_exe + 0xcda40, (void*)h_MoveToVf5Passthrough,
                              (void**)&o_MoveToVf5);
    }
    g_pi = *(float*)(g_exe + 0x178920);
    g_twoPi = *(float*)(g_exe + 0x178938);
    g_eps = *(float*)(g_exe + 0x1787e4);
    g_one = *(float*)(g_exe + 0x17884c);
    g_negPi = *(float*)(g_exe + 0x1789a4);
    g_angK1 = *(float*)(g_exe + 0x1787cc);
    g_angK2 = *(float*)(g_exe + 0x178814);
    g_sin = (Trig_t)(g_exe + 0x137160);
    g_cos = (Trig_t)(g_exe + 0x136560);
    g_geomSetTransform = (GeomSetTransform_t)GetProcAddress(rt, "?SetTransform@Geometry@Graphics@@QEAAXAEBVTransform@Math@@@Z");
    g_pickSetTransform = (PickSetTransform_t)GetProcAddress(rt, "?SetTransform@Scene@Picking@@QEAAXIAEBVTransform@Math@@@Z");
    g_verify = GameFileExists(L"bperf_verify.txt");
    g_on = ok && g_geomSetTransform && g_pickSetTransform;
}

// From the init thread: the entity-loop hooks (shared with loopfast's prefetching) and the workers.
void Install() {
    if (g_on || loopfast::On()) {
        HookAt("Entity::UpdateComponents", g_exe + 0xc82b0, (void*)h_EntityUpdate, (void**)&o_EntityUpdate);
        HookAt("EntityManager::Update", g_exe + 0xc9560, (void*)h_MgrUpdate, (void**)&o_MgrUpdate);
    }
    if (!g_on) return;
    HookAt("CharacterComponent::vf8", g_exe + 0x10e990, (void*)h_Vf8, (void**)&o_Vf8);
    HookAt("EntityManager::Destroy", g_exe + 0xc8db0, (void*)h_Destroy, (void**)&o_Destroy);
    if (g_parallelAI) HookAt("MoveTo::vf6", g_exe + 0xcde40, (void*)h_MoveToVf6, (void**)&o_MoveToVf6);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = (int)si.dwNumberOfProcessors;
    int want = cores >= 8 ? 4 : cores >= 4 ? 2 : cores >= 2 ? 1 : 0;  // results never depend on this
    for (int i = 0; i < want; i++) {
        g_wake[g_workers] = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset: one wake per phase
        if (HANDLE t = CreateThread(nullptr, 0, Worker, g_wake[g_workers], 0, nullptr)) {
            CloseHandle(t);
            g_workers++;
        }
    }
    Log("parmove: PARALLEL MOVEMENT on (%d worker threads); not vanilla-exact: every multiplayer player needs the same "
        "setting", g_workers);
    if (g_parallelAI)
        Log("parmove: PARALLEL AI on (steady walking steps on the workers); not vanilla-exact either");
}

}  // namespace parmove
