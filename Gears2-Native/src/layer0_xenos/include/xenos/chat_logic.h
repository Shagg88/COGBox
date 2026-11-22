// Layer 0 (game-free): the in-match chat's logic - cleaning what the player typed, the received lines with their fade,
// and word wrap. No drawing and no engine: xenos/chat_panel.h draws it, ue3/chat.h sends and receives
// (PR #421). Tested by cogbox/test/chat_logic_test.cpp. Our code (MIT).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xenos::chat {

constexpr size_t kMaxChars = 125;   // a message, after cleaning: with the team marker (3) it fits the 128 characters
                                    // UE3's PlayerController.Say / TeamSay keep (Left(Msg, 128))
constexpr size_t kMaxReceived = 128;   // a received message's text is cut to this (UE3 keeps 128; another game
                                      // or a modded host could send more)
constexpr int kMaxLines = 5;        // messages kept on screen
constexpr int kMaxWrap = 3;         // rows one message may wrap to
constexpr double kFadeAfter = 8.0;  // seconds a line stays fully visible
constexpr double kFadeTime = 1.0;   // seconds it then takes to fade out

// What may be sent: printable ASCII (0x20-0x7E) without ';' and '|' (both split a console line into more commands),
// '"' turned into '\'' (it would end the string argument), trimmed, at most kMaxChars.
// "" means there is nothing to send.
std::string CleanText(std::string_view typed);

// The game's received lines don't say Say from TeamSay, so a team message carries this marker in front of its text
// ("~T go left"); every player's game takes it off and draws the line as a team line. CleanText drops '~', so it
// can't be typed.
std::string MarkTeam(std::string_view text);
bool TakeTeamMarker(std::string* text);   // true (and the marker removed) when `text` starts with it

struct Line {
  std::string who, text;   // "Name#1234", the message
  bool team = false;       // drawn as a team line
  double at = 0;           // when it arrived (seconds, any clock the caller keeps using)
};

// "Name#1234: text" -> who / text (team when the text carries the team marker, which is removed). Without ": " who is
// empty and the whole string is the text.
// `name_len`: the sender's name length when the caller knows it (a name may hold ": " itself). The text is cut to
// kMaxReceived characters (after the team marker is taken off).
Line ParseReceived(std::string_view raw, double now, size_t name_len = 0);

class Log {
 public:
  void Add(Line line);   // keeps the newest kMaxLines
  // The lines to draw at `now`, oldest first, each with its alpha (1 = solid). While `typing`, nothing fades: each line
  // is held where it is (never past the start of its fade) and carries on from there once typing ends.
  std::vector<std::pair<Line, float>> Visible(double now, bool typing);
  void Clear();

 private:
  std::vector<Line> lines_;
};

// `text` in rows that each fit `width` by `measure`: word wrap, a word too long for a row is split. At most kMaxWrap rows
// (the last one ends in "..." if text had to be cut; 150 characters at the panel's width never need that).
std::vector<std::string> Wrap(std::string_view text, float width, const std::function<float(std::string_view)>& measure);

}  // namespace xenos::chat
