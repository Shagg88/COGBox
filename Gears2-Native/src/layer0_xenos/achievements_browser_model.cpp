// Layer 0 (game-free): the achievements browser's model - see xenos/achievements_browser_model.h. Our code (MIT).
#include <xenos/achievements_browser_model.h>
#include <xenos/achievements_popup_logic.h>

#include <cstdio>
#include <map>
#include <utility>

namespace xenos::ach {

namespace {
const char* const kDot = "\xC2\xB7";  // U+00B7 MIDDLE DOT

std::string Line(const Totals& t) {
  return std::to_string(t.unlocked) + " / " + std::to_string(t.total) + " " + kDot + " " + std::to_string(t.gp_earned) +
         " / " + std::to_string(t.gp_total) + " G";
}

BrowserEntry GameEntry(const std::vector<GameAchievements>& games, int i, bool this_game) {
  BrowserEntry e;
  e.game = i;
  e.this_game = this_game;
  e.name = kGames[size_t(i)].name;
  if (size_t(i) < games.size() && games[size_t(i)].loaded) {
    e.loaded = true;
    e.totals = games[size_t(i)].totals;
    e.line = Line(e.totals);
  } else {
    e.line = "Not played yet";
  }
  return e;
}

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm).
void CivilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = int(int64_t(yoe) + era * 400 + (*m <= 2));
}

constexpr uint64_t kUnixEpochFiletime = 116444736000000000ull;
int64_t UnixSeconds(uint64_t filetime) { return int64_t((filetime - kUnixEpochFiletime) / 10'000'000ull); }
}  // namespace

SortOrder NextSort(SortOrder s) {
  switch (s) {
    case SortOrder::kUnlockTime: return SortOrder::kRarity;
    case SortOrder::kRarity: return SortOrder::kGameOrder;
    case SortOrder::kGameOrder: return SortOrder::kGamerscore;
    case SortOrder::kGamerscore: return SortOrder::kName;
    default: return SortOrder::kUnlockTime;
  }
}

ShowFilter NextShow(ShowFilter f) {
  switch (f) {
    case ShowFilter::kAll: return ShowFilter::kUnlocked;
    case ShowFilter::kUnlocked: return ShowFilter::kLocked;
    default: return ShowFilter::kAll;
  }
}

const char* SortName(SortOrder s) {
  switch (s) {
    case SortOrder::kRarity: return "Rarity";
    case SortOrder::kGameOrder: return "Game order";
    case SortOrder::kGamerscore: return "Gamerscore";
    case SortOrder::kName: return "Name (A-Z)";
    default: return "Unlock time";
  }
}

const char* ShowName(ShowFilter f) {
  switch (f) {
    case ShowFilter::kUnlocked: return "Unlocked";
    case ShowFilter::kLocked: return "Locked";
    default: return "All";
  }
}

const char* GameLabel(uint32_t title_id) {
  switch (title_id) {
    case 0x4D5307D5: return "Gears 1";
    case 0x4D53082D: return "Gears 2";
    case 0x4D5308AB: return "Gears 3";
    case 0x4D530A26: return "Judgment";
    default: return "";
  }
}

std::vector<BrowserEntry> BuildSidebar(const std::vector<GameAchievements>& games, uint32_t running_title) {
  std::vector<BrowserEntry> out;
  int running = -1;
  for (size_t i = 0; i < kGames.size(); ++i)
    if (kGames[i].title_id == running_title) running = int(i);
  if (running >= 0) out.push_back(GameEntry(games, running, true));
  for (int i = 0; i < int(kGames.size()); ++i)
    if (i != running) out.push_back(GameEntry(games, i, false));
  BrowserEntry all;
  all.all = true;
  all.name = "All";
  all.totals = SumTotals(games);
  for (const auto& g : games) all.loaded = all.loaded || g.loaded;
  all.line = Line(all.totals);
  out.push_back(std::move(all));
  return out;
}

BrowserHeader HeaderFor(const BrowserEntry& e) {
  BrowserHeader h;
  h.title = e.all ? "All played games" : e.name;
  h.percent = PercentByGp(e.totals);
  h.counts = std::to_string(e.totals.unlocked) + " / " + std::to_string(e.totals.total) + " achievements " + kDot + " " +
             std::to_string(e.totals.gp_earned) + " / " + std::to_string(e.totals.gp_total) + " G " + kDot + " " +
             std::to_string(h.percent) + "%";
  return h;
}

std::string FormatUnlockTime(uint64_t filetime, int utc_offset_minutes) {
  if (filetime < kUnixEpochFiletime) return {};  // 0 (unknown) or before 1970
  const int64_t local = UnixSeconds(filetime) + int64_t(utc_offset_minutes) * 60;
  int64_t days = local / 86400, secs = local % 86400;
  if (secs < 0) {
    secs += 86400;
    --days;
  }
  int y = 0;
  unsigned m = 0, d = 0;
  CivilFromDays(days, &y, &m, &d);
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[48];
  std::snprintf(buf, sizeof buf, "%u %s %d, %02d:%02d", d, kMonths[m - 1], y, int(secs / 3600), int(secs / 60 % 60));
  return buf;
}

std::vector<BrowserRow> BuildRows(const std::vector<GameAchievements>& games, const BrowserEntry& e, SortOrder s,
                                  ShowFilter f, const std::function<int(int64_t)>& utc_offset_minutes) {
  std::vector<Achievement> list;
  std::map<std::pair<uint32_t, uint32_t>, const Achievement*> where;
  for (size_t i = 0; i < games.size(); ++i) {
    if (!games[i].loaded || (!e.all && int(i) != e.game)) continue;
    for (const Achievement& a : games[i].achievements) {
      if (f == ShowFilter::kUnlocked && !a.unlocked) continue;
      if (f == ShowFilter::kLocked && a.unlocked) continue;
      list.push_back(a);
      where[{a.title_id, a.id}] = &a;
    }
  }
  SortAchievements(list, s);

  std::vector<BrowserRow> rows;
  rows.reserve(list.size());
  for (const Achievement& a : list) {
    BrowserRow r;
    r.a = where[{a.title_id, a.id}];
    r.locked = !a.unlocked;
    r.hidden_secret = a.secret && !a.unlocked && !a.revealed;
    r.secret_tag = a.secret && !r.hidden_secret;
    r.gp = std::to_string(a.gamerscore) + " G";
    if (e.all) r.game_label = GameLabel(a.title_id);
    if (r.hidden_secret) {
      r.name = "Secret achievement";
      r.text = "Select to reveal";
    } else {
      r.name = a.label;
      r.text = a.unlocked || a.unachieved.empty() ? a.description : a.unachieved;
    }
    if (a.unlocked) {
      const int offset = a.unlocked_at >= kUnixEpochFiletime && utc_offset_minutes
                             ? utc_offset_minutes(UnixSeconds(a.unlocked_at))
                             : 0;
      const std::string when = FormatUnlockTime(a.unlocked_at, offset);
      r.status = when.empty() ? "Unlocked" : "Unlocked " + when;
    } else {
      r.status = "Locked";
    }
    if (IsRare(a.rarity)) {
      char buf[40];
      std::snprintf(buf, sizeof buf, "Rare %s %.1f%%", kDot, double(a.rarity.percent));
      r.rarity = buf;
    }
    rows.push_back(std::move(r));
  }
  return rows;
}

std::string EmptyMessage(const BrowserEntry& e, ShowFilter f) {
  if (!e.loaded) {
    return e.all ? "Start a game once from COGBox to load its achievements."
                 : "Start " + e.name + " once from COGBox to load its achievements.";
  }
  if (f == ShowFilter::kUnlocked) return "No unlocked achievements yet";
  if (f == ShowFilter::kLocked) return "Every achievement is unlocked!";
  return {};
}

const char* HintLine(bool ps_prompts) {
  return ps_prompts ? "L1 / R1: game   Cross: select   Square: sort   Triangle: show   Circle: back"
                    : "LB / RB: game   A: select   X: sort   Y: show   B: back";
}

}  // namespace xenos::ach
