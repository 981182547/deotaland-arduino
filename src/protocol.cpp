// Control‑plane protocol frame encoding/decoding implementation
#include "protocol.h"
#include <cstring>

namespace agentlink {

bool ParseFrameView(const uint8_t* data, size_t len, FrameView& out) {
    if (!data || len < kHeaderSize) return false;

    const uint8_t version = data[0];
    if (version != kVersion) return false;

    const uint8_t raw_type = data[1];
    const uint8_t msg_type = raw_type & 0x7F;
    if (msg_type < kMsgCommand || msg_type > kMsgEvent) return false;

    const uint16_t payload_len = static_cast<uint16_t>(data[4] | (data[5] << 8));  // little‑endian
    if (kHeaderSize + payload_len != len) return false;  // length must be self‑consistent

    out.msg_type   = msg_type;
    out.command_id = data[2];
    out.sequence   = data[3];
    out.encrypted  = (raw_type & kMsgEncrypted) != 0;
    out.payload.bytes = data + kHeaderSize;
    out.payload.length = payload_len;
    //Note: encryption is not implemented; the encrypted flag is retained
    return true;
}

bool ParseFrame(const uint8_t* data, size_t len, Frame& out) {
    FrameView view;
    if (!ParseFrameView(data,len,view)) return false;
    out.msg_type=view.msg_type; out.command_id=view.command_id;
    out.sequence=view.sequence; out.encrypted=view.encrypted;
    out.payload.assign(view.payload.data(),view.payload.data()+view.payload.size());
    return true;
}
static void Header(uint8_t* out, uint8_t type, uint8_t id, uint8_t seq, size_t len) {
    out[0]=kVersion; out[1]=type; out[2]=id; out[3]=seq;
    out[4]=static_cast<uint8_t>(len); out[5]=static_cast<uint8_t>(len>>8);
}
size_t EncodeEvent(uint8_t* out, size_t capacity, uint8_t id, const uint8_t* payload, size_t len) {
    if (!out || (len && !payload) || len>UINT16_MAX || capacity<kHeaderSize || len>capacity-kHeaderSize) return 0;
    if (len) std::memmove(out+kHeaderSize,payload,len);
    Header(out,kMsgEvent,id,0,len);
    return kHeaderSize+len;
}
size_t EncodeResponse(uint8_t* out, size_t capacity, uint8_t id, uint8_t seq,
                      uint8_t status, uint16_t error, const uint8_t* extra, size_t len) {
    if (!out || (len && !extra) || len>UINT16_MAX-4 || capacity<10 || len>capacity-10) return 0;
    if (len) std::memmove(out+10,extra,len);
    Header(out,kMsgResponse,id,seq,len+4);
    out[6]=id; out[7]=status; out[8]=static_cast<uint8_t>(error); out[9]=static_cast<uint8_t>(error>>8);
    return 10+len;
}

static void WriteHeader(std::vector<uint8_t>& f, uint8_t msg_type, uint8_t id,
                        uint8_t seq, uint16_t payload_len) {
    f.push_back(kVersion);
    f.push_back(msg_type);
    f.push_back(id);
    f.push_back(seq);
    f.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    f.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
}

std::vector<uint8_t> BuildResponse(uint8_t command_id, uint8_t sequence,
                                   uint8_t status, uint16_t error_code,
                                   const uint8_t* extra, size_t extra_len) {
    if ((extra_len && !extra) || extra_len > UINT16_MAX - 4) return {};
    const size_t ack = 4;  // acked_cmd + status + err(2, LE)
    const uint16_t payload_len = static_cast<uint16_t>(ack + extra_len);
    std::vector<uint8_t> f;
    f.reserve(kHeaderSize + payload_len);
    WriteHeader(f, kMsgResponse, command_id, sequence, payload_len);
    f.push_back(command_id);
    f.push_back(status);
    f.push_back(static_cast<uint8_t>(error_code & 0xFF));
    f.push_back(static_cast<uint8_t>((error_code >> 8) & 0xFF));
    if (extra && extra_len > 0) f.insert(f.end(), extra, extra + extra_len);
    return f;
}

std::vector<uint8_t> BuildEvent(uint8_t event_id, const uint8_t* payload, size_t len) {
    if ((len && !payload) || len > UINT16_MAX) return {};
    std::vector<uint8_t> f;
    f.reserve(kHeaderSize + len);
    WriteHeader(f, kMsgEvent, event_id, /*sequence=*/0, static_cast<uint16_t>(len));
    if (payload && len > 0) f.insert(f.end(), payload, payload + len);
    return f;
}

}  // namespace agentlink
