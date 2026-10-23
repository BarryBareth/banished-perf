// Test tool: grow a loaded town into a stress town. <game>\bperf_spawn.txt = "hamlets N x y tick": at that in-game tick
// (inside the sim tick: every run of a save gets the identical town) N hamlets are founded around world position x, y,
// each like a game start: a site of open land (trees and rocks cleared, as the multiplayer mod's Settle does), the start
// layout (exe+0xa7490 with the map generator's list: storage barn, stockpile, 6 houses on Medium), four workplaces
// (the same routine with a temporary list: gatherer, hunter, forester, woodcutter), the start families (exe+0xa62a0:
// 6 couples and children) and supplies (exe+0xa73d0). Never for real games.
#include "common.h"
#include <vector>

namespace stresstown {

static uint8_t* g_exe;
static int g_hamlets = -1, g_tick, g_withWork = 1;
static float g_x, g_y;

static const uintptr_t kTiMapComponent = 0x1cf9e0, kTiNaturalResource = 0x1cfb30;

static void* FindComponent(uint8_t* entity, uintptr_t typeinfoRva) {
    int want = *(int*)(g_exe + typeinfoRva), n = *(int*)(entity + 0x38);
    for (int i = 0; i < n; i++) {
        void* comp = (*(void***)(entity + 0x30))[i];
        if (((int(__fastcall*)(void*))(*(void***)comp)[2])(comp) == want) return comp;
    }
    return nullptr;
}

struct Natural { uint8_t* e; int x, y; };
static std::vector<Natural> NaturalsNear(uint8_t* mgr, int x, int y, int r) {
    std::vector<Natural> out;
    int n = *(int*)(mgr + 0x10);
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;  // dead
        uint8_t* map = (uint8_t*)FindComponent(e, kTiMapComponent);
        if (!map) continue;
        int cx = *(int16_t*)(map + 0x28), cy = *(int16_t*)(map + 0x2a);
        if ((cx - x) * (cx - x) + (cy - y) * (cy - y) > r * r) continue;
        if (FindComponent(e, kTiNaturalResource)) out.push_back({e, cx, cy});
    }
    return out;
}

// Once per founding: mask 1 = cells within 1 of a natural resource (trees, rocks: what clearing a site removes), 2 = cells
// within kKeepOut of anything else on the map (buildings, fields, orchards: their crops are natural resources too, and
// killing them leaves the field's work reading a dead plant - exe+0xf2556)
static const int kKeepOut = 14;  // a site checks cells within r and clears naturals within r+2; fields reach ~8 cells from their centre
static std::vector<uint8_t> SiteMask(uint8_t* mgr, int size) {
    std::vector<uint8_t> mask((size_t)size * size, 0);
    std::vector<std::pair<int, int>> built;
    int n = *(int*)(mgr + 0x10);
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;
        uint8_t* map = (uint8_t*)FindComponent(e, kTiMapComponent);
        if (!map) continue;
        int cx = *(int16_t*)(map + 0x28), cy = *(int16_t*)(map + 0x2a);
        if (!FindComponent(e, kTiNaturalResource)) {
            built.push_back({cx, cy});
            continue;
        }
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if ((unsigned)(cx + dx) < (unsigned)size && (unsigned)(cy + dy) < (unsigned)size)
                    mask[(size_t)(cy + dy) * size + cx + dx] |= 1;
    }
    for (auto& b : built)
        for (int dy = -kKeepOut; dy <= kKeepOut; dy++)
            for (int dx = -kKeepOut; dx <= kKeepOut; dx++)
                if ((unsigned)(b.first + dx) < (unsigned)size && (unsigned)(b.second + dy) < (unsigned)size)
                    mask[(size_t)(b.second + dy) * size + b.first + dx] = 2;
    return mask;
}

// every cell within r is open land or carries a wild natural resource, away from anything built
static bool Clearable(int* map, const std::vector<uint8_t>& mask, int x, int y, int r) {
    int size = map[0];
    if (x < r + 3 || y < r + 3 || x >= size - r - 3 || y >= size - r - 3) return false;
    uint8_t* cells = *(uint8_t**)((uint8_t*)map + 0x80);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            size_t c = (size_t)(y + dy) * size + (x + dx);
            if (mask[c] == 2 || (cells[c * 0x30 + 0x29] != 2 && !mask[c])) return false;
        }
    return true;
}

// The map generator's start-structure list entry (0x30 B): ExternalPtr {System::String path, int generation,
// resource} at +0/+8/+0x10, offset +0x18/+0x1c, fallback size +0x20/+0x24, rotation +0x28.
struct Placement { const char* path; int dx, dy, rot; };
static const Placement kWorkplaces[] = {
    {"Template\\GathererShelter.rsc", 6, -1, 0},
    {"Template\\HunterLodge.rsc", 6, -7, 0},
    {"Template\\ForesterLodge.rsc", 9, 3, 0},
    {"Template\\CutterYard.rsc", 10, -3, 0},
};
// extra homes in the layout's free corners (any that do not fit are skipped by the placement check); one more family
// moves in per house built
static const Placement kHouses[] = {
    {"Template\\WoodHouse.rsc", 9, 9, 0},   {"Template\\WoodHouse.rsc", 3, 12, 0}, {"Template\\WoodHouse.rsc", -3, 11, 0},
    {"Template\\WoodHouse.rsc", -8, 10, 0}, {"Template\\WoodHouse.rsc", 5, -12, 180},
};
using StringCtor_t = void*(__fastcall*)(void* self, const char* text);

static int g_added[4];  // new entities: start layout, workplaces, extra houses, families (log only)

// exe+0xa7490 (start structures around x, y) with `list` in place of the generator's; returns the entities it made
static int PlaceList(uint8_t* mg, uint8_t* mgr, std::vector<uint8_t>& list, int x, int y) {
    void* saved = *(void**)(mg + 0x80);
    int count = *(int*)(mg + 0x88), before = *(int*)(mgr + 0x10);
    *(void**)(mg + 0x80) = list.data();
    *(int*)(mg + 0x88) = (int)(list.size() / 0x30);
    ((void(__fastcall*)(void*, int, int))(g_exe + 0xa7490))(mg, x, y);
    *(void**)(mg + 0x80) = saved;
    *(int*)(mg + 0x88) = count;
    return *(int*)(mgr + 0x10) - before;
}

static void Found(uint8_t* gl, void* mapgen, int x, int y, std::vector<uint8_t>& workList,
                  std::vector<uint8_t>& houseList) {
    uint8_t* mg = (uint8_t*)mapgen;
    uint8_t* mgr = *(uint8_t**)(gl + 0xe8);
    int r = *(int*)(mg + 0x3c);
    for (auto& n : NaturalsNear(mgr, x, y, r + 2)) {  // clear the site
        ((void(__fastcall*)(void*))(g_exe + 0xc8370))(n.e);  // EntityKill
        void* ref = n.e;
        ((void(__fastcall*)(void*, int, void*))(g_exe + 0x3110))(mgr + 0xd0, *(int*)(mgr + 0xd8) + 1, &ref);
    }
    int before = *(int*)(mgr + 0x10);
    ((void(__fastcall*)(void*, int, int))(g_exe + 0xa7490))(mapgen, x, y);  // barn, stockpile, houses
    g_added[0] += *(int*)(mgr + 0x10) - before, before = *(int*)(mgr + 0x10);
    if (g_withWork) g_added[1] += PlaceList(mg, mgr, workList, x, y);
    int houses = PlaceList(mg, mgr, houseList, x, y);
    g_added[2] += houses;
    float pos[3];
    ((void(__fastcall*)(void*, float*, int, int))(g_exe + 0x2080))(*(void**)(gl + 0xf8), pos, x, y);  // cell -> world
    // families: the start's (mapgen+0x68 of them, 6 on Medium), then one per extra house - a second call with a lower
    // count, since the routine reads family i's makeup from a table of mapgen+0x68 entries at mapgen+0x60
    int families = *(int*)(mg + 0x68);
    before = *(int*)(mgr + 0x10);
    ((void(__fastcall*)(void*, float*))(g_exe + 0xa62a0))(mapgen, pos);
    if (houses > 0) {
        *(int*)(mg + 0x68) = houses < families ? houses : families;
        ((void(__fastcall*)(void*, float*))(g_exe + 0xa62a0))(mapgen, pos);
        *(int*)(mg + 0x68) = families;
    }
    g_added[3] += *(int*)(mgr + 0x10) - before;
    ((void(__fastcall*)(void*))(g_exe + 0xa73d0))(mapgen);               // supplies
}

void OnTick(uint64_t tick) {
    if (g_hamlets < 0) {
        g_exe = (uint8_t*)GetModuleHandleW(nullptr);
        g_hamlets = 0;
        if (FILE* f = _wfopen(GamePath(L"bperf_spawn.txt").c_str(), L"r")) {
            char kind[16] = {};
            if (fscanf(f, "%15s %d %f %f %d", kind, &g_hamlets, &g_x, &g_y, &g_tick) != 5 || strcmp(kind, "hamlets"))
                g_hamlets = 0;
            if (fscanf(f, "%d", &g_withWork) != 1) g_withWork = 1;  // optional 6th field: 0 = no workplaces
            fclose(f);
        }
    }
    if (g_hamlets <= 0 || tick != (uint64_t)g_tick) return;
    int want = g_hamlets;
    g_hamlets = 0;
    uint8_t* gl = *(uint8_t**)(g_exe + 0x1cbdd8);
    void* mapgen = gl ? *(void**)(gl + 0x40) : nullptr;
    if (!mapgen) {
        Log("stresstown: no map generator - nothing founded");
        return;
    }
    int* map = *(int**)(gl + 0xf8);
    int size = map[0], r = *(int*)((uint8_t*)mapgen + 0x3c);
    uint8_t* mgr = *(uint8_t**)(gl + 0xe8);
    auto strCtor = (StringCtor_t)RuntimeExport("??0String@System@@QEAA@PEBD@Z");
    if (!strCtor || *(int*)((uint8_t*)mapgen + 0x88) < 1) return;
    // our lists, built from copies of the generator's first entry (keeps whatever else the entry holds)
    auto makeList = [&](const Placement* p, size_t n) {
        std::vector<uint8_t> list(n * 0x30);
        for (size_t i = 0; i < n; i++) {
            uint8_t* e = list.data() + i * 0x30;
            memcpy(e, *(uint8_t**)((uint8_t*)mapgen + 0x80), 0x30);
            strCtor(e, p[i].path);  // (the copied String is overwritten, never freed: a test tool, once)
            *(int*)(e + 8) = -1;    // generation mismatch: resolve the path
            *(void**)(e + 0x10) = nullptr;
            *(int*)(e + 0x18) = p[i].dx;
            *(int*)(e + 0x1c) = p[i].dy;
            *(int*)(e + 0x28) = p[i].rot;
        }
        return list;
    };
    std::vector<uint8_t> workList = makeList(kWorkplaces, sizeof kWorkplaces / sizeof kWorkplaces[0]);
    std::vector<uint8_t> houseList = makeList(kHouses, sizeof kHouses / sizeof kHouses[0]);
    // sites: a spiral of candidate cells around the centre, hamlets at least 2r+6 apart
    int cx = (int)(g_x + (float)(size >> 1)), cy = (int)(g_y + (float)(size >> 1));
    std::vector<std::pair<int, int>> sites;
    int spacing = 2 * r + 2, founded = 0;
    std::vector<uint8_t> mask = SiteMask(mgr, size);
    for (int ring = 1; ring < size / 4 && founded < want; ring++) {
        for (int k = 0; k < 8 * ring && founded < want; k++) {
            // walk the square ring of radius `ring` (in units of 4 cells)
            int side = k / (2 * ring), t = k % (2 * ring) - ring;
            int dx = side == 0 ? t : side == 1 ? ring : side == 2 ? -t : -ring;
            int dy = side == 0 ? -ring : side == 1 ? t : side == 2 ? ring : -t;
            int x = cx + dx * 4, y = cy + dy * 4;
            bool farEnough = true;
            for (auto& s : sites)
                if ((s.first - x) * (s.first - x) + (s.second - y) * (s.second - y) < spacing * spacing) { farEnough = false; break; }
            if (!farEnough || !Clearable(map, mask, x, y, r)) continue;
            Found(gl, mapgen, x, y, workList, houseList);
            sites.push_back({x, y});
            founded++;
        }
    }
    Log("stresstown: founded %d of %d hamlets (%s) around cell %d,%d at tick %llu", founded, want,
        g_withWork ? "with workplaces" : "homes only", cx, cy, (unsigned long long)tick);
    Log("stresstown: new entities: %d start layout, %d workplaces, %d extra houses, %d villagers", g_added[0], g_added[1],
        g_added[2], g_added[3]);
}

}  // namespace stresstown
