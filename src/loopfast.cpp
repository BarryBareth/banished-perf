// Exact: memory prefetching for the entity manager's per-tick loop (exe+0xc9560). The loop walks a tree of the ticking
// entities in order and calls exe+0xc82b0 on each, which reads the entity's flags (+0x40), its component array (+0x30,
// count +0x38) and each component's flags (+0x20) before the virtual update - on a big town mostly cache misses, as
// the entities, arrays and components are scattered across the heap. This runs a few dozen entities ahead of the game
// and prefetches what the game will read there, in stages (tree node, entity, its array, its components) so each
// stage's address is already cached when the next one needs it. The order comes from the previous tick, recorded as
// the game calls exe+0xc82b0: the tree hardly changes from one tick to the next, and a flat list lets the lookahead
// run far ahead without reading the tree itself (an earlier version walked the tree a few entities ahead and stalled
// on those reads). The node addresses come from an in-order walk of the tree every 16 ticks (every tick, that walk
// cost ~1.7% of the main thread); in between an added or removed entity shifts them by a place or two, which only
// blunts those prefetches. Nothing is read through a guessed address except by the lookahead itself (under SEH), so
// what the game does is unchanged. bperf_noprefetch.txt turns it off.
// Tried and dropped, both for the same reason - the loop is bound by how many misses can be in flight, so deeper
// prefetching moved the stalls into the lookahead instead of removing them:
//   - characters three levels further (CharacterComponent::vf8's spatial-grid entry, picking object and its
//     transform - the first reads of the movefast fast paths): movefast -4% of the main thread, lookahead +5%;
//   - models two levels further (ModelComponent::vf8 -> MeshInstance::Update -> AnimationState's layer array,
//     component +0x80..+0x13f and *(+0x108)): AnimationState::Update -4%, lookahead +4%.
// (After the tree the same function runs bucketed updates - 5 entity arrays at +0x18 + 0x10 i, each worked through a
// quota at a time - but on the ~2,940-citizen stress town that is ~30 entities a tick against ~4,600 in the tree, and
// prefetching them measured as nothing.)
#include "common.h"
#include <intrin.h>
#include <vector>

namespace loopfast {

static bool g_on;
uint32_t g_ticks, g_lost;              // ticks; entities the prediction did not have (log only)
static uint64_t g_seen;

static std::vector<uint8_t*> g_pred, g_rec;  // last tick's entity order; this tick's, as the game goes
static std::vector<uint8_t*> g_predNode;     // tree nodes in order, as of the last walk
static size_t g_j, g_k;                      // the game's next entity in g_pred; entries in g_rec
static uint32_t g_sinceWalk;
static bool g_live;                               // inside the tree loop
static uint8_t* g_mgr;

// how far ahead of the game each stage runs, in entities
static const size_t kNode = 56, kEntity = 40, kArray = 28, kComps = 16;

static inline void PrefetchLine(const void* p) { _mm_prefetch((const char*)p, _MM_HINT_T0); }

static inline void PrefetchComponents(uint8_t* e) {
    int n = *(int*)(e + 0x38);
    uint8_t** comps = *(uint8_t***)(e + 0x30);
    for (int i = 0; i < n && i < 8; i++) PrefetchLine(comps[i]);
}

// the stages for the game's next entity being g_pred[g_j]
static void Stages(uint8_t* const* pred, size_t n, size_t j) {
    if (j + kNode < g_predNode.size()) PrefetchLine(g_predNode[j + kNode]);
    if (j + kEntity < n) {
        uint8_t* e = pred[j + kEntity];
        PrefetchLine(e + 0x30);
        PrefetchLine(e + 0x40);
    }
    if (j + kArray < n) PrefetchLine(*(uint8_t**)(pred[j + kArray] + 0x30));
    if (j + kComps < n) PrefetchComponents(pred[j + kComps]);
}

static bool StagesSafe(uint8_t* const* pred, size_t n, size_t j) {
    __try {  // the prediction may name an entity destroyed since: its memory is only ever read here
        Stages(pred, n, j);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Begin(void* mgr) {  // exe+0xc9560 entry (main thread)
    if (!g_on) return;
    if (++g_ticks % 5000 == 0)
        Log("loopfast: %u ticks, %.0f entities a tick, %.4f%% not predicted", g_ticks, (double)g_seen / g_ticks,
            100.0 * g_lost / (double)(g_seen ? g_seen : 1));
    g_mgr = (uint8_t*)mgr;
    g_j = g_k = 0;
    g_live = true;
    size_t cap = (size_t)*(int*)(g_mgr + 0x10) + 64;  // all entities: the tree's are a subset
    if (g_rec.size() < cap) g_rec.resize(cap * 2);
    for (size_t i = 0; i < kNode && i < g_predNode.size(); i++) PrefetchLine(g_predNode[i]);  // the first stretch
    for (size_t i = 0; i < kEntity && i < g_pred.size(); i++) {
        PrefetchLine(g_pred[i] + 0x30);
        PrefetchLine(g_pred[i] + 0x40);
    }
}

void Ahead(uint8_t* entity) {  // exe+0xc82b0 entry, before the game's update of this entity
    if (!g_live) return;
    g_seen++;
    if (g_k < g_rec.size()) g_rec[g_k++] = entity;
    uint8_t* const* pred = g_pred.data();
    size_t n = g_pred.size(), j = g_j;
    if (j < n && pred[j] == entity) {
        j++;
    } else {  // removed entities (or dead ones the game skips): look a little further
        size_t i = j + 1;
        while (i < n && i < j + 32 && pred[i] != entity) i++;
        if (i < n && i < j + 32) j = i + 1;
        else g_lost++;  // new since the previous tick
    }
    g_j = j;
    if (!StagesSafe(pred, n, j)) g_j = n;  // stop guessing for the rest of the tick
}

// Tree node: +0 entity, +0x08 parent, +0x10 left, +0x18 right (exe+0xc9560's in-order walk).
static inline uint8_t* Leftmost(uint8_t* n) {
    while (uint8_t* l = *(uint8_t**)(n + 0x10)) n = l;
    return n;
}
static inline uint8_t* Successor(uint8_t* n) {
    if (uint8_t* r = *(uint8_t**)(n + 0x18)) return Leftmost(r);
    uint8_t* p = *(uint8_t**)(n + 0x08);
    while (p && *(uint8_t**)(p + 0x18) == n) n = p, p = *(uint8_t**)(p + 0x08);
    return p;
}

// the tree's nodes in order now; false if it could not be read
static bool WalkSafe(uint8_t* mgr, uint8_t** nodes, size_t cap, size_t* count) {
    __try {
        size_t k = 0;
        uint8_t* root = *(uint8_t**)(mgr + 0x68);
        for (uint8_t* n = root ? Leftmost(root) : nullptr; n && k < cap; n = Successor(n)) nodes[k++] = n;
        *count = k;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void End() {  // exe+0xc9560 after its loops: other callers of exe+0xc82b0 are not ours
    if (!g_live) return;
    g_live = false;
    g_pred.assign(g_rec.begin(), g_rec.begin() + g_k);
    if (g_sinceWalk++ % 16 == 0) {
        g_predNode.resize(g_rec.size());
        size_t k = 0;
        if (!WalkSafe(g_mgr, g_predNode.data(), g_predNode.size(), &k)) k = 0;
        g_predNode.resize(k);
    }
}

bool On() { return g_on; }

void Install() {
    g_on = !GameFileExists(L"bperf_noprefetch.txt");
    Log("loopfast: entity-loop prefetch %s", g_on ? "on" : "off (bperf_noprefetch.txt)");
}

}  // namespace loopfast
