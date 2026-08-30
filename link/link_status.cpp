// OMGP trunk L2 — Status names. Errors by return value, never exceptions (embedded-path
// C++ API convention; see trunk §7 for the L2 retry/error semantics these values encode).
#include "link_types.hpp"

namespace omgp {
namespace link {

const char* status_name(Status s) {
    switch (s) {
    case Status::Ok:
        return "Ok";
    case Status::PayloadTooLong:
        return "PayloadTooLong";
    case Status::ReservedAddress:
        return "ReservedAddress";
    case Status::BufferTooSmall:
        return "BufferTooSmall";
    case Status::Busy:
        return "Busy";
    case Status::NotIdle:
        return "NotIdle";
    }
    return "?"; // unreachable for valid enumerators; keeps -Wreturn-type quiet on all compilers
}

} // namespace link
} // namespace omgp
