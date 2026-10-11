#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "agent_link_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Copy PCM16 mono 16 kHz into an application-owned playback queue without
 * waiting. Return the number of bytes accepted: an even value <= bytes.
 * Returning 0 retains all data for a later poll. The pointer is borrowed only
 * for this call; copy accepted bytes before returning. Called from poll(). */
typedef size_t (*agent_playback_write_fn)(const uint8_t* pcm16, size_t bytes, void* ctx);
/** Return unplayed PCM bytes currently held by the application (including
 * DMA buffers, when measurable). Must not wait. NULL uses elapsed-time
 * estimation; a real watermark is recommended for reliable flow control. */
typedef size_t (*agent_playback_queued_fn)(void* ctx);

typedef struct {
    agent_playback_write_fn write;
    agent_playback_queued_fn queued;
    void* ctx;
} agent_playback_config_t;

typedef struct {
    uint64_t accepted_pcm_bytes;
    uint32_t sink_calls;
    uint32_t backpressure_events;
    uint32_t invalid_sink_returns;
    uint32_t truncated_input_bytes;
    size_t pending_pcm_bytes;
    size_t max_pending_pcm_bytes;
} agent_playback_stats_t;

/** Select the nonblocking playback sink after successful init/begin;
 * NULL restores legacy on_audio_out.
 * The configuration is copied. The ctx object must outlive its registration.
 * Configure outside an active downlink stream. on_audio_end means every
 * complete PCM sample has been accepted, not that physical playback ended. */
esp_err_t agent_link_playback_configure(const agent_playback_config_t* config);
/** Snapshot counters for the currently installed sink, from the poll thread.
 * Counters reset when the sink is replaced, not at each audio session. */
void agent_link_playback_get_stats(agent_playback_stats_t* stats);
/** True while a protocol TTS reply is armed or its END is still draining the
 * core's retained PCM. False after on_audio_end, even if application/DMA audio
 * is still playing. Read on the poll thread; use this for reply cancellation,
 * rather than treating physical speaker occupancy as a new reply session. */
bool agent_link_playback_active(void);

#ifdef __cplusplus
}
#endif
