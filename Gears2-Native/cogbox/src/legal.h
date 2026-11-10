// COGBox 0.5.0 (legal050) - the "Welcome to COGBox" notice: who is responsible for what, in a few friendly lines.
// Shown on the first start and after each COGBox update, until the player ticks "Don't show this again"; a new
// kLegalNoticeVersion (the text changed) shows it again even then. Accept / Read more / Decline (Decline closes COGBox).
// CogBox.ini [Notices]: LegalAccepted = <notice version>, LegalAcceptedOn = <date>, LegalNever = <notice version>,
// LegalSeen = <COGBox version>.
// The full text is LEGAL.md beside CogBox.exe (from the repository root); PRIVACY.md says what COGBox sends.
#pragma once
#include <functional>
#include <string>
struct App;

static const char *const kLegalNoticeVersion = "4";   // raise this whenever the notice's wording changes (2: legal2-050, no warranty line, new affiliation line; 3, 4: 0.5.0 wording reviews)

// The rule, kept pure for test/legal_test.cpp: accepted = [Notices] LegalAccepted, never = LegalNever, seen = LegalSeen.
inline bool legal_due(const std::string &accepted, const std::string &never, const std::string &seen,
                      const std::string &notice, const std::string &version)
{
    if (accepted != notice) return true;     // never accepted, or the text changed since
    if (never == notice) return false;       // "Don't show this again" ticked for this text
    return seen != version;                  // otherwise once after each COGBox update
}
bool legal_notice_due(const App &a);   // true while the start-up notice still has to be shown/accepted
void legal_notice_ask(App &a);         // opens it (startup_notices calls this first)
