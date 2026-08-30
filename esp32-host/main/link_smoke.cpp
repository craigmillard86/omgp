// Links the omgp_link component into the firmware so the target build exercises the trunk
// L2 sources (CLAUDE.md rule 10: both builds green). link/ holds only crc16.hpp until T008;
// grows to reference encode_frame/Deframer/Master/Responder/HealthTracker in T047 (Phase 8).
// trunk-link-layer §4.
#include "crc16.hpp"

extern "C" uint16_t omgp_link_smoke(void) {
    const uint8_t probe[] = {0x00};
    return omgp::crc16_ccitt_false(probe, sizeof(probe));
}
