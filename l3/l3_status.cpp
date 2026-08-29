// OMGP L3 codec — Status names. protocol-l3 §3 (errors are values, never exceptions).
#include "l3_types.hpp"

namespace omgp {
namespace l3 {

const char* status_name(Status s) {
    switch (s) {
    case Status::Ok:
        return "Ok";
    case Status::Truncated:
        return "Truncated";
    case Status::LengthMismatch:
        return "LengthMismatch";
    case Status::OutOfRange:
        return "OutOfRange";
    case Status::UnknownOpcode:
        return "UnknownOpcode";
    case Status::MissingRequired:
        return "MissingRequired";
    case Status::DuplicateRecord:
        return "DuplicateRecord";
    case Status::StringTooLong:
        return "StringTooLong";
    case Status::BlobTooLarge:
        return "BlobTooLarge";
    case Status::BufferTooSmall:
        return "BufferTooSmall";
    case Status::InvalidUtf8:
        return "InvalidUtf8";
    case Status::MalformedRecord:
        return "MalformedRecord";
    case Status::ReservedViolation:
        return "ReservedViolation";
    }
    return "?"; // unreachable for valid enumerators; keeps -Wreturn-type quiet on all compilers
}

} // namespace l3
} // namespace omgp
