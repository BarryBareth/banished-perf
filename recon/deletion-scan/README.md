# Entity-deletion scan (tried 2026-10-22, removed)

Exact, but no measurable gain in exact mode. `deletion_scan.cpp` is the code (it lived in `src/movefast.cpp`).

## What the game does
For every deleted entity (felled tree, emptied pile...) exe+0x8f300(town, entityRef) walks all of the town's citizens
(town+0xa0, count +0xa8) and runs exe+0xca790 on each one's action queue (*(citizen+0x278)+0x48: [0] slots, [1] count):
every queued action is asked "do you refer to this entity?" (Action vf9, vtable +0x48); the first yes cancels the whole
queue. Every Action vf9 is a pure pointer comparison (CheckPtr is a 3-byte no-op in this build) - a field of the action
(+0x20/+0x28/+0x30/+0x38) or the owner (+0x10) of a component it points to; the per-type table is in the code.

## The fast path
The same comparisons inline per action type (by vtable), exe+0xca790 called only for a citizen with a match (it then
cancels exactly as before); unknown types / null fields: the real virtual call. A batched, prefetching variant (one
link of the pointer chain per pass) was no faster.

## Result (BigCity)
- Verify mode: 12.5 M citizen checks over 12,164 deletions, 0 mismatches (no queue was cancelled in those runs);
  harness vs vanilla IDENTICAL.
- Gain: the deletion path is 2.5-2.9% of the main thread in exact mode with or without it (profile shares, 2 pairs).
  The 4.5% seen earlier was measured with parallel movement on, whose phase runs inside the multiplayer mod's
  deletion hook and inflated that path. The scan is memory-latency bound; the virtual calls it removes were cheap.
