# Banished Perf

A performance mod for Banished 1.0.7 (Steam, 64-bit) that makes big towns simulate faster **without changing the
simulation**: every optimization returns bit-identical results, so a game with the mod plays out exactly like one
without it. That is what makes it safe next to [Banished Multiplayer](../banished-multiplayer): its lockstep needs
every player's simulation to match, and players with and without this mod still match.

`tools/bench.ps1`, median of every 1000-tick range of 3 alternating runs per configuration, 10x, 2026-10-23
("no optimizations" = `bperf_nopath.txt` + `bperf_nomovefast.txt` + `bperf_noprefetch.txt`; this PC ran about 20%
slower that day than on 2026-10-22, so compare within a table):

| BigCity (1,027 citizens) | ms per sim tick | max sim speed |
|---|---|---|
| no optimizations | 2.190 | 457 ticks/s |
| default: exact mode | 1.688 | 593 ticks/s (1.30x) |
| opt-in: delayed paths + parallel movement + parallel AI | 1.357 | 737 ticks/s (1.61x) |

| Stress town (~2,940 citizens: BigCity + 50 hamlets from `bperf_spawn.txt`, see Measuring) | ms per sim tick | max sim speed |
|---|---|---|
| no optimizations | 5.896 | 170 ticks/s |
| default: exact mode | 4.596 | 218 ticks/s (1.28x) |
| opt-in: delayed paths + parallel movement + parallel AI | 3.153 | 317 ticks/s (1.87x) |

Normal speed is 60 ticks/s and 10x is 600 (simulation only; drawing comes on top): with the opt-ins BigCity's
simulation keeps up with 10x, the stress town's with about 5x.

The opt-in modes are deterministic but not vanilla-exact; every multiplayer player needs the same ones (see below).

## How it loads
The engine DLL (`Runtime-steam-x64.dll`) imports one function from `dbghelp.dll` (`MiniDumpWriteDump`, crash
dumps) and dbghelp is not a KnownDLL, so a `dbghelp.dll` next to the exe is loaded at startup. `src/proxy.cpp`
forwards that function to the system dbghelp and installs the hooks (MinHook) from a thread. The multiplayer mod uses
the `dinput8.dll` slot, so both load side by side; hooks on the same functions chain.

## What it changes
| | Where | Speedup | Exactness |
|---|---|---|---|
| Path search | message 5 of the pathfinder (exe+0xab900; A* in exe+0x1d760/0x1d9a0) | 2.5 ms -> 0.74 ms per solve | same pop order (bucket queue on f, LIFO ties = the AVL tree's leftmost), node-pool growth replayed through the game's own pool functions, slot written with its own vector resize |
| Route distance | exe+0x1e560 (A* over the region graph, `(int)sqrtf` costs) | 42 us -> 12 us per query | same as above |

Both are `src/pathfast.cpp` (the solvers themselves in `src/pathsolve.h`). The multiplayer mod's sim heap sees the
same allocations (sizes, order, calling code) because the replays call the same exe functions the original solve
would. The grid solve keeps 8 bytes per map cell (a stamp/state/parent-direction word and g; it was 20 bytes with an
insertion number - unnecessary, since a cell is only ever re-queued with a strictly lower g) and searches on x/y:
~10% faster on solves recorded on the stress town, identical results (`tools/solvebench.cpp`, below).

`src/movefast.cpp` (also exact, on by default; `bperf_nomovefast.txt` = off) - bookkeeping of ~1000 moving characters
a tick that almost always concludes "nothing changed":
| | Where | Fast path | Exactness |
|---|---|---|---|
| Spatial grid | exe+0x19070 (re-files a character in a quadtree-like grid) | 99.9% of calls: cell unchanged -> return | the cell computed with the same float operations in the same order as the disassembly (rectangle, Center, Size, Max, exponent-bit level, Morton interleave); a changed cell runs the original |
| Picking scene | `Picking::Scene::SetTransform` -> rt+0x72fa0 (re-files the object's x/y box in the scene grid) | 95%: same cells -> only store the transform | box in double precision; used only when all four edges are >= 0.01 cell from a cell boundary (float error is far smaller), so the engine's truncation must give the same cells; otherwise the original |
Verify mode: 24.7 M grid updates and 17.3 M picking updates on BigCity, 0 mismatches.

`src/loopfast.cpp` (exact, on by default; `bperf_noprefetch.txt` = off) - the entity manager's per-tick loop
(exe+0xc9560 walking a tree of the ticking entities, exe+0xc82b0 per entity) spent ~13% of a big town's tick waiting
for memory: tree nodes, entity flags, component arrays and component flags are scattered across the heap. The order
of the previous tick (recorded as the game goes; it predicts 99.98% of the next tick's entities, tree nodes from a
walk every 16 ticks) lets it prefetch those four levels in stages 16-56 entities ahead of the game. It only reads, so
the game's work is unchanged; a wrong guess costs a useless prefetch. Stress town (~2,940 citizens):
the loop's own time 12-14% -> ~6% of the main thread, plus ~4% for the lookahead. Harness tick logs with and without
it are identical over 8,000 ticks (draws, entity state, villager positions, sim-heap allocations). Tried and dropped:
walking the tree itself a few entities ahead (stalled on the node reads), prefetching characters' picking and grid
data too (took 4% off movefast, cost the lookahead as much), the manager's bucketed updates (~30 entities a tick).

### Delayed paths (opt-in, not vanilla-exact)
`<game>\bperf_delaypaths.txt` (content: K ticks, default 1): a path result reaches its slot at the start of tick T+K
instead of in the request's tick, so villagers start walking K ticks later (1/60 s of game time at 1x), and the solves
run on worker threads meanwhile. Deterministic - delivery depends only on tick numbers, never on thread timing or core
count (no workers: solved inline, still delivered at T+K) - but a different game than vanilla, so:
- `BperfSimSignature()` (exported) returns `BanishedPerf-delayed-paths-K`; the multiplayer mod adds it to its mod list,
  so a player with another setting is refused at join with its usual "your mods differ" message. In exact mode it
  returns "" and nothing is added. **Needs Banished Multiplayer 0.9.6 or later** (the first release with that check) - an
  older one ignores the entry and would let mismatched players desync.
- Delivery runs in the World::Update hook (installed from DllMain so the multiplayer mod's hook wraps it): inside its
  tick, after the players' commands, pending results in request order. Each solve reads a snapshot of the solver
  block and walk grid taken at request time (shared by requests until the next grid/cost message), so grid/cost
  messages no longer wait for in-flight solves (that wait was ~2% of the stress town's main thread); a delivery that
  must wait solves queued jobs on the main thread meanwhile. The request-and-spin functions (exe+0xac300, 0xe2c40)
  stay synchronous; the pathfinder's destructor drops its jobs. Both changes leave the results as they were: tick
  logs IDENTICAL to the build before them (stress town, all opt-ins, 8,000 ticks, idle vs chaos).
- Only with the multiplayer mod's synchronous pathfinder; single player keeps the game's own pathfinder thread.
- Tests: idle vs chaos 12,000 ticks with the mode on: IDENTICAL (diverges from vanilla from tick 1, as it should).
  Bench: 1.65 -> 1.51 ms/tick (+8%; K=2 is no better).

### Parallel movement (opt-in, not vanilla-exact)
`<game>\bperf_parallelmove.txt` (`src/parmove.cpp`). A character's per-tick update (CharacterComponent::vf8,
exe+0x10e990) is its AI step then its movement. In this mode the entity loop runs only the AI steps and queues the
characters; before the entity manager destroys anything that tick (first exe+0xc8db0 call, or the end of
exe+0xc9560) a phase moves them:
- phase 1, worker threads: each character's new x/y (root motion x speed) and ground height (terrain heightfield +
  ray down the map's picking scene) - read-only work, called beneath the multiplayer mod's hooks (pass-through hooks
  installed from DllMain: its visual-scope stub and probes keep single-threaded global state). Ray casts use a private
  copy of the scene header whose query counter comes from one atomic counter in the negative range, so no two queries
  share a "visited" stamp whichever thread runs them.
  Phase 1 computes everything the movement stores - heading smoothing and wrapping, root-motion step, height
  (water 0, fixed height for flyers, ground height with move+0x44 as floor), rotation from the game's own sin/cos
  (exe+0x137160/0x136560) - transcribed operation by operation from the disassembly.
- phase 2, main thread, in update order: store the transform (model+0x148) and heading (+0x168), then the shared
  structures exactly as vf8 orders them: Geometry::SetTransform, Picking::Scene::SetTransform on the selection scene
  (GameLoop+0x70), spatial grid exe+0x19070. Phase 1 also notes the cache lines those stores will touch (model
  transform and geometry, picking object and its stored transform, spatial-grid entry) and phase 2 prefetches them
  four jobs ahead: -4.5% of the stress town's main thread (off with `bperf_noprefetch.txt`). With `bperf_verify.txt`
  phase 2 instead runs the game's own vf8 (AI step switched off via +0x60 = -1) and compares what it stores with the
  prediction: 19.1 M moves, 0 mismatches.
- The difference from vanilla: entities updated later in the loop see a character where it stood at the start of the
  tick, not after its move. Deterministic (idle vs chaos 12,000 ticks IDENTICAL); signature entry
  `parallel-movement` (combined with delayed paths: `BanishedPerf-delayed-paths-1-parallel-movement`).

### Parallel AI (opt-in, with parallel movement; not vanilla-exact) - tier 2, first slice
`<game>\bperf_parallelai.txt`. Most villagers are mid-walk; their AI step (exe+0xca120 on the action queue) is one
MoveTo::vf5 (exe+0xcda40) that steers along their own path. In this mode such characters (started, unchained MoveTo
with a computed path) skip the AI step in the entity loop; in phase 1 a worker re-checks the queue (later entities may
have cancelled it), decides with vf5's own cell arithmetic whether the call stays in its thread-safe branches (not
entering the next path cell - exe+0xce1f0 asks the ground height through the multiplayer mod's hooks -, not the end of
the path - animation change -, not a pending path) and then runs the game's own vf5 through a trampoline beneath the
multiplayer mod's action probe, followed by the movement prediction. Anything else (about 6%: waypoint changes,
arrivals, cancelled queues) runs the game's whole vf8 in phase 2, in order. Deterministic (idle vs chaos and a
two-player session with resync: IDENTICAL, 0 desyncs); signature entry `parallel-ai`.

Second slice, crowding checks: the time-sliced CharacterComponent::vf9 -> exe+0xca450 calls MoveTo::vf6
(exe+0xcde40) on walking villagers: nearest other character within 0.5 (exe+0x97730, a read-only walk of the spatial
grid); if it has the higher address and heads the same way, the walker's "slow down" flag (action +0x474) is set.
In parallel-AI mode those calls are collected during the entity update and run on the workers after phase 1's
steering and movement (every vf5 of the tick has read the flag by then), against the positions phase 2 hasn't
stored yet - the same ones the time-sliced updates see in this mode. An action cancelled meanwhile (re-checked: still
in its walker's current slot?) is skipped; its flag would have died with it. Same tests: IDENTICAL, 0 desyncs.

Tried and reverted (exact, but no faster - write-ups with the code):
- Route-distance cache (keyed on graph, from, to; emptied by every region-graph rebuild): exact, but only 7% of
  route queries repeat - villagers ask from too many different regions.
- [recon/async-paths](recon/async-paths/README.md): path solves on worker threads. MoveTo reads its result right after
  requesting it, so there is nothing to overlap.
- [recon/pick-cache](recon/pick-cache/README.md): caching the ground-height ray casts. Half the rays repeat, but the
  repeats are the cheap ones.
- [recon/deletion-scan](recon/deletion-scan/README.md): inline "does this action refer to the deleted entity?"
  checks. Memory-latency bound; no measurable gain.

Not attempted, with reasons: mesh/animation updates (ModelComponent::vf8 -> MeshInstance::Update, ~7%;
AnimationManager::Update, ~4%). The full pose computation (AnimationState::UpdateNodes) also produces the root motion
characters walk by, and the cheaper root-only path gives different root motion (the multiplayer mod's cross-PC desync
fix forces full updates), so poses can't be skipped exactly; and animation events (audio, RNG) plus the shared
AnimationManager table rule out worker threads.

## Checking exactness
- `<game>\bperf_verify.txt`: every request runs both the original and the fast solver; the original's answer is used
  and any difference is logged (`MISMATCH` in `bperf.log`). BigCity: 5,077 path solves + 71,058 route queries, 0.
- The multiplayer mod's harness, original solver vs this mod with UI chaos (12,000 ticks, BigCity):
  ```
  # in the multiplayer repo; bperf_nopath.txt switches the fast solvers off for run A
  .\tools\autotest.ps1 -Single -Mode idle -Save BigCity -Ticks 12000 -Speed 10     # with <game>\bperf_nopath.txt
  .\tools\autotest.ps1 -Single -Mode chaos -Save BigCity -Ticks 12000 -Speed 10
  python -I tools\compare_ticks.py <A ticks log> <B ticks log>
  ```
  Every metric IDENTICAL, including all 72,790 sim-heap allocations.

## Measuring
- `tools/bench.ps1 -Save BigCity -Configs 'off=bperf_nopath.txt','on='`: runs the multiplayer harness's
  deterministic idle mode (every run simulates the same ticks) per configuration (flag files in the game folder),
  alternating, and compares ms per 1000 ticks. Use this for any A/B decision; wall-clock profiles are too noisy.
- `tools/profile_save.ps1 -Save BigCity -Speed 10`: loads a save, takes a sampling profile, prints
  `tools/report.py` (frame times, hottest functions; names from the multiplayer repo's Ghidra exports via
  `BMP_RECON` in `local-paths.txt`). `tools/callees.py <profile> <function>` splits a function's time by callee.
  This mod's own functions are named from `build/dbghelp.pdb` - profile with the build that is in `build/`.
- In game: Ctrl+Shift+P starts / stops a profile (`bperf_profile_<time>.txt`).
- Path solver benchmark: `<game>\bperf_dumppaths.txt` = `start-tick count` records real solves (grid and region
  graph snapshots plus each request) to `<game>\bperf_paths.bin` in exact mode; `tools/solvebench.ps1` builds
  `build\bench\solvebench.exe <bperf_paths.bin> [repeats] [--profile] [--v1]`, which times `src/pathsolve.h` on
  them (best of the repeats, no game noise), prints a checksum of every result (equal checksums = identical paths and
  open-set peaks), `--v1` the first released grid solver for comparison, `--profile` time per source line.
- Stress town (test tool, never for real games): `<game>\bperf_spawn.txt` = `hamlets N x y tick [work]` founds up
  to N hamlets around world position x, y at that in-game tick, each like a game start on open land away from
  anything built: storage barn, stockpile, 11 houses, gatherer / hunter / forester / woodcutter (`work` 0 = homes
  only) and 11 families with the start supplies. Founding happens inside the sim tick, so every run of a save gets
  the same town. `hamlets 100 -41.8 44.8 60` on BigCity founds 50 (1,913 villagers). As a bench flag:
  `'big=bperf_spawn.txt#hamlets 100 -41.8 44.8 60'`.
- Both scripts set `bmp_nologs.txt`, which keeps the multiplayer harness's per-tick logs off (they cost ~10%).

## Flags (empty files in the game folder)
| File | Effect |
|---|---|
| `bperf_nopath.txt` | original path and route solvers |
| `bperf_delaypaths.txt` | delayed paths, K ticks (content, default 1) - every multiplayer player needs the same |
| `bperf_parallelmove.txt` | parallel movement - every multiplayer player needs the same |
| `bperf_parallelai.txt` | parallel AI (needs parallel movement) - every multiplayer player needs the same |
| `bperf_nomovefast.txt` | original spatial-grid / picking bookkeeping |
| `bperf_noprefetch.txt` | no entity-loop prefetching |
| `bperf_verify.txt` | run both solvers, keep the original's answer, log differences |
| `bperf_profile.txt` | take a profile now (content: seconds, default 20) |
| `bperf_bench.txt` | log ms per 1000 in-game ticks |

## Building
- Visual Studio 2022 C++ tools; paths in `local-paths.txt` (`BANISHED_DIR`, `VCVARS64`, `BMP_RECON`) or found
  automatically (`tools/paths.ps1`).
- `.\build.ps1 [-Deploy]` -> `build\dbghelp.dll` (and copies it into the game folder).
- Release: raise `BPERF_VERSION` in `src/version.h`, build, `.\tools\make_release.ps1` ->
  `release\BanishedPerf-<version>.zip` (DLL, Install.bat / Uninstall.bat / install.ps1, README.txt from
  `release_files/`). The installer recognises its DLL by the UTF-16 text "BanishedPerf" and backs up any other
  mod's dbghelp.dll.

## Limits
- Steam build only: exe addresses are the Steam 1.0.7 build's (TimeDateStamp 0x59b56a2d). On anything else the DLL
  loads, logs it and changes nothing.
- 64-bit only. `Runtime-steam-x32.dll` imports dbghelp.dll too, so with this installed the 32-bit game will not
  start (same as the multiplayer mod's dinput8.dll); uninstalling fixes it.

## Third party
MinHook (BSD 2-clause, `third_party/minhook/LICENSE.txt`).
