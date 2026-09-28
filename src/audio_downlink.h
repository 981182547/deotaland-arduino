#pragma once
#include <stddef.h>
#include <stdint.h>
#include "agent_link_platform.h"
#include "agent_link_playback.h"

namespace agentlink {
namespace audio {
enum Codec : uint8_t { kCodecPcm16 = 0, kCodecImaAdpcm = 1 };
using PcmSink = void (*)(const uint8_t* pcm16, size_t bytes);
using EventSink = void (*)(uint8_t event_id, const uint8_t* payload, size_t bytes);
using ReliableEventSink = esp_err_t (*)(uint8_t event_id, const uint8_t* payload, size_t bytes);
// All methods are cooperative: call only from the agent_link_poll() context.
void SetSinks(PcmSink pcm, EventSink event);
void SetReliableEventSink(ReliableEventSink event);
void SetPlaybackSink(agent_playback_write_fn write, agent_playback_queued_fn queued, void* ctx);
void GetStats(agent_playback_stats_t* stats);
bool SetCodec(uint8_t codec);
uint8_t CodecValue();
void Arm();
void Disarm();
bool IsArmed();
// Returns consumed input bytes; the caller must retain and retry the suffix.
// At most one decoded ADPCM block / 512 PCM bytes are held internally.
size_t Feed(const uint8_t* data, size_t length);
// Normal END drains retained PCM before disarming. Incomplete final input is
// counted and discarded. Arm/Disarm/disconnect cancel retained old-session data.
void Finish();
bool IsDrained();
size_t PendingBytes();
void SetBufferMs(uint32_t playable_ms);
void OnLinkState(bool connected);
// At most one flow-control event per tick. No timer task, waits or allocations.
void Tick();
}  // namespace audio
}  // namespace agentlink
