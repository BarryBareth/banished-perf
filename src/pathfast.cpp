// Faster pathfinding with bit-identical results.
//
// The game's pathfinder (Steam exe+0xab5e0..0xac6xx) is a message queue: messages 0-4 change its private copy of the
// walk grid / cost table / flags, message 5 solves a path into a result slot. The multiplayer mod switches it to
// synchronous mode, so every solve runs inline on the main thread - ~2.5 ms each on a 1000-citizen map, ~19% of the
// frame at 10x.
//
// The original solve (exe+0x1d760 -> 0x1d9a0) is A* whose open set is an AVL tree of pooled nodes keyed (f, seq),
// leftmost = lowest f, ties -> highest seq (most recently inserted). We replace message 5 with the same A* on flat
// arrays and a bucket queue with the same ordering, which pops nodes in exactly the same order -> the same path.
// Side effects the rest of the game can observe are reproduced exactly:
//   - solver generation counter (+0) += 1
//   - node pool growth: the original allocates a doubled chunk whenever the open set outgrows every earlier solve. We
//     replay it through the game's own pool functions (exe+0x1e090 / exe+0x1dd60), so allocation sizes, order and
//     caller match - the multiplayer mod's deterministic sim heap sees the same sequence.
//   - result slot: resized with the game's vector resize (exe+0x21d0), filled, done flag set, under the slot lock.
// <game>\bperf_verify.txt: run both solvers on every request, log any difference (the original's result is kept).
#include "common.h"
#include "pathsolve.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pathfast {

static uint8_t* g_exe;
template <class F> static F Exe(uintptr_t rva) { return (F)(g_exe + rva); }

using PathMsg_t = void(__fastcall*)(uint8_t*, const int*);
static PathMsg_t o_PathMsg;
using PoolAlloc_t = void*(__fastcall*)(uint8_t* tree, const int* key, void* parent);  // exe+0x1e090
using PoolFree_t = void(__fastcall*)(uint8_t* tree, void* node);                       // exe+0x1dd60 (subtree)
using Resize_t = void(__fastcall*)(void* vec, int n);                                  // exe+0x21d0 (int vector)
using Lock_t = void(__fastcall*)(void*);
static Lock_t g_lock, g_unlock;

static bool g_enabled, g_verify;
static std::mutex g_mutex;  // one solve at a time (main thread in sync mode, the pathfinder thread otherwise)

// counters for the profiler
uint32_t g_solves, g_solveUs, g_mismatches;

// ---- solver state -------------------------------------------------------------------------------------------------
// Solver object = pathfinder + 0x78:
//   +0x00 gen   +0x04 seq   +0x08 node array   +0x18 AVL tree (root, +8 size, +0x18 free list, +0x28 nodes owned,
//   +0x2c in use)   +0x50 int cost[type]   +0xd0 walkable type mask   +0xd4 8 directions   +0xd5 manhattan heuristic
//   +0xd6 bit 0x80 blocks   +0xd8 grid width n   +0xe0 grid bytes (n*n: low 7 bits type, 0x80 blocked)
// Open set = bucket queue on f: a stack per f value. Entries are pushed in increasing seq order, so a bucket's top is
// its highest seq - popping the lowest non-empty bucket's top is exactly the AVL tree's leftmost node.
using namespace pathsolve;
static RouteScratch g_r;
static std::mutex g_routeMutex;

// One per thread (the main thread; the pathfinder thread in single player): ~20 bytes per map cell.
static Scratch& MyScratch() {
    thread_local Scratch* s = new Scratch;
    return *s;
}

// The original's node pool: grow it exactly as the AVL solve would have (open set peak), then hand the nodes back.
static void ReplayPoolGrowth(uint8_t* S, int peak) {
    uint8_t* tree = S + 0x18;
    if (*(void**)tree) {  // a solve from before we were installed left nodes in the tree: release them like 0x1d9a0
        Exe<PoolFree_t>(0x1dd60)(tree, *(void**)tree);
        *(void**)tree = nullptr;
        *(int*)(tree + 8) = 0;
    }
    if (peak <= *(int*)(tree + 0x28)) return;
    static std::vector<void*> nodes;
    nodes.clear();
    const int key[3] = {0, 0, 0};
    for (int i = 0; i < peak; i++) nodes.push_back(Exe<PoolAlloc_t>(0x1e090)(tree, key, nullptr));
    for (void* p : nodes) Exe<PoolFree_t>(0x1dd60)(tree, p);
}

static void WriteSlot(uint8_t* pf, int slot, const int* path, int count) {
    g_lock(pf + 0x30);
    uint8_t* e = *(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)slot * 0x18;
    Exe<Resize_t>(0x21d0)(e + 8, count);
    g_unlock(pf + 0x30);
    e = *(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)slot * 0x18;
    memcpy(*(int**)(e + 8), path, count * sizeof(int));
    e[0] = 1;
}

static uint64_t NowUs() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)(q.QuadPart * 1000000 / f.QuadPart);
}

// ---- delayed-path mode (<game>\bperf_delaypaths.txt, content = K ticks, default 1) ------------------------------
// Not vanilla-exact: a path result reaches its slot at the start of tick T+K instead of during the request's tick T,
// so a villager starts walking K ticks (K/60 s of game time at 1x) later. In exchange solves run on worker threads
// while the main thread goes on with the tick. Deterministic: what gets delivered when depends only on tick numbers,
// never on thread timing or core count (with no workers the solve runs inline and still waits for T+K). Every player
// in a multiplayer game must use the same K - BperfSimSignature() puts it into the multiplayer mod's mod list, which
// refuses a join on any difference.
//   - delivery (OnTickStart, from the World::Update hook the multiplayer mod's own hook wraps: inside its tick, after
//     its commands): jobs due, in request order - pool growth replayed, slot written through the game's functions
//   - solves read a snapshot of the solver block and walk grid, taken at request time (shared by the requests until
//     the next message 0-4 changes the grid / costs) - so those messages go ahead at once instead of waiting for
//     pending solves (they used to: ~2% of a 3,000-citizen town's main thread)
//   - exe+0xac300 / exe+0xe2c40 request and spin on the flag: synchronous inside them, like without this mode
//   - pathfinder destroyed (exe+0xabbf0): its pending jobs are dropped
// Off unless the multiplayer mod's synchronous pathfinder is on (single player keeps the game's own thread).
struct Snapshot {
    uint8_t* pf;
    std::vector<uint8_t> S, grid;  // solver block (+0x78, 0x250 bytes: settings, cost table) with +0xe0 -> grid
};
static std::shared_ptr<Snapshot> g_snap;  // main thread: the live state's, until a message 0-4

struct Job {
    uint8_t* pf;
    std::shared_ptr<Snapshot> snap;
    int from, to, slot, id;
    uint64_t tick;
    int count = 0, peak = 0;
    std::vector<int> path;
    std::atomic<bool> done{false};
};
static int g_delayTicks;             // 0: mode off
static uint64_t g_tick;              // World::Update calls (main thread)
static std::vector<Job*> g_pending;  // main thread: requested, not yet delivered (request order)
static std::deque<Job*> g_queue;     // waiting for a worker
static std::mutex g_queueMutex;
static std::condition_variable g_queueCv, g_doneCv;
static int g_workers, g_forceSync;
uint32_t g_delayedJobs, g_deliverWaits, g_deliverWaitUs;

static void SolveJob(Scratch& s, Job* j) {
    j->count = Solve(s, j->snap->S.data(), j->from, j->to, &j->peak);
    j->path.assign(s.path, s.path + j->count);
}

static DWORD WINAPI Worker(LPVOID) {
    Scratch& s = MyScratch();
    for (;;) {
        Job* j;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return !g_queue.empty(); });
            j = g_queue.front();
            g_queue.pop_front();
        }
        SolveJob(s, j);
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            j->done = true;
        }
        g_doneCv.notify_all();
    }
}

// Waiting for a job: rather than sleep, solve queued jobs here - this one first if no worker has taken it yet (which
// thread solves a job never changes its result).
static void WaitSolved(Job* j) {
    if (j->done) return;
    uint64_t t0 = NowUs();
    std::unique_lock<std::mutex> lock(g_queueMutex);
    while (!j->done) {
        if (g_queue.empty()) {
            g_doneCv.wait(lock, [j] { return j->done.load() || !g_queue.empty(); });
            continue;
        }
        auto it = std::find(g_queue.begin(), g_queue.end(), j);
        Job* take = it != g_queue.end() ? j : g_queue.front();
        g_queue.erase(it != g_queue.end() ? it : g_queue.begin());
        lock.unlock();
        SolveJob(MyScratch(), take);
        lock.lock();
        take->done = true;
        g_doneCv.notify_all();
    }
    g_deliverWaits++;
    g_deliverWaitUs += (uint32_t)(NowUs() - t0);
}

static void Deliver(Job* j) {
    WaitSolved(j);
    uint8_t* pf = j->pf;
    ReplayPoolGrowth(pf + 0x78, j->peak);
    // the slot is still this request's (nothing released and re-used it meanwhile)
    if (j->slot < *(int*)(pf + 0x168)) {
        const uint8_t* e = *(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)j->slot * 0x18;
        if (*(const int*)(e + 4) == j->id && e[0] == 0) WriteSlot(pf, j->slot, j->path.data(), j->count);
    }
    delete j;
}

void OnTickStart() {
    g_tick++;
    if (g_pending.empty()) return;
    size_t n = 0;
    while (n < g_pending.size() && g_pending[n]->tick + g_delayTicks <= g_tick) n++;
    if (!n) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < n; i++) Deliver(g_pending[i]);
    g_pending.erase(g_pending.begin(), g_pending.begin() + n);
}

static bool Enqueue(uint8_t* pf, const int* msg) {
    Job* j = new Job;
    j->pf = pf;
    j->from = msg[1];
    j->to = msg[2];
    j->slot = msg[3];
    j->id = *(const int*)(*(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)msg[3] * 0x18 + 4);
    j->tick = g_tick;
    if (!g_snap || g_snap->pf != pf) {
        auto snap = std::make_shared<Snapshot>();
        const uint8_t* S = pf + 0x78;
        snap->pf = pf;
        snap->S.assign(S, S + 0x250);
        int n = *(const int*)(S + 0xd8);
        const uint8_t* grid = *(uint8_t* const*)(S + 0xe0);
        snap->grid.assign(grid, grid + (size_t)n * n);
        uint8_t* copy = snap->grid.data();
        memcpy(snap->S.data() + 0xe0, &copy, sizeof copy);
        g_snap = std::move(snap);
    }
    j->snap = g_snap;
    g_pending.push_back(j);
    g_delayedJobs++;
    if (!g_workers) {
        SolveJob(MyScratch(), j);
        j->done = true;
        return true;
    }
    {
        std::lock_guard<std::mutex> ql(g_queueMutex);
        g_queue.push_back(j);
    }
    g_queueCv.notify_one();
    return true;
}

// request-and-spin functions: synchronous inside them. exe+0xac300(pf, from, to, walk mask, out, max), exe+0xe2c40(x)
using RequestWait_t = void(__fastcall*)(void*, int, int, int, int*, int);
static RequestWait_t o_RequestWait;
static void __fastcall h_RequestWait(void* pf, int from, int to, int mask, int* out, int max) {
    g_forceSync++;
    o_RequestWait(pf, from, to, mask, out, max);
    g_forceSync--;
}
using RequestWait2_t = void(__fastcall*)(void*);
static RequestWait2_t o_RequestWait2;
static void __fastcall h_RequestWait2(void* planner) {
    g_forceSync++;
    o_RequestWait2(planner);
    g_forceSync--;
}
// pathfinder destructor exe+0xabbf0
using PathDtor_t = void(__fastcall*)(uint8_t*);
static PathDtor_t o_PathDtor;
static void __fastcall h_PathDtor(uint8_t* pf) {
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i]->pf == pf) {
            WaitSolved(g_pending[i]);
            if (g_snap && g_snap->pf == pf) g_snap.reset();
            delete g_pending[i];
            g_pending.erase(g_pending.begin() + i);
        } else {
            i++;
        }
    }
    o_PathDtor(pf);
}

static char g_signature[64];
const char* SimSignature() { return g_signature; }

// Vanilla-exact off-the-main-thread solving was tried and reverted: MoveTo reads its result right after requesting it,
// so there is nothing to overlap (recon/async-paths/README.md) - hence the delayed mode above.
// ---- recorder for tools/solvebench.cpp (<game>\bperf_dumppaths.txt = "start-tick records") -------------------------
// Writes <game>\bperf_paths.bin: grid snapshots ('G': n, solver bytes 0..0x250, n*n grid) every 250 path solves,
// region graph snapshots ('R': count, per node x, y, neighbour count, neighbours) every 1000 route queries, and each
// request with its result length ('P' / 'Q': from, to, length). Exact mode only; a test tool.
static FILE* g_dump;
static uint64_t g_dumpStart;
static int g_dumpLeft, g_dumpPaths, g_dumpRoutes;

static void DumpOpen() {
    FILE* f = _wfopen(GamePath(L"bperf_dumppaths.txt").c_str(), L"r");
    if (!f) return;
    unsigned long long start = 0;
    if (fscanf(f, "%llu %d", &start, &g_dumpLeft) != 2) g_dumpLeft = 4000;
    fclose(f);
    g_dumpStart = start;
    g_dump = _wfopen(GamePath(L"bperf_paths.bin").c_str(), L"wb");
    if (g_dump) Log("pathfast: recording %d solves from tick %llu to bperf_paths.bin", g_dumpLeft, start);
}

static bool DumpNow() {
    if (!g_dump || g_tick < g_dumpStart) return false;
    if (g_dumpLeft-- > 0) return true;
    fclose(g_dump);
    g_dump = nullptr;
    Log("pathfast: recording done (%d path solves, %d route queries)", g_dumpPaths, g_dumpRoutes);
    return false;
}

static void DumpPath(const uint8_t* S, int from, int to, int count) {
    if (!DumpNow()) return;
    if (g_dumpPaths++ % 250 == 0) {
        int n = *(const int*)(S + 0xd8);
        fputc('G', g_dump);
        fwrite(&n, 4, 1, g_dump);
        fwrite(S, 1, 0x250, g_dump);
        fwrite(*(uint8_t* const*)(S + 0xe0), 1, (size_t)n * n, g_dump);
    }
    int rec[3] = {from, to, count};
    fputc('P', g_dump);
    fwrite(rec, 4, 3, g_dump);
}

static void DumpRoute(const uint8_t* S, int from, int to, int len) {
    if (!DumpNow()) return;
    if (g_dumpRoutes++ % 1000 == 0) {
        const uint8_t* G = *(uint8_t* const*)(S + 0x68);
        int count = *(const int*)(S + 0x70);
        fputc('R', g_dump);
        fwrite(&count, 4, 1, g_dump);
        for (int i = 0; i < count; i++) {
            const uint8_t* node = G + (size_t)i * 0x30;
            fwrite(node, 4, 2, g_dump);
            fwrite(node + 0x18, 4, 1, g_dump);
            fwrite(*(int* const*)(node + 0x10), 4, *(const int*)(node + 0x18), g_dump);
        }
    }
    int rec[3] = {from, to, len};
    fputc('Q', g_dump);
    fwrite(rec, 4, 3, g_dump);
}

static void __fastcall h_PathMsg(uint8_t* pf, const int* msg) {
    if (msg[0] != 5) {
        if (GetCurrentThreadId() == g_mainTid) g_snap.reset();  // pending solves keep their snapshot
        return o_PathMsg(pf, msg);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    uint64_t t0 = NowUs();
    uint8_t* S = pf + 0x78;
    Scratch& s = MyScratch();
    if (!g_enabled) {
        o_PathMsg(pf, msg);
    } else if (g_verify) {
        int peak, count = Solve(s, S, msg[1], msg[2], &peak);
        int treeBefore = *(int*)(S + 0x18 + 0x28);
        o_PathMsg(pf, msg);
        const uint8_t* e = *(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)msg[3] * 0x18;
        int theirs = *(const int*)(e + 0x10);
        const int* tp = *(int* const*)(e + 8);
        bool same = theirs == count && (count == 0 || memcmp(tp, s.path, count * sizeof(int)) == 0);
        int grewTo = *(int*)(S + 0x18 + 0x28);
        bool poolOk = (peak <= treeBefore) == (grewTo == treeBefore);
        if (!same || !poolOk) {
            g_mismatches++;
            if (g_mismatches <= 20)
                Log("pathfast MISMATCH %d->%d: theirs %d steps, mine %d (pool %d -> %d, my peak %d)", msg[1], msg[2],
                    theirs, count, treeBefore, grewTo, peak);
        }
    } else if (g_delayTicks && pf[0x5c] && !g_forceSync && GetCurrentThreadId() == g_mainTid) {
        *(int*)S += 1;  // generation counter (0x1d760)
        Enqueue(pf, msg);
    } else {
        *(int*)S += 1;  // generation counter (0x1d760)
        int peak, count = Solve(s, S, msg[1], msg[2], &peak);
        ReplayPoolGrowth(S, peak);
        WriteSlot(pf, msg[3], s.path, count);
        if (g_dump) DumpPath(S, msg[1], msg[2], count);
    }
    g_solveUs += (uint32_t)(NowUs() - t0);
    g_solves++;
    if (g_verify && g_solves % 1000 == 0)
        Log("pathfast verify: %u path solves, %u route queries compared, %u mismatches", g_solves, g_routes, g_mismatches);
}

// exe+0x1e560(solver, from, to, unused, out): generation += 1, solve; with out: write up to 0x2000 nodes (start first)
// and return min(length, 0x2000); without: return the full length. 0 = no route.
using RouteSolve_t = int(__fastcall*)(uint8_t*, int, int, void*, int*);
static RouteSolve_t o_RouteSolve;
uint32_t g_routes, g_routeUs;

static int __fastcall h_RouteSolve(uint8_t* S, int from, int to, void* unused, int* out) {
    std::lock_guard<std::mutex> lock(g_routeMutex);
    uint64_t t0 = NowUs();
    int r;
    if (!g_enabled) {
        r = o_RouteSolve(S, from, to, unused, out);
    } else if (g_verify) {
        int peak;
        bool found = SolveRoute(g_r, S, from, to, &peak);
        int mine = found ? (int)g_r.path.size() : 0;
        int treeBefore = *(int*)(S + 0x18 + 0x28);
        r = o_RouteSolve(S, from, to, unused, out);
        int expect = out && mine > 0x2000 ? 0x2000 : mine;
        bool same = r == expect && (!out || memcmp(out, g_r.path.data(), r * sizeof(int)) == 0);
        int grewTo = *(int*)(S + 0x18 + 0x28);
        bool poolOk = (peak <= treeBefore) == (grewTo == treeBefore);
        if (!same || !poolOk) {
            g_mismatches++;
            if (g_mismatches <= 20)
                Log("route MISMATCH %d->%d: theirs %d, mine %d (pool %d -> %d, my peak %d)", from, to, r, expect,
                    treeBefore, grewTo, peak);
        }
    } else {
        *(int*)S += 1;
        int peak;
        bool found = SolveRoute(g_r, S, from, to, &peak);
        ReplayPoolGrowth(S, peak);
        int n = found ? (int)g_r.path.size() : 0;
        if (g_dump) DumpRoute(S, from, to, n);
        if (out) {
            if (n > 0x2000) n = 0x2000;
            memcpy(out, g_r.path.data(), n * sizeof(int));
        }
        r = n;
    }
    g_routeUs += (uint32_t)(NowUs() - t0);
    g_routes++;
    return r;
}

void Install() {
    g_exe = (uint8_t*)GetModuleHandleW(nullptr);
    g_lock = (Lock_t)RuntimeExport("?Lock@CriticalSection@System@@QEAAXXZ");
    g_unlock = (Lock_t)RuntimeExport("?Unlock@CriticalSection@System@@QEAAXXZ");
    g_enabled = !GameFileExists(L"bperf_nopath.txt") && g_lock && g_unlock;
    g_verify = GameFileExists(L"bperf_verify.txt");
    DumpOpen();
    HookAt("Pathfinder::Message", g_exe + 0xab900, (void*)h_PathMsg, (void**)&o_PathMsg);
    HookAt("RouteSolve", g_exe + 0x1e560, (void*)h_RouteSolve, (void**)&o_RouteSolve);
    if (g_enabled && !g_verify && GameFileExists(L"bperf_delaypaths.txt")) {
        g_delayTicks = 1;
        if (FILE* f = _wfopen(GamePath(L"bperf_delaypaths.txt").c_str(), L"r")) {
            int k;
            if (fscanf(f, "%d", &k) == 1 && k >= 1 && k <= 8) g_delayTicks = k;
            fclose(f);
        }
        snprintf(g_signature, sizeof g_signature, "BanishedPerf-delayed-paths-%d", g_delayTicks);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int cores = (int)si.dwNumberOfProcessors;
        g_workers = cores >= 6 ? 3 : cores >= 3 ? 1 : 0;  // results never depend on this (see above)
        for (int i = 0; i < g_workers; i++)
            if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
        HookAt("Path::RequestWait", g_exe + 0xac300, (void*)h_RequestWait, (void**)&o_RequestWait);
        HookAt("Path::RequestWait2", g_exe + 0xe2c40, (void*)h_RequestWait2, (void**)&o_RequestWait2);
        HookAt("Pathfinder::~Pathfinder", g_exe + 0xabbf0, (void*)h_PathDtor, (void**)&o_PathDtor);
    }
    Log("pathfast: %s%s", g_enabled ? "on" : "off (original solver)", g_verify ? ", VERIFY mode (both solvers, original result)" : "");
    if (g_delayTicks)
        Log("pathfast: DELAYED PATHS - results arrive %d tick(s) after the request (%d worker threads); not vanilla-exact: "
            "every multiplayer player needs the same setting (mod list entry %s)", g_delayTicks, g_workers, g_signature);
}

}  // namespace pathfast
