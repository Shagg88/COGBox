// COGBox - problem reports carry no other player's address and no online token (Shagg's 0.6.0 review, point 9).
#pragma once
#include <string>

// Public IPv4 addresses become a.x.x.d (private, loopback, link-local and CGNAT ones stay as they are), and every run
// of exactly 64 hex digits (the online account's token) becomes <token>.
std::string scrub_network(const std::string &text);
