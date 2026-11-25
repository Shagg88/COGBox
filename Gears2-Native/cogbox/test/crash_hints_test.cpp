// g++ -std=c++17 -Isrc test/crash_hints_test.cpp -o crash_hints_test.exe && crash_hints_test.exe
#include "crash_hints.h"
#include <cstdio>
#include <fstream>
#include <sstream>

static int fails = 0;
static void check(bool ok, const char *what) { if (!ok) { std::printf("FAIL: %s\n", what); fails++; } }

int main(int argc, char **argv)
{
    using crash_hints::log_out_of_memory;
    // GitHub #456's log (the lines that matter, paths left out)
    const std::string g456 =
        "[warning] [core] gow2_renderer: CreateHeap(textures) failed (0x8007000E) - placed textures off from here on\n"
        "[info] [core] achievements: 79 written to <path>/catalog.txt\n"
        "[error] [core] Win32 Error 0x000005AF in rex::thread::Thread::Create(...)\n"
        "[error] [sys] CreateThread failed\n"
        "[error] [sys] Could not create launch thread: C0000017\n";
    check(log_out_of_memory(g456) == 2, "#456 log is a hard out-of-memory");
    check(log_out_of_memory("[warning] gow2_renderer: CreateHeap(textures) failed (0x8007000E) - placed textures off\n") == 1,
          "a failed allocation alone is the weak kind");
    check(log_out_of_memory("[info] texture 8007000E0 cached ok\n") == 0, "the number without a failure isn't counted");
    check(log_out_of_memory("[warning] XEX patch signature hash doesn't match\n[info] fine\n") == 0, "a normal log isn't");
    check(log_out_of_memory("") == 0, "empty log");
    check(log_out_of_memory("8007000E failed") == 0, "'failed' after the number on the same line isn't the pattern");
    check(log_out_of_memory("x\nD3D12 Failed: 0x8007000E") == 1, "Failed before the number");
    if (argc > 1) {   // a real log file: print its reading
        std::ifstream f(argv[1], std::ios::binary); std::stringstream ss; ss << f.rdbuf();
        std::printf("%s: %d\n", argv[1], log_out_of_memory(ss.str()));
    }
    std::printf(fails ? "%d failed\n" : "crash_hints: all ok\n", fails);
    return fails ? 1 : 0;
}
