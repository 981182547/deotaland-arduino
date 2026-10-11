#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "agent_link_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional App-driven OTA. Disabled unless config.enable_ota is true.
 * ESP32 needs an OTA partition layout and a reliable, backpressured data channel.
 * Other platforms report unsupported and never acknowledge an upgrade as started.
 * After an accepted upgrade aborts or fails, reconnect before sending audio or retrying OTA.
 * Define AGENT_LINK_OTA_REQUIRE_NEWER_VERSION=1 for all library translation
 * units to reject an equal/older numeric image version (default: accept any).
 * Compares esp_app_desc's running version, independently of firmware_rev.
 * Versions use 1-3 uint32 numeric components; -prerelease/+build suffixes are
 * ignored for ordering. Invalid candidates are rejected; a nonnumeric running
 * version allows a valid candidate so a device can recover.
 */
typedef enum {
    AGENT_OTA_IDLE = 0,
    AGENT_OTA_RECEIVING = 1,
    AGENT_OTA_VERIFYING = 2,
    AGENT_OTA_SUCCESS = 3,
    AGENT_OTA_FAILED = 4,
} agent_ota_state_t;

typedef struct {
    agent_ota_state_t state;
    uint32_t received_bytes;
    uint32_t total_bytes;
    uint16_t error_code;
} agent_ota_status_t;

/** Invoked from agent_link_poll(), never from the flash worker. Return quickly. */
typedef void (*agent_ota_progress_cb_t)(const agent_ota_status_t* status, void* context);
void agent_link_ota_set_callback(agent_ota_progress_cb_t callback, void* context);
void agent_link_ota_get_status(agent_ota_status_t* out);
bool agent_link_ota_is_busy(void);
/** True after an accepted OTA finishes/aborts but still owns the shared data
 * channel. Reconnect before another OTA or AUDIO/TTS session. Rejected STARTs
 * that never acquired the channel do not require a reconnect. */
bool agent_link_ota_needs_reconnect(void);
/** True only when explicitly enabled and the platform/data channel support OTA. */
bool agent_link_ota_supported(void);
/** Optional setup-time prewarming, after agent_link_init()/begin(). Reserves the
 * RX buffer, progress queue, internal flash-write scratch and a sleeping worker.
 * Repeated calls are idempotent. Returns NOT_SUPPORTED when OTA is disabled or
 * unavailable, NOT_FOUND for a missing distinct update slot or valid otadata,
 * NO_MEM on allocation failure; retained partial reservations permit a later
 * retry. Resources remain
 * reserved until device reset. Start still prepares lazily when this is omitted.
 * Prewarming avoids library allocations/task creation in the START command;
 * ESP-IDF may still allocate in its flash worker. Flash operations can pause
 * other tasks and interrupts; this API does not provide hard realtime OTA.
 * Supported IDF versions erase incrementally while writing 4 KiB blocks;
 * define AGENT_LINK_OTA_BULK_ERASE to use whole-image pre-erasure instead.
 * IDF 5.1+ places the RX ring in PSRAM when available, with internal RAM as
 * fallback. Flash-write scratch and worker stack always remain in internal RAM.
 */
esp_err_t agent_link_ota_prepare(void);
/** Cooperative cancellation; a worker currently erasing flash finishes that call first.
 * Completion/status callbacks are dispatched by poll(), not by this call.
 */
void agent_link_ota_abort(void);
/** Explicit board health confirmation. Unsupported platforms return NOT_SUPPORTED. */
esp_err_t agent_link_ota_mark_valid(void);
const char* agent_link_ota_running_version(void);

#ifdef __cplusplus
}
#endif
