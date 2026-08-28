// Descriptor codec (spec 001 US3, T043). protocol-l3 §4, §4.1; mirrors
// tools/refimpl/test_descriptor.py. [vectors] consumes descriptor golden vectors through
// the generated header and the host-only canonical codec.
#include "canonical.hpp"
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_descriptor.hpp"
#include "omgp_protocol.h"
#include "omgp_vectors.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace omgp::l3;

namespace {

std::vector<uint8_t> tlv(uint8_t type, std::vector<uint8_t> value) {
    std::vector<uint8_t> out{type, static_cast<uint8_t>(value.size())};
    out.insert(out.end(), value.begin(), value.end());
    return out;
}
std::vector<uint8_t> str(const char* s) {
    return std::vector<uint8_t>(s, s + std::strlen(s));
}
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}
std::vector<uint8_t> with(std::vector<uint8_t> v, std::vector<uint8_t> extra) {
    v.insert(v.end(), extra.begin(), extra.end());
    return v;
}
std::vector<uint8_t> replace_once(const std::vector<uint8_t>& hay,
                                  const std::vector<uint8_t>& needle,
                                  const std::vector<uint8_t>& repl) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end());
    REQUIRE(it != hay.end());
    std::vector<uint8_t> out(hay.begin(), it);
    out.insert(out.end(), repl.begin(), repl.end());
    out.insert(out.end(), it + static_cast<long>(needle.size()), hay.end());
    return out;
}

const std::vector<uint8_t> PROTOCOL = tlv(omgp::TLV_PROTOCOL, {1, 0});
const std::vector<uint8_t> MODULE_TYPE = tlv(omgp::TLV_MODULE_TYPE, {omgp::MT_TUBE_PREAMP});
const std::vector<uint8_t> NAME = tlv(omgp::TLV_NAME, str("N"));
const std::vector<uint8_t> MANUFACTURER = tlv(omgp::TLV_MANUFACTURER, str("M"));
const std::vector<uint8_t> MODEL_ID = tlv(omgp::TLV_MODEL_ID, {0x01, 0x01, 0x02, 0x00, 0x03, 0x01});
const std::vector<uint8_t> CHANNEL0 = tlv(omgp::TLV_CHANNEL, with({0}, str("Clean")));
const std::vector<uint8_t> SWITCHING = tlv(omgp::TLV_SWITCHING, {0x01, 0x78, 0x00});
const std::vector<uint8_t> PARAM1 =
    tlv(omgp::TLV_PARAM, with({1, 0xFF, omgp::KIND_CONTINUOUS, 0x00, 0x08}, str("Gain")));
const std::vector<uint8_t> AUDIO = tlv(omgp::TLV_AUDIO, {0x03, 0x01, 0xF4, 0x01, 0xB0, 0x04});
const std::vector<uint8_t> POWER_LV = tlv(omgp::TLV_POWER_LV, {0x28, 0, 0x28, 0, 0, 0, 0x14, 0});
const std::vector<uint8_t> MIN = cat({PROTOCOL, MODULE_TYPE, NAME, MANUFACTURER, MODEL_ID, CHANNEL0,
                                      SWITCHING, PARAM1, AUDIO, POWER_LV});
const std::vector<uint8_t> UNKNOWN = tlv(0x55, {1, 2, 3, 4, 5, 6, 7});
const std::vector<uint8_t> VENDOR = tlv(omgp::TLV_VENDOR, {0x34, 0x12, 0xDE, 0xAD, 0xBE, 0xEF});

Status validate(const std::vector<uint8_t>& blob, DescriptorReport& r) {
    Status st;
    HEAP_FREE_SCOPE({ st = validate_descriptor(blob.data(), blob.size(), r); });
    return st;
}
Status validate(const std::vector<uint8_t>& blob) {
    DescriptorReport r;
    return validate(blob, r);
}

} // namespace

TEST_CASE("cursor walks records zero-copy and stops at the end", "[cursor]") {
    RecordCursor c(MIN.data(), MIN.size());
    RecordView v;
    size_t n = 0;
    HEAP_FREE_SCOPE({
        while (!c.at_end() && c.next(v) == Status::Ok)
            ++n;
    });
    REQUIRE(n == 10);
    REQUIRE(c.at_end());
    RecordCursor c2(MIN.data(), MIN.size());
    REQUIRE(c2.next(v) == Status::Ok);
    REQUIRE(
        (v.type == omgp::TLV_PROTOCOL && v.len == 2 && v.value == MIN.data() + 2 && v.offset == 0));
    auto cut = MIN;
    cut.pop_back();
    RecordCursor c3(cut.data(), cut.size());
    Status st = Status::Ok;
    while (!c3.at_end() && st == Status::Ok)
        st = c3.next(v);
    REQUIRE(st == Status::Truncated);
}

TEST_CASE("minimal and full descriptors validate with the right counts", "[required]") {
    DescriptorReport r;
    REQUIRE(validate(MIN, r) == Status::Ok);
    REQUIRE((r.skipped_unknown == 0 && r.channel_count == 1 && r.param_count == 1));
    auto full =
        cat({MIN, tlv(omgp::TLV_SERIAL, str("BP-0001")),
             tlv(omgp::TLV_CHANNEL, with({1}, str("Crunch"))),
             tlv(omgp::TLV_PARAM_ENUM, with({3, 0}, str("Bright"))),
             tlv(omgp::TLV_POWER_TUBE, {2, 2, 4, 0x58, 0x02, 0xBC, 0x02, 0xFA, 0x00, 12, 20}),
             VENDOR, UNKNOWN});
    REQUIRE(validate(full, r) == Status::Ok);
    REQUIRE((r.skipped_unknown == 1 && r.channel_count == 2 && r.param_count == 1));
}

TEST_CASE("each required record missing is named", "[required]") {
    const std::vector<std::pair<uint8_t, std::vector<uint8_t>>> req = {
        {omgp::TLV_PROTOCOL, PROTOCOL},   {omgp::TLV_MODULE_TYPE, MODULE_TYPE},
        {omgp::TLV_NAME, NAME},           {omgp::TLV_MANUFACTURER, MANUFACTURER},
        {omgp::TLV_MODEL_ID, MODEL_ID},   {omgp::TLV_CHANNEL, CHANNEL0},
        {omgp::TLV_SWITCHING, SWITCHING}, {omgp::TLV_PARAM, PARAM1},
        {omgp::TLV_AUDIO, AUDIO},         {omgp::TLV_POWER_LV, POWER_LV}};
    for (const auto& [type, chunk] : req) {
        auto blob = replace_once(MIN, chunk, {});
        DescriptorReport r;
        INFO("missing type " << int(type));
        REQUIRE(validate(blob, r) == Status::MissingRequired);
        REQUIRE(r.type == type);
        REQUIRE(r.offset == blob.size());
    }
}

TEST_CASE("unknown types are skipped by length, counted, and later records survive", "[unknown]") {
    auto blob = cat({PROTOCOL, UNKNOWN, MODULE_TYPE, NAME, MANUFACTURER, MODEL_ID, CHANNEL0,
                     SWITCHING, PARAM1, AUDIO, POWER_LV});
    DescriptorReport r;
    REQUIRE(validate(blob, r) == Status::Ok);
    REQUIRE(r.skipped_unknown == 1);
    auto over = with(MIN, {0x55, 10, 1, 2});
    REQUIRE(validate(over, r) == Status::Truncated);
    REQUIRE((r.type == 0x55 && r.offset == MIN.size()));
}

TEST_CASE("duplicates of non-repeated records are rejected", "[dup]") {
    DescriptorReport r;
    REQUIRE(validate(with(MIN, PROTOCOL), r) == Status::DuplicateRecord);
    REQUIRE((r.type == omgp::TLV_PROTOCOL && r.offset == MIN.size()));
    REQUIRE(validate(cat({MIN, tlv(omgp::TLV_CHANNEL, with({1}, str("B"))), VENDOR, VENDOR})) ==
            Status::Ok);
}

TEST_CASE("string limits: 24/25 and 16/17, empty allowed, UTF-8 enforced", "[strings][utf8]") {
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, std::vector<uint8_t>(24, 'x')))) ==
            Status::Ok);
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, std::vector<uint8_t>(25, 'x')))) ==
            Status::StringTooLong);
    REQUIRE(validate(with(MIN, tlv(omgp::TLV_SERIAL, std::vector<uint8_t>(16, 's')))) ==
            Status::Ok);
    REQUIRE(validate(with(MIN, tlv(omgp::TLV_SERIAL, std::vector<uint8_t>(17, 's')))) ==
            Status::StringTooLong);
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, {}))) == Status::Ok);
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, {0xFF, 0xFE}))) ==
            Status::InvalidUtf8);
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, {0xC0, 0xAF}))) ==
            Status::InvalidUtf8);
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, {0xED, 0xA0, 0x80}))) ==
            Status::InvalidUtf8);
    REQUIRE(validate(replace_once(MIN, NAME, tlv(omgp::TLV_NAME, {'R', 0xC3, 0xA9}))) ==
            Status::Ok);
}

TEST_CASE("2048 accepted, 2049 rejected before any record is read", "[cap]") {
    auto pad = MIN;
    while (pad.size() < omgp::LIMIT_max_descriptor_bytes - 2) {
        const size_t room = omgp::LIMIT_max_descriptor_bytes - 2 - pad.size();
        pad = with(pad, tlv(0x60, std::vector<uint8_t>(room < 200 ? room : 200, 0)));
    }
    if (pad.size() == omgp::LIMIT_max_descriptor_bytes - 2)
        pad = with(pad, tlv(0x61, {}));
    REQUIRE(pad.size() == omgp::LIMIT_max_descriptor_bytes);
    REQUIRE(validate(pad) == Status::Ok);
    auto over = cat({{0xFF}, pad});
    DescriptorReport r;
    REQUIRE(validate(over, r) == Status::BlobTooLarge);
    REQUIRE(r.offset == 0);
}

TEST_CASE("malformed fixed-length records and out-of-range values", "[rules]") {
    REQUIRE(validate(replace_once(MIN, PROTOCOL, tlv(omgp::TLV_PROTOCOL, {1}))) ==
            Status::MalformedRecord);
    REQUIRE(validate(replace_once(MIN, CHANNEL0, tlv(omgp::TLV_CHANNEL, {}))) ==
            Status::MalformedRecord);
    REQUIRE(validate(replace_once(MIN, SWITCHING, tlv(omgp::TLV_SWITCHING, {1, 0x78, 0, 0}))) ==
            Status::MalformedRecord);
    REQUIRE(validate(replace_once(MIN, MODULE_TYPE, tlv(omgp::TLV_MODULE_TYPE, {0x63}))) ==
            Status::OutOfRange);
    REQUIRE(validate(replace_once(MIN, PARAM1,
                                  tlv(omgp::TLV_PARAM, with({1, 0xFF, 6, 0, 8}, str("G"))))) ==
            Status::OutOfRange);
    REQUIRE(validate(replace_once(MIN, PARAM1,
                                  tlv(omgp::TLV_PARAM, with({1, 0xFF, 0, 0, 0x10}, str("G"))))) ==
            Status::OutOfRange);
    REQUIRE(validate(replace_once(MIN, AUDIO, tlv(omgp::TLV_AUDIO, {3, 2, 0xF4, 1, 0xB0, 4}))) ==
            Status::OutOfRange);
    REQUIRE(validate(with(MIN, tlv(omgp::TLV_POWER_TUBE, {0, 2, 4, 0x58, 2, 0xBC, 2, 0xFA, 0, 12,
                                                          20}))) == Status::OutOfRange);
    REQUIRE(validate(with(MIN, tlv(omgp::TLV_POWER_TUBE, {5, 2, 4, 0x58, 2, 0xBC, 2, 0xFA, 0, 12,
                                                          20}))) == Status::OutOfRange);
}

TEST_CASE("typed decoders expose views into the blob", "[cursor]") {
    RecordCursor c(MIN.data(), MIN.size());
    RecordView v;
    REQUIRE(c.next(v) == Status::Ok);
    ProtocolRec p;
    REQUIRE(decode_protocol(v, p) == Status::Ok);
    REQUIRE((p.major == 1 && p.minor == 0));
    for (int i = 0; i < 7; ++i)
        REQUIRE(c.next(v) == Status::Ok); // ... up to PARAM
    ParamRec pr;
    REQUIRE(decode_param(v, pr) == Status::Ok);
    REQUIRE((pr.param_id == 1 && pr.scope == 0xFF && pr.kind == omgp::KIND_CONTINUOUS &&
             pr.default_value == 2048));
    REQUIRE(std::string(reinterpret_cast<const char*>(pr.name.data), pr.name.len) == "Gain");
    REQUIRE(decode_protocol(v, p) == Status::MalformedRecord); // wrong type for this decoder
}

TEST_CASE("writer builds the minimal descriptor byte-identically and enforces the rules",
          "[writer]") {
    uint8_t buf[256];
    DescriptorWriter w(buf, sizeof buf);
    const uint8_t clean[] = {'C', 'l', 'e', 'a', 'n'};
    const uint8_t gain[] = {'G', 'a', 'i', 'n'};
    HEAP_FREE_SCOPE({
        REQUIRE(w.add_protocol(ProtocolRec{1, 0}) == Status::Ok);
        REQUIRE(w.add_module_type(ModuleTypeRec{omgp::MT_TUBE_PREAMP}) == Status::Ok);
        REQUIRE(w.add_name(Str{reinterpret_cast<const uint8_t*>("N"), 1}) == Status::Ok);
        REQUIRE(w.add_manufacturer(Str{reinterpret_cast<const uint8_t*>("M"), 1}) == Status::Ok);
        REQUIRE(w.add_model_id(ModelIdRec{0x0101, 0x0002, 0x0103}) == Status::Ok);
        REQUIRE(w.add_channel(ChannelRec{0, Str{clean, 5}}) == Status::Ok);
        REQUIRE(w.add_switching(SwitchingRec{0x01, 120}) == Status::Ok);
        REQUIRE(w.add_param(ParamRec{1, 0xFF, omgp::KIND_CONTINUOUS, 2048, Str{gain, 4}}) ==
                Status::Ok);
        REQUIRE(w.add_audio(AudioRec{0x03, 1, 500, 1200}) == Status::Ok);
        REQUIRE(w.add_power_lv(PowerLvRec{40, 40, 0, 20}) == Status::Ok);
    });
    DescriptorReport r;
    REQUIRE(w.finish(r) == Status::Ok);
    REQUIRE(w.size() == MIN.size());
    REQUIRE(std::memcmp(buf, MIN.data(), MIN.size()) == 0);
    REQUIRE(w.add_protocol(ProtocolRec{1, 0}) == Status::DuplicateRecord);
    REQUIRE(w.add_name(Str{reinterpret_cast<const uint8_t*>("xxxxxxxxxxxxxxxxxxxxxxxxx"), 25}) ==
            Status::DuplicateRecord); // NAME already present — duplicate is checked first
    uint8_t small[8];
    DescriptorWriter w2(small, sizeof small);
    REQUIRE(w2.add_protocol(ProtocolRec{1, 0}) == Status::Ok);
    REQUIRE(w2.add_model_id(ModelIdRec{1, 2, 3}) == Status::BufferTooSmall);
    REQUIRE(w2.size() == 4);
    DescriptorWriter w3(small, sizeof small);
    REQUIRE(w3.finish(r) == Status::MissingRequired);
    REQUIRE(w3.add_raw(0x55, UNKNOWN.data() + 2, 7) == Status::BufferTooSmall);
    REQUIRE(w3.add_raw(0x55, UNKNOWN.data() + 2, 5) == Status::Ok);
    uint8_t big[4096];
    DescriptorWriter w4(big, sizeof big); // cap is clamped to LIMIT_max_descriptor_bytes
    for (int i = 0; i < 10; ++i)
        REQUIRE(w4.add_raw(0x60, big, 200) == Status::Ok);
    REQUIRE(w4.add_raw(0x60, big, 200) == Status::BlobTooLarge);
}

TEST_CASE("descriptor_crc is CRC-16/CCITT-FALSE over the blob", "[crc]") {
    const uint8_t check[] = "123456789";
    uint16_t crc;
    HEAP_FREE_SCOPE({ crc = descriptor_crc(check, 9); });
    REQUIRE(crc == 0x29B1);
    REQUIRE(descriptor_crc(check, 0) == 0xFFFF);
}

TEST_CASE("descriptor vectors round-trip through the C++ codec", "[vectors]") {
    size_t seen = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.kind) != "descriptor")
            continue;
        ++seen;
        INFO(v.name);
        REQUIRE(omgp::canon::render_descriptor(v.bytes, v.len) == v.canonical);
        std::vector<uint8_t> out;
        std::string err;
        REQUIRE(omgp::canon::encode_descriptor(v.canonical, out, err));
        REQUIRE(out == std::vector<uint8_t>(v.bytes, v.bytes + v.len));
        REQUIRE(omgp::canon::validate_line(v.bytes, v.len).rfind("OK ", 0) == 0);
    }
    REQUIRE(seen >= 2); // descriptor_sample + descriptor_min (T047)
}
