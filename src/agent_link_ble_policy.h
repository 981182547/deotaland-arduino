#pragma once
#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include "agent_link_stream.h"

namespace agentlink { namespace blepolicy {
// Reserve FIFO capacity, rather than reordering packets: START/END and ACK
// retain their wire order even when telemetry/manifest traffic fills the queue.
inline bool CriticalControl(const uint8_t* frame, size_t size) {
    if (!frame || size < 6) return false;
    const uint8_t type = frame[1] & 0x7f;
    if (type == 2) return true;
    return type == 3 && (frame[2] == 0x20 ||
                         (frame[2] >= 0x52 && frame[2] <= 0x55) ||
                         (frame[2] == 0x38 && size >= 17 && frame[6] >= 2));
}
inline size_t ControlQueueLimit(size_t slots, const uint8_t* frame, size_t size) {
    return CriticalControl(frame, size) || slots < 2 ? slots : slots-2;
}
// Return true only when the producer must wake the worker. An unchanged sample
// must preserve a pending subscription sync or failed-notification retry.
inline bool UpdateBatteryLevel(std::atomic<uint8_t>& level, std::atomic<bool>& pending,
                               uint8_t percent) {
    const uint8_t next = percent > 100 ? 100 : percent;
    if (level.exchange(next, std::memory_order_acq_rel) == next) return false;
    pending.store(true, std::memory_order_release);
    return true;
}
inline size_t VoicePayload(size_t mtu) {
    const size_t payload = mtu > 18 ? mtu - 18 : 0;
    return (payload < 205 ? payload : 205) & ~size_t(1);
}
inline size_t VoiceDrainBytes(size_t available, size_t payload, size_t capacity) {
    if (!payload || capacity < payload) return 0;
    // Full notifications may span caller write boundaries. Flush a short tail
    // immediately when it is all that remains; no hidden coalescing delay.
    if (available < payload) return available & ~size_t(1);
    const size_t count = available < capacity ? available : capacity;
    return count - count % payload;
}
template<class State>
void StreamStatus(const State& stream, size_t capacity, bool ready, uint32_t session,
                   agent_stream_status_t* out) {
    const bool active = stream.active.load(std::memory_order_acquire);
    const bool draining = stream.end_req.load(std::memory_order_acquire);
    *out = {};
    out->accepted_bytes = stream.accepted_bytes.load();
    out->sent_bytes = stream.sent_bytes.load();
    out->rejected_writes = stream.rejected_writes.load();
    out->last_error = stream.last_error.load();
    if (active) {
        out->state = draining ? AGENT_STREAM_DRAINING : AGENT_STREAM_OPEN;
        // Include the worker's in-flight chunk as well as bytes in the ring.
        out->queued_bytes = out->accepted_bytes >= out->sent_bytes
                              ? out->accepted_bytes-out->sent_bytes : 0;
        const size_t queued = stream.queued_bytes.load();
        if (!draining && queued <= capacity) out->writable_bytes = capacity-queued;
    } else if (session) {
        out->state = out->last_error == ESP_OK ? AGENT_STREAM_COMPLETED : AGENT_STREAM_FAILED;
    } else out->state = ready ? AGENT_STREAM_IDLE : AGENT_STREAM_UNAVAILABLE;
}
} }
