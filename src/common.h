#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

// Logging to <game dir>\bperf.log (appended, flushed per line, every line tagged with the process id so several
// game instances - multiplayer testing - can share the file).
void LogInit();
void Log(const char* fmt, ...);

// <game dir>\<name>
std::wstring GamePath(const wchar_t* name);
bool GameFileExists(const wchar_t* name);

// The game's main thread (the one that loaded us: Runtime-steam-x64.dll imports dbghelp statically).
extern DWORD g_mainTid;

// Engine DLL export by mangled name (nullptr + log line if missing).
void* RuntimeExport(const char* mangled);

// Hook a function with MinHook (logs failures). Works alongside the multiplayer mod's hooks on the same targets.
bool HookAt(const char* label, void* target, void* detour, void** orig);

namespace movefast {
void Install();  // exact fast paths for moving characters' bookkeeping (Steam build)
extern uint32_t g_gridCalls, g_gridSkipped, g_mismatches, g_pickCalls, g_pickSkipped, g_pickFast;
}

namespace parmove {
void InstallEarly();  // from DllMain: pass-through hooks the multiplayer mod's hooks must wrap (bperf_parallelmove.txt)
void Install();       // from the init thread: entity-loop hooks + workers
bool On();
bool ParallelAI();  // bperf_parallelai.txt (with parallel movement): steady-walking AI steps on the workers too
extern uint32_t g_aiDeferred, g_aiParallel, g_crowdChecks;  // AI steps left for phase 1; of those, run there; crowding checks there
extern uint32_t g_jobs, g_served, g_computed, g_fast, g_mismatches;  // moves; heights served / recomputed; phase 2 fast; verify
}

namespace loopfast {
void Install();  // exact: prefetching for the entity manager's per-tick loop (bperf_noprefetch.txt = off)
bool On();
void Begin(void* mgr);        // exe+0xc9560 entry
void Ahead(uint8_t* entity);  // exe+0xc82b0 entry
void End();                   // exe+0xc9560 exit
extern uint32_t g_ticks, g_lost;
}

namespace profiler {
void OnTick();  // start of every World::Update (tick counts)
void Install();  // frame/step timing hooks + the sampler thread (Ctrl+Shift+P or <game>\bperf_profile.txt)
}

namespace pathfast {
void Install();  // exact fast path solver (Steam build only)
extern uint32_t g_solves, g_solveUs, g_mismatches, g_routes, g_routeUs;
extern uint32_t g_delayedJobs, g_deliverWaits, g_deliverWaitUs;  // delayed-path mode: jobs; deliveries that waited
void OnTickStart();          // start of every World::Update, inside the multiplayer mod's tick (main thread)
const char* SimSignature();  // "" = vanilla-exact; else the mod-list entry every multiplayer player must share
}

namespace stresstown {
void OnTick(uint64_t inGameTick);  // test tool: <game>\\bperf_spawn.txt = "hamlets N x y tick"
}
