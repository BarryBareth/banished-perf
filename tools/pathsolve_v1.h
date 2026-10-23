// The solver as first released (20-byte cells), for tools/solvebench.cpp to compare against: same results, timing.
#pragma once
#include <climits>
#include <cstdint>
#include <vector>
namespace pathsolve_v1 {
struct Cell {
    uint32_t open, closed, seq;  // open/closed: == stamp when in that set this solve
    int g, parent;
};
struct Entry { int cell; uint32_t seq; int next; };
struct Scratch {
    int n = 0;
    std::vector<Cell> cells;
    std::vector<int> head;     // bucket f -> top entry (-1 = empty)
    std::vector<int> touched;  // buckets made non-empty this solve (reset afterwards)
    std::vector<Entry> entries;
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

    size_t ncells = (size_t)n * n;
    if (s.n != n || s.cells.size() != ncells) {
        s.n = n;
        s.cells.assign(ncells, Cell{0, 0, 0, 0, -1});
        s.stamp = 0;
    }
    if (++s.stamp == 0) {  // wrapped: clear the stamps once
        for (Cell& c : s.cells) c.open = c.closed = 0;
        s.stamp = 1;
    }
    const uint32_t st = s.stamp;
    Cell* C = s.cells.data();
    s.entries.clear();
    uint32_t seq = 0;
    int open = 0, peak = 0;
    int minF = INT_MAX;

    auto insert = [&](int cell, int par, int g, int f) {
        Cell& c = C[cell];
        if (c.open != st) open++;  // else: decrease-key, the original removes then re-inserts
        c.open = st;
        c.parent = par;
        c.g = g;
        c.seq = seq;
        if ((size_t)f >= s.head.size()) s.head.resize((size_t)f + 1024, -1);
        int& h = s.head[f];
        if (h < 0) s.touched.push_back(f);
        s.entries.push_back({cell, seq, h});
        h = (int)s.entries.size() - 1;
        if (f < minF) minF = f;
        seq++;
        if (open > peak) peak = open;
    };

    insert(from, -1, 0, heur(from % n, from / n));
    bool found = false;
    while (open > 0) {
        while (s.head[minF] < 0) minF++;
        const Entry e = s.entries[s.head[minF]];
        s.head[minF] = e.next;
        const int cur = e.cell;
        Cell& cc = C[cur];
        if (cc.open != st || cc.seq != e.seq) continue;  // stale entry (re-keyed later)
        if (cur == to) { found = true; break; }
        cc.open = 0;
        open--;
        cc.closed = st;
        const int cx = cur % n, cy = cur / n, gcur = cc.g;
        for (int d = 0; d < dirs; d++) {
            const int x = cx + dxs[d], y = cy + dys[d];
            if ((unsigned)x >= (unsigned)n || (unsigned)y >= (unsigned)n) continue;
            const int nb = y * n + x;
            const uint8_t cell = grid[nb];
            if (blockBit && (cell & 0x80)) continue;
            if (!((mask >> (cell & 0x1f)) & 1)) continue;
            const int gnew = gcur + cost[cell & 0x7f] * (d < 4 ? 10 : 14);
            Cell& nc = C[nb];
            if (nc.closed == st) {
                if (nc.g <= gnew) continue;
                nc.closed = 0;  // re-opened
            }
            if (nc.open != st || gnew < nc.g) insert(nb, cur, gnew, heur(x, y) + gnew);
        }
    }
    for (int f : s.touched) s.head[f] = -1;
    s.touched.clear();
    *peakOut = peak;
    if (!found) return 0;
    int count = 0;
    for (int c = to; c != -1; c = C[c].parent) count++;
    int i = count - 1;
    for (int c = to; i >= 0; c = C[c].parent, i--)
        if (i < 0x4000) s.path[i] = c;
    return count > 0x4000 ? 0x4000 : count;
}

}  // namespace pathsolve_v1
