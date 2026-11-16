// CogBox test of crash_dump (GitHub #286 / reviews r75, r76: the game's crash minidump written by the launcher).
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -static
//       -I../src -I../../src/layer0_xenos/include crash_dump_test.cpp ../src/crash_dump.cpp -o crash_dump_test.exe
//   crash_dump_test.exe [folder]   (Windows, or wine; default folder: the current one)
// This process plays the game: it makes the info block and events exactly as crash_report.cpp does, fills them with
// a captured context of this thread, asks, and checks that the launcher side answered and wrote a real minidump. Also:
// the names, the path rules (only <folder>\crash-<8 digits>-<6 digits>.dmp), no answer without a watcher, refused
// paths, no overwrite, the dump type mask, the atomic claim, and a stray request not using the watcher up.
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
    void fill(const std::wstring &path, EXCEPTION_POINTERS *ep, uint32_t type = MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory)
    {
        wcsncpy(info->path, path.c_str(), ipc::kPathChars - 1);
        info->thread_id = GetCurrentThreadId();
        info->exception_pointers = reinterpret_cast<uint64_t>(ep);
        info->dump_type = type;
        info->result = ipc::kResultNone;
        info->magic = ipc::kMagic;
    }
};

static long long file_size(const std::wstring &path, char *head4)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    ReadFile(f, head4, 4, &got, nullptr);
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    CloseHandle(f);
    return size.QuadPart;
}

int main(int argc, char **argv)
{
    const std::wstring folder = crash_dump_folder(argc > 1 ? argv[1] : ".");
    CHECK(!folder.empty());
    const std::wstring pre = L"\\\\?\\" + folder + L"\\";

    // names
    wchar_t nm[64];
    ipc::Name(nm, 64, 1234, L"req");
    CHECK(std::wcscmp(nm, L"Local\\cogbox-crash-1234-req") == 0);
    ipc::Name(nm, 64, 0, L"info");
    CHECK(std::wcscmp(nm, L"Local\\cogbox-crash-0-info") == 0);
    ipc::Name(nm, 64, 4294967295u, L"done");
    CHECK(std::wcscmp(nm, L"Local\\cogbox-crash-4294967295-done") == 0);

    // the path rules
    const std::wstring F = L"C:\\Games\\Gears2\\logs\\20261113-163355";
    CHECK(crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\crash-20261113-163425.dmp", F));
    CHECK(crash_dump_path_ok(L"\\\\?\\c:\\games\\GEARS2\\logs\\20261113-163355\\crash-20261113-163425.dmp", F));   // case
    CHECK(crash_dump_path_ok(L"\\\\?\\UNC\\srv\\share\\logs\\crash-20261113-163425.dmp", L"\\\\srv\\share\\logs"));
    CHECK(crash_dump_path_ok(L"\\\\?\\C:\\crash-20261113-163425.dmp", L"C:"));                                      // drive root
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\other\\crash-20261113-163425.dmp", F));            // other folder
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\sub\\crash-20261113-163425.dmp", F));
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\crash-20261113-163425.txt", F));
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\crash-2026111x-163425.dmp", F));  // digits
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\crash-20261113-16342.dmp", F));
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\x.txt:crash-20261113-163425.dmp", F)); // ADS
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\..\\crash-20261113-163425.dmp", F));
    CHECK(!crash_dump_path_ok(L"\\\\.\\pipe\\crash-20261113-163425.dmp", F));                                      // pipe
    CHECK(!crash_dump_path_ok(L"C:\\Games\\Gears2\\logs\\20261113-163355\\crash-20261113-163425.dmp", F));         // no \\?\ form
    CHECK(!crash_dump_path_ok(L"\\\\?\\C:\\Games\\Gears2\\logs\\20261113-163355\\crash-20261113-163425.dmp", L""));
    CHECK(!crash_dump_path_ok(L"", F));

    // no watcher: no ack (the game then takes the request back and dumps itself)
    {
        Game g(4000000001u);
        CHECK(g.info && g.req && g.ack && g.done);
        g.fill(pre + L"crash-00000000-000001.dmp", nullptr);
        SetEvent(g.req);
        CHECK(WaitForSingleObject(g.ack, 300) == WAIT_TIMEOUT);
    }
    // crash_dump_write directly: nothing to claim, a refused path, a taken-back request, no overwrite
    {
        const DWORD fake = 4000000002u;
        DWORD err = 0;
        CHECK(crash_dump_write(GetCurrentProcess(), fake, folder, nullptr, &err) == 0);   // no block yet
        Game g(fake);
        CHECK(crash_dump_write(GetCurrentProcess(), fake, folder, nullptr, &err) == 0);   // block, nothing asked
        g.fill(pre + L"crash-00000000-000002.txt", nullptr);
        CHECK(crash_dump_write(GetCurrentProcess(), fake, folder, g.ack, &err) == CrashDumpStatus::kFailed);
        CHECK(g.info->result == ipc::kResultFailed && g.info->magic == ipc::kTaken);
        CHECK(WaitForSingleObject(g.ack, 0) == WAIT_OBJECT_0);   // claimed requests are acked
        CHECK(GetFileAttributesW((pre + L"crash-00000000-000002.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
        g.fill(L"\\\\?\\C:\\elsewhere\\crash-00000000-000003.dmp", nullptr);                 // another folder
        CHECK(crash_dump_write(GetCurrentProcess(), fake, folder, nullptr, &err) == CrashDumpStatus::kFailed);
        g.fill(pre + L"crash-00000000-000004.dmp", nullptr);
        g.info->magic = 0;                                                                   // the game took it back
        CHECK(crash_dump_write(GetCurrentProcess(), fake, folder, nullptr, &err) == 0);
        CHECK(GetFileAttributesW((pre + L"crash-00000000-000004.dmp").c_str()) == INVALID_FILE_ATTRIBUTES);
        const std::wstring existing = pre + L"crash-00000000-000005.dmp";                   // never over a file
        HANDLE f = CreateFileW(existing.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(f, "keep", 4, &w, nullptr);
        CloseHandle(f);
        g.fill(existing, nullptr);
        CHECK(crash_dump_write(GetCurrentProcess(), fake, folder, nullptr, &err) == CrashDumpStatus::kFailed);
        CHECK(err == ERROR_FILE_EXISTS);
        char head[4] = {};
        CHECK(file_size(existing, head) == 4 && std::memcmp(head, "keep", 4) == 0);
        DeleteFileW(existing.c_str());
    }
    // the real thing: watcher on this process; a stray request first (ignored, the watcher stays); then a captured
    // exception with a FullMemory dump type asked for (masked to the game's two flags) -> a minidump with MDMP
    {
        const DWORD pid = GetCurrentProcessId();
        HANDLE self = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        CHECK(self != nullptr);
        Game g(pid);
        std::shared_ptr<CrashDumpStatus> st = crash_dump_watch(self, pid, argc > 1 ? argv[1] : ".");
        CHECK(st != nullptr);
        CloseHandle(self);   // the watcher has its own copy
        SetEvent(g.req);     // stray: nothing filled in
        CHECK(WaitForSingleObject(g.ack, 500) == WAIT_TIMEOUT);
        CHECK(st && st->state == CrashDumpStatus::kWatching);
        CONTEXT ctx = {};
        RtlCaptureContext(&ctx);
        EXCEPTION_RECORD er = {};
        er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
        EXCEPTION_POINTERS ep = { &er, &ctx };
        const size_t big = 128u << 20;
        char *block = static_cast<char *>(VirtualAlloc(nullptr, big, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(block != nullptr);
        if (block) for (size_t i = 0; i < big; i += 4096) block[i] = char(i >> 12);
        const std::wstring path = pre + L"crash-20261115-120000.dmp";
        DeleteFileW(path.c_str());
        g.fill(path, &ep, MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemory);
        SetEvent(g.req);
        CHECK(WaitForSingleObject(g.ack, 3000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(g.done, 60000) == WAIT_OBJECT_0);
        CHECK(g.info->result == ipc::kResultWritten);
        CHECK(st && st->state == CrashDumpStatus::kWritten);
        char head[4] = {};
        const long long size = file_size(path, head);
        CHECK(std::memcmp(head, "MDMP", 4) == 0);
        CHECK(size > 1000);
        // a full-memory dump would hold the 128 MB block below; masked to the game's flags it is a small one
        CHECK(size < 64LL * 1024 * 1024);
        std::printf("minidump %lld bytes\n", size);
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        DeleteFileW(path.c_str());
    }
    std::printf(g_fail ? "crash_dump_test: %d FAILED\n" : "crash_dump_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
