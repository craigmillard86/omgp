// Host-only canonical-text codec (see canonical.hpp). Names come from the generated
// omgp_names.h; range markers (*_MIN/*_MAX) render as hex, matching the Python renderer.
#include "canonical.hpp"

#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"
#include "link/frame.hpp"
#include "omgp_names.h"
#include "omgp_protocol.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace omgp {
namespace canon {

using namespace omgp::l3;

namespace {

// --- scalar rendering ---------------------------------------------------------------------

std::string hex2(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "0x%02X", v);
    return b;
}
std::string hex4(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "0x%04X", v);
    return b;
}
std::string dec(unsigned v) {
    return std::to_string(v);
}

bool is_range_marker(const char* name) {
    const size_t n = std::strlen(name);
    return n > 4 &&
           (std::strcmp(name + n - 4, "_MIN") == 0 || std::strcmp(name + n - 4, "_MAX") == 0);
}

template <size_t N> std::string name_or_hex(const names::Entry (&table)[N], uint8_t code) {
    for (const auto& e : table)
        if (e.code == code && !is_range_marker(e.name))
            return e.name;
    return hex2(code);
}

template <size_t N>
bool lookup_name(const names::Entry (&table)[N], const std::string& tok, unsigned& code) {
    for (const auto& e : table)
        if (tok == e.name) {
            code = e.code;
            return true;
        }
    return false;
}

bool parse_uint(const std::string& tok, unsigned& v) {
    // strtoul silently accepts a leading '-' and negates (so "-0" parses as 0, and any other
    // negative token only fails downstream because unsigned long happens to be wider than
    // unsigned on this host) — reject the sign up front so rejection is a property of the
    // grammar, not of the host's integer widths (review @ 641ee1e).
    if (tok.empty() || tok[0] == '-' || tok[0] == '+')
        return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long x = std::strtoul(tok.c_str(), &end, 0);
    if (end == nullptr || *end != '\0')
        return false;
    // strtoul saturates to ULONG_MAX (with errno == ERANGE) above its own range, and on an
    // LP64 host (unsigned long wider than unsigned) silently accepts values above UINT_MAX
    // that would truncate on the cast below — reject both here so a range check downstream
    // never sees a value the input text didn't name (review @ d30ef1c).
    if (errno == ERANGE || x > UINT_MAX)
        return false;
    v = static_cast<unsigned>(x);
    return true;
}

template <size_t N>
bool parse_named(const names::Entry (&table)[N], const std::string& tok, unsigned& v) {
    return lookup_name(table, tok, v) || parse_uint(tok, v);
}

// --- token map ----------------------------------------------------------------------------

struct Tokens {
    std::map<std::string, std::string> kv;
    bool bad = false;
    bool take(const char* key, std::string& out) {
        auto it = kv.find(key);
        if (it == kv.end()) {
            bad = true;
            return false;
        }
        out = it->second;
        kv.erase(it);
        return true;
    }
    bool take_uint(const char* key, unsigned& v) {
        std::string s;
        if (!take(key, s) || !parse_uint(s, v)) {
            bad = true;
            return false;
        }
        return true;
    }
    template <size_t N>
    bool take_named(const char* key, const names::Entry (&table)[N], unsigned& v) {
        std::string s;
        if (!take(key, s) || !parse_named(table, s, v)) {
            bad = true;
            return false;
        }
        return true;
    }
    bool take_hex(const char* key, std::vector<uint8_t>& v) {
        std::string s;
        if (!take(key, s) || !parse_hex(s, v)) {
            bad = true;
            return false;
        }
        return true;
    }
};

bool tokenize(const std::string& line, Tokens& t) {
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
            ++i;
        if (i >= line.size())
            break;
        size_t j = line.find(' ', i);
        if (j == std::string::npos)
            j = line.size();
        const std::string tok = line.substr(i, j - i);
        const size_t eq = tok.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string key = tok.substr(0, eq);
        if (t.kv.count(key))
            return false;
        t.kv[key] = tok.substr(eq + 1);
        i = j;
    }
    return true;
}

// An opcode the protocol defines at all (regardless of direction). A request to a
// response-only opcode is a violation of a *known* opcode and is rejected; an opcode absent
// from the table is forwarded verbatim (FR-012) — the same split canonical.py makes.
bool known_opcode(uint8_t opcode) {
    for (const auto& o : OPCODE_INFO)
        if (o.code == opcode)
            return true;
    return false;
}

// --- payload rendering (mirrors canonical.py _render_payload) -------------------------------

std::string render_status_block(const StatusBlock& s) {
    return "state=" + name_or_hex(names::NODE_STATE_TABLE, s.state) +
           " channel=" + dec(s.active_channel) + " bypass=" + dec(s.bypass) +
           " fault=" + hex2(s.fault_code) + " uptime_s=" + dec(s.uptime_s) +
           " pending=" + dec(s.event_pending);
}

// Returns Ok and fills `text` (possibly empty) or a Status the decoder produced.
Status render_payload(uint8_t opcode, Dir dir, Bytes p, std::string& text) {
    text.clear();
    uint8_t mn, mx;
    bool opaque;
    if (payload_bounds(opcode, dir, mn, mx, opaque) != Status::Ok) {
        if (known_opcode(opcode))
            return Status::UnknownOpcode;         // e.g. ERROR sent as a request
        text = "raw=" + hex_lower(p.data, p.len); // unknown opcode: forward verbatim
        return Status::Ok;
    }
    if (opaque) {
        OpaquePayload o;
        const Status st = decode_opaque(p.data, p.len, o);
        if (st != Status::Ok)
            return st;
        text = "opaque=" + hex_lower(o.bytes.data, o.bytes.len);
        return Status::Ok;
    }
    const bool resp = dir == Dir::Response;
    if (opcode == OP_IDENTIFY && resp) {
        IdentifyResp r;
        const Status st = decode_identify_resp(p.data, p.len, r);
        if (st != Status::Ok)
            return st;
        text = "major=" + dec(r.major) + " minor=" + dec(r.minor) +
               " mt=" + name_or_hex(names::MODULE_TYPE_TABLE, r.module_type) +
               " desc_len=" + dec(r.desc_len) + " desc_crc=" + hex4(r.desc_crc);
        return Status::Ok;
    }
    if (opcode == OP_READ_DESC) {
        if (!resp) {
            ReadDescReq r;
            const Status st = decode_read_desc_req(p.data, p.len, r);
            if (st != Status::Ok)
                return st;
            text = "offset=" + dec(r.offset) + " max_len=" + dec(r.max_len);
        } else {
            ReadDescResp r;
            const Status st = decode_read_desc_resp(p.data, p.len, r);
            if (st != Status::Ok)
                return st;
            text = "offset=" + dec(r.offset) + " bytes=" + hex_lower(r.bytes.data, r.bytes.len);
        }
        return Status::Ok;
    }
    if (opcode == OP_SELECT_CHANNEL && !resp) {
        SelectChannelReq r;
        const Status st = decode_select_channel(p.data, p.len, r);
        if (st != Status::Ok)
            return st;
        text = "channel=" + dec(r.channel);
        return Status::Ok;
    }
    if (opcode == OP_SET_BYPASS && !resp) {
        SetBypassReq r;
        const Status st = decode_set_bypass(p.data, p.len, r);
        if (st != Status::Ok)
            return st;
        text = "bypass=" + dec(r.bypass);
        return Status::Ok;
    }
    if (opcode == OP_SET_PARAM && !resp) {
        SetParamReq r;
        const Status st = decode_set_param(p.data, p.len, r);
        if (st != Status::Ok)
            return st;
        text = "param_id=" + dec(r.param_id) + " scope=" + hex2(r.scope) + " value=" + dec(r.value);
        return Status::Ok;
    }
    if (opcode == OP_GET_PARAM) {
        if (!resp) {
            GetParamReq r;
            const Status st = decode_get_param_req(p.data, p.len, r);
            if (st != Status::Ok)
                return st;
            text = "param_id=" + dec(r.param_id) + " scope=" + hex2(r.scope);
        } else {
            GetParamResp r;
            const Status st = decode_get_param_resp(p.data, p.len, r);
            if (st != Status::Ok)
                return st;
            text = "param_id=" + dec(r.param_id) + " scope=" + hex2(r.scope) +
                   " value=" + dec(r.value);
        }
        return Status::Ok;
    }
    if (opcode == OP_GET_STATUS && resp) {
        StatusBlock s;
        const Status st = decode_status_block(p.data, p.len, s);
        if (st != Status::Ok)
            return st;
        text = render_status_block(s);
        return Status::Ok;
    }
    if (opcode == OP_GET_EVENT && resp) {
        GetEventResp r;
        const Status st = decode_get_event_resp(p.data, p.len, r);
        if (st != Status::Ok)
            return st;
        text = "evt=" + name_or_hex(names::EVENT_TABLE, r.event_type) +
               " remaining=" + dec(r.remaining_count) +
               " detail=" + hex_lower(r.detail.data, r.detail.len);
        return Status::Ok;
    }
    if (opcode == OP_ERROR && resp) {
        ErrorResp r;
        const Status st = decode_error_resp(p.data, p.len, r);
        if (st != Status::Ok)
            return st;
        text = "err=" + name_or_hex(names::ERROR_TABLE, r.code) +
               " detail=" + hex_lower(r.detail.data, r.detail.len);
        return Status::Ok;
    }
    // Every remaining known opcode/direction carries an empty payload.
    return p.len == 0 ? Status::Ok : Status::LengthMismatch;
}

// --- payload parsing (mirrors canonical.py _parse_payload) ----------------------------------

// Fills `out` with the encoded payload. Returns Ok, a codec Status, or sets bad on t.
Status parse_payload(uint8_t opcode, Dir dir, Tokens& t, std::vector<uint8_t>& out) {
    out.assign(LIMIT_max_l3_payload, 0);
    size_t n = 0;
    Status st = Status::Ok;
    uint8_t mn, mx;
    bool opaque;
    std::vector<uint8_t> tail;
    unsigned a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    if (payload_bounds(opcode, dir, mn, mx, opaque) != Status::Ok) {
        if (known_opcode(opcode))
            return Status::UnknownOpcode; // e.g. ERROR as a request
        if (!t.take_hex("raw", tail))
            return Status::Ok;
        out = tail;
        return Status::Ok;
    }
    const bool resp = dir == Dir::Response;
    if (opaque) {
        if (!t.take_hex("opaque", tail))
            return Status::Ok;
        st = encode_opaque(
            OpaquePayload{
                Bytes{tail.data(), static_cast<uint8_t>(tail.size() > 0xFF ? 0xFF : tail.size())}},
            out.data(), out.size(), n);
        if (tail.size() > LIMIT_max_l3_payload)
            st = Status::OutOfRange;
    } else if (opcode == OP_IDENTIFY && resp) {
        if (!(t.take_uint("major", a) && t.take_uint("minor", b) &&
              t.take_named("mt", names::MODULE_TYPE_TABLE, c) && t.take_uint("desc_len", d) &&
              t.take_uint("desc_crc", e)))
            return Status::Ok;
        st = encode_identify_resp(IdentifyResp{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                               static_cast<uint8_t>(c), static_cast<uint16_t>(d),
                                               static_cast<uint16_t>(e)},
                                  out.data(), out.size(), n);
    } else if (opcode == OP_READ_DESC && !resp) {
        if (!(t.take_uint("offset", a) && t.take_uint("max_len", b)))
            return Status::Ok;
        st = encode_read_desc_req(ReadDescReq{static_cast<uint16_t>(a), static_cast<uint8_t>(b)},
                                  out.data(), out.size(), n);
    } else if (opcode == OP_READ_DESC && resp) {
        if (!(t.take_uint("offset", a) && t.take_hex("bytes", tail)))
            return Status::Ok;
        if (tail.size() > LIMIT_max_l3_payload)
            return Status::OutOfRange;
        st = encode_read_desc_resp(
            ReadDescResp{static_cast<uint16_t>(a),
                         Bytes{tail.data(), static_cast<uint8_t>(tail.size())}},
            out.data(), out.size(), n);
    } else if (opcode == OP_SELECT_CHANNEL && !resp) {
        if (!t.take_uint("channel", a))
            return Status::Ok;
        st = encode_select_channel(SelectChannelReq{static_cast<uint8_t>(a)}, out.data(),
                                   out.size(), n);
    } else if (opcode == OP_SET_BYPASS && !resp) {
        if (!t.take_uint("bypass", a))
            return Status::Ok;
        st = encode_set_bypass(SetBypassReq{static_cast<uint8_t>(a)}, out.data(), out.size(), n);
    } else if (opcode == OP_SET_PARAM && !resp) {
        if (!(t.take_uint("param_id", a) && t.take_uint("scope", b) && t.take_uint("value", c)))
            return Status::Ok;
        if (c > 0xFFFF)
            return Status::OutOfRange;
        st = encode_set_param(
            SetParamReq{static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint16_t>(c)},
            out.data(), out.size(), n);
    } else if (opcode == OP_GET_PARAM && !resp) {
        if (!(t.take_uint("param_id", a) && t.take_uint("scope", b)))
            return Status::Ok;
        st = encode_get_param_req(GetParamReq{static_cast<uint8_t>(a), static_cast<uint8_t>(b)},
                                  out.data(), out.size(), n);
    } else if (opcode == OP_GET_PARAM && resp) {
        if (!(t.take_uint("param_id", a) && t.take_uint("scope", b) && t.take_uint("value", c)))
            return Status::Ok;
        if (c > 0xFFFF)
            return Status::OutOfRange;
        st = encode_get_param_resp(GetParamResp{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                                static_cast<uint16_t>(c)},
                                   out.data(), out.size(), n);
    } else if (opcode == OP_GET_STATUS && resp) {
        if (!(t.take_named("state", names::NODE_STATE_TABLE, a) && t.take_uint("channel", b) &&
              t.take_uint("bypass", c) && t.take_uint("fault", d) && t.take_uint("uptime_s", e) &&
              t.take_uint("pending", f)))
            return Status::Ok;
        st = encode_status_block(StatusBlock{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                             static_cast<uint8_t>(c), static_cast<uint8_t>(d),
                                             static_cast<uint16_t>(e), static_cast<uint8_t>(f)},
                                 out.data(), out.size(), n);
    } else if (opcode == OP_GET_EVENT && resp) {
        if (!(t.take_named("evt", names::EVENT_TABLE, a) && t.take_uint("remaining", b) &&
              t.take_hex("detail", tail)))
            return Status::Ok;
        if (tail.size() > LIMIT_max_l3_payload)
            return Status::OutOfRange;
        st = encode_get_event_resp(
            GetEventResp{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                         Bytes{tail.data(), static_cast<uint8_t>(tail.size())}},
            out.data(), out.size(), n);
    } else if (opcode == OP_ERROR && resp) {
        if (!(t.take_named("err", names::ERROR_TABLE, a) && t.take_hex("detail", tail)))
            return Status::Ok;
        if (tail.size() > LIMIT_max_l3_payload)
            return Status::OutOfRange;
        st = encode_error_resp(ErrorResp{static_cast<uint8_t>(a),
                                         Bytes{tail.data(), static_cast<uint8_t>(tail.size())}},
                               out.data(), out.size(), n);
    } else {
        n = 0; // empty payload
    }
    if (st != Status::Ok)
        return st;
    out.resize(n);
    return Status::Ok;
}

} // namespace

// --- public API -------------------------------------------------------------------------------

std::string status_line(Status s) {
    return std::string("ERR ") + status_name(s);
}

std::string hex_lower(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 0x0F]);
    }
    return s;
}

bool parse_hex(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (char ch : text) {
        if (ch == ' ')
            continue;
        int v;
        if (ch >= '0' && ch <= '9')
            v = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            v = ch - 'A' + 10;
        else
            return false;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return hi < 0;
}

std::string render_message(const uint8_t* data, size_t len) {
    Header h;
    Bytes p;
    Status st = decode_message(data, len, h, p);
    if (st != Status::Ok)
        return status_line(st);
    const Dir dir = (h.flags & FLAG_response) ? Dir::Response : Dir::Request;
    std::string tail;
    st = render_payload(h.opcode, dir, p, tail);
    if (st != Status::Ok)
        return status_line(st);
    std::string s = "op=" + name_or_hex(names::OPCODE_TABLE, h.opcode) +
                    " node=" + hex2(h.node_id) + " seq=" + dec(h.seq) + " flags=" + hex2(h.flags);
    if (!tail.empty())
        s += " " + tail;
    return s;
}

bool encode_message(const std::string& canonical, std::vector<uint8_t>& out, std::string& error) {
    Tokens t;
    unsigned opcode, node, seq, flags;
    if (!tokenize(canonical, t) || !t.take_named("op", names::OPCODE_TABLE, opcode) ||
        !t.take_uint("node", node) || !t.take_uint("seq", seq) || !t.take_uint("flags", flags) ||
        opcode > 0xFF || node > 0xFF || seq > 0xFF || flags > 0xFF) {
        error = "ERR BadRequest";
        return false;
    }
    const Dir dir = (flags & FLAG_response) ? Dir::Response : Dir::Request;
    std::vector<uint8_t> payload;
    const Status pst = parse_payload(static_cast<uint8_t>(opcode), dir, t, payload);
    if (t.bad || !t.kv.empty()) {
        error = "ERR BadRequest";
        return false;
    }
    if (pst != Status::Ok) {
        error = status_line(pst);
        return false;
    }
    if (payload.size() > LIMIT_max_l3_payload) {
        error = status_line(Status::OutOfRange);
        return false;
    }
    uint8_t hdr[HEADER_LEN];
    size_t n;
    const Status hst = encode_header(
        Header{static_cast<uint8_t>(opcode), static_cast<uint8_t>(node), static_cast<uint8_t>(seq),
               static_cast<uint8_t>(flags), static_cast<uint8_t>(payload.size())},
        hdr, sizeof hdr, n);
    if (hst != Status::Ok) {
        error = status_line(hst);
        return false;
    }
    out.assign(hdr, hdr + n);
    out.insert(out.end(), payload.begin(), payload.end());
    error.clear();
    return true;
}

std::string render_status(const uint8_t* data, size_t len) {
    StatusBlock s;
    const Status st = decode_status_block(data, len, s);
    return st == Status::Ok ? render_status_block(s) : status_line(st);
}

bool encode_status(const std::string& canonical, std::vector<uint8_t>& out, std::string& error) {
    Tokens t;
    unsigned a, b, c, d, e, f;
    if (!tokenize(canonical, t) ||
        !(t.take_named("state", names::NODE_STATE_TABLE, a) && t.take_uint("channel", b) &&
          t.take_uint("bypass", c) && t.take_uint("fault", d) && t.take_uint("uptime_s", e) &&
          t.take_uint("pending", f)) ||
        !t.kv.empty()) {
        error = "ERR BadRequest";
        return false;
    }
    out.assign(LIMIT_max_l3_payload, 0);
    size_t n;
    const Status st = encode_status_block(
        StatusBlock{static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c),
                    static_cast<uint8_t>(d), static_cast<uint16_t>(e), static_cast<uint8_t>(f)},
        out.data(), out.size(), n);
    if (st != Status::Ok) {
        error = status_line(st);
        return false;
    }
    out.resize(n);
    error.clear();
    return true;
}

// ================================================================================================
// Frames (spec 002 US1, T023). Mirrors the frame half of tools/refimpl/canonical.py; the line
// grammar is contracts/frame-vectors.md "Canonical frame line".
// ================================================================================================

std::string render_frame(const omgp::link::FrameFields& f) {
    const unsigned flags = (f.response ? 0x01u : 0u) | (f.retry ? 0x02u : 0u);
    return "frame dst=" + hex2(f.dst) + " src=" + hex2(f.src) + " flags=" + hex2(flags) +
           " seq=" + dec(f.seq) + " payload=" + hex_lower(f.payload, f.len);
}

bool parse_frame_line(const std::string& canonical, omgp::link::FrameFields& out,
                      std::vector<uint8_t>& payload_storage, std::string& error) {
    const size_t sp = canonical.find(' ');
    const std::string prefix = canonical.substr(0, sp);
    Tokens t;
    unsigned dst = 0, src = 0, flags = 0, seq = 0;
    std::vector<uint8_t> payload;
    // payload.size() > LIMIT_max_l3_payload is left for encode_frame's own PayloadTooLong
    // check below; only >0xFF (which out.len, a uint8_t, cannot represent at all) is
    // rejected here as malformed text.
    // flags' real domain is 0x00-0x03: trunk §4's ctrl byte is bit0=response, bit1=retry,
    // bits2-3=reserved 0, bits4-7=seq (supplied separately by the `seq=` field above), so
    // anything above 0x03 is a caller passing a whole ctrl byte here, not a valid flags
    // value — reject it rather than silently masking to a different, valid request
    // (review @ a95b531).
    if (prefix != "frame" || sp == std::string::npos || !tokenize(canonical.substr(sp + 1), t) ||
        !t.take_uint("dst", dst) || !t.take_uint("src", src) || !t.take_uint("flags", flags) ||
        !t.take_uint("seq", seq) || !t.take_hex("payload", payload) || !t.kv.empty() ||
        dst > 0xFF || src > 0xFF || flags > 0x03 || seq > 0x0F || payload.size() > 0xFF) {
        error = "ERR BadRequest";
        return false;
    }
    payload_storage = std::move(payload);
    out.dst = static_cast<uint8_t>(dst);
    out.src = static_cast<uint8_t>(src);
    out.response = (flags & 0x01u) != 0;
    out.retry = (flags & 0x02u) != 0;
    out.seq = static_cast<uint8_t>(seq);
    out.len = static_cast<uint8_t>(payload_storage.size());
    out.payload = payload_storage.data();
    error.clear();
    return true;
}

std::string discard_line(omgp::link::Discard d) {
    using omgp::link::Discard;
    switch (d) {
    case Discard::BadCrc:
        return "ERR BadCrc";
    case Discard::BadLength:
        return "ERR BadLength";
    case Discard::BadEscape:
        return "ERR BadEscape";
    case Discard::TooLong:
        return "ERR TooLong";
    case Discard::ReservedAddress:
        return "ERR ReservedAddress";
    case Discard::COUNT:
        break;
    }
    return "ERR ?"; // unreachable for valid enumerators; keeps -Wreturn-type quiet everywhere
}

std::string link_status_line(omgp::link::Status s) {
    return std::string("ERR ") + omgp::link::status_name(s);
}

bool encode_frame_line(const std::string& canonical, std::vector<uint8_t>& out,
                       std::string& error) {
    omgp::link::FrameFields f{};
    std::vector<uint8_t> payload;
    if (!parse_frame_line(canonical, f, payload, error))
        return false;
    uint8_t wire[omgp::link::kMaxWire];
    size_t written = 0;
    const omgp::link::Status st = omgp::link::encode_frame(f, wire, sizeof wire, written);
    if (st != omgp::link::Status::Ok) {
        error = link_status_line(st);
        return false;
    }
    out.assign(wire, wire + written);
    error.clear();
    return true;
}

std::string fdec_line(const uint8_t* data, size_t len) {
    omgp::link::Deframer d;
    omgp::link::FrameView view{};
    omgp::link::DeframerStats before = d.stats();
    for (size_t i = 0; i < len; ++i) {
        if (d.feed(data[i], view))
            return "OK " + render_frame(view.f);
        const omgp::link::DeframerStats& after = d.stats();
        for (size_t r = 0; r < static_cast<size_t>(omgp::link::Discard::COUNT); ++r) {
            if (after.discarded[r] != before.discarded[r])
                return discard_line(static_cast<omgp::link::Discard>(r));
        }
        before = after;
    }
    return "ERR BadRequest"; // bytes ran out with no frame delivered and no discard counted
}

std::string fstream_lines(const uint8_t* data, size_t len) {
    omgp::link::Deframer d;
    omgp::link::FrameView view{};
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        if (d.feed(data[i], view))
            out += "OK " + render_frame(view.f) + "\n";
    }
    const omgp::link::DeframerStats& stats = d.stats();
    uint32_t discards = 0;
    for (size_t r = 0; r < static_cast<size_t>(omgp::link::Discard::COUNT); ++r)
        discards += stats.discarded[r];
    out += "END " + dec(discards);
    return out;
}

std::string fenc_response(const std::string& canonical) {
    std::vector<uint8_t> out;
    std::string error;
    if (!encode_frame_line(canonical, out, error))
        return error;
    return "OK " + hex_lower(out.data(), out.size());
}

std::string fstream_response(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (!parse_hex(hex, bytes))
        return "ERR BadRequest\nEND 0";
    return fstream_lines(bytes.data(), bytes.size());
}

} // namespace canon
} // namespace omgp

// ================================================================================================
// Descriptors (spec 001 US3, T050). Mirrors the descriptor half of tools/refimpl/canonical.py.
// ================================================================================================

#include "l3/l3_descriptor.hpp"

namespace omgp {
namespace canon {

using namespace omgp::l3;

namespace {

std::string sv(Str s) {
    return quote_str(s.data, s.len);
}

std::string render_record(const RecordView& v) {
    const uint8_t t = v.type;
    if (t == TLV_PROTOCOL) {
        ProtocolRec r;
        decode_protocol(v, r);
        return "PROTOCOL major=" + dec(r.major) + " minor=" + dec(r.minor);
    }
    if (t == TLV_MODULE_TYPE)
        return "MODULE_TYPE mt=" + name_or_hex(names::MODULE_TYPE_TABLE, v.value[0]);
    if (t == TLV_NAME)
        return "NAME s=" + quote_str(v.value, v.len);
    if (t == TLV_MANUFACTURER)
        return "MANUFACTURER s=" + quote_str(v.value, v.len);
    if (t == TLV_MODEL_ID) {
        ModelIdRec r;
        decode_model_id(v, r);
        return "MODEL_ID model=" + hex4(r.vendor_model) + " hw=" + hex4(r.hw_rev) +
               " fw=" + hex4(r.fw_rev);
    }
    if (t == TLV_SERIAL)
        return "SERIAL s=" + quote_str(v.value, v.len);
    if (t == TLV_CHANNEL) {
        ChannelRec r;
        decode_channel(v, r);
        return "CHANNEL idx=" + dec(r.index) + " s=" + sv(r.name);
    }
    if (t == TLV_SWITCHING) {
        SwitchingRec r;
        decode_switching(v, r);
        return "SWITCHING flags=" + hex2(r.flags) + " settle_ms=" + dec(r.settle_ms);
    }
    if (t == TLV_PARAM) {
        ParamRec r;
        decode_param(v, r);
        return "PARAM id=" + dec(r.param_id) + " scope=" + hex2(r.scope) +
               " kind=" + name_or_hex(names::PARAM_KIND_TABLE, r.kind) +
               " default=" + dec(r.default_value) + " s=" + sv(r.name);
    }
    if (t == TLV_PARAM_ENUM) {
        ParamEnumRec r;
        decode_param_enum(v, r);
        return "PARAM_ENUM id=" + dec(r.param_id) + " idx=" + dec(r.index) + " s=" + sv(r.label);
    }
    if (t == TLV_AUDIO) {
        AudioRec r;
        decode_audio(v, r);
        return "AUDIO io=" + hex2(r.io_flags) + " input_mode=" + dec(r.input_mode) +
               " in_max=" + dec(r.in_max_mvrms) + " out_max=" + dec(r.out_max_mvrms);
    }
    if (t == TLV_POWER_LV) {
        PowerLvRec r;
        decode_power_lv(v, r);
        return "POWER_LV p15=" + dec(r.p15_ma) + " n15=" + dec(r.n15_ma) + " p9=" + dec(r.p9_ma) +
               " p5=" + dec(r.p5_ma);
    }
    if (t == TLV_POWER_TUBE) {
        PowerTubeRec r;
        decode_power_tube(v, r);
        return "POWER_TUBE class=" + dec(r.power_class) + " tubes=" + dec(r.tubes) +
               " sections=" + dec(r.sections) + " heater_nom=" + dec(r.heater_nom_ma) +
               " heater_max=" + dec(r.heater_max_ma) + " bplus_v=" + dec(r.bplus_nom_v) +
               " bplus_exp=" + dec(r.bplus_exp_ma) + " bplus_max=" + dec(r.bplus_max_ma);
    }
    if (t == TLV_VENDOR) {
        VendorRec r;
        decode_vendor(v, r);
        return "VENDOR vendor=" + hex4(r.vendor_id) + " data=" + hex_lower(r.data.data, r.data.len);
    }
    return "UNKNOWN type=" + hex2(t) + " data=" + hex_lower(v.value, v.len);
}

// Split on '|' outside quotes, then each chunk into NAME + key=value tokens (quotes honoured).
bool split_records(const std::string& text, std::vector<std::string>& out) {
    std::string cur;
    bool quoted = false, esc = false;
    for (char ch : text) {
        if (quoted) {
            cur.push_back(ch);
            if (esc)
                esc = false;
            else if (ch == '\\')
                esc = true;
            else if (ch == '"')
                quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            cur.push_back(ch);
        } else if (ch == '|') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (quoted)
        return false;
    out.push_back(cur);
    std::vector<std::string> trimmed;
    for (auto& s : out) {
        size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
        if (a != std::string::npos)
            trimmed.push_back(s.substr(a, b - a + 1));
    }
    out = trimmed;
    return true;
}

bool record_tokens(const std::string& chunk, std::string& name, Tokens& t) {
    std::vector<std::string> toks;
    std::string cur;
    bool quoted = false, esc = false;
    for (char ch : chunk) {
        if (quoted) {
            cur.push_back(ch);
            if (esc)
                esc = false;
            else if (ch == '\\')
                esc = true;
            else if (ch == '"')
                quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            cur.push_back(ch);
        } else if (ch == ' ') {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        toks.push_back(cur);
    if (toks.empty())
        return false;
    name = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) {
        const size_t eq = toks[i].find('=');
        if (eq == std::string::npos || t.kv.count(toks[i].substr(0, eq)))
            return false;
        t.kv[toks[i].substr(0, eq)] = toks[i].substr(eq + 1);
    }
    return true;
}

bool take_str(Tokens& t, const char* key, std::vector<uint8_t>& out) {
    std::string s;
    if (!t.take(key, s) || !unquote_str(s, out) || out.size() > 0xFF) {
        t.bad = true;
        return false;
    }
    return true;
}

// Parses one canonical record and appends it to the writer. Returns Ok/codec Status; sets
// t.bad (→ BadRequest) on malformed text.
Status add_record(DescriptorWriter& w, const std::string& chunk, Tokens& t) {
    std::string name;
    if (!record_tokens(chunk, name, t)) {
        t.bad = true;
        return Status::Ok;
    }
    unsigned a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
    std::vector<uint8_t> s1;
    Status st = Status::Ok;
    if (name == "PROTOCOL") {
        if (!(t.take_uint("major", a) && t.take_uint("minor", b)))
            return Status::Ok;
        st = w.add_protocol(ProtocolRec{static_cast<uint8_t>(a), static_cast<uint8_t>(b)});
    } else if (name == "MODULE_TYPE") {
        if (!t.take_named("mt", names::MODULE_TYPE_TABLE, a))
            return Status::Ok;
        st = w.add_module_type(ModuleTypeRec{static_cast<uint8_t>(a)});
    } else if (name == "NAME" || name == "MANUFACTURER" || name == "SERIAL") {
        if (!take_str(t, "s", s1))
            return Status::Ok;
        const Str s{s1.data(), static_cast<uint8_t>(s1.size())};
        st = name == "NAME"           ? w.add_name(s)
             : name == "MANUFACTURER" ? w.add_manufacturer(s)
                                      : w.add_serial(s);
    } else if (name == "MODEL_ID") {
        if (!(t.take_uint("model", a) && t.take_uint("hw", b) && t.take_uint("fw", c)))
            return Status::Ok;
        st = w.add_model_id(ModelIdRec{static_cast<uint16_t>(a), static_cast<uint16_t>(b),
                                       static_cast<uint16_t>(c)});
    } else if (name == "CHANNEL") {
        if (!(t.take_uint("idx", a) && take_str(t, "s", s1)))
            return Status::Ok;
        st = w.add_channel(
            ChannelRec{static_cast<uint8_t>(a), Str{s1.data(), static_cast<uint8_t>(s1.size())}});
    } else if (name == "SWITCHING") {
        if (!(t.take_uint("flags", a) && t.take_uint("settle_ms", b)))
            return Status::Ok;
        st = w.add_switching(SwitchingRec{static_cast<uint8_t>(a), static_cast<uint16_t>(b)});
    } else if (name == "PARAM") {
        if (!(t.take_uint("id", a) && t.take_uint("scope", b) &&
              t.take_named("kind", names::PARAM_KIND_TABLE, c) && t.take_uint("default", d) &&
              take_str(t, "s", s1)))
            return Status::Ok;
        if (d > 0xFFFF)
            return Status::OutOfRange;
        st = w.add_param(ParamRec{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                  static_cast<uint8_t>(c), static_cast<uint16_t>(d),
                                  Str{s1.data(), static_cast<uint8_t>(s1.size())}});
    } else if (name == "PARAM_ENUM") {
        if (!(t.take_uint("id", a) && t.take_uint("idx", b) && take_str(t, "s", s1)))
            return Status::Ok;
        st = w.add_param_enum(ParamEnumRec{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                           Str{s1.data(), static_cast<uint8_t>(s1.size())}});
    } else if (name == "AUDIO") {
        if (!(t.take_uint("io", a) && t.take_uint("input_mode", b) && t.take_uint("in_max", c) &&
              t.take_uint("out_max", d)))
            return Status::Ok;
        st = w.add_audio(AudioRec{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                  static_cast<uint16_t>(c), static_cast<uint16_t>(d)});
    } else if (name == "POWER_LV") {
        if (!(t.take_uint("p15", a) && t.take_uint("n15", b) && t.take_uint("p9", c) &&
              t.take_uint("p5", d)))
            return Status::Ok;
        st = w.add_power_lv(PowerLvRec{static_cast<uint16_t>(a), static_cast<uint16_t>(b),
                                       static_cast<uint16_t>(c), static_cast<uint16_t>(d)});
    } else if (name == "POWER_TUBE") {
        if (!(t.take_uint("class", a) && t.take_uint("tubes", b) && t.take_uint("sections", c) &&
              t.take_uint("heater_nom", d) && t.take_uint("heater_max", e) &&
              t.take_uint("bplus_v", f) && t.take_uint("bplus_exp", g) &&
              t.take_uint("bplus_max", h)))
            return Status::Ok;
        st = w.add_power_tube(PowerTubeRec{static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                           static_cast<uint8_t>(c), static_cast<uint16_t>(d),
                                           static_cast<uint16_t>(e), static_cast<uint16_t>(f),
                                           static_cast<uint8_t>(g), static_cast<uint8_t>(h)});
    } else if (name == "VENDOR") {
        if (!(t.take_uint("vendor", a) && t.take_hex("data", s1)))
            return Status::Ok;
        if (s1.size() > 253)
            return Status::OutOfRange;
        st = w.add_vendor(
            VendorRec{static_cast<uint16_t>(a), Bytes{s1.data(), static_cast<uint8_t>(s1.size())}});
    } else if (name == "UNKNOWN") {
        if (!(t.take_uint("type", a) && t.take_hex("data", s1)))
            return Status::Ok;
        if (s1.size() > 0xFF)
            return Status::OutOfRange;
        st = w.add_raw(static_cast<uint8_t>(a), s1.data(), static_cast<uint8_t>(s1.size()));
    } else {
        t.bad = true;
        return Status::Ok;
    }
    if (!t.kv.empty())
        t.bad = true;
    return t.bad ? Status::Ok : st;
}

} // namespace

std::string quote_str(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string q = "\"";
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b == '"')
            q += "\\\"";
        else if (b == '\\')
            q += "\\\\";
        else if (b >= 0x20 && b <= 0x7E)
            q.push_back(static_cast<char>(b));
        else {
            q += "\\x";
            q.push_back(digits[b >> 4]);
            q.push_back(digits[b & 0x0F]);
        }
    }
    q.push_back('"');
    return q;
}

bool unquote_str(const std::string& q, std::vector<uint8_t>& out) {
    out.clear();
    if (q.size() < 2 || q.front() != '"' || q.back() != '"')
        return false;
    for (size_t i = 1; i + 1 < q.size(); ++i) {
        const char c = q[i];
        if (c != '\\') {
            out.push_back(static_cast<uint8_t>(c));
            continue;
        }
        if (i + 2 >= q.size())
            return false;
        const char n = q[i + 1];
        if (n == '"' || n == '\\') {
            out.push_back(static_cast<uint8_t>(n));
            ++i;
        } else if (n == 'x' && i + 4 < q.size()) {
            std::vector<uint8_t> byte;
            if (!parse_hex(q.substr(i + 2, 2), byte) || byte.size() != 1)
                return false;
            out.push_back(byte[0]);
            i += 3;
        } else {
            return false;
        }
    }
    return true;
}

std::string validate_line(const uint8_t* blob, size_t len) {
    DescriptorReport r;
    const Status st = validate_descriptor(blob, len, r);
    if (st == Status::Ok)
        return "OK skipped=" + dec(r.skipped_unknown) + " channels=" + dec(r.channel_count) +
               " params=" + dec(r.param_count);
    return status_line(st) + " type=" + hex2(r.type) + " offset=" + dec(r.offset);
}

std::string render_descriptor(const uint8_t* blob, size_t len) {
    DescriptorReport r;
    const Status st = validate_descriptor(blob, len, r);
    if (st != Status::Ok)
        return status_line(st);
    std::string out;
    RecordCursor c(blob, len);
    RecordView v;
    bool first = true;
    while (!c.at_end() && c.next(v) == Status::Ok) {
        if (!first)
            out += " | ";
        first = false;
        out += render_record(v);
    }
    return out;
}

bool encode_descriptor(const std::string& canonical, std::vector<uint8_t>& out,
                       std::string& error) {
    std::vector<std::string> chunks;
    if (!split_records(canonical, chunks)) {
        error = "ERR BadRequest";
        return false;
    }
    std::vector<uint8_t> buf(DESC_MAX_BYTES);
    DescriptorWriter w(buf.data(), buf.size());
    for (const auto& chunk : chunks) {
        Tokens t;
        const Status st = add_record(w, chunk, t);
        if (t.bad) { // malformed text: unknown record, missing/extra/unparsable keys
            error = "ERR BadRequest";
            return false;
        }
        if (st != Status::Ok) {
            error = status_line(st);
            return false;
        }
    }
    DescriptorReport r;
    const Status fin = w.finish(r);
    if (fin != Status::Ok) {
        error = status_line(fin);
        return false;
    }
    out.assign(buf.begin(), buf.begin() + static_cast<long>(w.size()));
    error.clear();
    return true;
}

} // namespace canon
} // namespace omgp
