#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/preparation_progress.h>

using xenos::preparation::Progress;

TEST_CASE("Shader preparation reports partial coverage without claiming completion", "[preparation]") {
  Progress p;
  p.state = "partial";
  p.seconds = 42;
  p.files = p.files_total = 120;
  p.files_failed = 2;
  p.variants = p.variants_total = 15000;
  p.variants_failed = 4;
  p.pipelines = 5900;
  p.pipelines_total = 6000;
  p.pipelines_missing = 90;
  p.pipelines_failed = 10;
  Progress read;
  REQUIRE(Progress::Parse(p.Serialize(), read));
  CHECK(read.Finished());
  CHECK(read.state == "partial");
  CHECK(read.pipelines == 5900);
  CHECK(read.pipelines_missing == 90);
  CHECK(read.variants_failed == 4);
  CHECK(read.Serialize() == p.Serialize());
}

TEST_CASE("A truncated or invalid preparation update leaves the previous display intact", "[preparation]") {
  Progress previous;
  previous.files = previous.files_total = 7;
  const auto original = previous.Serialize();
  for (const auto& bad : {
           "COGBOX_PREPARE 2 reading 1 2", "COGBOX_PREPARE 3 done 0 0 0 0 0 0 0 0 0 0 0",
           "COGBOX_PREPARE 2 done -1 0 0 0 0 0 0 0 0 0 0",
           "COGBOX_PREPARE 2 done 0 2 1 0 0 0 0 0 0 0 0",
           "COGBOX_PREPARE 2 done 0 0 0 0 0 0 0 2 1 0 0",
           "COGBOX_PREPARE 2 unknown 0 0 0 0 0 0 0 0 0 0 0",
           "COGBOX_PREPARE 2 done 18446744073709551616 0 0 0 0 0 0 0 0 0 0"}) {
    CHECK_FALSE(Progress::Parse(bad, previous));
    CHECK(previous.Serialize() == original);
  }
}

TEST_CASE("Stopping preparation is distinct from ready and from a failed worker", "[preparation]") {
  Progress p;
  p.state = "saving";
  CHECK_FALSE(p.Finished());
  p.state = "cancelled";
  CHECK(p.Finished());
  Progress read;
  REQUIRE(Progress::Parse(p.Serialize(), read));
  CHECK(read.state == "cancelled");
}
