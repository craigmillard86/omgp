// OMGP L3 codec — descriptor (TLV) parse/validate/build. protocol-l3 §4, §4.1.
// Streaming: O(1) memory, no record-count caps, no heap (research R-06). Required/
// repeated/max_len come from the generated TLV_INFO (FR-020).
//
// Check order inside a record (shared with the Python reference so the differential test
// compares Status names): DuplicateRecord → MalformedRecord (length shape) → StringTooLong
// (max_len) → InvalidUtf8 → OutOfRange. Whole blob: BlobTooLarge before any record is
// read; Truncated when a length overruns; MissingRequired after the last record.
#pragma once

#include "l3_types.hpp"

namespace omgp {
namespace l3 {

// One raw TLV record, viewing the caller's blob.
struct RecordView {
    uint8_t type;
    uint8_t len;
    const uint8_t* value;
    uint16_t offset; // byte offset of the type byte within the blob
};

// Zero-copy iterator over the records of a blob. Does NOT check the 2048 cap.
// Loop: while (!c.at_end() && c.next(v) == Status::Ok) { ... }
// next() at the end returns Ok and leaves `out` untouched; Truncated when a record's
// length runs past the blob (at_end() becomes true so the loop terminates either way).
class RecordCursor {
  public:
    RecordCursor(const uint8_t* blob, size_t len) : blob_(blob), len_(len), pos_(0) {}
    Status next(RecordView& out);
    bool at_end() const {
        return pos_ >= len_;
    }

  private:
    const uint8_t* blob_;
    size_t len_;
    size_t pos_;
};

// Typed records (§4.1). Strings and opaque tails are views into the blob; nothing owns memory.
struct ProtocolRec {
    uint8_t major, minor;
};
struct ModuleTypeRec {
    uint8_t type;
};
struct ModelIdRec {
    uint16_t vendor_model, hw_rev, fw_rev;
};
struct ChannelRec {
    uint8_t index;
    Str name;
};
struct SwitchingRec {
    uint8_t flags; // bit0 mute-required, bit1 seamless; bits 2-7 preserved
    uint16_t settle_ms;
};
struct ParamRec {
    uint8_t param_id, scope, kind;
    uint16_t default_value;
    Str name;
};
struct ParamEnumRec {
    uint8_t param_id, index;
    Str label;
};
struct AudioRec {
    uint8_t io_flags, input_mode;
    uint16_t in_max_mvrms, out_max_mvrms;
};
struct PowerLvRec {
    uint16_t p15_ma, n15_ma, p9_ma, p5_ma;
};
struct PowerTubeRec {
    uint8_t power_class, tubes, sections;
    uint16_t heater_nom_ma, heater_max_ma, bplus_nom_v;
    uint8_t bplus_exp_ma, bplus_max_ma;
};
struct VendorRec {
    uint16_t vendor_id;
    Bytes data;
};

// Validates a record of a known type without materialising it (shape, strings, ranges).
// Unknown types are always Ok (they are skipped by length).
Status check_record(uint8_t type, const uint8_t* value, uint8_t len);

// One pass over the blob: cap, per-record checks, duplicates, required presence, counts.
Status validate_descriptor(const uint8_t* blob, size_t len, DescriptorReport& report);

// Typed decoders: MalformedRecord if the view's type is not the decoder's type.
Status decode_protocol(const RecordView&, ProtocolRec&);
Status decode_module_type(const RecordView&, ModuleTypeRec&);
Status decode_name(const RecordView&, Str&);
Status decode_manufacturer(const RecordView&, Str&);
Status decode_model_id(const RecordView&, ModelIdRec&);
Status decode_serial(const RecordView&, Str&);
Status decode_channel(const RecordView&, ChannelRec&);
Status decode_switching(const RecordView&, SwitchingRec&);
Status decode_param(const RecordView&, ParamRec&);
Status decode_param_enum(const RecordView&, ParamEnumRec&);
Status decode_audio(const RecordView&, AudioRec&);
Status decode_power_lv(const RecordView&, PowerLvRec&);
Status decode_power_tube(const RecordView&, PowerTubeRec&);
Status decode_vendor(const RecordView&, VendorRec&);

// Appends records into a caller buffer, enforcing the same rules at write time. The
// effective capacity is min(cap, DESC_MAX_BYTES): exceeding the buffer is BufferTooSmall,
// exceeding the protocol cap is BlobTooLarge. Nothing is written on failure.
class DescriptorWriter {
  public:
    DescriptorWriter(uint8_t* buf, size_t cap);
    Status add_protocol(const ProtocolRec&);
    Status add_module_type(const ModuleTypeRec&);
    Status add_name(Str);
    Status add_manufacturer(Str);
    Status add_model_id(const ModelIdRec&);
    Status add_serial(Str);
    Status add_channel(const ChannelRec&);
    Status add_switching(const SwitchingRec&);
    Status add_param(const ParamRec&);
    Status add_param_enum(const ParamEnumRec&);
    Status add_audio(const AudioRec&);
    Status add_power_lv(const PowerLvRec&);
    Status add_power_tube(const PowerTubeRec&);
    Status add_vendor(const VendorRec&);
    // Verbatim record (unknown/vendor passthrough). Known types are still validated.
    Status add_raw(uint8_t type, const uint8_t* value, uint8_t len);
    size_t size() const {
        return size_;
    }
    // Runs validate_descriptor over what has been written (required-record check).
    Status finish(DescriptorReport& report) const;

  private:
    Status append(uint8_t type, const uint8_t* value, uint8_t len);
    uint8_t* buf_;
    size_t cap_;
    size_t size_;
    uint32_t seen_[8]; // 256-bit bitmap of record types written
};

// CRC-16/CCITT-FALSE over the whole blob exactly as served by READ_DESC (§4.1; ruling
// 2026-08-28; YAML descriptor.crc). IDENTIFY's desc_crc is this value.
uint16_t descriptor_crc(const uint8_t* blob, size_t len);

} // namespace l3
} // namespace omgp
