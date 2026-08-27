#include "omgp_protocol.h"
#include "../../link/crc16.hpp"
#include <cstdio>
#include <cstring>
static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++fails; } }while(0)
int main() {
    // generated constants match the YAML source of truth
    CHECK(omgp::OP_PING == 0x01);
    CHECK(omgp::OP_ERROR == 0x7F);
    CHECK(omgp::ERR_BUSY == 0x04);
    CHECK(omgp::TRUNK_T_poll_us == 2000);
    CHECK(omgp::LIMIT_max_l3_payload == 64);
    CHECK(omgp::ADDR_module_i2c_base == 0x20);
    // CRC published check value: "123456789" -> 0x29B1
    const char* s = "123456789";
    CHECK(omgp::crc16_ccitt_false(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) == 0x29B1);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("unit: all checks passed\n");
    return 0;
}
