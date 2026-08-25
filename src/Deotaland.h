#pragma once

#if !defined(ARDUINO_ARCH_ESP32)
#error "deotaland-arduino supports only the Arduino ESP32 core"
#endif

#include <Arduino.h>

#include "agent_link.h"

/**
 * Arduino-style facade over the stable C API in agent_link.h.
 *
 * The transport is asynchronous; no call from loop() is required. Callbacks can
 * run in the NimBLE host task, so they must enqueue work and return quickly.
 */
class DeotalandClass {
public:
    /** Initialize and start the selected transport. Call once from setup(). */
    esp_err_t begin(const agent_link_config_t& config);

    /**
     * Convenience BLE startup. Strings and callback storage must remain valid
     * for the lifetime of the connection.
     */
    esp_err_t begin(const char* device_name, uint32_t capabilities,
                    const agent_output_cb_t* output = nullptr,
                    agent_state_cb_t on_state = nullptr,
                    void* state_context = nullptr);

    void end();
    agent_state_t state() const;
    bool isReady() const;

    esp_err_t registerIo(const agent_link_io_desc_t& desc,
                         agent_io_actuate_cb_t callback = nullptr,
                         void* context = nullptr);
    esp_err_t pushReading(const char* id, const void* value, size_t length);
    esp_err_t pushVoice(const int16_t* samples, size_t sample_count);
    esp_err_t pushVoiceBytes(const uint8_t* pcm16, size_t length);
    esp_err_t endVoice();
    esp_err_t startAsr(const char* name = nullptr);
    esp_err_t pushAsr(const uint8_t* audio, size_t length);
    esp_err_t endAsr(bool complete = true);
    esp_err_t startRecording(const char* name, const uint8_t* wav_header, size_t header_length);
    esp_err_t pushRecording(const uint8_t* data, size_t length);
    esp_err_t endRecording(bool complete, const uint8_t* final_header = nullptr,
                           size_t header_length = 0);
    esp_err_t pushEvent(agent_event_t type, const uint8_t* data = nullptr, size_t length = 0);
    esp_err_t reportBattery(uint8_t percent, bool charging);
    esp_err_t selectAgent(const char* agent_id);
    esp_err_t notifyManifestChanged();
    esp_err_t pushVideo(const uint8_t* frame, size_t length, uint32_t pts_ms, bool keyframe);
    esp_err_t endVideo();

private:
    bool started_ = false;
};

extern DeotalandClass Deotaland;
