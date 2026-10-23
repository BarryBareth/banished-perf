// dbghelp.dll proxy. Runtime-steam-x64.dll imports exactly one function from dbghelp.dll (MiniDumpWriteDump, used
// only by its crash handler), and dbghelp is not a KnownDLL, so a copy next to the exe is loaded at startup. The
// multiplayer mod already owns the dinput8.dll slot; this one leaves it free so both mods load side by side.
#include "common.h"
#include "version.h"
#include "MinHook.h"
#include <cstdarg>
#include <mutex>

DWORD g_mainTid;
// Install.bat / Uninstall.bat recognise this DLL by this text (UTF-16) before touching a dbghelp.dll.
static const wchar_t kMarker[] = L"BanishedPerf";
static std::wstring g_gameDir;
static FILE* g_log;
static std::mutex g_logMutex;

std::wstring GamePath(const wchar_t* name) { return g_gameDir + name; }
bool GameFileExists(const wchar_t* name) { return GetFileAttributesW(GamePath(name).c_str()) != INVALID_FILE_ATTRIBUTES; }

void LogInit() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    *(wcsrchr(path, L'\\') + 1) = 0;
    g_gameDir = path;
    // one session's worth is plenty: a log over 1 MB is moved to bperf_prev.log at start
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(GamePath(L"bperf.log").c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (1u << 20)))
        MoveFileExW(GamePath(L"bperf.log").c_str(), GamePath(L"bperf_prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen(GamePath(L"bperf.log").c_str(), L"a", _SH_DENYNO);
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_logMutex);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), buf);
    fflush(g_log);
}

void* RuntimeExport(const char* mangled) {
    HMODULE rt = GetModuleHandleW(L"Runtime-steam-x64.dll");
    void* p = rt ? (void*)GetProcAddress(rt, mangled) : nullptr;
    if (!p) Log("missing export %s", mangled);
    return p;
}

bool HookAt(const char* label, void* target, void* detour, void** orig) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) Log("hook %s failed: %s", label, MH_StatusToString(s));
    return s == MH_OK;
}

// MiniDumpWriteDump: forward to the system dbghelp (loaded on first use - the game only calls it when crashing).
using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
extern "C" BOOL WINAPI MiniDumpWriteDump(HANDLE proc, DWORD pid, HANDLE file, int type, void* ex, void* user, void* cb) {
    static MiniDumpWriteDump_t real;
    if (!real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dbghelp.dll");
        HMODULE m = LoadLibraryW(path);
        real = m ? (MiniDumpWriteDump_t)GetProcAddress(m, "MiniDumpWriteDump") : nullptr;
    }
    return real ? real(proc, pid, file, type, ex, user, cb) : FALSE;
}

// World::Update (exe+0xab1e0) starts every sim tick. Hooked from DllMain, before the multiplayer mod's dinput8.dll
// installs its hooks (on the engine's first DirectInput8Create): its hook on the same function then wraps this one,
// so this runs inside its tick context and after the players' commands - the same on every PC.
using WorldUpdate_t = void(__fastcall*)(void*, const int64_t*);
static WorldUpdate_t o_WorldUpdate;
static void __fastcall h_WorldUpdate(void* world, const int64_t* dt) {
    pathfast::OnTickStart();
    profiler::OnTick();
    o_WorldUpdate(world, dt);
}

// For the multiplayer mod: what this DLL does to the simulation, as an entry for its mod list ("" = nothing: a game
// with this mod plays out exactly like one without it). Players whose entries differ cannot play together.
extern "C" const char* __stdcall BperfSimSignature() {
    static std::string sig = [] {
        std::string s = pathfast::SimSignature();  // "BanishedPerf-delayed-paths-K" or ""
        if (parmove::On()) s += s.empty() ? "BanishedPerf-parallel-movement" : "-parallel-movement";
        if (parmove::ParallelAI()) s += "-parallel-ai";
        return s;
    }();
    return sig.c_str();
}

// Hooks go in from a thread: DllMain runs under the loader lock while Runtime itself is still being loaded. The thread
// starts once the import chain is done; MinHook suspends the other threads while it patches.
static DWORD WINAPI InitThread(LPVOID) {
    Log("BanishedPerf %s loaded (%ls), main thread %lu", BPERF_VERSION, kMarker, g_mainTid);
    auto exe = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp == 0x59b56a2d)  // Steam 1.0.7: the build the exe addresses are for
    {
        pathfast::Install();
        movefast::Install();
        loopfast::Install();
        parmove::Install();
    }
    else
        Log("exe build %08x is not the Steam 1.0.7 build: optimizations off", nt->FileHeader.TimeDateStamp);
    profiler::Install();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_mainTid = GetCurrentThreadId();
        LogInit();
        MH_STATUS mh = MH_Initialize();
        if (mh != MH_OK) Log("MH_Initialize: %s", MH_StatusToString(mh));
        auto exe = (uint8_t*)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
        if (mh == MH_OK && nt->FileHeader.TimeDateStamp == 0x59b56a2d)
        {
            HookAt("World::Update", exe + 0xab1e0, (void*)h_WorldUpdate, (void**)&o_WorldUpdate);
            parmove::InstallEarly();
        }
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
