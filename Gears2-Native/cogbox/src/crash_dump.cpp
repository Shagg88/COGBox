// COGBox - GitHub #286 (review r75): the game's crash minidump, written by the launcher from outside the game.
// A minidump made inside a crashed game can hang for good on a lock (process heap, loader) held by a thread dbghelp
// suspended - #286 got a 32-byte .dmp. From here no lock of the game's matters. The game writes its .txt first, then
// asks (xenos/crash_dump_ipc.h); when nobody answers (the game run without COGBox) it writes the dump itself.
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
};

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
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        SetEvent(w->ack);
        crash_dump_write(w->proc, w->pid);
        SetEvent(w->done);
    }
    CloseHandle(w->req);
    CloseHandle(w->ack);
    CloseHandle(w->done);
    CloseHandle(w->proc);
    delete w;
    return 0;
}

}  // namespace

bool crash_dump_write(HANDLE proc, DWORD pid)
{
    wchar_t name[64];
    ipc::Name(name, 64, pid, L"info");
    HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!map) return false;
    auto *info = static_cast<ipc::CrashDumpInfo *>(MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ipc::CrashDumpInfo)));
    bool asked = false;
    if (info && info->magic == ipc::kMagic) {
        asked = true;
        wchar_t path[ipc::kPathChars];
        for (size_t i = 0; i < ipc::kPathChars; i++) path[i] = info->path[i];   // the game can't change it under us
        path[ipc::kPathChars - 1] = 0;
        DWORD err = ERROR_INVALID_PARAMETER;
        bool ok = false;
        using Fn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Fn fn = dbghelp ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
        if (!fn) err = dbghelp ? GetLastError() : ERROR_MOD_NOT_FOUND;
        else if (ipc::PathOk(path)) {
            HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) err = GetLastError();
            else {
                MINIDUMP_EXCEPTION_INFORMATION mei = {};
                mei.ThreadId = info->thread_id;
                mei.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<uintptr_t>(info->exception_pointers));
                mei.ClientPointers = TRUE;   // the pointer is in the game's memory
                ok = fn(proc, pid, f, static_cast<MINIDUMP_TYPE>(info->dump_type), info->exception_pointers ? &mei : nullptr,
                        nullptr, nullptr) != FALSE;
                err = ok ? 0 : GetLastError();
                CloseHandle(f);
            }
        }
        if (dbghelp) FreeLibrary(dbghelp);
        info->error = err;
        info->result = ok ? ipc::kResultWritten : ipc::kResultFailed;
    }
    if (info) UnmapViewOfFile(info);
    CloseHandle(map);
    return asked;
}

void crash_dump_watch(HANDLE proc, DWORD pid)
{
    Watch *w = new Watch{};
    w->pid = pid;
    if (!DuplicateHandle(GetCurrentProcess(), proc, GetCurrentProcess(), &w->proc, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        delete w;
        return;
    }
    w->req = make_event(pid, L"req", false);
    w->ack = make_event(pid, L"ack", true);
    w->done = make_event(pid, L"done", true);
    HANDLE t = (w->req && w->ack && w->done) ? CreateThread(nullptr, 0, watch_main, w, 0, nullptr) : nullptr;
    if (t) {
        CloseHandle(t);
        return;
    }
    for (HANDLE h : { w->req, w->ack, w->done, w->proc }) if (h) CloseHandle(h);
    delete w;
}
