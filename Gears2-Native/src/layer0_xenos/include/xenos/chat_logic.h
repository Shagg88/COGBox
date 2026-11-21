// Layer 0 (game-free): the in-match chat's logic - cleaning what the player typed, the received lines with their fade,
// and word wrap. No drawing and no engine: xenos/chat_panel.h draws it, ue3/chat.h sends and receives
// (docs/superpowers/specs/2026-11-21-match-chat-design.md). Tested by cogbox/test/chat_logic_test.cpp. Our code (MIT).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xenos::chat {

constexpr size_t kMaxChars = 150;   // a message, after cleaning
constexpr int kMaxLines = 5;        // messages kept on screen
constexpr int kMaxWrap = 3;         // rows one message may wrap to
constexpr double kFadeAfter = 8.0;  // seconds a line stays fully visible
constexpr double kFadeTime = 1.0;   // seconds it then takes to fade out

// What may be sent: printable ASCII (0x20-0x7E) without ';' and '|' (both split a console line into more commands),
// '"' turned into '\'' (it would end the string argument), trimmed, at most kMaxChars.
// "" means there is nothing to send.
std::string CleanText(std::string_view typed);

struct Line {
  std::string who, text;   // "Name#1234", the message
  bool team = false;       // drawn as a team line
  double at = 0;           // when it arrived (seconds, any clock the caller keeps using)
};

// "Name#1234: text" -> who / text. Without ": " who is empty and the whole string is the text.
Line ParseReceived(std::string_view raw, double now);

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
