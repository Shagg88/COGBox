// COGBox social step 5: the Guide / PS button opens the Social page once per press (held = one press).
#pragma once

namespace xenos {

struct GuideEdge {
  bool down = false;
  // true on the read where the button goes down
  bool Pressed(bool now) {
    const bool edge = now && !down;
    down = now;
    return edge;
  }
};

}  // namespace xenos
