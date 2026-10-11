#include "DeotalandImageUpload.h"

#include "agent_link_queue.h"

namespace {
constexpr size_t kChunkBytes = 512;
constexpr uint32_t kAbortTimeoutMs = 1000;

bool Retryable(esp_err_t error) {
    return error == ESP_ERR_NO_MEM || error == ESP_ERR_TIMEOUT;
}

bool ImageOccupied() {
    if (agent_link_stream_is_open(AGENT_STREAM_IMAGE)) return true;
    agent_stream_status_t status = {};
    return agent_link_stream_get_status(AGENT_STREAM_IMAGE, &status) == ESP_OK &&
        (status.state == AGENT_STREAM_OPEN || status.state == AGENT_STREAM_DRAINING);
}
}

bool DeotalandImageUpload::busy() const {
    return state_ == State::Opening || state_ == State::Writing ||
        state_ == State::Closing || state_ == State::Aborting;
}

esp_err_t DeotalandImageUpload::start(const agent_stream_opts_t& opts, const void* data,
                                     size_t bytes, uint32_t now_ms, uint32_t timeout_ms) {
    if (busy()) return ESP_ERR_INVALID_STATE;
    if (!data || !bytes || !opts.width || !opts.height || !timeout_ms ||
        timeout_ms > 0x7fffffffu ||
        (opts.encoding != AGENT_ENC_RAW && opts.encoding != AGENT_ENC_JPEG)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes > UINT32_MAX || (opts.total_bytes && opts.total_bytes != bytes)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (opts.encoding == AGENT_ENC_RAW &&
        uint64_t(opts.width) * opts.height * 2u != bytes) return ESP_ERR_INVALID_SIZE;
    if (ImageOccupied()) return ESP_ERR_INVALID_STATE;

    opts_ = opts;
    opts_.name = nullptr; // IMAGE has no name field on the wire; never borrow it.
    opts_.total_bytes = static_cast<uint32_t>(bytes);
    data_ = static_cast<const uint8_t*>(data);
    bytes_ = bytes;
    accepted_ = 0;
    handle_ = nullptr;
    started_ms_ = now_ms;
    timeout_ms_ = timeout_ms;
    generation_ = agentlink::RxGeneration();
    abort_time_known_ = false;
    result_ = ESP_ERR_INVALID_STATE;
    state_ = State::Opening;
    return ESP_OK;
}

void DeotalandImageUpload::finish(State state, esp_err_t error) {
    data_ = nullptr;
    handle_ = nullptr;
    state_ = state;
    result_ = error;
}

void DeotalandImageUpload::abort(esp_err_t error, uint32_t now_ms, bool have_time) {
    data_ = nullptr;
    result_ = error;
    if (!handle_) {
        finish(State::Failed, error);
        return;
    }
    state_ = State::Aborting;
    abort_ms_ = now_ms;
    abort_time_known_ = have_time;
}

void DeotalandImageUpload::cancel() {
    if (busy() && state_ != State::Aborting) abort(ESP_FAIL, 0, false);
}

void DeotalandImageUpload::poll(uint32_t now_ms) {
    if (!busy()) return;

    // A quick disconnect/reconnect can already be READY again. Compare the RX
    // session generation instead of readiness before touching the old handle.
    if (generation_ != agentlink::RxGeneration()) {
        finish(State::Failed, state_ == State::Aborting ? result_ : ESP_ERR_INVALID_STATE);
        return;
    }
    if (state_ != State::Aborting && uint32_t(now_ms - started_ms_) >= timeout_ms_) {
        abort(ESP_ERR_TIMEOUT, now_ms, true);
        return;
    }

    if (state_ == State::Opening) {
        // open() intentionally returns an existing handle. Check every attempt
        // so two helpers (or a manual sender) cannot acquire the same session.
        if (ImageOccupied()) {
            finish(State::Failed, ESP_ERR_INVALID_STATE);
            return;
        }
        const esp_err_t error = agent_link_stream_open(AGENT_STREAM_IMAGE, &opts_, &handle_);
        if (generation_ != agentlink::RxGeneration()) {
            finish(State::Failed, ESP_ERR_INVALID_STATE);
        } else if (error == ESP_OK) {
            state_ = State::Writing;
        } else if (!Retryable(error) && error != ESP_ERR_INVALID_STATE) {
            finish(State::Failed, error);
        }
        return;
    }

    if (state_ == State::Writing) {
        const size_t remaining = bytes_ - accepted_;
        const size_t chunk = remaining < kChunkBytes ? remaining : kChunkBytes;
        const esp_err_t error = agent_link_stream_write(handle_, data_ + accepted_, chunk);
        if (error == ESP_OK) accepted_ += chunk;
        if (generation_ != agentlink::RxGeneration()) {
            finish(State::Failed, ESP_ERR_INVALID_STATE);
        } else if (error == ESP_OK) {
            if (accepted_ == bytes_) {
                data_ = nullptr;
                state_ = State::Closing;
            }
        } else if (!Retryable(error)) {
            abort(error, now_ms, true);
        }
        return;
    }

    if (state_ == State::Closing) {
        const esp_err_t error = agent_link_stream_close(handle_, true);
        if (generation_ != agentlink::RxGeneration()) {
            finish(State::Failed, ESP_ERR_INVALID_STATE);
        } else if (error == ESP_OK) {
            finish(State::Completed, ESP_OK);
        } else if (!Retryable(error)) {
            abort(error, now_ms, true);
        }
        return;
    }

    if (state_ == State::Aborting) {
        if (!abort_time_known_) {
            abort_ms_ = now_ms;
            abort_time_known_ = true;
        }
        if (uint32_t(now_ms - abort_ms_) >= kAbortTimeoutMs) {
            // Do not hide an unreleased core stream by reopening it. Its OPEN
            // state will reject start() until the backend/link is reset.
            finish(State::Failed, result_);
            return;
        }
        const esp_err_t error = agent_link_stream_close(handle_, false);
        if (generation_ != agentlink::RxGeneration() || error == ESP_OK) {
            finish(State::Failed, result_);
        }
        // All END failures get bounded retries: even a permanent backend error
        // may recover before the abort deadline, but never retains our buffer.
    }
}
