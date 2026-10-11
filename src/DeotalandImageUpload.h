#pragma once

#include "agent_link_stream.h"

/** Cooperative, allocation-free IMAGE uploader for a complete image in memory.
 *
 * Call poll(millis()) from loop(), alongside Deotaland.poll(). Each call submits
 * at most one stream operation, with writes limited to 512 bytes. The transport
 * copies accepted chunks, so photographs may exceed its bounded transmit queue.
 * RAW images must already contain big-endian RGB565 bytes; JPEG bytes are sent
 * unchanged. This class validates metadata and length, not the JPEG bitstream.
 *
 * The source buffer is borrowed until busy() becomes false, or cancel() returns.
 * Keep the uploader itself alive and poll it while busy. Use it from the same
 * loop as the Agent Link API; concurrent stream callers need serialization.
 * Completed means payload and END were accepted locally, not a peer ACK.
 */
class DeotalandImageUpload {
public:
    enum class State { Idle, Opening, Writing, Closing, Aborting, Completed, Failed };

    DeotalandImageUpload() = default;
    DeotalandImageUpload(const DeotalandImageUpload&) = delete;
    DeotalandImageUpload& operator=(const DeotalandImageUpload&) = delete;

    /** Begin without blocking. Zero total_bytes is filled from bytes; a nonzero
     * value must match. timeout_ms must be in [1, INT32_MAX]. An existing IMAGE
     * stream, including a transport still draining END, returns INVALID_STATE.
     * A peer's IMAGE channel may become ready while Opening is polled.
     */
    esp_err_t start(const agent_stream_opts_t& opts, const void* data, size_t bytes,
                    uint32_t now_ms, uint32_t timeout_ms = 30000);
    void poll(uint32_t now_ms);

    /** Stop reading the source immediately. An open session then gets END(false)
     * retries for at most 1000 ms of polling; cancellation result is ESP_FAIL.
     * If END cannot be accepted, the core retains its stream and blocks another
     * start until the connection is reset. Continue polling while busy().
     */
    void cancel();

    bool busy() const;
    State state() const { return state_; }
    /** ESP_ERR_INVALID_STATE while uploading, ESP_OK after Completed, or the
     * original error after failure/cancellation (also during abort cleanup).
     */
    esp_err_t result() const { return result_; }
    size_t acceptedBytes() const { return accepted_; }

private:
    void abort(esp_err_t error, uint32_t now_ms, bool have_time);
    void finish(State state, esp_err_t error);

    State state_ = State::Idle;
    esp_err_t result_ = ESP_OK;
    agent_stream_opts_t opts_ = {};
    agent_stream_handle_t handle_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t bytes_ = 0;
    size_t accepted_ = 0;
    uint32_t started_ms_ = 0;
    uint32_t timeout_ms_ = 0;
    uint32_t generation_ = 0;
    uint32_t abort_ms_ = 0;
    bool abort_time_known_ = false;
};
