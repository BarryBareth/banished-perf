// Times the exact path and route solvers (src/pathsolve.h) on solves recorded in a real game, and checksums every
// result, so a solver change can be measured without the game's noise and checked for identical output.
//   record: <game>\bperf_dumppaths.txt = "start-tick records" (exact mode) -> <game>\bperf_paths.bin
//   build:  .\tools\solvebench.ps1        run: build\bench\solvebench.exe <bperf_paths.bin> [repeats] [--profile] [--v1]
// Prints per-solve times (best of the repeats), the share of results whose length matches the game's (requests are
// replayed on the latest snapshot before them, so a few differ where the map changed in between) and a checksum of
// all paths and open-set peaks - equal checksums = equal results.
#include "../src/pathsolve.h"
#include "pathsolve_v1.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// --profile: a sampler thread suspends the main thread every ~0.2 ms and counts its instruction pointer per source line
static HANDLE g_mainThread;
static volatile bool g_sampling;
static DWORD64 g_ips[1 << 22];  // no allocation while the main thread is suspended (it may hold the heap lock)
static volatile long g_nips;
static DWORD WINAPI Sampler(LPVOID) {
    while (g_sampling) {
        if (SuspendThread(g_mainThread) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_mainThread, &ctx) && g_nips < (1 << 22)) g_ips[g_nips++] = ctx.Rip;
            ResumeThread(g_mainThread);
        }
        Sleep(0);
        for (volatile int i = 0; i < 20000; i++) {}
    }
    return 0;
}
static void PrintProfile() {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    std::map<std::string, int> lines;
    int total = 0;
    std::map<DWORD64, int> samples;
    for (long i = 0; i < g_nips; i++) samples[g_ips[i]]++;
    for (auto& [ip, n] : samples) {
        total += n;
        IMAGEHLP_LINE64 ln{sizeof ln};
        DWORD disp = 0;
        std::string key = "?";
        if (SymGetLineFromAddr64(proc, ip, &disp, &ln)) {
            const char* f = strrchr(ln.FileName, '\\');
            key = std::string(f ? f + 1 : ln.FileName) + ":" + std::to_string(ln.LineNumber);
        }
        lines[key] += n;
    }
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : lines) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    printf("profile: %d samples\n", total);
    for (size_t i = 0; i < v.size() && i < 25; i++) printf("  %5.1f%%  %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
}

using namespace pathsolve;

struct Request { char kind; int snap, from, to, len; };

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: solvebench <bperf_paths.bin> [repeats]\n");
        return 1;
    }
    int repeats = argc > 2 ? atoi(argv[2]) : 5;
    bool profile = false, v1 = false;  // --profile: where the time goes; --v1: the first released grid solver
    for (int i = 3; i < argc; i++) profile |= !strcmp(argv[i], "--profile"), v1 |= !strcmp(argv[i], "--v1");
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    // grid snapshots: solver bytes with the grid pointer patched; route snapshots: 0x30-byte nodes like the game's
    struct GridSnap { std::vector<uint8_t> S, grid; };
    struct RouteSnap { std::vector<uint8_t> S, nodes; std::vector<std::vector<int>> nbs; };
    std::vector<GridSnap> grids;
    std::vector<RouteSnap> routes;
    std::vector<Request> reqs;
    for (int c; (c = fgetc(f)) != EOF;) {
        if (c == 'G') {
            int n;
            fread(&n, 4, 1, f);
            GridSnap g;
            g.S.resize(0x250);
            fread(g.S.data(), 1, 0x250, f);
            g.grid.resize((size_t)n * n);
            fread(g.grid.data(), 1, g.grid.size(), f);
            grids.push_back(std::move(g));
        } else if (c == 'R') {
            int count;
            fread(&count, 4, 1, f);
            RouteSnap r;
            r.S.assign(0x100, 0);
            r.nodes.assign((size_t)count * 0x30, 0);
            r.nbs.resize(count);
            for (int i = 0; i < count; i++) {
                int xyn[3];
                fread(xyn, 4, 3, f);
                memcpy(&r.nodes[(size_t)i * 0x30], xyn, 8);
                memcpy(&r.nodes[(size_t)i * 0x30 + 0x18], &xyn[2], 4);
                r.nbs[i].resize(xyn[2]);
                fread(r.nbs[i].data(), 4, xyn[2], f);
            }
            routes.push_back(std::move(r));
        } else if (c == 'P' || c == 'Q') {
            int rec[3];
            fread(rec, 4, 3, f);
            reqs.push_back({(char)c, (int)(c == 'P' ? grids.size() : routes.size()) - 1, rec[0], rec[1], rec[2]});
        } else {
            fprintf(stderr, "bad record '%c'\n", c);
            return 1;
        }
    }
    fclose(f);
    for (auto& g : grids) {
        uint8_t* p = g.grid.data();
        memcpy(&g.S[0xe0], &p, 8);
    }
    for (auto& r : routes) {
        int count = (int)r.nbs.size();
        for (int i = 0; i < count; i++) {
            int* p = r.nbs[i].data();
            memcpy(&r.nodes[(size_t)i * 0x30 + 0x10], &p, 8);
        }
        uint8_t* g = r.nodes.data();
        memcpy(&r.S[0x68], &g, 8);
        memcpy(&r.S[0x70], &count, 4);
    }
    int nP = 0, nQ = 0;
    for (auto& q : reqs) (q.kind == 'P' ? nP : nQ)++;
    printf("%zu grid snapshots, %zu graph snapshots (%zu nodes), %d path solves, %d route queries\n", grids.size(),
           routes.size(), routes.empty() ? 0 : routes[0].nbs.size(), nP, nQ);

    static Scratch s;
    static pathsolve_v1::Scratch s1;
    static RouteScratch rs;
    double bestP = 1e30, bestQ = 1e30;
    uint64_t hash = 0;
    int okP = 0, okQ = 0;
    HANDLE sampler = nullptr;
    if (profile) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        g_sampling = true;
        sampler = CreateThread(nullptr, 0, Sampler, nullptr, 0, nullptr);
    }
    for (int rep = 0; rep < repeats; rep++) {
        double tP = 0, tQ = 0;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](int v) { h = (h ^ (uint32_t)v) * 1099511628211ull; };
        int matchP = 0, matchQ = 0;
        for (auto& q : reqs) {
            if (q.snap < 0) continue;
            int peak;
            auto t0 = std::chrono::steady_clock::now();
            if (q.kind == 'P') {
                int count = v1 ? pathsolve_v1::Solve(s1, grids[q.snap].S.data(), q.from, q.to, &peak)
                               : Solve(s, grids[q.snap].S.data(), q.from, q.to, &peak);
                const int* path = v1 ? s1.path : s.path;
                tP += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                matchP += count == q.len;
                mix(count), mix(peak);
                for (int i = 0; i < count; i++) mix(path[i]);
            } else {
                bool found = SolveRoute(rs, routes[q.snap].S.data(), q.from, q.to, &peak);
                tQ += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                int len = found ? (int)rs.path.size() : 0;
                matchQ += len == q.len;
                mix(len), mix(peak);
                for (int v : rs.path) mix(v);
            }
        }
        if (tP < bestP) bestP = tP;
        if (tQ < bestQ) bestQ = tQ;
        hash = h, okP = matchP, okQ = matchQ;
    }
    if (sampler) {
        g_sampling = false;
        WaitForSingleObject(sampler, INFINITE);
    }
    printf("path solves:   %8.1f us each (best of %d)   lengths as in the game: %.2f%%\n", bestP / (nP ? nP : 1), repeats,
           100.0 * okP / (nP ? nP : 1));
    printf("route queries: %8.2f us each (best of %d)   lengths as in the game: %.2f%%\n", bestQ / (nQ ? nQ : 1), repeats,
           100.0 * okQ / (nQ ? nQ : 1));
    printf("checksum %016llx\n", (unsigned long long)hash);
    if (profile) PrintProfile();
    return 0;
}
