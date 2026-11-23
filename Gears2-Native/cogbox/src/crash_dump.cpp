// COGBox - GitHub #286 (review r75 / r76): the game's crash minidump, written by the launcher from outside the game.
// A minidump made inside a crashed game can hang for good on a lock (process heap, loader) held by a thread dbghelp
// suspended - #286 got a 32-byte .dmp. From here no lock of the game's matters. The game writes its .txt first, then
// asks (xenos/crash_dump_ipc.h); when nobody answers (the game run without COGBox) it writes the dump itself.
// r76: only <the play's folder>\crash-YYYYMMDD-HHMMSS.dmp, never over an existing file, only the game's two dump flags;
// the request is claimed atomically (the game or COGBox writes the dump, never both); stray requests are ignored.
#include "crash_dump.h"

#include <xenos/crash_dump_ipc.h>

#include <dbghelp.h>

#include <cstdint>
#include <initializer_list>

namespace ipc = xenos_crash_ipc;

namespace {

struct Watch {
    HANDLE proc, req, ack, done;
    DWORD pid;
    std::wstring folder;
    DWORD extra;   // docs/logging.md: the Everything logging level's extra dump flags (kDumpTypeExtraAllowed)
    std::shared_ptr<CrashDumpStatus> status;
};

SRWLOCK g_dbghelp_lock = SRWLOCK_INIT;   // r76 L5: dbghelp is single-threaded (two plays' watchers)

HANDLE make_event(DWORD pid, const wchar_t *what, bool manual)
{
    wchar_t name[64];
    ipc::Name(name, 64, pid, what);
    return CreateEventW(nullptr, manual, FALSE, name);   // opens the game's when it made it first
}

DWORD WINAPI watch_main(void *p)
{
    Watch *w = static_cast<Watch *>(p);
    HANDLE waits[2] = { w->req, w->proc };
    // r76 L6: a request that isn't a valid one (stray, or one the game already gave up on) doesn't use the watcher up
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        DWORD err = 0;
        const int r = crash_dump_write(w->proc, w->pid, w->folder, w->ack, &err, w->extra);
        if (!r) continue;
        w->status->error = err;
        InterlockedExchange(&w->status->state, r);
        SetEvent(w->done);
    }
    CloseHandle(w->req);
    CloseHandle(w->ack);
    CloseHandle(w->done);
    CloseHandle(w->proc);
    delete w;
    return 0;
}

bool digits(const wchar_t *s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] < L'0' || s[i] > L'9') return false;
    return true;
}

}  // namespace

std::wstring crash_dump_folder(const std::string &logDir)
{
    wchar_t w[2048], full[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, logDir.c_str(), -1, w, 2048)) return std::wstring();
    const DWORD n = GetFullPathNameW(w, 2048, full, nullptr);
    if (!n || n >= 2048) return std::wstring();
    std::wstring f = full;
    for (wchar_t &c : f) if (c == L'/') c = L'\\';
    while (!f.empty() && f.back() == L'\\') f.pop_back();
    return f;
}

bool crash_dump_path_ok(const wchar_t *path, const std::wstring &folder)
{
    if (folder.size() < 2) return false;
    // the game's form: "\\?\" + "C:\..." or "\\?\UNC\server\share\..." for "\\server\share\..."
    std::wstring prefix = folder.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + folder.substr(2) : L"\\\\?\\" + folder;
    prefix += L'\\';
    size_t n = 0;
    while (n < ipc::kPathChars && path[n]) n++;
    if (n != prefix.size() + 25) return false;   // crash-YYYYMMDD-HHMMSS.dmp
    if (CompareStringOrdinal(path, (int)prefix.size(), prefix.c_str(), (int)prefix.size(), TRUE) != CSTR_EQUAL) return false;
    const wchar_t *f = path + prefix.size();
    return std::wstring(f, 6) == L"crash-" && digits(f + 6, 8) && f[14] == L'-' && digits(f + 15, 6) &&
           std::wstring(f + 21, 4) == L".dmp";
}

int crash_dump_write(HANDLE proc, DWORD pid, const std::wstring &folder, HANDLE ack, DWORD *error, DWORD extraType)
{
    *error = 0;
    wchar_t name[64];
    ipc::Name(name, 64, pid, L"info");
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!map) return 0;
    auto *info = static_cast<ipc::CrashDumpInfo *>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ipc::CrashDumpInfo)));
    int r = 0;
    // r76 L3: claimed atomically - the game, giving up on an ack, takes kMagic back the same way; one side wins
    if (info && InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&info->magic), (LONG)ipc::kTaken,
                                           (LONG)ipc::kMagic) == (LONG)ipc::kMagic) {
        if (ack) SetEvent(ack);
        wchar_t path[ipc::kPathChars];
        for (size_t i = 0; i < ipc::kPathChars; i++) path[i] = info->path[i];   // the game can't change it under us
        path[ipc::kPathChars - 1] = 0;
        const DWORD tid = info->thread_id;
        const uint64_t ep = info->exception_pointers;
        const DWORD type = info->dump_type & ipc::kDumpTypeAllowed;   // r76 M1: the game's flags, nothing more
        // docs/logging.md (Everything): a few more of the game's own details - globals, handles, unloaded modules, thread
        // and memory-region info (tens of MB, no full memory: that would be GBs and hold the player's data)
        const DWORD type2 = type | (extraType & kDumpTypeExtraAllowed);
        DWORD err = ERROR_INVALID_PARAMETER;   // a path that isn't the play's crash-*.dmp
        bool ok = false;
        if (crash_dump_path_ok(path, folder)) {
            AcquireSRWLockExclusive(&g_dbghelp_lock);
            using Fn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
            HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!dbghelp) err = GetLastError();   // r76 L4: the real reason
            Fn fn = dbghelp ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
            if (dbghelp && !fn) err = GetLastError();
            if (fn) {
                // CREATE_NEW: never over an existing file (r76 M1)
                HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f == INVALID_HANDLE_VALUE) err = GetLastError();
                else {
                    MINIDUMP_EXCEPTION_INFORMATION mei = {};
                    mei.ThreadId = tid;
                    mei.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<uintptr_t>(ep));
                    mei.ClientPointers = TRUE;   // the pointer is in the game's memory
                    ok = fn(proc, pid, f, static_cast<MINIDUMP_TYPE>(type2), ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
                    err = ok ? 0 : GetLastError();
                    CloseHandle(f);
                }
            }
            if (dbghelp) FreeLibrary(dbghelp);
            ReleaseSRWLockExclusive(&g_dbghelp_lock);
        }
        info->error = err;
        info->result = ok ? ipc::kResultWritten : ipc::kResultFailed;
        *error = err;
        r = ok ? CrashDumpStatus::kWritten : CrashDumpStatus::kFailed;
    }
    if (info) UnmapViewOfFile(info);
    CloseHandle(map);
    return r;
}

std::shared_ptr<CrashDumpStatus> crash_dump_watch(HANDLE proc, DWORD pid, const std::string &logDir, DWORD extraType)
{
    Watch *w = new Watch{};
    w->pid = pid;
    w->extra = extraType;
    w->folder = crash_dump_folder(logDir);
    w->status = std::make_shared<CrashDumpStatus>();
    if (w->folder.empty() ||
        !DuplicateHandle(GetCurrentProcess(), proc, GetCurrentProcess(), &w->proc, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        delete w;
        return nullptr;
    }
    w->req = make_event(pid, L"req", false);
    w->ack = make_event(pid, L"ack", true);
    w->done = make_event(pid, L"done", true);
    std::shared_ptr<CrashDumpStatus> status = w->status;
    HANDLE t = (w->req && w->ack && w->done) ? CreateThread(nullptr, 0, watch_main, w, 0, nullptr) : nullptr;
    if (t) {
        CloseHandle(t);
        return status;
    }
    for (HANDLE h : { w->req, w->ack, w->done, w->proc }) if (h) CloseHandle(h);
    delete w;
    return nullptr;
}
