// COGBox social step 5: the in-game social worker only exists when the game runs online.
#include <catch2/catch_test_macros.hpp>

#include <xenos/social_ingame.h>

TEST_CASE("in-game social: nothing without a configured master server", "[social]") {
  CHECK_FALSE(XenosSocialAvailable());
  XenosSocialStart();
  CHECK_FALSE(XenosSocialView().configured);
  CHECK(XenosSocialTakeEvents().empty());
}

#include <xenos/social_page.h>

TEST_CASE("Social page: B while typing stays on the page", "[social]") {
  xenos::social::OnLeave();                              // keyboard closed
  CHECK(xenos::social::ConsumeBack(true));               // typing in the text box: B / Esc only ends typing
  CHECK_FALSE(xenos::social::ConsumeBack(false));        // nothing active: back to the settings page
}
