# Off-main-thread path solving (tried 2026-10-22, reverted)

Exact, but no faster: kept here in case a later change gives the solves something to overlap with.

## What it did
- **bperf** (`pathfast_async_block.cpp` replaced `h_PathMsg` in `src/pathfast.cpp`): with the multiplayer mod's
  synchronous pathfinder, a solve request (message 5) became a job for one of 3 worker threads. The main thread
  joined (wait, replay node-pool growth, write the result slot) before anything could look at the slot: MoveTo::vf5
  (exe+0xcda40), the result take exe+0xac220, messages 0-4, the end of GameLoop::Update (exe+0x9b850); the two
  request-and-spin functions exe+0xac300 / exe+0xe2c40 solved synchronously.
- **Multiplayer mod** (`mp_hooks_cpp_snippets.txt`, plus `BmpPathMemory` in `src/dinput8.def` and the GOG
  addresses ab710 -> a6240, ab900 -> a6430 in `src/gog_rvas.h`): a thread-local `t_pathMem` scope that keeps path
  memory out of the sim heap (allocations and free-list reuse), set around message 5 (exe+0xab900) and result-slot
  allocation (exe+0xab710, which deep-copies every slot's path when the slot array grows), and exported as
  `BmpPathMemory` for bperf. Needed because whether/when those allocate depends on path lengths, and bperf hands
  results over later than the synchronous solve.

## Result (BigCity, 10x, realistic no-log MP setup)
- MP harness, original solver vs async + UI chaos, 12000 ticks: every metric IDENTICAL (sim-heap allocations
  72,762 - path memory gone from the sim heap in both runs).
- But 5481 of 5675 joins had to wait, ~790 us each = a whole solve: MoveTo polls its slot right after requesting it
  (same action update), so there is nothing to overlap. 546 ticks/s async vs 576 synchronous (noise level).

## What would make it pay
A request issued well before its first read - e.g. predicting a villager's next walk. Not exact-safe without the join
anyway, so the join cost stays unless reads move later, which would change the game.
