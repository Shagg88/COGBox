// Unit tests for xenos/player_tags.h (GitHub #353): the COGBox tags of the players in a match, fetched in batches.
// Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include "xenos/player_tags.h"

using xenos::tags::Cache;
using xenos::tags::Tag;

TEST_CASE("player tags: unknown XUIDs are asked once, in one batch, and answered from the cache", "[tags]") {
  Cache c;
  CHECK(!c.Want(1001, 0));
  CHECK(!c.Want(1002, 0));
  CHECK(!c.Want(1001, 0));  // asked again before the answer: still one entry
  const auto batch = c.TakeBatch(0);
  CHECK(batch == std::vector<uint64_t>{1001, 1002});
  CHECK(c.TakeBatch(0).empty());  // in flight: not asked twice
  c.Answer(batch, {{1001, Tag{"Marcus", "0042"}}}, 10);
  const auto t = c.Want(1001, 20);
  REQUIRE(t);
  CHECK(t->name == "Marcus");
  CHECK(t->discriminator == "0042");
  CHECK(!c.Want(1002, 20));                 // no COGBox account (a bot, a guest): not a tag
  CHECK(c.TakeBatch(20).empty());           // ... and not asked again soon
  CHECK(c.TakeBatch(20 + Cache::kNoAccountRetryMs).size() == 1);  // but again after a while (an account made since)
}

TEST_CASE("player tags: a failed request is retried later; XUID 0 (no online id) is never asked", "[tags]") {
  Cache c;
  CHECK(!c.Want(0, 0));
  CHECK(!c.Want(7, 0));
  const auto batch = c.TakeBatch(0);
  CHECK(batch == std::vector<uint64_t>{7});
  c.Failed(batch, 100);
  CHECK(c.TakeBatch(100).empty());
  CHECK(c.TakeBatch(100 + Cache::kFailedRetryMs) == std::vector<uint64_t>{7});
}

TEST_CASE("player tags: one batch holds at most the server's limit", "[tags]") {
  Cache c;
  for (uint64_t x = 1; x <= Cache::kBatch + 3; ++x) c.Want(x, 0);
  CHECK(c.TakeBatch(0).size() == Cache::kBatch);
  CHECK(c.TakeBatch(0).size() == 3);
}

// Recent players (0.6.5): the COGBox players met in a match are reported to the server, each at most once per interval.
using xenos::tags::Met;

TEST_CASE("players met: each player is reported once, again only after the interval", "[tags]") {
  Met m;
  m.Seen(11, 0);
  m.Seen(12, 0);
  m.Seen(11, 5);
  CHECK(m.TakeReport(10) == std::vector<uint64_t>{11, 12});
  CHECK(m.TakeReport(20).empty());
  m.Seen(11, 30);  // still in the match
  CHECK(m.TakeReport(30).empty());
  m.Seen(11, Met::kReportEveryMs + 40);
  CHECK(m.TakeReport(Met::kReportEveryMs + 40) == std::vector<uint64_t>{11});
}

TEST_CASE("players met: a failed report is sent again; one report holds at most the server's limit", "[tags]") {
  Met m;
  for (uint64_t x = 1; x <= Met::kBatch + 2; ++x) m.Seen(x, 0);
  const auto first = m.TakeReport(0);
  CHECK(first.size() == Met::kBatch);
  m.Failed(first, 0);
  CHECK(m.TakeReport(0).size() == 2);                        // the rest, now
  CHECK(m.TakeReport(0).empty());                            // the failed ones wait
  CHECK(m.TakeReport(Cache::kFailedRetryMs).size() == Met::kBatch);
}

// Kill feed: only the tag's own markup comes out of a name (before + "#" + digits + after); the tag itself stays.
TEST_CASE("kill feed: the tag's markup is removed, the tag kept", "[tags]") {
  using xenos::tags::StripMarkup;
  std::string n = "Flapper<font color='#808080'>#5531</font>";
  CHECK(StripMarkup(&n, "<font color='#808080'>", "</font>"));
  CHECK(n == "Flapper#5531");
  n = "Flapper<Color:R=0.5,G=0.5,B=0.5,A=1.0>#5531<Color:/>";
  CHECK(StripMarkup(&n, "<Color:R=0.5,G=0.5,B=0.5,A=1.0>", "<Color:/>"));
  CHECK(n == "Flapper#5531");
  n = "A<b>#1</b> killed C<b>#2</b>";
  CHECK(StripMarkup(&n, "<b>", "</b>"));
  CHECK(n == "A#1 killed C#2");  // every one
}

TEST_CASE("kill feed: nothing else is touched", "[tags]") {
  using xenos::tags::StripMarkup;
  std::string n = "Flapper#5531";
  CHECK(!StripMarkup(&n, "<font color='#808080'>", "</font>"));  // no markup
  CHECK(n == "Flapper#5531");
  n = "[Clan] Marcus[#0042]";  // a custom "[|]" style: only the tag's brackets go
  CHECK(StripMarkup(&n, "[", "]"));
  CHECK(n == "[Clan] Marcus#0042");
  n = "<b>bold</b> and <b>#x</b>";  // no digits: not a tag
  CHECK(!StripMarkup(&n, "<b>", "</b>"));
  n = "Dom";
  CHECK(!StripMarkup(&n, "", ""));  // no markup in use
}

TEST_CASE("kill feed: a tag's markup with no digits, or a doubled closing tag", "[tags]") {
  using xenos::tags::StripMarkup;
  std::string n = "Dom<b>#</b>";  // "#" with no digits: not a tag, left alone
  CHECK(!StripMarkup(&n, "<b>", "</b>"));
  CHECK(n == "Dom<b>#</b>");
  n = "Dom<b>#12</b></b>";  // only the tag's own closing tag goes
  CHECK(StripMarkup(&n, "<b>", "</b>"));
  CHECK(n == "Dom#12</b>");
}
