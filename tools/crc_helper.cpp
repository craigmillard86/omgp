// Reads stdin bytes, prints CRC-16/CCITT-FALSE in hex. Used by diffcheck.py.
#include "../link/crc16.hpp"
#include <cstdio>
#include <vector>
int main() {
    std::vector<uint8_t> buf;
    int c;
    while ((c = std::getc(stdin)) != EOF)
        buf.push_back(static_cast<uint8_t>(c));
    std::printf("0x%04X\n", omgp::crc16_ccitt_false(buf.data(), buf.size()));
    return 0;
}
