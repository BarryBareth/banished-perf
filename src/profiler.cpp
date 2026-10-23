// Built-in profiler: where does a big city spend its frame?
//  - Timing hooks on Engine::MainLoop::Frame / Update (one fixed 16.6 ms sim step) / Draw give per-frame numbers.
//  - A sampler thread suspends every busy game thread once per millisecond and unwinds its stack from the modules'
//    .pdata (no RtlLookupFunctionEntry: it can take loader locks a suspended thread might hold).
// Start/stop with Ctrl+Shift+P in game, or create <game>\bperf_profile.txt (optional content: seconds, default 20).
// Output: <game>\bperf_profile_<time>.txt -> python -I tools\report.py <file> resolves names and ranks hotspots.
#include "common.h"
#include <tlhelp32.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <vector>

namespace profiler {

// ---- frame timing -------------------------------------------------------------------------------------------------
struct FrameRec { uint32_t frameUs, updateUs, drawUs; uint16_t updates, ticks; uint32_t solves, solveUs; };
static std::vector<FrameRec> g_frames;  // reserved before g_active goes true; main thread appends
static std::atomic<bool> g_active;
static LARGE_INTEGER g_qpf;
static uint64_t g_updUs, g_drawUs;
static uint16_t g_updCount, g_tickCount;
static uint32_t g_solves0, g_solveUs0;  // pathfast counters at the frame's start

static inline uint64_t NowUs() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint64_t)(q.QuadPart * 1000000 / g_qpf.QuadPart);
}

using Frame_t = bool(__fastcall*)(void*, bool);
using Update_t = bool(__fastcall*)(void*, const void*);
using Draw_t = void(__fastcall*)(void*, float);
static Frame_t o_Frame;
static Update_t o_Update;
static Draw_t o_Draw;

static bool __fastcall h_Frame(void* loop, bool b) {
    if (!g_active) return o_Frame(loop, b);
    g_updUs = g_drawUs = 0;
    g_updCount = g_tickCount = 0;
    g_solves0 = pathfast::g_solves;
    g_solveUs0 = pathfast::g_solveUs;
    uint64_t t0 = NowUs();
    bool r = o_Frame(loop, b);
    uint64_t t = NowUs() - t0;
    if (g_active && g_frames.size() < g_frames.capacity())
        g_frames.push_back({(uint32_t)t, (uint32_t)g_updUs, (uint32_t)g_drawUs, g_updCount, g_tickCount,
                            pathfast::g_solves - g_solves0, pathfast::g_solveUs - g_solveUs0});
    return r;
}
static bool __fastcall h_Update(void* loop, const void* step) {
    if (!g_active) return o_Update(loop, step);
    uint64_t t0 = NowUs();
    bool r = o_Update(loop, step);
    g_updUs += NowUs() - t0;
    g_updCount++;
    return r;
}
// Benchmark (always on, Steam build): time spent in GameLoop::Update (exe+0x9b850: the frame's sim ticks, then UI)
// per 1000 in-game ticks -> bperf.log "bench" (with <game>\bperf_bench.txt). With the multiplayer mod's deterministic harness (same save, idle mode)
// every run simulates exactly the same ticks, so these lines compare like with like (tools/bench.ps1).
static bool g_inGameLoop, g_bench;  // g_bench: <game>\bperf_bench.txt (tools/bench.ps1 creates it)
static uint64_t g_benchTicks, g_benchUs, g_benchCycles, g_benchFrom;
using GameLoopUpdate_t = uint64_t(__fastcall*)(void* self, const int64_t* dt);
static GameLoopUpdate_t o_GameLoopUpdate;
static uint64_t __fastcall h_GameLoopUpdate(void* self, const int64_t* dt) {
    // wall time, and the main thread's CPU cycles (immune to other processes taking the CPU; worker threads not included)
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    uint64_t t0 = NowUs();
    g_inGameLoop = true;
    uint64_t r = o_GameLoopUpdate(self, dt);
    g_inGameLoop = false;
    g_benchUs += NowUs() - t0;
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    g_benchCycles += c1 - c0;
    if (g_bench && g_benchTicks - g_benchFrom >= 1000) {
        double n = (double)(g_benchTicks - g_benchFrom);
        Log("bench: ticks %llu-%llu %.3f ms/tick %.3f Mcycles/tick", g_benchFrom, g_benchTicks, g_benchUs / 1000.0 / n,
            g_benchCycles / 1e6 / n);
        g_benchFrom = g_benchTicks;
        g_benchUs = g_benchCycles = 0;
    }
    return r;
}

// World::Update (Steam exe+0xab1e0) starts every sim tick - in game and in the main menu's scene; proxy.cpp owns the
// hook. One MainLoop::Update (a 60 Hz step) runs game-speed many ticks.
void OnTick() {
    if (g_inGameLoop) stresstown::OnTick(g_benchTicks);
    if (g_inGameLoop && GameFileExists(L"bperf_probe_mapgen.txt")) {  // dev probe: is the map generator alive in a loaded game?
        static bool once;
        if (!once) {
            once = true;
            uint8_t* gl = *(uint8_t**)((uint8_t*)GetModuleHandleW(nullptr) + 0x1cbdd8);
            uint8_t* mg = gl ? *(uint8_t**)(gl + 0x40) : nullptr;
            Log("probe: GameLoop %p mapgen %p (+0x58 %p, +0x60 %p, +0x68 %d)", gl, mg, mg ? *(void**)(mg + 0x58) : nullptr,
                mg ? *(void**)(mg + 0x60) : nullptr, mg ? *(int*)(mg + 0x68) : -1);
            if (mg) {  // the starting structures (mapgen+0x80, 0x30 B entries: System::String path, +0x18/+0x1c offset)
                int n = *(int*)(mg + 0x88);
                for (int i = 0; i < n && i < 40; i++) {
                    uint8_t* e = *(uint8_t**)(mg + 0x80) + (size_t)i * 0x30;
                    const char* path = *(const char**)e;  // System::String: char* first (assumed)
                    Log("probe: start structure %d: %s at %+d %+d rot %d", i, path ? path : "?", *(int*)(e + 0x18),
                        *(int*)(e + 0x1c), *(int*)(e + 0x28));
                }
                Log("probe: start radius %d", *(int*)(mg + 0x3c));
            }
            float* cam = gl && *(uint8_t**)(gl + 0xe0) ? (float*)(*(uint8_t**)(gl + 0xe0) + 0x1c) : nullptr;
            if (cam) Log("probe: camera looks at world %.1f %.1f %.1f (map size %d)", cam[0], cam[1], cam[2], **(int**)(gl + 0xf8));
        }
    }
    g_tickCount++;
    if (g_inGameLoop) g_benchTicks++;
}

static void __fastcall h_Draw(void* loop, float alpha) {
    if (!g_active) return o_Draw(loop, alpha);
    uint64_t t0 = NowUs();
    o_Draw(loop, alpha);
    g_drawUs += NowUs() - t0;
}

// ---- stack sampler ------------------------------------------------------------------------------------------------
struct Module {
    uint64_t base, end;
    const RUNTIME_FUNCTION* pdata;
    size_t count;
    char name[64];
};
static std::vector<Module> g_mods;

static void LoadModules() {
    g_mods.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{sizeof me};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        Module m{};
        m.base = (uint64_t)me.modBaseAddr;
        m.end = m.base + me.modBaseSize;
        auto dos = (IMAGE_DOS_HEADER*)me.modBaseAddr;
        auto nt = (IMAGE_NT_HEADERS64*)(me.modBaseAddr + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        m.pdata = (const RUNTIME_FUNCTION*)(me.modBaseAddr + dir.VirtualAddress);
        m.count = dir.VirtualAddress ? dir.Size / sizeof(RUNTIME_FUNCTION) : 0;
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, m.name, sizeof m.name, nullptr, nullptr);
        g_mods.push_back(m);
    }
    CloseHandle(snap);
    std::sort(g_mods.begin(), g_mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
}

static const Module* FindModule(uint64_t pc) {
    auto it = std::upper_bound(g_mods.begin(), g_mods.end(), pc, [](uint64_t v, const Module& m) { return v < m.base; });
    if (it == g_mods.begin()) return nullptr;
    --it;
    return pc < it->end ? &*it : nullptr;
}

static const RUNTIME_FUNCTION* FindFunction(const Module* m, uint64_t pc) {
    DWORD rva = (DWORD)(pc - m->base);
    size_t lo = 0, hi = m->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < m->pdata[mid].BeginAddress) hi = mid;
        else if (rva >= m->pdata[mid].EndAddress) lo = mid + 1;
        else return &m->pdata[mid];
    }
    return nullptr;
}

static const int kDepth = 48;

// Called with the thread suspended: no allocation, no locks. Returns the number of frames.
static int Unwind(CONTEXT* ctx, uint64_t* out) {
    int n = 0;
    __try {
        while (n < kDepth && ctx->Rip) {
            out[n++] = ctx->Rip;
            const Module* m = FindModule(ctx->Rip);
            if (!m) break;
            const RUNTIME_FUNCTION* fe = FindFunction(m, ctx->Rip);
            if (!fe) {  // leaf function: return address on top of the stack
                ctx->Rip = *(DWORD64*)ctx->Rsp;
                ctx->Rsp += 8;
            } else {
                void* hd;
                DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, ctx->Rip, (PRUNTIME_FUNCTION)fe, ctx, &hd, &est, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

struct Thread { DWORD tid; HANDLE h; ULONG64 cycles; uint64_t busy, idle; };

struct StackKey {
    uint32_t thread, depth;
    uint64_t pc[kDepth];
    bool operator==(const StackKey& o) const {
        return thread == o.thread && depth == o.depth && memcmp(pc, o.pc, depth * 8) == 0;
    }
};
struct StackHash {
    size_t operator()(const StackKey& k) const {
        uint64_t h = 1469598103934665603ull ^ k.thread;
        for (uint32_t i = 0; i < k.depth; i++) h = (h ^ k.pc[i]) * 1099511628211ull;
        return (size_t)h;
    }
};

static std::vector<Thread> OpenThreads() {
    std::vector<Thread> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) out.push_back({te.th32ThreadID, h, 0, 0, 0});
    }
    CloseHandle(snap);
    // main thread first (report.py treats thread 0 as main)
    std::stable_sort(out.begin(), out.end(), [](const Thread& a, const Thread&) { return a.tid == g_mainTid; });
    return out;
}

static bool TriggerPressed() {
    static bool down;
    bool now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('P') & 0x8000);
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    now = now && pid == GetCurrentProcessId();
    bool edge = now && !down;
    down = now;
    return edge;
}

static int TriggerFile() {  // seconds requested via <game>\bperf_profile.txt, 0 = none
    std::wstring p = GamePath(L"bperf_profile.txt");
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) return 0;
    int s = 0;
    if (fscanf(f, "%d", &s) != 1 || s <= 0) s = 20;
    fclose(f);
    DeleteFileW(p.c_str());
    return s;
}

static void WriteProfile(const std::vector<Thread>& threads,
                         const std::unordered_map<StackKey, uint32_t, StackHash>& stacks, double seconds) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"bperf_profile_%04d%02d%02d_%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    FILE* f = _wfopen(GamePath(name).c_str(), L"w");
    if (!f) return;
    fprintf(f, "bperf 1\nseconds %.3f\n", seconds);
    for (size_t i = 0; i < g_mods.size(); i++)
        fprintf(f, "module %zu %s %llx %llx\n", i, g_mods[i].name, g_mods[i].base, g_mods[i].end - g_mods[i].base);
    for (size_t i = 0; i < threads.size(); i++)
        fprintf(f, "thread %zu %lu busy %llu idle %llu%s\n", i, threads[i].tid, threads[i].busy, threads[i].idle,
                threads[i].tid == g_mainTid ? " main" : "");
    for (auto& fr : g_frames) fprintf(f, "frame %u %u %u %u %u %u %u\n", fr.frameUs, fr.updateUs, fr.drawUs, fr.updates, fr.ticks,
                                      fr.solves, fr.solveUs);
    for (auto& [k, n] : stacks) {
        fprintf(f, "stack %u %u", k.thread, n);
        for (uint32_t i = 0; i < k.depth; i++) {
            const Module* m = FindModule(k.pc[i]);
            if (m) fprintf(f, " %d:%llx", (int)(m - g_mods.data()), k.pc[i] - m->base);
            else fprintf(f, " ?:%llx", k.pc[i]);
        }
        fputc('\n', f);
    }
    fclose(f);
    char n8[64];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, n8, sizeof n8, nullptr, nullptr);
    Log("profile written: %s (%zu frames, %zu stacks); path solves so far %u (%u us), routes %u (%u us), verify mismatches %u",
        n8, g_frames.size(), stacks.size(), pathfast::g_solves, pathfast::g_solveUs, pathfast::g_routes, pathfast::g_routeUs,
        pathfast::g_mismatches);
    if (movefast::g_gridCalls)
        Log("  spatial grid updates %u, unchanged (fast path) %u, verify mismatches %u", movefast::g_gridCalls,
            movefast::g_gridSkipped, movefast::g_mismatches);
    if (movefast::g_pickCalls)
        Log("  picking transforms set %u, identical %u, same cells (fast path) %u", movefast::g_pickCalls,
            movefast::g_pickSkipped, movefast::g_pickFast);
    if (parmove::g_jobs)
        Log("  parallel movement: %u character moves (%u stored from phase 1), heights from phase 1 %u, computed again %u, verify mismatches %u", parmove::g_jobs, parmove::g_fast,
            parmove::g_served, parmove::g_computed, parmove::g_mismatches);
    if (parmove::g_aiDeferred)
        Log("  parallel AI: %u AI steps left for the phase, %u of them run on the workers; %u crowding checks", parmove::g_aiDeferred,
            parmove::g_aiParallel, parmove::g_crowdChecks);
    if (pathfast::g_delayedJobs)
        Log("  delayed path jobs %u, deliveries that waited %u (%u us)", pathfast::g_delayedJobs, pathfast::g_deliverWaits,
            pathfast::g_deliverWaitUs);
}

static void Profile(int seconds) {
    LoadModules();
    std::vector<Thread> threads = OpenThreads();
    for (auto& th : threads) QueryThreadCycleTime(th.h, &th.cycles);
    std::unordered_map<StackKey, uint32_t, StackHash> stacks;
    g_frames.clear();
    g_frames.reserve(200000);
    g_active = true;
    Log("profiling %d s (%zu threads, %zu modules)", seconds, threads.size(), g_mods.size());
    timeBeginPeriod(1);
    uint64_t start = NowUs(), stop = start + (uint64_t)seconds * 1000000;
    StackKey key;
    while (NowUs() < stop && !TriggerPressed()) {
        for (uint32_t i = 0; i < threads.size(); i++) {
            Thread& th = threads[i];
            ULONG64 c = 0;
            QueryThreadCycleTime(th.h, &c);
            if (c - th.cycles < 20000) {  // ran < ~10 us since the last sample: waiting, don't count it
                th.cycles = c;
                th.idle++;
                continue;
            }
            th.cycles = c;
            if (SuspendThread(th.h) == (DWORD)-1) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            key.depth = GetThreadContext(th.h, &ctx) ? Unwind(&ctx, key.pc) : 0;
            ResumeThread(th.h);
            if (!key.depth) continue;
            key.thread = i;
            stacks[key]++;
            th.busy++;
        }
        Sleep(1);
    }
    g_active = false;
    timeEndPeriod(1);
    Sleep(100);  // let a frame in flight finish its push_back
    WriteProfile(threads, stacks, (NowUs() - start) / 1e6);
    for (auto& th : threads) CloseHandle(th.h);
}

static DWORD WINAPI SamplerThread(LPVOID) {
    for (;;) {
        int s = TriggerPressed() ? 600 : TriggerFile();  // hotkey: until pressed again (max 10 min)
        if (s) Profile(s);
        Sleep(50);
    }
}

void Install() {
    QueryPerformanceFrequency(&g_qpf);
    HookAt("MainLoop::Frame", RuntimeExport("?Frame@MainLoop@Engine@@QEAA_N_N@Z"), (void*)h_Frame, (void**)&o_Frame);
    HookAt("MainLoop::Update", RuntimeExport("?Update@MainLoop@Engine@@QEAA_NAEBVTime@System@@@Z"), (void*)h_Update, (void**)&o_Update);
    HookAt("MainLoop::Draw", RuntimeExport("?Draw@MainLoop@Engine@@QEAAXM@Z"), (void*)h_Draw, (void**)&o_Draw);
    auto exe = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp == 0x59b56a2d) {  // Steam 1.0.7 build
        g_bench = GameFileExists(L"bperf_bench.txt");
        HookAt("GameLoop::Update", exe + 0x9b850, (void*)h_GameLoopUpdate, (void**)&o_GameLoopUpdate);
    } else {
        Log("exe build %08x is not the Steam 1.0.7 build: no tick counts", nt->FileHeader.TimeDateStamp);
    }
    if (HANDLE t = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(t);
    }
}

}  // namespace profiler
