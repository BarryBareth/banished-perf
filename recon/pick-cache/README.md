# Ray-cast cache for villager ground heights (tried 2026-10-22, removed)

Exact, but no faster. `pickcache.cpp` is the full implementation (it was installed from DllMain so the multiplayer
mod's `Intersect` hook would wrap it).

## Where the time goes
- Ground height exe+0xa48b0 = terrain heightfield (exe+0xb6460) plus, on cells with walkable geometry
  (`cell+0x2d > 0`), a straight-down ray through the map's picking scene: `Picking::Scene::Intersect` (rt+0x126f50).
- Intersect: hashed grid (cell = (int)(coord * scene+4), key (x, y), no clamping; rt+0x7c1a0 iterates the ray's
  cell box), and for each object in those cells (deduped by a per-query counter at scene+0x5c), for each instance
  transform: inverse transform, then brute force over every triangle - no bounding-box rejection. Closest hit wins,
  ties to the later triangle.
- Objects change only through SetTransform (rt+0x72c80 -> 0x72fa0 re-files the object), AddMesh, RemoveMesh
  (and SetMeshUserData, which Intersect does not read).

## The cache
Direct-mapped, key = scene + exact ray bits, valid while every cell under the ray keeps the version it had; versions
bumped by the three functions above (SetTransform only when the transform or its cells really change); a hit advances
scene+0x5c like a real call.

## Result (BigCity, 10x, tools/bench.ps1 deterministic harness runs)
- Verify mode: 3.7 M ray casts, 51% answered from cache, 0 mismatches.
- Intersect fell from 4.5% to 2.9% of the main thread, but ms/tick did not move: 1.532 without vs 1.549 with
  (3 alternating runs each, within noise). The repeated rays are the cheap ones, and the per-call key/version checks
  plus the per-move bookkeeping cost about what the hits save.

## What would pay
An exact re-implementation of Intersect with per-object box rejection: the float math of the triangle test would have
to be reproduced operation for operation, and a rejected object must be one the test can never accept. ~4.5% at best.
