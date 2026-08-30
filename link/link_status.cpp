// OMGP trunk L2 — Status names. trunk §4 (errors are values, never exceptions).
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
