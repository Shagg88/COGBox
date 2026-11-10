// legal050: when the welcome notice shows. g++ -std=c++17 -Wall -Wextra -I../src legal_test.cpp -o legal_test
#include "legal.h"
#include <cstdio>
static int fails = 0;
static void t(bool got, bool want, const char *what) { if (got != want) { fails++; std::printf("FAIL %s\n", what); } }
int main()
{
    t(legal_due("", "", "", "1", "0.5.0"), true, "first start");
    t(legal_due("1", "", "0.5.0", "1", "0.5.0"), false, "accepted this version");
    t(legal_due("1", "", "0.5.0", "1", "0.5.1"), true, "after an update");
    t(legal_due("1", "1", "0.5.0", "1", "0.5.1"), false, "never ticked: hidden after update");
    t(legal_due("1", "1", "0.5.0", "2", "0.5.1"), true, "text changed: shown although ticked");
    t(legal_due("2", "1", "0.5.0", "2", "0.5.0"), false, "accepted new text, old tick ignored, same version");
    t(legal_due("", "1", "", "1", "0.5.0"), true, "tick without accept never hides it");
    std::printf(fails ? "%d failed\n" : "legal_test: all passed\n", fails);
    return fails != 0;
}
