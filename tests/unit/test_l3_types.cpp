// First Catch2 test: proves the vendored framework, the EXECUTED listener and the heap
// guard work end-to-end on both pipeline paths (spec 001 T018), and pins the shared
// types of protocol-l3 §3 / §3.3.
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_types.hpp"

#include <type_traits>

using omgp::l3::Status;

TEST_CASE("status_name covers every enumerator", "[types]") {
    // Each name must be distinct and never the fallback "?" (a new enumerator without a
    // name would show up here before it shows up in a tool's output).
    const Status all[] = {Status::Ok,
                          Status::Truncated,
                          Status::LengthMismatch,
                          Status::OutOfRange,
                          Status::UnknownOpcode,
                          Status::MissingRequired,
                          Status::DuplicateRecord,
                          Status::StringTooLong,
                          Status::BlobTooLarge,
                          Status::BufferTooSmall,
                          Status::InvalidUtf8,
                          Status::MalformedRecord,
                          Status::ReservedViolation};
    for (Status s : all) {
        const char* n = omgp::l3::status_name(s);
        REQUIRE(n != nullptr);
        REQUIRE(std::string(n) != "?");
    }
    REQUIRE(std::string(omgp::l3::status_name(Status::Ok)) == "Ok");
    REQUIRE(std::string(omgp::l3::status_name(Status::ReservedViolation)) == "ReservedViolation");
    REQUIRE(static_cast<int>(Status::Ok) == 0);
}

TEST_CASE("header and payload structs are wire-shaped PODs", "[types]") {
    STATIC_REQUIRE(sizeof(omgp::l3::Header) == 5); // §3: five header bytes
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::Header>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::IdentifyResp>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::SetParamReq>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::StatusBlock>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::GetEventResp>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::DescriptorReport>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::Bytes>::value);
    STATIC_REQUIRE(std::is_trivially_copyable<omgp::l3::Str>::value);
}

TEST_CASE("heap guard counts allocations and stays flat across a non-allocating call",
          "[types][heap]") {
    // Discriminating check: the guard must move when we allocate, or a green
    // HEAP_FREE_SCOPE later proves nothing.
    const unsigned long before = omgp_test::heap_calls;
    int* p = new int(42);
    REQUIRE(omgp_test::heap_calls > before);
    delete p;

    HEAP_FREE_SCOPE({ (void)omgp::l3::status_name(Status::Truncated); });
}
