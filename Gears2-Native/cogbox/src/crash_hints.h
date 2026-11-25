// Pure helpers for the launcher's reading of a game that closed (play.cpp), kept here so test/crash_hints_test.cpp
// can check them without the launcher.
#pragma once
#include <string>

namespace crash_hints {

// GitHub #456: the game's log says Windows had no memory left to give it - a thread or a memory block it asked for
// couldn't be made. Win32 0x5AF is ERROR_COMMITMENT_LIMIT (the page file is too small, or full), C0000017 is
// STATUS_NO_MEMORY, 0x8007000E is E_OUTOFMEMORY. This is what made the game stop, so the launcher says it instead of
// a guess drawn from a harmless earlier warning (the title update's signature line).
// Returns 2 when the log names a hard out-of-memory failure (a thread that couldn't be made, a commit limit), 1 when
// it only names a failed allocation that the game may have carried on from ("CreateHeap(textures) failed
// (0x8007000E) - placed textures off"), 0 otherwise. The caller counts 1 only when the game stopped itself (0xC0000409)
// or ran out of memory by its exit code, so a later unrelated crash isn't blamed on it.
inline int log_out_of_memory(const std::string &log)
{
    for (const char *s : { "Win32 Error 0x000005AF", "Could not create launch thread: C0000017", "STATUS_NO_MEMORY",
                           "ERROR_COMMITMENT_LIMIT", "std::bad_alloc" })
        if (log.find(s) != std::string::npos) return 2;
    // "CreateHeap(textures) failed (0x8007000E)", "... failed: 0x8007000E" and the like
    for (size_t p = log.find("8007000E"); p != std::string::npos; p = log.find("8007000E", p + 8)) {
        const size_t line = log.rfind('\n', p);
        const std::string head = log.substr(line == std::string::npos ? 0 : line + 1, p - (line == std::string::npos ? 0 : line + 1));
        if (head.find("failed") != std::string::npos || head.find("Failed") != std::string::npos) return 1;
    }
    return 0;
}

// What to do about it (also said on its own when the exit code already named out of memory, so it isn't said twice)
inline const char *out_of_memory_advice()
{
    return "close other programs while you play, and let Windows manage the page file size (Settings > System > About > "
           "Advanced system settings > Performance Settings > Advanced > Virtual memory: \"Automatically manage paging file "
           "size\"), or make the page file bigger";
}

inline std::string out_of_memory_text()
{
    return std::string("Windows ran out of memory for it (its log says memory or a thread it asked for couldn't be made) - ") +
           out_of_memory_advice();
}

}   // namespace crash_hints
