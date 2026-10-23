Banished Perf {VERSION}
=======================

Makes big towns run faster at high game speeds, without changing anything about how the game plays.

On a 1,000-citizen town the simulation needs almost a quarter less time per tick (2.19 -> 1.69 ms on the test PC),
with the optional modes below over a third less (1.36 ms) - enough for real 10x speed there. On a 3,000-citizen
town: 5.90 -> 4.60 ms, with the optional modes 3.15 ms (about 1.9x as fast as without the mod).

What it does
------------
Villagers find their way with a path search that takes milliseconds per walk in a big town, and the game asks
"how far is it to X?" tens of thousands of times a minute when choosing jobs, storage and markets. Banished Perf
replaces both searches with faster versions that give exactly the same answers - the same paths, the same
distances, the same choices. It also skips bookkeeping that would change nothing (villagers that stay inside the
same map cell) and fetches the data of the next villagers and buildings from memory ahead of time. Your town
behaves exactly as it would without the mod; it just gets there sooner.

Multiplayer
-----------
Works with Banished Multiplayer, and players with and without Banished Perf can play together: the game stays
identical either way. (Tested with the multiplayer mod's own desync checks.)

Install
-------
1. Close Banished.
2. Unpack this zip anywhere and run Install.bat.
   It finds your Steam copy of Banished and copies dbghelp.dll next to the game. If another mod already uses a
   dbghelp.dll, that file is kept as dbghelp.dll.bperf-backup and put back when you uninstall.
3. Start Banished as usual. Nothing to set up.

Uninstall
---------
Close Banished and run Uninstall.bat (or delete dbghelp.dll from the Banished folder).

Requirements
------------
- Banished 1.0.7, Steam version. (The GOG version isn't supported yet: the installer says so and does nothing.)
- The 64-bit game (Application-steam-x64.exe). If you start the 32-bit one instead, it won't start while this
  mod is installed - uninstall it, or switch to the 64-bit game.
- Saves, mods and the Workshop work as before; saves made with the mod load fine without it and the other way round.

If something goes wrong
-----------------------
The mod writes bperf.log in the Banished folder. If the game behaves differently with the mod than without it,
that's a bug: please report it with that log.

Extras
------
- Ctrl+Shift+P in game starts / stops a performance profile (written to bperf_profile_<time>.txt in the Banished
  folder) - useful if you want to report a slow town.
- Empty files in the Banished folder switch parts off, for comparing: bperf_nopath.txt (the faster searches),
  bperf_nomovefast.txt (the bookkeeping shortcuts), bperf_noprefetch.txt (fetching ahead).

Optional: delayed paths (a bit more speed, multiplayer players must agree)
--------------------------------------------------------------------------
With Banished Multiplayer, a file named bperf_delaypaths.txt in the Banished folder lets path searches run in the
background: villagers start walking one game tick (1/60 s of game time) after deciding to, and big towns run
about 8% faster again. This does change the game very slightly, so in multiplayer EVERY player needs the same
setting - the multiplayer mod checks it when someone joins and says so if it differs. (It needs a multiplayer
version that knows about it; without one, don't use this file in multiplayer.) Single player is unaffected.

Optional: parallel movement (more speed, multiplayer players must agree)
------------------------------------------------------------------------
A file named bperf_parallelmove.txt in the Banished folder moves all villagers and animals using several CPU cores:
first everyone decides what to do, then everyone moves. The only difference to the normal game is that, within one
game tick, nobody sees another villager's step of that same tick - you won't notice it, but in multiplayer EVERY
player needs the same setting (checked when someone joins, with a multiplayer version that knows about it). Works in
single player too. Together with bperf_delaypaths.txt, big towns run roughly 10% faster again.

Optional: parallel AI (with parallel movement; multiplayer players must agree)
------------------------------------------------------------------------------
A file named bperf_parallelai.txt (next to bperf_parallelmove.txt) lets villagers who are just walking along their
route take their walking step on the other CPU cores too, and checks for "someone is in my way" there. Same rule
as above: in multiplayer EVERY player needs the same setting. With all three optional files, a 3,000-citizen town
runs about 30% faster than with the mod's default settings.
