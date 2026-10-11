#include "Deotaland.h"
#include <cstring>

DeotalandClass Deotaland;

esp_err_t DeotalandClass::begin(const agent_link_config_t& config) {
    esp_err_t result = agent_link_init(&config);
    if (result != ESP_OK) return result;

    result = agent_link_start();
    if (result != ESP_OK) agent_link_stop();
    return result;
}

esp_err_t DeotalandClass::begin(const char* device_name, uint32_t capabilities,
                                const agent_output_cb_t* output,
                                agent_state_cb_t on_state,
                                void* state_context) {
    agent_link_config_t config = {};
    config.device_name = device_name;
    config.caps = capabilities;
    config.output = output;
    config.on_state = on_state;
    config.state_ctx = state_context;
    config.transport = AGENT_TRANSPORT_BLE;
    return begin(config);
}

void DeotalandClass::end() {
    agent_link_stop();
}
bool DeotalandClass::canBegin() const { return agent_link_can_begin(); }
esp_err_t DeotalandClass::configurePlayback(const agent_playback_config_t* c) { return agent_link_playback_configure(c); }
agent_playback_stats_t DeotalandClass::playbackStats() const { agent_playback_stats_t s={}; agent_link_playback_get_stats(&s); return s; }
bool DeotalandClass::playbackActive() const { return agent_link_playback_active(); }
esp_err_t DeotalandClass::prepareStreams(uint32_t mask) { return agent_link_prepare_streams(mask); }
esp_err_t DeotalandClass::prepareOta() { return agent_link_ota_prepare(); }
void DeotalandClass::setOtaCallback(agent_ota_progress_cb_t callback, void* context) { agent_link_ota_set_callback(callback,context); }
agent_ota_status_t DeotalandClass::otaStatus() const { agent_ota_status_t s={}; agent_link_ota_get_status(&s); return s; }
bool DeotalandClass::otaBusy() const { return agent_link_ota_is_busy(); }
bool DeotalandClass::otaSupported() const { return agent_link_ota_supported(); }
bool DeotalandClass::otaNeedsReconnect() const { return agent_link_ota_needs_reconnect(); }
void DeotalandClass::abortOta() { agent_link_ota_abort(); }
esp_err_t DeotalandClass::markOtaValid() { return agent_link_ota_mark_valid(); }
const char* DeotalandClass::runningVersion() const { return agent_link_ota_running_version(); }
esp_err_t DeotalandClass::streamStatus(agent_stream_t kind,agent_stream_status_t* out) const { return agent_link_stream_get_status(kind,out); }
esp_err_t DeotalandClass::updateReading(const char* id,const void* value,size_t n) { return agent_link_update_reading(id,value,n); }

agent_state_t DeotalandClass::state() const { return agent_link_state(); }

bool DeotalandClass::isReady() const { return state() == AGENT_STATE_READY; }
const char* DeotalandClass::stateName(agent_state_t state) {
    switch (state) {
    case AGENT_STATE_DISCONNECTED: return "DISCONNECTED";
    case AGENT_STATE_CONNECTED: return "CONNECTED";
    case AGENT_STATE_READY: return "READY";
    default: return "UNKNOWN";
    }
}
const char* DeotalandClass::errorName(esp_err_t error) { return esp_err_to_name(error); }
bool DeotalandClass::isRetryable(esp_err_t error) {
    return error == ESP_ERR_NO_MEM || error == ESP_ERR_TIMEOUT;
}
const char* DeotalandClass::errorHint(esp_err_t error) {
    switch (error) {
    case ESP_OK: return "Accepted locally; this is not a peer delivery acknowledgement.";
    case ESP_ERR_NO_MEM: return "Send queue full: retry later; during setup, check RAM and endpoint count.";
    case ESP_ERR_TIMEOUT: return "Retry this operation later from loop(); keep the same stream bytes.";
    case ESP_ERR_INVALID_ARG: return "Check required fields, endpoint type and value (finite numbers; BOOL 0/1).";
    case ESP_ERR_INVALID_STATE: return "Check begin(), READY, duplicate IDs, stream subscription and canBegin().";
    case ESP_ERR_INVALID_SIZE: return "Check UTF-8 byte length, value width, cache limit and current MTU.";
    case ESP_ERR_NOT_FOUND: return "Register this exact input endpoint ID before updating a reading.";
    case ESP_ERR_NOT_SUPPORTED: return "Check chip/Core support: BLE, CoC, transport and enabled features.";
    default: return "Inspect the operation's error code and status(); check the troubleshooting guide.";
    }
}
size_t DeotalandClass::maxPromptBytes() const { return agent_link_event_payload_capacity(); }

namespace {
esp_err_t UpdateScalar(const char* id, agent_val_t type, uint32_t bits, size_t bytes) {
    const uint8_t encoded[4] = {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
        static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24)};
    return agent_link_update_reading_typed(id, type, encoded, bytes);
}
}
esp_err_t DeotalandClass::updateBool(const char* id, bool value) {
    return UpdateScalar(id, AGENT_VAL_BOOL, value ? 1u : 0u, 1);
}
esp_err_t DeotalandClass::updateInt32(const char* id, int32_t value) {
    return UpdateScalar(id, AGENT_VAL_I32, static_cast<uint32_t>(value), 4);
}
esp_err_t DeotalandClass::updateUInt16(const char* id, uint16_t value) {
    return UpdateScalar(id, AGENT_VAL_U16, value, 2);
}
esp_err_t DeotalandClass::updateFloat(const char* id, float value) {
    static_assert(sizeof(float) == 4, "Agent Link requires 32-bit float");
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return UpdateScalar(id, AGENT_VAL_F32, bits, 4);
}
esp_err_t DeotalandClass::updateText(const char* id, const char* utf8) {
    if (!utf8) return ESP_ERR_INVALID_ARG;
    size_t length = 0;
    while (length <= AGENT_LINK_READING_CACHE_BYTES && utf8[length]) ++length;
    if (length > AGENT_LINK_READING_CACHE_BYTES) return ESP_ERR_INVALID_SIZE;
    return agent_link_update_reading_typed(id, AGENT_VAL_STR, utf8, length);
}
agent_link_status_t DeotalandClass::status() const { agent_link_status_t s = {}; agent_link_get_status(&s); return s; }
const char* DeotalandClass::deviceId() const { return agent_link_device_id(); }
esp_err_t DeotalandClass::forget() { return agent_link_forget(); }

esp_err_t DeotalandClass::registerIo(const agent_link_io_desc_t& desc,
                                     agent_io_actuate_cb_t callback,
                                     void* context) {
    return agent_link_register_io(&desc, callback, context);
}

esp_err_t DeotalandClass::pushReading(const char* id, const void* value, size_t length) {
    return agent_link_push_reading(id, value, length);
}

esp_err_t DeotalandClass::pushEvent(agent_event_t type, const uint8_t* data, size_t length) {
    return agent_link_push_event(type, data, length);
}

esp_err_t DeotalandClass::reportBattery(uint8_t percent, bool charging) {
    return agent_link_report_battery(percent, charging);
}

esp_err_t DeotalandClass::selectAgent(const char* agent_id) {
    return agent_link_report_selected_agent(agent_id);
}

esp_err_t DeotalandClass::notifyManifestChanged() {
    return agent_link_notify_manifest_changed();
}

size_t DeotalandClass::poll(size_t n) { return agent_link_poll(n); }
size_t DeotalandClass::poll(size_t n, uint32_t us) { return agent_link_poll_budget(n,us); }
void DeotalandClass::reset() { agent_link_deinit(); }
agent_link_stats_t DeotalandClass::stats() const { agent_link_stats_t s = {}; agent_link_get_stats(&s); return s; }
esp_err_t DeotalandClass::pushPrompt(const char* s) { return agent_link_push_prompt(s); }
void DeotalandClass::setPlaybackBufferMs(uint32_t ms) { agent_link_playback_set_buffer_ms(ms); }
esp_err_t DeotalandClass::openStream(agent_stream_t k, const agent_stream_opts_t* o, agent_stream_handle_t* h) { return agent_link_stream_open(k,o,h); }
esp_err_t DeotalandClass::writeStream(agent_stream_handle_t h,const void* p,size_t n) { return agent_link_stream_write(h,p,n); }
esp_err_t DeotalandClass::closeStream(agent_stream_handle_t h,bool ok) { return agent_link_stream_close(h,ok); }
esp_err_t DeotalandClass::sendStream(agent_stream_t k,const agent_stream_opts_t* o,const void* p,size_t n) { return agent_link_stream_send(k,o,p,n); }
bool DeotalandClass::isStreamOpen(agent_stream_t kind) const { return agent_link_stream_is_open(kind); }
