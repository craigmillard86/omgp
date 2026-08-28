// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout)
// trunk spec §4: computed over unstuffed dst..payload bytes.
#pragma once
#include <cstddef>
#include <cstdint>
namespace omgp {
inline uint16_t crc16_ccitt_false(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(d[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}
} // namespace omgp
