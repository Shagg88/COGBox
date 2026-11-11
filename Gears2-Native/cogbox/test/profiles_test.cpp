// CogBox - test of profiles: a profile's online account folder. Prints FAIL lines; exit code = failures.
//   clang++ -std=c++17 -Isrc test/profiles_test.cpp src/profiles.cpp src/util.cpp -lole32 -lshell32 -luuid -o profiles_test.exe
#include "../src/profiles.h"

#include <cstdio>

int main() {
    if (profile_dir("C:\\CB", "1") != "C:\\CB\\profiles\\1" || profile_dir("C:\\CB", "2") != "C:\\CB\\profiles\\2") {
        std::printf("FAIL profile_dir\n");
        return 1;
    }
    return 0;
}
