#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent_link.h"
#include "agent_link_ota.h"
#include "DeotalandImageUpload.h"

/**
 * Arduino-style facade over the stable C API in agent_link.h.
 *
 * Call poll() from every loop iteration. Hardware callbacks run on that same
 * thread. Transports enqueue data and never wait for network delivery.
 */
class DeotalandClass {
public:
    /** Initialize and start the selected transport. Call once from setup(). */
    esp_err_t begin(const agent_link_config_t& config);

    /**
     * Convenience BLE startup. Configuration strings and callbacks are copied.
     */
    esp_err_t begin(const char* device_name, uint32_t capabilities,
                    const agent_output_cb_t* output = nullptr,
                    agent_state_cb_t on_state = nullptr,
                    void* state_context = nullptr);

    void end();
    bool canBegin() const;
    /** Up to max_events RX slices; default cooperative budget is 2000 us. */
    size_t poll(size_t max_events = 4);
    /** Override time budget; 0 disables it. Callbacks cannot be preempted. */
    size_t poll(size_t max_events, uint32_t max_us);
    void reset();
    agent_link_stats_t stats() const;
    esp_err_t pushPrompt(const char* utf8);
    void setPlaybackBufferMs(uint32_t milliseconds);
    esp_err_t configurePlayback(const agent_playback_config_t* config);
    agent_playback_stats_t playbackStats() const;
    /** Protocol TTS is active; independent of the speaker's remaining DMA. */
    bool playbackActive() const;
    esp_err_t prepareStreams(uint32_t stream_mask);
    esp_err_t prepareOta();
    /** Optional OTA helpers. Set callbacks and inspect status from the same
     * thread as poll(); callbacks are dispatched by poll(), never the worker. */
    void setOtaCallback(agent_ota_progress_cb_t callback, void* context = nullptr);
    agent_ota_status_t otaStatus() const;
    bool otaBusy() const;
    bool otaSupported() const;
    bool otaNeedsReconnect() const;
    void abortOta();
    /** Call only after the board's own peripheral/health checks succeed. */
    esp_err_t markOtaValid();
    const char* runningVersion() const;
    esp_err_t streamStatus(agent_stream_t kind, agent_stream_status_t* out) const;
    esp_err_t openStream(agent_stream_t kind, const agent_stream_opts_t* opts, agent_stream_handle_t* out);
    esp_err_t writeStream(agent_stream_handle_t stream, const void* data, size_t bytes);
    esp_err_t closeStream(agent_stream_handle_t stream, bool complete = true);
    esp_err_t sendStream(agent_stream_t kind, const agent_stream_opts_t* opts, const void* data, size_t bytes);
    bool isStreamOpen(agent_stream_t kind) const;
    agent_state_t state() const;
    bool isReady() const;
    /** Readable diagnostics without Serial, Arduino String or heap allocation. */
    static const char* stateName(agent_state_t state);
    static const char* errorName(esp_err_t error);
    static const char* errorHint(esp_err_t error);
    /** Send congestion only. Retry later in loop, never in a busy while loop.
     * NO_MEM during begin/prepare may instead require reducing RAM usage. */
    static bool isRetryable(esp_err_t error);
    /** Current single-prompt UTF-8 byte capacity; 0 until READY. */
    size_t maxPromptBytes() const;
    agent_link_status_t status() const;
    const char* deviceId() const;
    esp_err_t forget();

    esp_err_t registerIo(const agent_link_io_desc_t& desc,
                         agent_io_actuate_cb_t callback = nullptr,
                         void* context = nullptr);
    esp_err_t pushReading(const char* id, const void* value, size_t length);
    esp_err_t updateReading(const char* id, const void* value, size_t length);
    /** Beginner-friendly cached readings: no pointers/sizeof required. Each
     * checks the endpoint type, encodes little-endian, and copies before return.
     * Register first; offline updates are allowed. ESP_OK means cached, not
     * delivered. poll() publishes the latest sample when the link permits it. */
    esp_err_t updateBool(const char* id, bool value);
    esp_err_t updateInt32(const char* id, int32_t value);
    esp_err_t updateUInt16(const char* id, uint16_t value);
    esp_err_t updateFloat(const char* id, float value);
    /** Text must fit AGENT_LINK_READING_CACHE_BYTES, excluding NUL. Empty text
     * clears the cached STR value. nullptr is rejected; input is copied. */
    esp_err_t updateText(const char* id, const char* utf8);
    esp_err_t pushEvent(agent_event_t type, const uint8_t* data = nullptr, size_t length = 0);
    esp_err_t reportBattery(uint8_t percent, bool charging);
    /** Reserved upstream selection report; returns NOT_SUPPORTED when READY. */
    esp_err_t selectAgent(const char* agent_id);
    esp_err_t notifyManifestChanged();

};

extern DeotalandClass Deotaland;
