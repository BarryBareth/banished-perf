// ---- off the main thread ------------------------------------------------------------------------------------------
// With the multiplayer mod (synchronous pathfinder) a solve request becomes a job for a worker; the main thread goes
// on with the tick. Nothing can tell the difference as long as every result is in its slot before anything looks:
//   - MoveTo::vf5 (exe+0xcda40) polls its slot's done flag      -> join that slot first
//   - exe+0xac220 takes a result                                  -> join that slot first
//   - exe+0xac300 and exe+0xe2c40 request and spin on the flag    -> join everything, then solve synchronously inside
//   - messages 0-4 change the grid/costs the workers read         -> join everything first
//   - end of every GameLoop::Update (UI, saving, leaving follow)  -> join everything
// The join replays the pool growth and writes the slot on the main thread through the game's own functions, inside
// the multiplayer mod's BmpPathMemory scope: path memory is never sim-heap memory, so the sim heap sees exactly what
// it sees without this mod. A multiplayer build without that export: solves stay synchronous.
using PathMemory_t = void(__stdcall*)(int enter);
static PathMemory_t g_pathMemory;
struct Job {
    uint8_t* pf;
    int from, to, slot, id;
    int count = 0, peak = 0;
    std::vector<int> path;
    std::atomic<bool> done{false};
};
static std::vector<Job*> g_pending;  // main thread: requested, not yet joined (request order)
static std::deque<Job*> g_queue;     // waiting for a worker
static std::mutex g_queueMutex;
static std::condition_variable g_queueCv, g_doneCv;
static int g_workers, g_forceSync;
uint32_t g_asyncJobs, g_joinWaits, g_joinWaitUs;

static DWORD WINAPI Worker(LPVOID) {
    Scratch& s = MyScratch();
    for (;;) {
        Job* j;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return !g_queue.empty(); });
            j = g_queue.front();
            g_queue.pop_front();
        }
        j->count = Solve(s, j->pf + 0x78, j->from, j->to, &j->peak);
        j->path.assign(s.path, s.path + j->count);
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            j->done = true;
        }
        g_doneCv.notify_all();
    }
}

struct PathMemoryScope {
    PathMemoryScope() { if (g_pathMemory) g_pathMemory(1); }
    ~PathMemoryScope() { if (g_pathMemory) g_pathMemory(0); }
};

static void Finish(Job* j) {
    if (!j->done) {
        uint64_t t0 = NowUs();
        std::unique_lock<std::mutex> lock(g_queueMutex);
        g_doneCv.wait(lock, [j] { return j->done.load(); });
        g_joinWaits++;
        g_joinWaitUs += (uint32_t)(NowUs() - t0);
    }
    PathMemoryScope scope;
    ReplayPoolGrowth(j->pf + 0x78, j->peak);
    // deliver if the slot is still this request's (nothing released and re-used it meanwhile)
    uint8_t* pf = j->pf;
    if (j->slot < *(int*)(pf + 0x168)) {
        const uint8_t* e = *(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)j->slot * 0x18;
        if (*(const int*)(e + 4) == j->id && e[0] == 0) WriteSlot(pf, j->slot, j->path.data(), j->count);
    }
    delete j;
}

void JoinAll() {
    for (Job* j : g_pending) Finish(j);
    g_pending.clear();
}

static void JoinSlot(int slot) {
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i]->slot == slot) {
            Finish(g_pending[i]);
            g_pending.erase(g_pending.begin() + i);
        } else {
            i++;
        }
    }
}

static void __fastcall h_PathMsg(uint8_t* pf, const int* msg) {
    if (msg[0] != 5) {
        if (!g_pending.empty() && GetCurrentThreadId() == g_mainTid) JoinAll();
        return o_PathMsg(pf, msg);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    uint64_t t0 = NowUs();
    uint8_t* S = pf + 0x78;
    Scratch& s = MyScratch();
    bool main = GetCurrentThreadId() == g_mainTid;
    if (!g_enabled) {
        o_PathMsg(pf, msg);
    } else if (g_verify) {
        int peak, count = Solve(s, S, msg[1], msg[2], &peak);
        int treeBefore = *(int*)(S + 0x18 + 0x28);
        o_PathMsg(pf, msg);
        const uint8_t* e = *(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)msg[3] * 0x18;
        int theirs = *(const int*)(e + 0x10);
        const int* tp = *(int* const*)(e + 8);
        bool same = theirs == count && (count == 0 || memcmp(tp, s.path, count * sizeof(int)) == 0);
        int grewTo = *(int*)(S + 0x18 + 0x28);
        bool poolOk = (peak <= treeBefore) == (grewTo == treeBefore);
        if (!same || !poolOk) {
            g_mismatches++;
            if (g_mismatches <= 20)
                Log("pathfast MISMATCH %d->%d: theirs %d steps, mine %d (pool %d -> %d, my peak %d)", msg[1], msg[2],
                    theirs, count, treeBefore, grewTo, peak);
        }
    } else if (g_workers && pf[0x5c] && !g_forceSync && main) {
        *(int*)S += 1;  // generation counter (0x1d760)
        Job* j = new Job;
        j->pf = pf;
        j->from = msg[1];
        j->to = msg[2];
        j->slot = msg[3];
        j->id = *(const int*)(*(uint8_t**)(pf + 0x160) + (size_t)(uint32_t)msg[3] * 0x18 + 4);
        g_pending.push_back(j);
        {
            std::lock_guard<std::mutex> ql(g_queueMutex);
            g_queue.push_back(j);
        }
        g_queueCv.notify_one();
        g_asyncJobs++;
    } else {
        if (main) JoinAll();
        *(int*)S += 1;  // generation counter (0x1d760)
        int peak, count = Solve(s, S, msg[1], msg[2], &peak);
        PathMemoryScope scope;
        ReplayPoolGrowth(S, peak);
        WriteSlot(pf, msg[3], s.path, count);
    }
    g_solveUs += (uint32_t)(NowUs() - t0);
    g_solves++;
}

// MoveTo::vf5: its slot index is at +0x20 (one pathfinder per game, so the slot alone identifies the request)
using ActionVf_t = int(__fastcall*)(int64_t* action, void* arg);
static ActionVf_t o_MoveToVf5;
static int __fastcall h_MoveToVf5(int64_t* action, void* arg) {
    if (!g_pending.empty()) {
        int slot = *(int*)((uint8_t*)action + 0x20);
        if (slot >= 0) JoinSlot(slot);
    }
    return o_MoveToVf5(action, arg);
}
// exe+0xac220(pf, int* slot, out, max): copies a result out and frees the slot
using TakeResult_t = int(__fastcall*)(uint8_t* pf, int* slot, int* out, int max);
static TakeResult_t o_TakeResult;
static int __fastcall h_TakeResult(uint8_t* pf, int* slot, int* out, int max) {
    if (!g_pending.empty()) JoinSlot(*slot);
    return o_TakeResult(pf, slot, out, max);
}
// request-and-spin functions: synchronous while inside (their own request is solved on the spot).
// exe+0xac300(pf, from, to, walk mask, out, max) and exe+0xe2c40(planner)
using RequestWait_t = void(__fastcall*)(void*, int, int, int, int*, int);
static RequestWait_t o_RequestWait;
static void __fastcall h_RequestWait(void* pf, int from, int to, int mask, int* out, int max) {
    JoinAll();
    g_forceSync++;
    o_RequestWait(pf, from, to, mask, out, max);
    g_forceSync--;
}
using RequestWait2_t = void(__fastcall*)(void*);
static RequestWait2_t o_RequestWait2;
static void __fastcall h_RequestWait2(void* planner) {
    JoinAll();
    g_forceSync++;
    o_RequestWait2(planner);
    g_forceSync--;
}
// GameLoop::Update (exe+0x9b850): the frame's sim ticks, then UI and tools
using GameLoopUpdate_t = uint64_t(__fastcall*)(void* self, const int64_t* dt);
static GameLoopUpdate_t o_GameLoopUpdate;
static uint64_t __fastcall h_GameLoopUpdate(void* self, const int64_t* dt) {
    uint64_t r = o_GameLoopUpdate(self, dt);
    JoinAll();
    return r;
}

