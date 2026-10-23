// The exact path and route solvers (see pathfast.cpp), in a header so tools/solvebench.cpp can time the same code
// on recorded solves. Reads the game's solver object S; touches nothing else.
#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pathsolve {

struct Cell {
    uint32_t open, closed, seq;  // open/closed: == stamp when in that set this solve
    int g, parent;
};
struct Entry { int cell; uint32_t seq; int next; };
// The grid solve's cell, 8 bytes (the map's cells do not fit in cache; this was 20): tag = stamp << 5 | closed << 4 |
// has parent << 3 | direction from the parent (index into dxs/dys); tag >> 5 != stamp = in neither set this solve.
// No insertion number: a cell is only ever re-inserted with a strictly lower g, so of its queue entries the current
// one is the one with the cell's g - the rest are stale, as the seq check found them. The search runs on x/y (queue
// entries hold y << 16 | x): no division per step. (Tried, no faster on recorded solves: cells in 8x8 tiles; one
// table lookup instead of the walkable tests.)
struct GridCell {
    uint32_t tag;
    int g;
};
struct GridEntry { int xy, g, next; };
struct Scratch {
    int n = 0;
    std::vector<GridCell> cells;
    std::vector<int> head;     // bucket f -> top entry (-1 = empty)
    std::vector<int> touched;  // buckets made non-empty this solve (reset afterwards)
    std::vector<GridEntry> entries;
    uint32_t stamp = 0;
    int path[0x4000];
};
// Returns the path length the original would (0 = no path; at most 0x4000, start first, in s.path) and the open set's
// peak size. Reads the solver's settings and grid; nothing else.
static int Solve(Scratch& s, const uint8_t* S, int from, int to, int* peakOut) {
    const int n = *(const int*)(S + 0xd8);
    const uint8_t* grid = *(uint8_t* const*)(S + 0xe0);
    const uint32_t mask = *(const uint32_t*)(S + 0xd0);
    const int* cost = (const int*)(S + 0x50);  // indexed by type (0..0x7f) exactly like the original
    const bool diag = S[0xd4] != 0, manhattan = S[0xd5] != 0, blockBit = S[0xd6] != 0;
    const int dirs = diag ? 8 : 4;
    const int tx = to % n, ty = to / n;
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dys[8] = {0, 0, 1, -1, 1, -1, -1, 1};
    auto heur = [&](int x, int y) {
        int dx = x - tx, dy = y - ty;
        dx = dx < 0 ? -dx : dx;
        dy = dy < 0 ? -dy : dy;
        if (diag) return (dx + dy) * 10 - 6 * (dx < dy ? dx : dy);
        if (manhattan) return (dx + dy) * 10;
        return (dx > dy ? dx : dy) * 10;
    };

    const size_t ncells = (size_t)n * n;
    if (s.n != n || s.cells.size() != ncells) {
        s.n = n;
        s.cells.assign(ncells, GridCell{0, 0});
        s.stamp = 0;
    }
    auto at = [n](int x, int y) { return (size_t)y * n + x; };
    if (++s.stamp >= (1u << 27)) {  // wrapped: clear the tags once
        for (GridCell& c : s.cells) c.tag = 0;
        s.stamp = 1;
    }
    const uint32_t openTag = s.stamp << 5, closedTag = openTag | 0x10;  // compare with tag & ~0xf
    GridCell* C = s.cells.data();
    s.entries.clear();
    int open = 0, peak = 0;
    int minF = INT_MAX;

    auto insert = [&](GridCell& c, int xy, uint32_t from, int g, int f) {  // from: has-parent bit | direction
        if ((c.tag & ~0xfu) != openTag) open++;  // else: decrease-key, the original removes then re-inserts
        c.tag = openTag | from;
        c.g = g;
        if ((size_t)f >= s.head.size()) s.head.resize((size_t)f + 1024, -1);
        int& h = s.head[f];
        if (h < 0) s.touched.push_back(f);
        s.entries.push_back({xy, g, h});
        h = (int)s.entries.size() - 1;
        if (f < minF) minF = f;
        if (open > peak) peak = open;
    };

    const int fx = from % n, fy = from / n;
    insert(C[at(fx, fy)], fy << 16 | fx, 0, 0, heur(fx, fy));
    const int toXY = ty << 16 | tx;
    bool found = false;
    while (open > 0) {
        while (s.head[minF] < 0) minF++;
        const GridEntry e = s.entries[s.head[minF]];
        s.head[minF] = e.next;
        const int cx = e.xy & 0xffff, cy = e.xy >> 16;
        GridCell& cc = C[at(cx, cy)];
        if ((cc.tag & ~0xfu) != openTag || cc.g != e.g) continue;  // stale entry (re-keyed later, or closed)
        if (e.xy == toXY) { found = true; break; }
        open--;
        cc.tag = (cc.tag & 0xf) | closedTag;
        const int gcur = cc.g;
        for (int d = 0; d < dirs; d++) {
            const int x = cx + dxs[d], y = cy + dys[d];
            if ((unsigned)x >= (unsigned)n || (unsigned)y >= (unsigned)n) continue;
            const uint8_t cell = grid[y * n + x];
            if (blockBit && (cell & 0x80)) continue;
            if (!((mask >> (cell & 0x1f)) & 1)) continue;
            const int gnew = gcur + cost[cell & 0x7f] * (d < 4 ? 10 : 14);
            GridCell& nc = C[at(x, y)];
            const uint32_t t = nc.tag & ~0xfu;
            if (t == closedTag) {
                if (nc.g <= gnew) continue;  // else re-opened
            } else if (t == openTag && gnew >= nc.g) {
                continue;
            }
            insert(nc, y << 16 | x, 8u | (uint32_t)d, gnew, heur(x, y) + gnew);
        }
    }
    for (int f : s.touched) s.head[f] = -1;
    s.touched.clear();
    *peakOut = peak;
    if (!found) return 0;
    int count = 0;
    for (int x = tx, y = ty;;) {  // back to the start (the one cell without a parent)
        count++;
        uint32_t t = C[at(x, y)].tag;
        if (!(t & 8)) break;
        x -= dxs[t & 7], y -= dys[t & 7];
    }
    int i = count - 1;
    for (int x = tx, y = ty; i >= 0; i--) {
        if (i < 0x4000) s.path[i] = y * n + x;
        uint32_t t = C[at(x, y)].tag;
        x -= dxs[t & 7], y -= dys[t & 7];
    }
    return count > 0x4000 ? 0x4000 : count;
}

// ---- route distances ------------------------------------------------------------------------------------------------
// exe+0x1e560 (-> 0x1e630): the same AVL A* over the map's region graph, used for "how far is X" questions (job,
// storage and market choices; ~8% of a big town's frame). Solver layout as above (+0x18 tree, +8 node array) plus
// +0x68 region nodes (0x30 B: +0 int x, +4 int y, +0x10 int* neighbours, +0x18 count), +0x70 region count.
// Edge cost and heuristic: (int)sqrtf((float)(dx*dx + dy*dy)) on the region coordinates (SSE: correctly rounded).
// (Tried, no faster on recorded queries: a compact copy of the graph - coordinates, neighbours, cached edge costs.)
struct RouteScratch {
    int count = 0;
    std::vector<Cell> cells;
    std::vector<int> head, touched;
    std::vector<Entry> entries;
    uint32_t stamp = 0;
    std::vector<int> path;
};
static inline int RegionDist(const uint8_t* G, int a, int b) {
    const int* pa = (const int*)(G + (size_t)a * 0x30);
    const int* pb = (const int*)(G + (size_t)b * 0x30);
    int dx = pa[0] - pb[0], dy = pa[1] - pb[1];
    return (int)sqrtf((float)(int)((unsigned)dx * (unsigned)dx + (unsigned)dy * (unsigned)dy));
}

// Fills s.path (start first, uncapped) when found; returns found. peakOut = open set peak.
static bool SolveRoute(RouteScratch& s, const uint8_t* S, int from, int to, int* peakOut) {
    const uint8_t* G = *(uint8_t* const*)(S + 0x68);
    const int nodes = *(const int*)(S + 0x70);
    if (s.count != nodes || s.cells.size() != (size_t)nodes) {
        s.count = nodes;
        s.cells.assign(nodes, Cell{0, 0, 0, 0, -1});
        s.stamp = 0;
    }
    if (++s.stamp == 0) {
        for (Cell& c : s.cells) c.open = c.closed = 0;
        s.stamp = 1;
    }
    const uint32_t st = s.stamp;
    Cell* C = s.cells.data();
    s.entries.clear();
    uint32_t seq = 0;
    int open = 0, peak = 0, minF = INT_MAX;
    auto insert = [&](int node, int par, int g, int f) {
        Cell& c = C[node];
        if (c.open != st) open++;
        c.open = st;
        c.parent = par;
        c.g = g;
        c.seq = seq;
        if (f < 0) f = 0;  // cannot happen with real coordinates; keeps the bucket index valid
        if ((size_t)f >= s.head.size()) s.head.resize((size_t)f + 1024, -1);
        int& h = s.head[f];
        if (h < 0) s.touched.push_back(f);
        s.entries.push_back({node, seq, h});
        h = (int)s.entries.size() - 1;
        if (f < minF) minF = f;
        seq++;
        if (open > peak) peak = open;
    };
    insert(from, -1, 0, RegionDist(G, from, to));
    bool found = false;
    while (open > 0) {
        while (s.head[minF] < 0) minF++;
        const Entry e = s.entries[s.head[minF]];
        s.head[minF] = e.next;
        const int cur = e.cell;
        Cell& cc = C[cur];
        if (cc.open != st || cc.seq != e.seq) continue;
        if (cur == to) { found = true; break; }
        cc.open = 0;
        open--;
        cc.closed = st;
        const uint8_t* rc = G + (size_t)cur * 0x30;
        const int* nbs = *(int* const*)(rc + 0x10);
        const int nn = *(const int*)(rc + 0x18), gcur = cc.g;
        for (int i = 0; i < nn; i++) {
            const int nb = nbs[i];
            if (nb < 0) continue;
            const int gnew = RegionDist(G, cur, nb) + gcur;
            Cell& nc = C[nb];
            if (nc.closed == st) {
                if (gnew >= nc.g) continue;
                nc.closed = 0;
            }
            if (nc.open == st && gnew >= nc.g) continue;
            insert(nb, cur, gnew, RegionDist(G, nb, to) + gnew);
        }
    }
    for (int f : s.touched) s.head[f] = -1;
    s.touched.clear();
    *peakOut = peak;
    s.path.clear();
    if (!found) return false;
    for (int c = to; c != -1; c = C[c].parent) s.path.push_back(c);
    std::reverse(s.path.begin(), s.path.end());
    return true;
}

}  // namespace pathsolve
