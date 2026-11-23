// Layer 0 (game-free): the in-match chat's logic (xenos/chat_logic.h). Our code (MIT).
#include <xenos/chat_logic.h>

#include <algorithm>

namespace xenos::chat {

std::string CleanText(std::string_view typed) {
  std::string out;
  out.reserve(typed.size());
  for (const char ch : typed) {
    const unsigned char c = static_cast<unsigned char>(ch);
    // ';' and '|' split a console line into more commands; '"' would end the string argument
    // ('~' is the team marker's)
    if (c >= 0x20 && c <= 0x7E && c != ';' && c != '|' && c != '~') out.push_back(c == '"' ? '\'' : ch);
  }
  const size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) return std::string();
  out = out.substr(first, out.find_last_not_of(' ') - first + 1);
  if (out.size() > kMaxChars) {
    out.resize(kMaxChars);
    out.erase(out.find_last_not_of(' ') + 1);
  }
  return out;
}

namespace {
constexpr std::string_view kTeamMarker = "~T ";
}

std::string MarkTeam(std::string_view text) { return std::string(kTeamMarker) + std::string(text); }

bool TakeTeamMarker(std::string* text) {
  if (text->compare(0, kTeamMarker.size(), kTeamMarker) != 0) return false;
  text->erase(0, kTeamMarker.size());
  return true;
}

Line ParseReceived(std::string_view raw, double now, size_t name_len) {
  Line line;
  line.at = now;
  const size_t colon = (name_len && raw.compare(name_len, 2, ": ") == 0) ? name_len : raw.find(": ");
  if (colon == std::string_view::npos || colon == 0) {
    line.text = std::string(raw);
  } else {
    line.who = std::string(raw.substr(0, colon));
    line.text = std::string(raw.substr(colon + 2));
  }
  line.team = TakeTeamMarker(&line.text);
  if (line.text.size() > kMaxReceived) line.text.resize(kMaxReceived);
  return line;
}

void Log::Add(Line line) {
  lines_.push_back(std::move(line));
  if (lines_.size() > size_t(kMaxLines)) lines_.erase(lines_.begin(), lines_.end() - kMaxLines);
}

std::vector<std::pair<Line, float>> Log::Visible(double now, bool typing) {
  if (typing) {
    for (Line& l : lines_) {
      const double age = now - l.at;
      if (age > kFadeAfter - 0.01) l.at = now - (kFadeAfter - 0.01);   // held just before its fade starts
    }
  }
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [now](const Line& l) { return now - l.at >= kFadeAfter + kFadeTime; }),
               lines_.end());
  std::vector<std::pair<Line, float>> out;
  out.reserve(lines_.size());
  for (const Line& l : lines_) {
    const double age = now - l.at;
    const double alpha = age <= kFadeAfter ? 1.0 : 1.0 - (age - kFadeAfter) / kFadeTime;
    out.emplace_back(l, float(std::clamp(alpha, 0.0, 1.0)));
  }
  return out;
}

void Log::Clear() { lines_.clear(); }

std::vector<std::string> Wrap(std::string_view text, float width,
                              const std::function<float(std::string_view)>& measure) {
  std::vector<std::string> rows;
  std::string row;
  size_t i = 0;
  bool cut = false;
  auto push = [&](std::string r) {
    if (int(rows.size()) == kMaxWrap) {
      cut = true;
      return;
    }
    rows.push_back(std::move(r));
  };
  while (i < text.size() && !cut) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t end = text.find(' ', i);
    if (end == std::string_view::npos) end = text.size();
    std::string word(text.substr(i, end - i));
    i = end;
    if (word.empty()) break;
    const std::string joined = row.empty() ? word : row + " " + word;
    if (measure(joined) <= width) {
      row = joined;
      continue;
    }
    if (!row.empty()) {
      push(row);
      row.clear();
      if (cut) break;
    }
    // the word alone: split it while it is too long for a row
    while (!word.empty() && measure(word) > width) {
      size_t n = word.size();
      while (n > 1 && measure(std::string_view(word).substr(0, n)) > width) --n;
      push(word.substr(0, n));
      if (cut) break;
      word.erase(0, n);
    }
    row = word;
  }
  if (!cut && !row.empty()) push(row);
  if (cut && !rows.empty()) {
    std::string& last = rows.back();
    while (!last.empty() && measure(last + "...") > width) last.pop_back();
    last += "...";
  }
  return rows;
}

}  // namespace xenos::chat
