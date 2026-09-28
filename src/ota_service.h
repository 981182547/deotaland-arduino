#pragma once
#include "agent_link_ota.h"

namespace agentlink {
namespace ota {
constexpr uint16_t kErrSuccess = 0;
constexpr uint16_t kErrBusinessFailed = 1003;
constexpr uint16_t kErrInvalidPayload = 1004;
constexpr uint16_t kErrConfigFailed = 1005;
constexpr uint16_t kErrVersionIncompat = 2030;
constexpr uint16_t kErrFirmwareTooLarge = 2031;
constexpr uint16_t kErrPartitionFail = 2032;
constexpr uint16_t kErrWriteFail = 2033;
constexpr uint16_t kErrSha256Mismatch = 2034;
constexpr uint16_t kErrAborted = 2035;
constexpr uint16_t kErrImageInvalid = 2036;
constexpr uint16_t kErrModelMismatch = 2037;
constexpr uint8_t kEvtOtaProgress = 0x38;
using EventSink = void (*)(uint8_t event_id, const uint8_t* payload, size_t length);
using ReliableEventSink = esp_err_t (*)(uint8_t event_id, const uint8_t* payload, size_t length);
using ResumeRxHook = void (*)(void);

// Encode the upstream 0x38 payload; independent of platform/worker state.
void EncodeProgress(const agent_ota_status_t& status, uint8_t payload[11]);

// Lifecycle/control methods run in the same context as agent_link_poll().
void Configure(bool enabled, bool data_channel_supported);
void SetEventSink(EventSink sink);
// Preferred by the core: terminal events retry at 200ms intervals until accepted.
// Success starts its reboot countdown only after this sink returns ESP_OK.
void SetReliableEventSink(ReliableEventSink sink);
void SetResumeRxHook(ResumeRxHook hook);
void SetDeviceModel(const char* model);
const char* DeviceModel();
esp_err_t Prepare();
bool Start(uint32_t firmware_size, const uint8_t sha256[32], const char* version,
           const char* model, uint16_t* error_code);
// The transport calls FeedData directly, without an intermediate lossy RX queue.
// Exactly one RX producer; never waits. Insufficient space returns 0 without consuming
// data so the transport can retain the SDU and retry. Terminal-session bytes are discarded.
size_t FeedData(const uint8_t* data, size_t length);
// Latches from an accepted upgrade until link loss. Never route leftover firmware to audio.
// A new OTA session requires reconnecting after an accepted session finishes or aborts.
bool OwnsDataChannel();
// Check before replenishing CoC credits. The transport must retain/retry a held RX buffer.
bool WantsMore();
void Abort(const char* reason);
bool IsBusy();
// Includes cancellation/cleanup of an old session after IsBusy becomes false.
// A prewarmed worker sleeping between sessions does not block reinitialization.
bool WorkerActive();
void GetStatus(agent_ota_status_t* out);
// The false path touches atomics only and may be called immediately by the RX host task
// before queuing the ordinary connection callback. Repeating it from poll is harmless.
void OnLinkState(bool connected);
// At most one progress event and its corresponding board callback per call.
// Terminal delivery is retained/retried; stale-session records are discarded.
void Poll();
}  // namespace ota
}  // namespace agentlink
