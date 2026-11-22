// CogBox - test of the in-match chat's game-free logic (../src/layer0_xenos/chat_logic.cpp). Plain checks, like
// achievements_tab_test. Build and run from Gears2-Native/cogbox (clang++ from LLVM):
//   clang++ -std=c++20 -I../src/layer0_xenos/include test/chat_logic_test.cpp ../src/layer0_xenos/chat_logic.cpp
//     -o out/chat_logic_test.exe && out\chat_logic_test.exe
#include <xenos/chat_logic.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {
int g_failed = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failed;                                                      \
    }                                                                  \
  } while (0)

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

void CleanTextTests() {
  using xenos::chat::CleanText;
  CHECK(CleanText("  hi  ") == "hi");
  CHECK(CleanText("a;b") == "ab");
  CHECK(CleanText("\x01tab\there") == "tabhere");
  CHECK(CleanText("caf\xC3\xA9") == "caf");
  CHECK(CleanText("   ").empty());
  CHECK(CleanText(std::string(200, 'x')).size() == 125);   // the marker (3) + 125 fit UE3 Say's Left(Msg, 128)
  // '"' would end (or start) the console's string argument: it becomes '\''
  CHECK(CleanText("\"quoted\" 100% -x") == "'quoted' 100% -x");
  CHECK(CleanText("hi; quit") == "hi quit");
  CHECK(CleanText("hi | quit") == "hi  quit");   // UE3's Exec runs each '|' part as its own command
  CHECK(CleanText("|") .empty());
  CHECK(CleanText("~T fake team") == "T fake team");   // '~' is the team marker's: it can't be typed
}

void TeamMarkerTests() {
  using namespace xenos::chat;
  CHECK(MarkTeam("go left") == "~T go left");
  std::string t = "~T go left";
  CHECK(TakeTeamMarker(&t) && t == "go left");
  t = "go left";
  CHECK(!TakeTeamMarker(&t) && t == "go left");
  const auto line = ParseReceived("Baird#7310: ~T flank", 2);
  CHECK(line.who == "Baird#7310" && line.text == "flank" && line.team);
  CHECK(!ParseReceived("Baird#7310: flank", 2).team);
  // a name with ": " in it: the caller gives the name's length
  const auto odd = ParseReceived("Mr: X#0001: ~T hi", 2, 10);
  CHECK(odd.who == "Mr: X#0001" && odd.text == "hi" && odd.team);
  // a received text longer than UE3's 128 is cut (after the team marker comes off)
  const auto longer = ParseReceived("Cole#1: ~T " + std::string(400, 'x'), 2);
  CHECK(longer.text.size() == kMaxReceived && longer.team);
}

void ParseTests() {
  using xenos::chat::ParseReceived;
  const auto a = ParseReceived("Flapper#5531: gg ez", 1);
  CHECK(a.who == "Flapper#5531");
  CHECK(a.text == "gg ez");
  CHECK(a.at == 1);
  const auto b = ParseReceived("no colon here", 1);
  CHECK(b.who.empty());
  CHECK(b.text == "no colon here");
}

void LogTests() {
  xenos::chat::Log log;
  for (int i = 1; i <= 7; ++i) log.Add({"P#0001", "line " + std::to_string(i), false, 0});
  auto v = log.Visible(0, false);
  CHECK(v.size() == 5);
  CHECK(!v.empty() && v.front().first.text == "line 3");
  CHECK(!v.empty() && v.back().first.text == "line 7");
  v = log.Visible(8.5, false);
  CHECK(v.size() == 5 && Near(v.front().second, 0.5f));
  CHECK(log.Visible(9.1, false).empty());

  xenos::chat::Log typing;
  for (int i = 0; i < 5; ++i) typing.Add({"P#0001", "t" + std::to_string(i), false, 0});
  v = typing.Visible(20, true);
  CHECK(v.size() == 5);
  for (const auto& [line, alpha] : v) CHECK(Near(alpha, 1.0f));
  // typing stopped at 20: the lines were held just before their fade, so they fade from about 20 on, gone by ~21
  v = typing.Visible(20.5, false);
  CHECK(v.size() == 5 && v.front().second < 1.0f && v.front().second > 0.0f);
  CHECK(typing.Visible(21.2, false).empty());

  typing.Clear();
  CHECK(typing.Visible(0, false).empty());
}

void WrapTests() {
  using xenos::chat::Wrap;
  const auto seven = [](std::string_view s) { return 7.0f * float(s.size()); };
  const auto rows = Wrap(std::string(150, 'x'), 420, seven);
  CHECK(rows.size() == 3);
  for (const auto& r : rows) CHECK(r.size() <= 60);
  CHECK(Wrap("aaa bbb", 420, seven).size() == 1);
  const auto words = Wrap("aaaa bbbb cccc", 56, seven);   // 8 chars per row: one word each
  CHECK(words.size() == 3 && words[0] == "aaaa" && words[1] == "bbbb");
  CHECK(Wrap("", 420, seven).empty());
}
}  // namespace

int main() {
  CleanTextTests();
  TeamMarkerTests();
  ParseTests();
  LogTests();
  WrapTests();
  if (g_failed) {
    std::printf("chat_logic_test: %d failed\n", g_failed);
    return 1;
  }
  std::printf("chat_logic_test: all passed\n");
  return 0;
}
