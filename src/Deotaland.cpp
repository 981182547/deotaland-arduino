#include "Deotaland.h"

DeotalandClass Deotaland;

esp_err_t DeotalandClass::begin(const agent_link_config_t& config) {
    if (started_) return ESP_ERR_INVALID_STATE;

    esp_err_t result = agent_link_init(&config);
    if (result != ESP_OK) return result;

    result = agent_link_start();
    if (result == ESP_OK) started_ = true;
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
    if (!started_) return;
    agent_link_stop();
    started_ = false;
}

agent_state_t DeotalandClass::state() const { return agent_link_state(); }

bool DeotalandClass::isReady() const { return state() == AGENT_STATE_READY; }

esp_err_t DeotalandClass::registerIo(const agent_link_io_desc_t& desc,
                                     agent_io_actuate_cb_t callback,
                                     void* context) {
    return agent_link_register_io(&desc, callback, context);
}

esp_err_t DeotalandClass::pushReading(const char* id, const void* value, size_t length) {
    return agent_link_push_reading(id, value, length);
}

esp_err_t DeotalandClass::pushVoice(const int16_t* samples, size_t sample_count) {
    if (!samples || sample_count == 0) return ESP_ERR_INVALID_ARG;
    return agent_link_push_voice(reinterpret_cast<const uint8_t*>(samples),
                                 sample_count * sizeof(int16_t));
}

esp_err_t DeotalandClass::pushVoiceBytes(const uint8_t* pcm16, size_t length) {
    return agent_link_push_voice(pcm16, length);
}

esp_err_t DeotalandClass::endVoice() { return agent_link_voice_end(); }

esp_err_t DeotalandClass::startAsr(const char* name) { return agent_link_asr_start(name); }

esp_err_t DeotalandClass::pushAsr(const uint8_t* audio, size_t length) {
    return agent_link_asr_push(audio, length);
}

esp_err_t DeotalandClass::endAsr(bool complete) { return agent_link_asr_end(complete); }

esp_err_t DeotalandClass::startRecording(const char* name, const uint8_t* wav_header,
                                         size_t header_length) {
    return agent_link_recording_start(name, wav_header, header_length);
}

esp_err_t DeotalandClass::pushRecording(const uint8_t* data, size_t length) {
    return agent_link_recording_data(data, length);
}

esp_err_t DeotalandClass::endRecording(bool complete, const uint8_t* final_header,
                                       size_t header_length) {
    return agent_link_recording_end(complete, final_header, header_length);
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

esp_err_t DeotalandClass::pushVideo(const uint8_t* frame, size_t length,
                                    uint32_t pts_ms, bool keyframe) {
    return agent_link_push_video(frame, length, pts_ms, keyframe);
}

esp_err_t DeotalandClass::endVideo() { return agent_link_video_end(); }
