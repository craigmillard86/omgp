// UTF-8 well-formedness check for descriptor strings. protocol-l3 §4: "Strings UTF-8,
// not NUL-terminated (length-delimited)". Table-free, no allocation; rejects overlong
// forms, surrogates (U+D800-DFFF) and code points above U+10FFFF (RFC 3629 §4), so it
// agrees with Python's strict bytes.decode("utf-8") used by the reference implementation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace omgp {
namespace l3 {

inline bool utf8_valid(const uint8_t* s, size_t n) {
    // Byte-class boundaries from RFC 3629 §4 — not protocol values.
    constexpr uint8_t kAscii = 0x80;      // literal-ok: UTF-8 byte class boundary (RFC 3629)
    constexpr uint8_t kContLo = 0x80;     // literal-ok: UTF-8 continuation byte range
    constexpr uint8_t kContHi = 0xBF;     // literal-ok: UTF-8 continuation byte range
    constexpr uint8_t kLead2Lo = 0xC2;    // literal-ok: UTF-8 lead byte range (RFC 3629)
    constexpr uint8_t kLead2Hi = 0xDF;    // literal-ok: UTF-8 lead byte range (RFC 3629)
    constexpr uint8_t kLeadE0 = 0xE0;     // literal-ok: UTF-8 lead byte (RFC 3629)
    constexpr uint8_t kE0SecondLo = 0xA0; // literal-ok: UTF-8 second-byte bound (RFC 3629)
    constexpr uint8_t kLead3Hi = 0xEC;    // literal-ok: UTF-8 lead byte range (RFC 3629)
    constexpr uint8_t kLeadED = 0xED;     // literal-ok: UTF-8 lead byte (RFC 3629)
    constexpr uint8_t kEDSecondHi = 0x9F; // literal-ok: UTF-8 second-byte bound (RFC 3629)
    constexpr uint8_t kLead3bHi = 0xEF;   // literal-ok: UTF-8 lead byte range (RFC 3629)
    constexpr uint8_t kLeadF0 =
        0xF0; // literal-ok: UTF-8 lead byte (RFC 3629), not EVT_USER_DEFINED_MIN
    constexpr uint8_t kF0SecondLo = 0x90; // literal-ok: UTF-8 second-byte bound (RFC 3629)
    constexpr uint8_t kLead4Hi = 0xF3;    // literal-ok: UTF-8 lead byte range (RFC 3629)
    constexpr uint8_t kLeadF4 = 0xF4;     // literal-ok: UTF-8 lead byte (RFC 3629)
    constexpr uint8_t kF4SecondHi = 0x8F; // literal-ok: UTF-8 second-byte bound (RFC 3629)

    size_t i = 0;
    while (i < n) {
        const uint8_t c = s[i];
        if (c < kAscii) {
            ++i;
            continue;
        }
        size_t len;
        uint8_t lo = kContLo, hi = kContHi;
        if (c >= kLead2Lo && c <= kLead2Hi) {
            len = 2;
        } else if (c == kLeadE0) {
            len = 3;
            lo = kE0SecondLo;
            // mutant-ok(equivalent, cxx_gt_to_ge): c == kLeadE0 is taken by the branch above
        } else if (c > kLeadE0 && c <= kLead3Hi) {
            len = 3;
        } else if (c == kLeadED) {
            len = 3;
            hi = kEDSecondHi;
            // mutant-ok(equivalent, cxx_gt_to_ge): c == kLeadED is taken by the branch above
        } else if (c > kLeadED && c <= kLead3bHi) {
            len = 3;
        } else if (c == kLeadF0) {
            len = 4;
            lo = kF0SecondLo;
            // mutant-ok(equivalent, cxx_gt_to_ge): c == kLeadF0 is taken by the branch above
        } else if (c > kLeadF0 && c <= kLead4Hi) {
            len = 4;
        } else if (c == kLeadF4) {
            len = 4;
            hi = kF4SecondHi;
        } else {
            return false; // 0x80-0xC1 (stray continuation / overlong 2-byte) or 0xF5+
        }
        if (n - i < len)
            return false;
        if (s[i + 1] < lo || s[i + 1] > hi)
            return false;
        for (size_t k = 2; k < len; ++k)
            if (s[i + k] < kContLo || s[i + k] > kContHi)
                return false;
        i += len;
    }
    return true;
}

} // namespace l3
} // namespace omgp
