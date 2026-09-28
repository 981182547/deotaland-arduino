#include "audio_downlink.h"
#include "agent_link_platform.h"
#include "adpcm.h"
#include <cstring>

namespace agentlink {
namespace audio {
namespace {
PcmSink s_pcm = nullptr;
agent_playback_write_fn s_write = nullptr;
agent_playback_queued_fn s_queued = nullptr;
void* s_playback_ctx = nullptr;
agent_playback_stats_t s_stats = {};
bool s_in_callback = false;
EventSink s_event = nullptr;
ReliableEventSink s_reliable_event = nullptr;
bool s_flow_pending = false;
uint8_t s_flow[4] = {};
uint8_t s_codec = kCodecPcm16;
bool s_armed = false;
bool s_finishing = false;
bool s_paused = false;
uint8_t s_repeats = 0;
uint8_t s_tail_resumes = 0;
uint32_t s_buffer_ms = 400;
uint32_t s_base_ms = 0;
uint64_t s_delivered = 0;
uint32_t s_last_tick_ms = 0;
uint32_t s_last_event_ms = 0;
uint8_t s_block[adpcm::kBlockBytes] = {};
// This single aligned slot is shared by decoded ADPCM and staged PCM. Keeping
// the unaccepted suffix avoids both heap work and a second PCM copy on retry.
int16_t s_decoded[adpcm::kSamplesPerBlock] = {};
size_t s_pending_length = 0;
size_t s_pending_offset = 0;
size_t s_block_length = 0;
uint8_t s_pcm_tail = 0;
bool s_have_pcm_tail = false;
uint32_t s_generation = 0;

uint32_t EstimatedMs(uint32_t now) {
    const uint32_t elapsed = now - s_base_ms;
    const uint64_t played_bytes = static_cast<uint64_t>(elapsed) * 32u;
    if (s_delivered <= played_bytes) {
        s_base_ms = now;
        s_delivered = 0;
        return 0;
    }
    const uint64_t backlog = (s_delivered - played_bytes) / 32u;
    return backlog > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(backlog);
}
uint32_t BufferedMs(uint32_t now) {
    uint64_t queued_bytes = 0;
    if (s_queued) {
        s_in_callback = true;
        queued_bytes = s_queued(s_playback_ctx);
        s_in_callback = false;
    } else queued_bytes = static_cast<uint64_t>(EstimatedMs(now)) * 32u;
    // Saturate before adding to support a defensive SIZE_MAX watermark.
    const uint64_t cap = static_cast<uint64_t>(UINT32_MAX) * 32u;
    if (queued_bytes >= cap) return UINT32_MAX;
    queued_bytes += s_pending_length - s_pending_offset;
    return queued_bytes >= cap ? UINT32_MAX : static_cast<uint32_t>(queued_bytes / 32u);
}
void FlushFlow() {
    if (!s_flow_pending) return;
    const uint32_t generation=s_generation;
    const bool was_in_callback=s_in_callback;
    s_in_callback=true;
    esp_err_t result=ESP_OK;
    if (s_reliable_event) {
        result=s_reliable_event(0x20,s_flow,sizeof(s_flow));
    } else if (s_event) s_event(0x20,s_flow,sizeof(s_flow));
    s_in_callback=was_in_callback;
    if (result != ESP_OK) return;
    if (generation != s_generation) return;
    s_flow_pending = false;
}
void SendFlow(bool paused, uint32_t buffered) {
    const uint16_t ms = buffered > 65535u ? 65535u : static_cast<uint16_t>(buffered);
    s_flow[0]=static_cast<uint8_t>(paused ? 1 : 0);
    s_flow[1]=static_cast<uint8_t>(ms); s_flow[2]=static_cast<uint8_t>(ms>>8); s_flow[3]=0;
    s_flow_pending=true; // Coalesce an obsolete pause into the latest resume.
    FlushFlow();
}
void UpdateFlow() {
    const uint32_t now = agent_link_now_ms();
    if (!s_armed) {
        if (s_tail_resumes && now - s_last_event_ms >= 200) {
            --s_tail_resumes;
            s_last_event_ms = now;
            SendFlow(false, 0);
        } else FlushFlow();
        return;
    }
    const uint32_t generation = s_generation;
    const uint32_t ms = BufferedMs(now);
    if (generation != s_generation) return;
    const bool blocked = s_pending_length != s_pending_offset;
    const uint32_t high = s_buffer_ms * 5u / 8u;
    const uint32_t low = s_buffer_ms * 3u / 16u;
    const uint32_t pause_ms = high < 5000 ? high : 5000;
    const uint32_t resume_ms = low < 1500 ? low : 1500;
    bool send = false;
    if (!s_paused && (blocked || ms >= pause_ms)) { s_paused = true; s_repeats = 2; send = true; }
    else if (s_paused && !blocked && ms <= resume_ms) { s_paused = false; s_repeats = 2; send = true; }
    else if (now - s_last_tick_ms >= 200) {
        if (s_repeats) { --s_repeats; send = true; }
        else if (now - s_last_event_ms >= 2000) send = s_paused || ms < 256;
    }
    if (send || now - s_last_tick_ms >= 200) s_last_tick_ms = now;
    if (send) { s_last_event_ms = now; SendFlow(s_paused, ms); }
    else FlushFlow();
}
void ClearInput() {
    s_have_pcm_tail = false;
    s_block_length = 0;
    s_pending_length = s_pending_offset = 0;
}
void SetPending(size_t bytes) {
    s_pending_offset = 0;
    s_pending_length = bytes;
    if (bytes > s_stats.max_pending_pcm_bytes) s_stats.max_pending_pcm_bytes = bytes;
}
// Exactly one application callback. The caller decides when another retry is
// allowed; do not turn a short accept into a busy loop in the Arduino task.
void DrainPending() {
    const size_t bytes = s_pending_length - s_pending_offset;
    if (!bytes || s_in_callback) return;
    const uint32_t generation = s_generation;
    const uint8_t* pcm = reinterpret_cast<const uint8_t*>(s_decoded) + s_pending_offset;
    EstimatedMs(agent_link_now_ms());
    size_t accepted = bytes;
    s_in_callback = true;
    ++s_stats.sink_calls;
    if (s_write) accepted = s_write(pcm, bytes, s_playback_ctx);
    else if (s_pcm) s_pcm(pcm, bytes);
    s_in_callback = false;
    if (generation != s_generation) return;
    if (accepted > bytes || (accepted & 1u)) {
        // An invalid accept cannot safely advance a PCM boundary. Retain the
        // suffix and expose the contract violation instead of overrunning it.
        ++s_stats.invalid_sink_returns;
        accepted = 0;
    }
    s_delivered += accepted;
    s_stats.accepted_pcm_bytes += accepted;
    s_pending_offset += accepted;
    if (accepted < bytes) ++s_stats.backpressure_events;
    if (s_pending_offset == s_pending_length) s_pending_offset = s_pending_length = 0;
}
void CompleteFinish() {
    if (!s_finishing || s_pending_length || !s_armed) return;
    // Keep the END gate closed until the next Arm(). Disarm() is also the
    // explicit cancellation API and therefore normally clears that gate.
    const uint32_t generation=s_generation;
    Disarm();
    if (s_generation == generation + 1u) s_finishing = true;
}
}  // namespace

void SetSinks(PcmSink pcm, EventSink event) {
    ++s_generation; ClearInput(); s_finishing=false;
    s_pcm = pcm; s_event = event; s_reliable_event=nullptr; s_flow_pending=false;
    s_write=nullptr; s_queued=nullptr; s_playback_ctx=nullptr; s_stats={};
}
void SetReliableEventSink(ReliableEventSink event) { s_reliable_event=event; }
void SetPlaybackSink(agent_playback_write_fn write, agent_playback_queued_fn queued, void* ctx) {
    ++s_generation; ClearInput(); s_finishing=false;
    s_write=write; s_queued=write ? queued : nullptr; s_playback_ctx=ctx; s_stats={};
    s_delivered=0; s_base_ms=agent_link_now_ms();
}
void GetStats(agent_playback_stats_t* stats) {
    if (!stats) return;
    *stats=s_stats;
    stats->pending_pcm_bytes=s_pending_length-s_pending_offset;
}
bool SetCodec(uint8_t codec) {
    if (codec > kCodecImaAdpcm) return false;
    if (!s_armed && s_codec != codec) {
        ++s_generation;
        ClearInput();
        s_codec = codec;
    }
    return true;  // A valid mid-stream codec change is ignored, matching upstream.
}
uint8_t CodecValue() { return s_codec; }
void Arm() {
    ++s_generation;
    ClearInput(); s_finishing=false;
    s_paused = false;
    s_repeats = 0;
    s_tail_resumes = 0;
    s_flow_pending = false;
    s_base_ms = s_last_tick_ms = s_last_event_ms = agent_link_now_ms();
    s_delivered = 0;
    s_armed = true;
}
void Disarm() {
    ++s_generation;
    ClearInput(); s_finishing=false;
    const bool resume = s_armed && s_paused;
    s_armed = false;
    s_paused = false;
    s_repeats = 0;
    s_block_length = 0;
    if (resume) {
        s_tail_resumes = 2;
        s_last_event_ms = agent_link_now_ms();
        SendFlow(false, 0);
    }
}
bool IsArmed() { return s_armed; }
size_t Feed(const uint8_t* data, size_t length) {
    if (!data || !length || s_in_callback || s_finishing) return 0;
    // Tick owns retries for retained data. Feed never calls a rejecting sink
    // again simply because another transport chunk is presented in this poll.
    if (s_pending_length) return 0;
    const size_t initial_length = length;
    const uint32_t generation = s_generation;
    if (s_codec == kCodecPcm16) {
        // Neither transport fragment boundaries nor incoming pointer alignment
        // are PCM sample boundaries. Keep a dangling byte and stage aligned pairs.
        while (length) {
            uint8_t* const stage = reinterpret_cast<uint8_t*>(s_decoded);
            size_t staged = 0;
            if (s_have_pcm_tail) {
                stage[staged++] = s_pcm_tail;
                stage[staged++] = *data++;
                --length;
                s_have_pcm_tail = false;
            }
            const size_t room = 512 - staged;
            const size_t even = length & ~static_cast<size_t>(1);
            const size_t take = even < room ? even : room;
            if (take) { std::memcpy(stage + staged, data, take); data += take; length -= take; staged += take; }
            if (length == 1 && staged < 512) {
                s_pcm_tail = *data++; --length; s_have_pcm_tail = true;
            }
            if (staged) { SetPending(staged); DrainPending(); }
            if (generation != s_generation) return initial_length - length;
            if (staged) UpdateFlow();
            if (generation != s_generation || s_pending_length) return initial_length - length;
        }
        return initial_length;
    }
    while (length) {
        const size_t room = sizeof(s_block) - s_block_length;
        const size_t take = length < room ? length : room;
        std::memcpy(s_block + s_block_length, data, take);
        s_block_length += take;
        data += take;
        length -= take;
        if (s_block_length == sizeof(s_block)) {
            adpcm::DecodeBlock(s_block, s_decoded);
            s_block_length = 0;
            SetPending(sizeof(s_decoded)); DrainPending();
            if (generation != s_generation) return initial_length - length;
            UpdateFlow();
            if (generation != s_generation || s_pending_length) return initial_length - length;
        }
    }
    return initial_length;
}
void Finish() {
    if (s_finishing) return;
    s_finishing = true;
    s_stats.truncated_input_bytes += static_cast<uint32_t>(s_block_length + (s_have_pcm_tail ? 1 : 0));
    s_block_length=0; s_have_pcm_tail=false;
    CompleteFinish();
}
bool IsDrained() { return !s_pending_length && !s_block_length && !s_have_pcm_tail; }
size_t PendingBytes() { return s_pending_length - s_pending_offset; }
void SetBufferMs(uint32_t playable_ms) {
    s_buffer_ms = playable_ms < 120 ? 120 : (playable_ms > 120000 ? 120000 : playable_ms);
}
void OnLinkState(bool connected) {
    if (connected) return;
    // A disconnect is cancellation, not normal end-of-playback. Do not emit
    // Disarm's resume to a newly connected peer while dispatching old state.
    ++s_generation;
    s_armed=false; s_paused=false; s_repeats=0;
    ClearInput(); s_finishing=false; s_flow_pending=false;
    s_tail_resumes = 0;  // The old peer is gone; never send its recovery events to a new peer.
    s_codec = kCodecPcm16;
    s_delivered = 0;
}
void Tick() {
    if (s_in_callback) return;
    const uint32_t generation = s_generation;
    DrainPending();
    if (generation != s_generation) return;
    if (s_finishing && !s_pending_length && s_armed) {
        CompleteFinish(); // Disarm may emit one resume; never retry it twice here.
        return;
    }
    UpdateFlow();
}
}  // namespace audio
}  // namespace agentlink

extern "C" void agent_link_playback_set_buffer_ms(uint32_t playable_ms) {
    agentlink::audio::SetBufferMs(playable_ms);
}
