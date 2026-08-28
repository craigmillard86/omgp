// OMGP L3 codec — descriptor (TLV). protocol-l3 §4 (u8 type, u8 len, u8[len] value;
// unknown types skipped by length; max 2048 bytes; LE integers; UTF-8 strings), §4.1
// (record types), §4.2 (minor versions purely additive — the skip rule carries evolution).
#include "l3_descriptor.hpp"

#include "../link/crc16.hpp"
#include "l3_utf8.hpp"
#include "omgp_protocol.h"

namespace omgp {
namespace l3 {

namespace {

constexpr size_t kMaxBlob = DESC_MAX_BYTES;

inline uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

const TlvInfo* tlv_info(uint8_t type) {
    for (const auto& t : TLV_INFO)
        if (t.type == type)
            return &t;
    return nullptr;
}

template <size_t N> inline bool in_codes(const uint8_t (&table)[N], uint8_t v) {
    for (size_t i = 0; i < N; ++i)
        if (table[i] == v)
            return true;
    return false;
}

inline bool bit(const uint32_t (&map)[8], uint8_t i) {
    return (map[i >> 5] >> (i & 31)) & 1u;
}
inline void set_bit(uint32_t (&map)[8], uint8_t i) {
    map[i >> 5] |= 1u << (i & 31);
}

// Fixed sizes of the non-string records (§4.1).
constexpr uint8_t kProtocolLen = 2, kModuleTypeLen = 1, kModelIdLen = 6, kSwitchingLen = 3,
                  kAudioLen = 6, kPowerLvLen = 8, kPowerTubeLen = 11;
constexpr uint8_t kChannelMin = 1, kParamMin = 5, kParamEnumMin = 2, kVendorMin = 2;
constexpr uint8_t kPowerClassMin = 1, kPowerClassMax = 4; // T1-T4

// String-only records: max_len from TLV_INFO, then UTF-8.
Status check_string(uint8_t type, const uint8_t* value, uint8_t len) {
    const TlvInfo* info = tlv_info(type);
    if (info != nullptr && info->max_len != 0 && len > info->max_len)
        return Status::StringTooLong;
    return utf8_valid(value, len) ? Status::Ok : Status::InvalidUtf8;
}

} // namespace

// --- per-record validation (order shared with the Python reference) --------------------------

Status check_record(uint8_t type, const uint8_t* value, uint8_t len) {
    if (type == TLV_NAME || type == TLV_MANUFACTURER || type == TLV_SERIAL)
        return check_string(type, value, len);
    if (type == TLV_PROTOCOL)
        return len == kProtocolLen ? Status::Ok : Status::MalformedRecord;
    if (type == TLV_MODULE_TYPE) {
        if (len != kModuleTypeLen)
            return Status::MalformedRecord;
        return in_codes(MODULE_TYPE_CODES, value[0]) ? Status::Ok : Status::OutOfRange;
    }
    if (type == TLV_MODEL_ID)
        return len == kModelIdLen ? Status::Ok : Status::MalformedRecord;
    if (type == TLV_CHANNEL) {
        if (len < kChannelMin)
            return Status::MalformedRecord;
        return utf8_valid(value + 1, len - 1) ? Status::Ok : Status::InvalidUtf8;
    }
    if (type == TLV_SWITCHING)
        return len == kSwitchingLen ? Status::Ok : Status::MalformedRecord;
    if (type == TLV_PARAM) {
        if (len < kParamMin)
            return Status::MalformedRecord;
        if (!utf8_valid(value + kParamMin, len - kParamMin))
            return Status::InvalidUtf8;
        if (!in_codes(PARAM_KIND_CODES, value[2]))
            return Status::OutOfRange;
        if (get16(value + 3) > LIMIT_param_value_max)
            return Status::OutOfRange;
        return Status::Ok;
    }
    if (type == TLV_PARAM_ENUM) {
        if (len < kParamEnumMin)
            return Status::MalformedRecord;
        return utf8_valid(value + kParamEnumMin, len - kParamEnumMin) ? Status::Ok
                                                                      : Status::InvalidUtf8;
    }
    if (type == TLV_AUDIO) {
        if (len != kAudioLen)
            return Status::MalformedRecord;
        return value[1] > 1 ? Status::OutOfRange : Status::Ok;
    }
    if (type == TLV_POWER_LV)
        return len == kPowerLvLen ? Status::Ok : Status::MalformedRecord;
    if (type == TLV_POWER_TUBE) {
        if (len != kPowerTubeLen)
            return Status::MalformedRecord;
        return (value[0] < kPowerClassMin || value[0] > kPowerClassMax) ? Status::OutOfRange
                                                                        : Status::Ok;
    }
    if (type == TLV_VENDOR)
        return len >= kVendorMin ? Status::Ok : Status::MalformedRecord;
    return Status::Ok; // unknown type: skipped by length (§4)
}

// --- cursor ------------------------------------------------------------------------------------

Status RecordCursor::next(RecordView& out) {
    if (pos_ >= len_)
        return Status::Ok;
    if (len_ - pos_ < 2) {
        pos_ = len_;
        return Status::Truncated;
    }
    const uint8_t type = blob_[pos_];
    const uint8_t len = blob_[pos_ + 1];
    if (len_ - pos_ - 2 < len) {
        pos_ = len_;
        return Status::Truncated;
    }
    out.type = type;
    out.len = len;
    out.value = blob_ + pos_ + 2;
    out.offset = static_cast<uint16_t>(pos_);
    pos_ += 2 + static_cast<size_t>(len);
    return Status::Ok;
}

// --- whole-blob validation ---------------------------------------------------------------------

Status validate_descriptor(const uint8_t* blob, size_t len, DescriptorReport& r) {
    r = DescriptorReport{Status::Ok, 0, 0, 0, 0, 0};
    if (len > kMaxBlob) {
        r.status = Status::BlobTooLarge;
        return r.status;
    }
    uint32_t seen[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t pos = 0;
    while (pos < len) {
        const uint8_t type = blob[pos];
        const uint16_t off = static_cast<uint16_t>(pos);
        if (len - pos < 2) {
            r.status = Status::Truncated;
            r.type = type;
            r.offset = off;
            return r.status;
        }
        const uint8_t rlen = blob[pos + 1];
        if (len - pos - 2 < rlen) {
            r.status = Status::Truncated;
            r.type = type;
            r.offset = off;
            return r.status;
        }
        const uint8_t* value = blob + pos + 2;
        const TlvInfo* info = tlv_info(type);
        if (info == nullptr) {
            ++r.skipped_unknown;
        } else {
            if (!info->repeated && bit(seen, type)) {
                r.status = Status::DuplicateRecord;
                r.type = type;
                r.offset = off;
                return r.status;
            }
            set_bit(seen, type);
            const Status st = check_record(type, value, rlen);
            if (st != Status::Ok) {
                r.status = st;
                r.type = type;
                r.offset = off;
                return st;
            }
            if (type == TLV_CHANNEL)
                ++r.channel_count;
            else if (type == TLV_PARAM)
                ++r.param_count;
        }
        pos += 2 + static_cast<size_t>(rlen);
    }
    for (const auto& t : TLV_INFO) // sorted by type
        if (t.required && !bit(seen, t.type)) {
            r.status = Status::MissingRequired;
            r.type = t.type;
            r.offset = static_cast<uint16_t>(len);
            return r.status;
        }
    return Status::Ok;
}

// --- typed decoders ------------------------------------------------------------------------------

namespace {
inline Status typed(const RecordView& v, uint8_t expect) {
    if (v.type != expect)
        return Status::MalformedRecord;
    return check_record(v.type, v.value, v.len);
}
} // namespace

Status decode_protocol(const RecordView& v, ProtocolRec& out) {
    const Status st = typed(v, TLV_PROTOCOL);
    if (st != Status::Ok)
        return st;
    out.major = v.value[0];
    out.minor = v.value[1];
    return Status::Ok;
}
Status decode_module_type(const RecordView& v, ModuleTypeRec& out) {
    const Status st = typed(v, TLV_MODULE_TYPE);
    if (st != Status::Ok)
        return st;
    out.type = v.value[0];
    return Status::Ok;
}
Status decode_name(const RecordView& v, Str& out) {
    const Status st = typed(v, TLV_NAME);
    if (st != Status::Ok)
        return st;
    out = Str{v.value, v.len};
    return Status::Ok;
}
Status decode_manufacturer(const RecordView& v, Str& out) {
    const Status st = typed(v, TLV_MANUFACTURER);
    if (st != Status::Ok)
        return st;
    out = Str{v.value, v.len};
    return Status::Ok;
}
Status decode_model_id(const RecordView& v, ModelIdRec& out) {
    const Status st = typed(v, TLV_MODEL_ID);
    if (st != Status::Ok)
        return st;
    out.vendor_model = get16(v.value);
    out.hw_rev = get16(v.value + 2);
    out.fw_rev = get16(v.value + 4);
    return Status::Ok;
}
Status decode_serial(const RecordView& v, Str& out) {
    const Status st = typed(v, TLV_SERIAL);
    if (st != Status::Ok)
        return st;
    out = Str{v.value, v.len};
    return Status::Ok;
}
Status decode_channel(const RecordView& v, ChannelRec& out) {
    const Status st = typed(v, TLV_CHANNEL);
    if (st != Status::Ok)
        return st;
    out.index = v.value[0];
    out.name = Str{v.value + 1, static_cast<uint8_t>(v.len - 1)};
    return Status::Ok;
}
Status decode_switching(const RecordView& v, SwitchingRec& out) {
    const Status st = typed(v, TLV_SWITCHING);
    if (st != Status::Ok)
        return st;
    out.flags = v.value[0];
    out.settle_ms = get16(v.value + 1);
    return Status::Ok;
}
Status decode_param(const RecordView& v, ParamRec& out) {
    const Status st = typed(v, TLV_PARAM);
    if (st != Status::Ok)
        return st;
    out.param_id = v.value[0];
    out.scope = v.value[1];
    out.kind = v.value[2];
    out.default_value = get16(v.value + 3);
    out.name = Str{v.value + kParamMin, static_cast<uint8_t>(v.len - kParamMin)};
    return Status::Ok;
}
Status decode_param_enum(const RecordView& v, ParamEnumRec& out) {
    const Status st = typed(v, TLV_PARAM_ENUM);
    if (st != Status::Ok)
        return st;
    out.param_id = v.value[0];
    out.index = v.value[1];
    out.label = Str{v.value + kParamEnumMin, static_cast<uint8_t>(v.len - kParamEnumMin)};
    return Status::Ok;
}
Status decode_audio(const RecordView& v, AudioRec& out) {
    const Status st = typed(v, TLV_AUDIO);
    if (st != Status::Ok)
        return st;
    out.io_flags = v.value[0];
    out.input_mode = v.value[1];
    out.in_max_mvrms = get16(v.value + 2);
    out.out_max_mvrms = get16(v.value + 4);
    return Status::Ok;
}
Status decode_power_lv(const RecordView& v, PowerLvRec& out) {
    const Status st = typed(v, TLV_POWER_LV);
    if (st != Status::Ok)
        return st;
    out.p15_ma = get16(v.value);
    out.n15_ma = get16(v.value + 2);
    out.p9_ma = get16(v.value + 4);
    out.p5_ma = get16(v.value + 6);
    return Status::Ok;
}
Status decode_power_tube(const RecordView& v, PowerTubeRec& out) {
    const Status st = typed(v, TLV_POWER_TUBE);
    if (st != Status::Ok)
        return st;
    out.power_class = v.value[0];
    out.tubes = v.value[1];
    out.sections = v.value[2];
    out.heater_nom_ma = get16(v.value + 3);
    out.heater_max_ma = get16(v.value + 5);
    out.bplus_nom_v = get16(v.value + 7);
    out.bplus_exp_ma = v.value[9];
    out.bplus_max_ma = v.value[10];
    return Status::Ok;
}
Status decode_vendor(const RecordView& v, VendorRec& out) {
    const Status st = typed(v, TLV_VENDOR);
    if (st != Status::Ok)
        return st;
    out.vendor_id = get16(v.value);
    out.data = Bytes{v.value + kVendorMin, static_cast<uint8_t>(v.len - kVendorMin)};
    return Status::Ok;
}

// --- writer --------------------------------------------------------------------------------------

DescriptorWriter::DescriptorWriter(uint8_t* buf, size_t cap)
    : buf_(buf), cap_(cap < kMaxBlob ? cap : kMaxBlob), size_(0), seen_{0, 0, 0, 0, 0, 0, 0, 0} {}

Status DescriptorWriter::append(uint8_t type, const uint8_t* value, uint8_t len) {
    const TlvInfo* info = tlv_info(type);
    if (info != nullptr) {
        if (!info->repeated && bit(seen_, type))
            return Status::DuplicateRecord;
        const Status st = check_record(type, value, len);
        if (st != Status::Ok)
            return st;
    }
    const size_t need = 2 + static_cast<size_t>(len);
    if (size_ + need > cap_)
        return cap_ == kMaxBlob ? Status::BlobTooLarge : Status::BufferTooSmall;
    buf_[size_] = type;
    buf_[size_ + 1] = len;
    for (size_t i = 0; i < len; ++i)
        buf_[size_ + 2 + i] = value[i];
    size_ += need;
    if (info != nullptr)
        set_bit(seen_, type);
    return Status::Ok;
}

Status DescriptorWriter::add_raw(uint8_t type, const uint8_t* value, uint8_t len) {
    return append(type, value, len);
}
Status DescriptorWriter::add_protocol(const ProtocolRec& r) {
    const uint8_t v[kProtocolLen] = {r.major, r.minor};
    return append(TLV_PROTOCOL, v, kProtocolLen);
}
Status DescriptorWriter::add_module_type(const ModuleTypeRec& r) {
    return append(TLV_MODULE_TYPE, &r.type, kModuleTypeLen);
}
Status DescriptorWriter::add_name(Str s) {
    return append(TLV_NAME, s.data, s.len);
}
Status DescriptorWriter::add_manufacturer(Str s) {
    return append(TLV_MANUFACTURER, s.data, s.len);
}
Status DescriptorWriter::add_model_id(const ModelIdRec& r) {
    uint8_t v[kModelIdLen];
    put16(v, r.vendor_model);
    put16(v + 2, r.hw_rev);
    put16(v + 4, r.fw_rev);
    return append(TLV_MODEL_ID, v, kModelIdLen);
}
Status DescriptorWriter::add_serial(Str s) {
    return append(TLV_SERIAL, s.data, s.len);
}
Status DescriptorWriter::add_channel(const ChannelRec& r) {
    uint8_t v[256];
    if (r.name.len > 254)
        return Status::StringTooLong;
    v[0] = r.index;
    for (size_t i = 0; i < r.name.len; ++i)
        v[1 + i] = r.name.data[i];
    return append(TLV_CHANNEL, v, static_cast<uint8_t>(1 + r.name.len));
}
Status DescriptorWriter::add_switching(const SwitchingRec& r) {
    uint8_t v[kSwitchingLen];
    v[0] = r.flags;
    put16(v + 1, r.settle_ms);
    return append(TLV_SWITCHING, v, kSwitchingLen);
}
Status DescriptorWriter::add_param(const ParamRec& r) {
    uint8_t v[256];
    if (r.name.len > 250)
        return Status::StringTooLong;
    v[0] = r.param_id;
    v[1] = r.scope;
    v[2] = r.kind;
    put16(v + 3, r.default_value);
    for (size_t i = 0; i < r.name.len; ++i)
        v[kParamMin + i] = r.name.data[i];
    return append(TLV_PARAM, v, static_cast<uint8_t>(kParamMin + r.name.len));
}
Status DescriptorWriter::add_param_enum(const ParamEnumRec& r) {
    uint8_t v[256];
    if (r.label.len > 253)
        return Status::StringTooLong;
    v[0] = r.param_id;
    v[1] = r.index;
    for (size_t i = 0; i < r.label.len; ++i)
        v[kParamEnumMin + i] = r.label.data[i];
    return append(TLV_PARAM_ENUM, v, static_cast<uint8_t>(kParamEnumMin + r.label.len));
}
Status DescriptorWriter::add_audio(const AudioRec& r) {
    uint8_t v[kAudioLen];
    v[0] = r.io_flags;
    v[1] = r.input_mode;
    put16(v + 2, r.in_max_mvrms);
    put16(v + 4, r.out_max_mvrms);
    return append(TLV_AUDIO, v, kAudioLen);
}
Status DescriptorWriter::add_power_lv(const PowerLvRec& r) {
    uint8_t v[kPowerLvLen];
    put16(v, r.p15_ma);
    put16(v + 2, r.n15_ma);
    put16(v + 4, r.p9_ma);
    put16(v + 6, r.p5_ma);
    return append(TLV_POWER_LV, v, kPowerLvLen);
}
Status DescriptorWriter::add_power_tube(const PowerTubeRec& r) {
    uint8_t v[kPowerTubeLen];
    v[0] = r.power_class;
    v[1] = r.tubes;
    v[2] = r.sections;
    put16(v + 3, r.heater_nom_ma);
    put16(v + 5, r.heater_max_ma);
    put16(v + 7, r.bplus_nom_v);
    v[9] = r.bplus_exp_ma;
    v[10] = r.bplus_max_ma;
    return append(TLV_POWER_TUBE, v, kPowerTubeLen);
}
Status DescriptorWriter::add_vendor(const VendorRec& r) {
    uint8_t v[256];
    if (r.data.len > 253)
        return Status::OutOfRange;
    put16(v, r.vendor_id);
    for (size_t i = 0; i < r.data.len; ++i)
        v[kVendorMin + i] = r.data.data[i];
    return append(TLV_VENDOR, v, static_cast<uint8_t>(kVendorMin + r.data.len));
}

Status DescriptorWriter::finish(DescriptorReport& report) const {
    return validate_descriptor(buf_, size_, report);
}

// --- crc -----------------------------------------------------------------------------------------

uint16_t descriptor_crc(const uint8_t* blob, size_t len) {
    return crc16_ccitt_false(blob, len);
}

} // namespace l3
} // namespace omgp
