// CogBox test of crash_dump (GitHub #286 / review r75: the game's crash minidump written by the launcher).
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -static -I../src
//       -I../../src/layer0_xenos/include crash_dump_test.cpp ../src/crash_dump.cpp -o crash_dump_test.exe
//   crash_dump_test.exe [folder]   (Windows, or wine; default folder: the current one)
// This process plays the game: it makes the info block and events exactly as crash_report.cpp does, fills them with
// a captured context of this thread, asks, and checks that the launcher side answered and wrote a real minidump. Also:
// the names, the path check, no answer without a watcher, and a refused (non-.dmp) path.
#include "crash_dump.h"

#include <xenos/crash_dump_ipc.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ipc = xenos_crash_ipc;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Game {   // what the game makes at start (crash_report.cpp XenosInstallCrashHandler)
    HANDLE map = nullptr, req = nullptr, ack = nullptr, done = nullptr;
    ipc::CrashDumpInfo *info = nullptr;
    explicit Game(DWORD pid)
    {
        wchar_t nm[64];
        ipc::Name(nm, 64, pid, L"info");
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ipc::CrashDumpInfo), nm);
        if (map) info = static_cast<ipc::CrashDumpInfo *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::CrashDumpInfo)));
        ipc::Name(nm, 64, pid, L"req");
        req = CreateEventW(nullptr, FALSE, FALSE, nm);
        ipc::Name(nm, 64, pid, L"ack");
        ack = CreateEventW(nullptr, TRUE, FALSE, nm);
        ipc::Name(nm, 64, pid, L"done");
        done = CreateEventW(nullptr, TRUE, FALSE, nm);
    }
    ~Game()
    {
        if (info) UnmapViewOfFile(info);
        for (HANDLE h : { map, req, ack, done }) if (h) CloseHandle(h);
    }
    void fill(const std::wstring &path, EXCEPTION_POINTERS *ep)
    {
        wcsncpy(info->path, path.c_str(), ipc::kPathChars - 1);
        info->thread_id = GetCurrentThreadId();
        info->exception_pointers = reinterpret_cast<uint64_t>(ep);
        info->dump_type = MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory;
        info->result = ipc::kResultNone;
        info->magic = ipc::kMagic;
    }
};

static std::wstring full(const std::wstring &dir, const wchar_t *name)
{
    wchar_t buf[2048];
    GetFullPathNameW((dir + L"\\" + name).c_str(), 2048, buf, nullptr);
    return std::wstring(L"\\\\?\\") + buf;
}

int main(int argc, char **argv)
{
    std::wstring dir = L".";
    if (argc > 1) { wchar_t w[1024]; MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, w, 1024); dir = w; }

    // names and the path check
    wchar_t nm[64];
    ipc::Name(nm, 64, 1234, L"req");
    CHECK(std::wcscmp(nm, L"Local\\cogbox-crash-1234-req") == 0);
    ipc::Name(nm, 64, 0, L"info");
    CHECK(std::wcscmp(nm, L"Local\\cogbox-crash-0-info") == 0);
    ipc::Name(nm, 64, 4294967295u, L"done");
    CHECK(std::wcscmp(nm, L"Local\\cogbox-crash-4294967295-done") == 0);
    CHECK(ipc::PathOk(L"\\\\?\\C:\\x\\crash-20261113-163425.dmp"));
    CHECK(ipc::PathOk(L"a.DMP"));
    CHECK(!ipc::PathOk(L"\\\\?\\C:\\x\\crash.txt"));
    CHECK(!ipc::PathOk(L".dmp"));
    CHECK(!ipc::PathOk(L""));

    // no watcher: no ack (the game then dumps itself)
    {
        Game g(4000000001u);
        CHECK(g.info && g.req && g.ack && g.done);
        g.fill(full(dir, L"crash-test-none.dmp"), nullptr);
        SetEvent(g.req);
        CHECK(WaitForSingleObject(g.ack, 300) == WAIT_TIMEOUT);
    }
    // a path that isn't a .dmp is refused, and no request at all is "not asked"
    {
        const DWORD fake = 4000000002u;
        CHECK(!crash_dump_write(GetCurrentProcess(), fake));   // no info block yet
        Game g(fake);
        CHECK(!crash_dump_write(GetCurrentProcess(), fake));   // block, but not filled in
        g.fill(full(dir, L"crash-test-refused.txt"), nullptr);
        CHECK(crash_dump_write(GetCurrentProcess(), fake));
        CHECK(g.info->result == ipc::kResultFailed);
        CHECK(GetFileAttributesW(full(dir, L"crash-test-refused.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    }
    // the real thing: watcher on this process, a captured exception, a minidump with the MDMP signature
    {
        const DWORD pid = GetCurrentProcessId();
        HANDLE self = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        CHECK(self != nullptr);
        Game g(pid);
        crash_dump_watch(self, pid);
        CloseHandle(self);   // the watcher has its own copy
        CONTEXT ctx = {};
        RtlCaptureContext(&ctx);
        EXCEPTION_RECORD er = {};
        er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
        EXCEPTION_POINTERS ep = { &er, &ctx };
        const std::wstring path = full(dir, L"crash-test-launcher.dmp");
        g.fill(path, &ep);
        SetEvent(g.req);
        CHECK(WaitForSingleObject(g.ack, 3000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(g.done, 60000) == WAIT_OBJECT_0);
        CHECK(g.info->result == ipc::kResultWritten);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(f != INVALID_HANDLE_VALUE);
        if (f != INVALID_HANDLE_VALUE) {
            char head[4] = {};
            DWORD got = 0;
            ReadFile(f, head, 4, &got, nullptr);
            LARGE_INTEGER size = {};
            GetFileSizeEx(f, &size);
            CloseHandle(f);
            CHECK(got == 4 && std::memcmp(head, "MDMP", 4) == 0);
            CHECK(size.QuadPart > 1000);
            std::printf("minidump %lld bytes\n", static_cast<long long>(size.QuadPart));
        }
        DeleteFileW(path.c_str());
    }
    std::printf(g_fail ? "crash_dump_test: %d FAILED\n" : "crash_dump_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
