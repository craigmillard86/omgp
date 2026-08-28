// libFuzzer target: descriptor cursor, validator and every typed decoder (protocol-l3 §4).
// The input is the raw blob. Invariants checked: the cursor never reads past the blob
// (ASan), a validated blob re-emits byte-identically through DescriptorWriter::add_raw,
// and every typed decoder accepts or rejects each record without crashing.
#include "l3/l3_descriptor.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

using namespace omgp::l3;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    DescriptorReport r;
    const Status st = validate_descriptor(data, size, r);

    RecordCursor c(data, size);
    RecordView v;
    while (!c.at_end() && c.next(v) == Status::Ok) {
        ProtocolRec a;
        ModuleTypeRec b;
        Str s;
        ModelIdRec d;
        ChannelRec e;
        SwitchingRec f;
        ParamRec g;
        ParamEnumRec h;
        AudioRec i;
        PowerLvRec j;
        PowerTubeRec k;
        VendorRec l;
        (void)decode_protocol(v, a);
        (void)decode_module_type(v, b);
        (void)decode_name(v, s);
        (void)decode_manufacturer(v, s);
        (void)decode_model_id(v, d);
        (void)decode_serial(v, s);
        (void)decode_channel(v, e);
        (void)decode_switching(v, f);
        (void)decode_param(v, g);
        (void)decode_param_enum(v, h);
        (void)decode_audio(v, i);
        (void)decode_power_lv(v, j);
        (void)decode_power_tube(v, k);
        (void)decode_vendor(v, l);
        (void)check_record(v.type, v.value, v.len);
    }

    if (st == Status::Ok) {
        static uint8_t out[omgp::DESC_MAX_BYTES];
        DescriptorWriter w(out, sizeof out);
        RecordCursor c2(data, size);
        while (!c2.at_end() && c2.next(v) == Status::Ok)
            if (w.add_raw(v.type, v.value, v.len) != Status::Ok)
                __builtin_trap(); // a validated record must be writable
        DescriptorReport r2;
        if (w.finish(r2) != Status::Ok || w.size() != size)
            __builtin_trap();
        for (size_t x = 0; x < size; ++x)
            if (out[x] != data[x])
                __builtin_trap();
        (void)descriptor_crc(data, size);
    }
    return 0;
}
