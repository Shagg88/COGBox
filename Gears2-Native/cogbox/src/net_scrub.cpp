// COGBox - see net_scrub.h
#include "net_scrub.h"

#include <cctype>

namespace {
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// An IPv4 address starting at i (not part of a longer dotted or alphanumeric run): its four parts and its length.
bool read_ipv4(const std::string &s, size_t i, unsigned (&part)[4], size_t *len)
{
    if (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '.')) return false;
    size_t k = i;
    for (int n = 0; n < 4; ++n) {
        if (n && (k >= s.size() || s[k++] != '.')) return false;
        const size_t start = k;
        unsigned v = 0;
        while (k < s.size() && is_digit(s[k]) && k - start < 3) v = v * 10 + unsigned(s[k++] - '0');
        if (k == start || v > 255 || (k < s.size() && is_digit(s[k]))) return false;
        part[n] = v;
    }
    if (k < s.size() && (std::isalnum(static_cast<unsigned char>(s[k])) ||
                         (s[k] == '.' && k + 1 < s.size() && is_digit(s[k + 1]))))
        return false;
    *len = k - i;
    return true;
}

bool is_local(const unsigned (&p)[4])
{
    return p[0] == 10 || p[0] == 127 || (p[0] == 172 && p[1] >= 16 && p[1] <= 31) || (p[0] == 192 && p[1] == 168) ||
           (p[0] == 169 && p[1] == 254) || (p[0] == 100 && p[1] >= 64 && p[1] <= 127) || p[0] == 0;
}
}  // namespace

std::string scrub_network(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        // a run of exactly 64 hex digits: the account token
        if (is_hex(s[i]) && (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])))) {
            size_t k = i;
            while (k < s.size() && is_hex(s[k])) ++k;
            if (k - i == 64 && (k == s.size() || !std::isalnum(static_cast<unsigned char>(s[k])))) {
                out += "<token>";
                i = k;
                continue;
            }
        }
        unsigned p[4];
        size_t len = 0;
        if (is_digit(s[i]) && read_ipv4(s, i, p, &len)) {
            if (is_local(p)) out.append(s, i, len);
            else out += std::to_string(p[0]) + ".x.x." + std::to_string(p[3]);
            i += len;
            continue;
        }
        out += s[i++];
    }
    return out;
}
