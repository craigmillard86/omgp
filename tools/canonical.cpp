// Host-only canonical-text codec (see canonical.hpp). Names come from the generated
// omgp_names.h; range markers (*_MIN/*_MAX) render as hex, matching the Python renderer.
#include "canonical.hpp"

#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"
#include "omgp_names.h"
#include "omgp_protocol.h"

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
    if (tok.empty())
        return false;
    char* end = nullptr;
    const unsigned long x = std::strtoul(tok.c_str(), &end, 0);
    if (end == nullptr || *end != '\0')
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

} // namespace canon
} // namespace omgp
