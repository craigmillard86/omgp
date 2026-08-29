// Links the omgp_l3 component into the firmware so the target build exercises the codec
// sources (CLAUDE.md rule 10: both builds green). Called once from app_main().
// protocol-l3 §3.
#include "l3_types.hpp"

extern "C" const char* omgp_l3_smoke(void) {
    // Grows as codecs land: encode_header/decode_header round-trip from US2 onward.
    return omgp::l3::status_name(omgp::l3::Status::Ok);
}
