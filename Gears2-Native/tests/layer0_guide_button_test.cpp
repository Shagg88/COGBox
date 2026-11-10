// COGBox social step 5: the Guide / PS button opens the Social page once per press.
#include <catch2/catch_test_macros.hpp>

#include <xenos/guide_button.h>

TEST_CASE("Guide button: one press, one open", "[social]") {
  xenos::GuideEdge g;
  CHECK_FALSE(g.Pressed(false));
  CHECK(g.Pressed(true));
  CHECK_FALSE(g.Pressed(true));   // held: no repeat
  CHECK_FALSE(g.Pressed(false));
  CHECK(g.Pressed(true));         // pressed again
}
