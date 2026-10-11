#include "ota_service.h"
#include "vendor/nimble-arduino/DeotalandNimblePlatform.h"
#include <cstring>

// This is the only module that knows about flash partitions and the ESP32 OTA API.
// No platform headers enter the public protocol surface.
#if defined(ARDUINO_ARCH_ESP32) && !defined(AGENT_LINK_DISABLE_OTA)
#define AGENT_LINK_HAS_OTA_WORKER 1
#include <atomic>
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0) && __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#if configSUPPORT_STATIC_ALLOCATION == 1
#define AGENT_LINK_OTA_HAS_BUFFER_CAPS 1
#endif
#endif
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"
#include "sdkconfig.h"
#else
#define AGENT_LINK_HAS_OTA_WORKER 0
#endif

namespace agentlink {
namespace ota {
namespace {
#if AGENT_LINK_HAS_OTA_WORKER
bool s_enabled = false;
bool s_data_supported = false;
EventSink s_sink = nullptr;
ReliableEventSink s_reliable_sink = nullptr;
ResumeRxHook s_resume = nullptr;
agent_ota_progress_cb_t s_callback = nullptr;
void* s_callback_context = nullptr;
#endif
char s_model[64] = {};

bool Supported() {
#if AGENT_LINK_HAS_OTA_WORKER && !defined(AGENT_LINK_DISABLE_BLE) && \
    (DEOTALAND_NIMBLE_ACTIVE || \
     (defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) && CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0))
    return s_enabled && s_data_supported;
#else
#if AGENT_LINK_HAS_OTA_WORKER
    (void)s_enabled; (void)s_data_supported;
#endif
    return false;
#endif
}

#if AGENT_LINK_HAS_OTA_WORKER
// IDF can return the sole ota_0 slot even when it is the running image. Reject
// that layout before allocating a worker or acknowledging an upgrade. Boot
// selection also needs two sectors of otadata, even with a factory image.
const esp_partition_t* UpdatePartition() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return nullptr;
    const esp_partition_t* target = esp_ota_get_next_update_partition(running);
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    return target && target->address != running->address && otadata && otadata->size >= 8192u
        ? target : nullptr;
}

#if defined(AGENT_LINK_OTA_REQUIRE_NEWER_VERSION) && AGENT_LINK_OTA_REQUIRE_NEWER_VERSION
// Compare the numeric release, as the upstream opt-in gate does. Accept one to
// three components and ignore a nonempty -prerelease/+build suffix; malformed
// numeric components and overflow are rejected instead of wrapping through sscanf.
bool ParseVersion(const char* text, uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!text || !*text) return false;
    unsigned component = 0;
    for (;;) {
        if (*text < '0' || *text > '9') return false;
        uint32_t value = 0;
        while (*text >= '0' && *text <= '9') {
            const uint32_t digit = static_cast<uint32_t>(*text++ - '0');
            if (value > (UINT32_MAX - digit) / 10u) return false;
            value = value * 10u + digit;
        }
        out[component++] = value;
        if (!*text) return true;
        if (*text == '.') {
            if (component == 3) return false;
            ++text;
            continue;
        }
        if (*text != '-' && *text != '+') return false;
        if (!*++text) return false;
        for (; *text; ++text) {
            const char ch = *text;
            if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '-' || ch == '+')) return false;
        }
        return true;
    }
}

bool IsNewerVersion(const char* candidate, const char* current) {
    uint32_t next[3], running[3];
    if (!ParseVersion(candidate, next)) return false;
    // Preserve upstream recovery behavior for a nonnumeric running image version.
    if (!ParseVersion(current, running)) return true;
    for (unsigned i = 0; i < 3; ++i)
        if (next[i] != running[i]) return next[i] > running[i];
    return false;
}
#endif

#ifndef AGENT_LINK_OTA_RX_BUFFER_BYTES
#define AGENT_LINK_OTA_RX_BUFFER_BYTES (32u * 1024u)
#endif
constexpr size_t kBufferBytes = AGENT_LINK_OTA_RX_BUFFER_BYTES;
static_assert(kBufferBytes >= 16384, "OTA buffer must cover in-flight CoC SDUs");
constexpr size_t kFlashChunk = 4096;
constexpr size_t kGateBytes = 8192;
constexpr uint32_t kWorkerPollMs = 100;
constexpr uint32_t kIdleTimeoutMs = 30000;
std::atomic<agent_ota_state_t> s_state{AGENT_OTA_IDLE};
std::atomic<uint32_t> s_received{0};
std::atomic<uint32_t> s_total{0};
std::atomic<uint16_t> s_error{0};
std::atomic<bool> s_abort{false};
std::atomic<bool> s_worker_alive{false};
std::atomic<bool> s_owned{false};
std::atomic<bool> s_rx_active{false};
std::atomic<uint32_t> s_generation{1};
std::atomic<uint32_t> s_active_generation{0};
std::atomic<uint32_t> s_success_dispatched{0};
struct ProgressRecord { agent_ota_status_t status; uint32_t generation; };
ProgressRecord s_pending_record = {};
bool s_have_pending = false;
bool s_pending_callback_sent = false;
uint32_t s_pending_attempt_ms = 0;
StreamBufferHandle_t s_buffer = nullptr;  // Kept after first use; the RX task may still hold it.
QueueHandle_t s_progress = nullptr;
TaskHandle_t s_worker = nullptr;  // Persistent worker; idle sessions consume no CPU.
uint8_t* s_chunk = nullptr;      // Internal RAM remains accessible around flash/cache operations.
const esp_partition_t* s_partition = nullptr;
const esp_partition_t* s_previous_boot = nullptr;
uint8_t s_expected_sha[32] = {};
uint32_t s_last_report_ms = 0;

void Snapshot(agent_ota_status_t* out) {
    out->state = s_state.load(std::memory_order_acquire);
    out->received_bytes = s_received.load(std::memory_order_acquire);
    out->total_bytes = s_total.load(std::memory_order_acquire);
    out->error_code = s_error.load(std::memory_order_acquire);
}

// Only transitions use this queue (at most receiving/verify/terminal per session).
// Periodic progress is sampled by Poll, so an unpolled sketch cannot fill it.
void Transition(uint32_t generation, agent_ota_state_t state, uint16_t error = 0) {
    s_error.store(error, std::memory_order_release);
    s_state.store(state, std::memory_order_release);
    ProgressRecord record;
    Snapshot(&record.status);
    record.generation = generation;
    if (s_progress) (void)xQueueSend(s_progress, &record, 0);
}

void InvalidateSession() {
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    s_owned.store(false, std::memory_order_release);
    // Unconditional: link loss can race the small window before Start publishes its worker.
    s_abort.store(true, std::memory_order_release);
}

int ShaStart(mbedtls_sha256_context* context) {
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256_starts(context, 0);
#else
    return mbedtls_sha256_starts_ret(context, 0);
#endif
}
int ShaUpdate(mbedtls_sha256_context* context, const uint8_t* bytes, size_t length) {
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256_update(context, bytes, length);
#else
    return mbedtls_sha256_update_ret(context, bytes, length);
#endif
}
int ShaFinish(mbedtls_sha256_context* context, uint8_t hash[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256_finish(context, hash);
#else
    return mbedtls_sha256_finish_ret(context, hash);
#endif
}

void RunSession(uint32_t generation) {
    esp_ota_handle_t handle = 0;
    bool handle_open = false;
    uint16_t failure = generation != s_generation.load(std::memory_order_acquire) ? kErrAborted : 0;
    if (!failure && s_abort.load(std::memory_order_acquire)) {
        const uint16_t requested=s_error.load(std::memory_order_acquire);
        failure=requested ? requested : kErrAborted;
    }
    uint8_t* const chunk = s_chunk;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    const uint32_t total = s_total.load(std::memory_order_acquire);
    if (!failure && (!chunk || ShaStart(&sha) != 0)) failure = kErrConfigFailed;
    if (!failure) {
        // A 4 KiB sequential writer needs only the next sector erased. This
        // moves erase work into bounded write chunks instead of erasing the
        // whole image before consuming RX. Actual flash stalls still exist.
#if defined(OTA_WITH_SEQUENTIAL_WRITES) && !defined(AGENT_LINK_OTA_BULK_ERASE)
        const size_t begin_size=OTA_WITH_SEQUENTIAL_WRITES;
#else
        const size_t begin_size=total;
#endif
        if (esp_ota_begin(s_partition, begin_size, &handle) != ESP_OK) failure = kErrPartitionFail;
        else handle_open = true;
    }
    uint32_t written = 0;
    uint32_t idle = 0;
    size_t staged = 0;
    while (!failure && written < total) {
        if (s_abort.load(std::memory_order_acquire)) {
            const uint16_t requested = s_error.load(std::memory_order_acquire);
            failure = requested ? requested : kErrAborted;
            break;
        }
        const size_t remaining = total - written - staged;
        const size_t room = kFlashChunk - staged;
        const size_t want = remaining < room ? remaining : room;
        const size_t received = xStreamBufferReceive(s_buffer, chunk + staged, want, pdMS_TO_TICKS(kWorkerPollMs));
        if (!received) {
            idle += kWorkerPollMs;
            if (idle >= kIdleTimeoutMs) failure = kErrWriteFail;
            continue;
        }
        idle = 0;
        staged += received;
        if (staged < kFlashChunk && staged < total - written) continue;
        if (ShaUpdate(&sha, chunk, staged) != 0) failure = kErrConfigFailed;
        else if (esp_ota_write(handle, chunk, staged) != ESP_OK) failure = kErrWriteFail;
        else written += static_cast<uint32_t>(staged);
        staged = 0;
    }
    if (!failure && s_abort.load(std::memory_order_acquire)) failure = kErrAborted;
    if (!failure) {
        Transition(generation, AGENT_OTA_VERIFYING);
        uint8_t actual[32];
        if (ShaFinish(&sha, actual) != 0) failure = kErrConfigFailed;
        else if (std::memcmp(actual, s_expected_sha, sizeof(actual)) != 0) failure = kErrSha256Mismatch;
        else {
            const esp_err_t result = esp_ota_end(handle);
            handle_open = false;  // ESP-IDF consumes the handle on either outcome.
            if (result != ESP_OK) failure = kErrImageInvalid;
            else if (s_abort.load(std::memory_order_acquire)) failure = kErrAborted;
            else if (esp_ota_set_boot_partition(s_partition) != ESP_OK) failure = kErrPartitionFail;
        }
    }
    mbedtls_sha256_free(&sha);
    if (handle_open) (void)esp_ota_abort(handle);
    if (!failure) {
        Transition(generation, AGENT_OTA_SUCCESS);
        // Do not reboot before poll() has observed and dispatched the success. A stopped
        // sketch may be reinitializing a different link; it must never receive this event
        // or unexpectedly reboot because an older session's worker finished later.
        bool reported = false;
        uint32_t reported_at = 0;
        for (;;) {
            if (s_abort.load(std::memory_order_acquire) ||
                generation != s_generation.load(std::memory_order_acquire)) {
                failure = (!s_previous_boot || esp_ota_set_boot_partition(s_previous_boot) != ESP_OK)
                    ? kErrPartitionFail : kErrAborted;
                break;
            }
            const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
            if (!reported && s_success_dispatched.load(std::memory_order_acquire) == generation) {
                reported = true;
                reported_at = now;
            }
            if (reported && now - reported_at >= 2000) { esp_restart(); break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (failure) Transition(generation, AGENT_OTA_FAILED, failure);
    } else {
        Transition(generation, AGENT_OTA_FAILED, failure);
    }
    // Keep terminal status available to get_status()/Poll until the next Start.
    s_worker_alive.store(false, std::memory_order_release);
}
void Worker(void*) {
    for (;;) {
        // Only accepted START commands notify this private task slot. A notify
        // that arrives just before this wait is retained by FreeRTOS.
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_worker_alive.load(std::memory_order_acquire))
            RunSession(s_active_generation.load(std::memory_order_acquire));
    }
}

bool Dispatch(const agent_ota_status_t& status, bool notify_board) {
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    bool accepted = false;
    if (s_sink || s_reliable_sink) {
        uint8_t payload[11];
        EncodeProgress(status, payload);
        if (s_reliable_sink) accepted = s_reliable_sink(kEvtOtaProgress, payload, sizeof(payload)) == ESP_OK;
        else { s_sink(kEvtOtaProgress, payload, sizeof(payload)); accepted = true; }
    }
    if (notify_board && s_callback && generation == s_generation.load(std::memory_order_acquire))
        s_callback(&status, s_callback_context);
    return accepted;
}
#endif
}  // namespace

void EncodeProgress(const agent_ota_status_t& status, uint8_t payload[11]) {
    payload[0] = status.state == AGENT_OTA_VERIFYING ? 1 :
        status.state == AGENT_OTA_SUCCESS ? 2 : status.state == AGENT_OTA_FAILED ? 3 : 0;
    for (unsigned i = 0; i < 4; ++i) {
        payload[1 + i] = static_cast<uint8_t>(status.received_bytes >> (8 * i));
        payload[5 + i] = static_cast<uint8_t>(status.total_bytes >> (8 * i));
    }
    payload[9] = static_cast<uint8_t>(status.error_code);
    payload[10] = static_cast<uint8_t>(status.error_code >> 8);
}

void Configure(bool enabled, bool data_channel_supported) {
#if AGENT_LINK_HAS_OTA_WORKER
    InvalidateSession();
    s_enabled = enabled;
    s_data_supported = data_channel_supported;
#else
    (void)enabled; (void)data_channel_supported;
#endif
    if (!Supported()) Abort("disabled");
}
void SetEventSink(EventSink sink) {
#if AGENT_LINK_HAS_OTA_WORKER
    s_sink = sink; s_reliable_sink = nullptr;
#else
    (void)sink;
#endif
}
void SetReliableEventSink(ReliableEventSink sink) {
#if AGENT_LINK_HAS_OTA_WORKER
    s_reliable_sink = sink;
#else
    (void)sink;
#endif
}
void SetResumeRxHook(ResumeRxHook hook) {
#if AGENT_LINK_HAS_OTA_WORKER
    s_resume = hook;
#else
    (void)hook;
#endif
}
void SetDeviceModel(const char* model) {
    s_model[0] = '\0';
    if (!model) return;
    std::strncpy(s_model, model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
}
const char* DeviceModel() { return s_model; }
esp_err_t Prepare() {
    if (!Supported()) return ESP_ERR_NOT_SUPPORTED;
#if AGENT_LINK_HAS_OTA_WORKER
    if (!UpdatePartition()) return ESP_ERR_NOT_FOUND;
    if (s_worker) return ESP_OK;
    // Retain each successful reservation on failure, so retrying is idempotent
    // and cannot invalidate pointers held by the independent RX host task.
    if (!s_buffer) {
#if defined(AGENT_LINK_OTA_HAS_BUFFER_CAPS)
        s_buffer=xStreamBufferCreateWithCaps(kBufferBytes,1,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!s_buffer) s_buffer=xStreamBufferCreate(kBufferBytes,1);
    }
    if (!s_buffer) return ESP_ERR_NO_MEM;
    if (!s_progress) s_progress=xQueueCreate(8,sizeof(ProgressRecord));
    if (!s_progress) return ESP_ERR_NO_MEM;
    if (!s_chunk) s_chunk=static_cast<uint8_t*>(heap_caps_malloc(kFlashChunk,MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_chunk) return ESP_ERR_NO_MEM;
    // ESP-IDF task stacks use internal memory, independent of PSRAM settings.
    if (xTaskCreate(Worker,"agentlink_ota",8192,nullptr,5,&s_worker) != pdPASS) {
        s_worker=nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool Start(uint32_t firmware_size, const uint8_t sha256[32], const char* version,
           const char* model, uint16_t* error_code) {
    if (error_code) *error_code = kErrBusinessFailed;
    if (!Supported()) return false;
#if AGENT_LINK_HAS_OTA_WORKER
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    if (s_worker_alive.load(std::memory_order_acquire) || s_owned.load(std::memory_order_acquire) ||
        s_rx_active.load(std::memory_order_acquire)) return false;
    if (!sha256 || !version || !*version || !firmware_size) {
        if (error_code) *error_code = kErrInvalidPayload;
        return false;
    }
    // Older Apps omit model. If supplied, never silently truncate it to a matching prefix.
    if (model && *model && s_model[0] && std::strcmp(model, s_model) != 0) {
        if (error_code) *error_code = kErrModelMismatch;
        return false;
    }
#if defined(AGENT_LINK_OTA_REQUIRE_NEWER_VERSION) && AGENT_LINK_OTA_REQUIRE_NEWER_VERSION
    if (!IsNewerVersion(version, agent_link_ota_running_version())) {
        if (error_code) *error_code = kErrVersionIncompat;
        return false;
    }
#endif
    s_partition = UpdatePartition();
    s_previous_boot = esp_ota_get_boot_partition();
    if (!s_partition || !s_previous_boot) {
        if (error_code) *error_code=kErrPartitionFail;
        return false;
    }
    if (firmware_size > s_partition->size) {
        if (error_code) *error_code=kErrFirmwareTooLarge;
        return false;
    }
    if (Prepare() != ESP_OK) {
        if (error_code) *error_code = kErrConfigFailed;
        return false;
    }
    (void)xStreamBufferReset(s_buffer);
    (void)xQueueReset(s_progress);
    s_have_pending=false;
    s_pending_callback_sent=false;
    std::memcpy(s_expected_sha, sha256, sizeof(s_expected_sha));
    s_total.store(firmware_size, std::memory_order_release);
    s_received.store(0, std::memory_order_release);
    s_abort.store(false, std::memory_order_release);
    s_active_generation.store(generation, std::memory_order_release);
    s_success_dispatched.store(0, std::memory_order_release);
    s_last_report_ms = agent_link_now_ms();
    Transition(generation, AGENT_OTA_RECEIVING);
    s_worker_alive.store(true, std::memory_order_release);
    s_owned.store(true, std::memory_order_release);
    if (generation != s_generation.load(std::memory_order_acquire)) {
        s_owned.store(false, std::memory_order_release);
        s_worker_alive.store(false, std::memory_order_release);
        s_abort.store(true, std::memory_order_release);
        Transition(generation, AGENT_OTA_FAILED, kErrAborted);
        if (error_code) *error_code = kErrAborted;
        return false;
    }
    xTaskNotifyGive(s_worker);
    if (error_code) *error_code = kErrSuccess;
    return true;
#else
    (void)firmware_size; (void)sha256; (void)version; (void)model;
    return false;
#endif
}

size_t FeedData(const uint8_t* data, size_t length) {
#if AGENT_LINK_HAS_OTA_WORKER
    if (!data || !length || s_rx_active.exchange(true, std::memory_order_acq_rel)) return 0;
    struct RxGuard { ~RxGuard() { s_rx_active.store(false, std::memory_order_release); } } guard;
    if (!s_owned.load(std::memory_order_acquire)) return 0;
    if (s_state.load(std::memory_order_acquire) != AGENT_OTA_RECEIVING ||
        s_abort.load(std::memory_order_acquire)) return length;  // Drain stale firmware, never play it.
    if (!s_buffer) return 0;
    const uint32_t received = s_received.load(std::memory_order_acquire);
    const uint32_t total = s_total.load(std::memory_order_acquire);
    if (received > total || length > total - received) {
        s_error.store(kErrInvalidPayload, std::memory_order_release);
        s_abort.store(true, std::memory_order_release);
        return length;  // Consume this invalid tail; cancellation remains latched.
    }
    if (xStreamBufferSpacesAvailable(s_buffer) < length) return 0;
    const size_t accepted = xStreamBufferSend(s_buffer, data, length, 0);
    s_received.fetch_add(static_cast<uint32_t>(accepted), std::memory_order_acq_rel);
    if (accepted != length) {
        // A breached credit gate must fail explicitly, not wait for a timeout or hash accident.
        s_error.store(kErrWriteFail, std::memory_order_release);
        s_abort.store(true, std::memory_order_release);
    }
    return accepted;
#else
    (void)data; (void)length;
    return 0;
#endif
}
bool WantsMore() {
#if AGENT_LINK_HAS_OTA_WORKER
    if (s_state.load(std::memory_order_acquire) != AGENT_OTA_RECEIVING) return true;
    if (s_abort.load(std::memory_order_acquire)) return true;  // Drain/discard a cancelled session's tail.
    return s_buffer && xStreamBufferSpacesAvailable(s_buffer) >= kGateBytes;
#else
    return true;
#endif
}
void Abort(const char* reason) {
    (void)reason;
#if AGENT_LINK_HAS_OTA_WORKER
    if (s_worker_alive.load(std::memory_order_acquire)) s_abort.store(true, std::memory_order_release);
#endif
}
bool IsBusy() {
#if AGENT_LINK_HAS_OTA_WORKER
    if (s_active_generation.load(std::memory_order_acquire) != s_generation.load(std::memory_order_acquire)) return false;
    const agent_ota_state_t state = s_state.load(std::memory_order_acquire);
    return state == AGENT_OTA_RECEIVING || state == AGENT_OTA_VERIFYING;
#else
    return false;
#endif
}
bool WorkerActive() {
#if AGENT_LINK_HAS_OTA_WORKER
    return s_worker_alive.load(std::memory_order_acquire);
#else
    return false;
#endif
}
void GetStatus(agent_ota_status_t* out) {
    if (!out) return;
#if AGENT_LINK_HAS_OTA_WORKER
    if (s_active_generation.load(std::memory_order_acquire) == s_generation.load(std::memory_order_acquire)) Snapshot(out);
    else *out = agent_ota_status_t{};
#else
    *out = agent_ota_status_t{};
#endif
}
void OnLinkState(bool connected) {
#if AGENT_LINK_HAS_OTA_WORKER
    if (!connected) InvalidateSession();
#else
    (void)connected;
#endif
    // Health confirmation is explicit: reaching BLE alone does not prove board peripherals work.
}
void Poll() {
#if AGENT_LINK_HAS_OTA_WORKER
    static bool polling = false;
    if (polling) return;
    polling = true;
    struct PollGuard { bool& active; ~PollGuard() { active = false; } } guard{polling};
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    // The transport resumes from the same polling context as its own state machine.
    if (s_resume && WantsMore()) s_resume();
    if (generation != s_generation.load(std::memory_order_acquire)) return;
    const uint32_t now = agent_link_now_ms();
    for (unsigned i = 0; i < 4; ++i) {
        if (!s_have_pending) {
            if (!s_progress || xQueueReceive(s_progress, &s_pending_record, 0) != pdTRUE) break;
            s_have_pending = true;
            s_pending_callback_sent = false;
        }
        const auto& record = s_pending_record;
        if (record.generation != generation ||
            (record.status.state == AGENT_OTA_SUCCESS && s_abort.load(std::memory_order_acquire))) {
            s_have_pending = false;
            continue;
        }
        if (s_pending_callback_sent && now - s_pending_attempt_ms < 200) return;
        s_pending_attempt_ms = now;
        const bool accepted = Dispatch(record.status, !s_pending_callback_sent);
        s_pending_callback_sent = true;
        if (generation != s_generation.load(std::memory_order_acquire)) { s_have_pending = false; return; }
        const bool terminal = record.status.state == AGENT_OTA_SUCCESS || record.status.state == AGENT_OTA_FAILED;
        if (terminal && !accepted) return;  // One retained record, no spin and no repeated board callback.
        if (record.status.state == AGENT_OTA_SUCCESS) s_success_dispatched.store(generation, std::memory_order_release);
        s_have_pending = false;
        s_last_report_ms = now;
        return;  // One progress event / matching callback is the per-poll quota.
    }
    if (!s_have_pending && IsBusy() && now - s_last_report_ms >= 500) {
        s_last_report_ms = now;
        agent_ota_status_t snapshot;
        Snapshot(&snapshot);
        // A worker can finish between IsBusy and Snapshot. Let its queued
        // terminal transition own delivery and the success/reboot handshake.
        if (snapshot.state==AGENT_OTA_RECEIVING || snapshot.state==AGENT_OTA_VERIFYING)
            (void)Dispatch(snapshot, true);
    }
#endif
}
bool OwnsDataChannel() {
#if AGENT_LINK_HAS_OTA_WORKER
    return s_owned.load(std::memory_order_acquire);
#else
    return false;
#endif
}
void SetCallback(agent_ota_progress_cb_t callback, void* context) {
#if AGENT_LINK_HAS_OTA_WORKER
    s_callback = callback;
    s_callback_context = context;
#else
    (void)callback; (void)context;
#endif
}
bool IsSupported() { return Supported(); }
}  // namespace ota
}  // namespace agentlink

extern "C" void agent_link_ota_set_callback(agent_ota_progress_cb_t callback, void* context) {
    agentlink::ota::SetCallback(callback, context);
}
extern "C" void agent_link_ota_get_status(agent_ota_status_t* out) { agentlink::ota::GetStatus(out); }
extern "C" bool agent_link_ota_is_busy(void) { return agentlink::ota::IsBusy(); }
extern "C" bool agent_link_ota_needs_reconnect(void) { return agentlink::ota::OwnsDataChannel() && !agentlink::ota::IsBusy(); }
extern "C" bool agent_link_ota_supported(void) { return agentlink::ota::IsSupported(); }
extern "C" esp_err_t agent_link_ota_prepare(void) { return agentlink::ota::Prepare(); }
extern "C" void agent_link_ota_abort(void) { agentlink::ota::Abort("board requested"); }
extern "C" const char* agent_link_ota_running_version(void) {
#if AGENT_LINK_HAS_OTA_WORKER
    const esp_app_desc_t* descriptor = esp_app_get_description();
    return descriptor && descriptor->version[0] ? descriptor->version : "0.0.0";
#else
    return "0.0.0";
#endif
}
extern "C" esp_err_t agent_link_ota_mark_valid(void) {
#if AGENT_LINK_HAS_OTA_WORKER
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_INVALID_STATE;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    return esp_ota_mark_app_valid_cancel_rollback();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
