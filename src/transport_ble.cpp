// agent_link BLE transport backend — advertising + control-plane GATT (NimBLE).
//
// Control plane:
//   - Advertise the device name; connectable.
//   - GATT Service C 0xFFC0: 0xFFC1 command channel (WRITE receives commands + NOTIFY returns
//     responses), 0xFFC4 event channel (NOTIFY pushes events).
//   - Standard services: 0x180F Battery (0x2A19) + 0x180A Device Information (0x2A29/0x2A24/0x2A26).
//   - A write to 0xFFC1 -> s_on_recv up to the core; send_ctrl routes by frame message_type:
//     response (0x02) -> notify 0xFFC1, event (0x03) -> notify 0xFFC4.
// Data plane:
//   - Voice uplink (device -> App): Service A 0xFFA0, GATT Notify 0xFFA1 (event 0x40 VoiceChunk).
//   - TTS downlink and recording/ASR uplink: L2CAP CoC PSM 0x0081.
// Service B 0xFFB0/0xFFB1 carries file/OTA commands; IMAGE uses CoC PSM 0x0082.
// Public writes enqueue bounded work; only dedicated BLE workers may wait.
#include "agent_link_transport.h"
#include "agent_link.h"
#include "device_identity.h"

#if defined(ARDUINO_ARCH_ESP32) && !defined(AGENT_LINK_DISABLE_BLE)
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#endif

// Ordinary Arduino builds compile the bundled NimBLE-Arduino host, including
// CoC. The optional ESP-IDF component project can explicitly keep IDF's host.
#include "vendor/nimble-arduino/DeotalandNimblePlatform.h"

// Arduino-ESP32 3.3+ releases the controller's BLE memory during initArduino()
// unless a linked BLE library marks it as in use before app_main().  Direct
// NimBLE C API users must include the same marker as the bundled BLE libraries.
// Older Arduino cores do not provide this header and do not need the marker.
#if defined(ARDUINO_ARCH_ESP32) && !defined(AGENT_LINK_DISABLE_BLE) && __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#endif

#if DEOTALAND_NIMBLE_ACTIVE || (defined(DEOTALAND_USE_IDF_NIMBLE) && \
    defined(ARDUINO_ARCH_ESP32) && !defined(AGENT_LINK_DISABLE_BLE) && \
    defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED && \
    defined(SOC_BLE_SUPPORTED) && SOC_BLE_SUPPORTED)

#define DEOTALAND_BLE_AVAILABLE 1

#include <cstring>
#include "agent_link_buffer.h"
#include "agent_link_ble_policy.h"
#include <mutex>
#include <atomic>
#include <algorithm>
#include <new>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if DEOTALAND_NIMBLE_ACTIVE
#include "vendor/nimble-arduino/DeotalandNimble.h"
#include "esp_bt.h"
#include "nvs_flash.h"
#else
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_l2cap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"      // os_msys_get_pkthdr / os_mbuf_copydata
#endif

extern "C" void ble_store_config_init(void);

namespace {
constexpr const char* TAG = "agent_link.ble";

// ── Service A (intercom / realtime voice): device -> App PCM voice stream ──
constexpr uint16_t kSvcVoice      = 0xFFA0;
constexpr uint16_t kChrVoiceDown  = 0xFFA1;  // NOTIFY: device -> App PCM (event 0x40 VoiceChunk)
// ── Service C (device control) ──
constexpr uint16_t kSvcFile = 0xFFB0;
constexpr uint16_t kChrFileCmd = 0xFFB1;
constexpr uint16_t kSvcDeviceControl = 0xFFC0;
constexpr uint16_t kChrCmdRequest    = 0xFFC1;  // WRITE receives commands + NOTIFY returns responses
constexpr uint16_t kChrDeviceEvent   = 0xFFC4;  // NOTIFY pushes events
// Note: 0xFFA2 (PTT) / 0xFFC2 (state) / 0xFFC3 (config) were unused placeholders and have been removed.
// ── Standard services ──
constexpr uint16_t kSvcBattery      = 0x180F;   // Battery Service
constexpr uint16_t kChrBatteryLevel = 0x2A19;   // READ + NOTIFY, 1-byte battery level 0-100
constexpr uint16_t kSvcDeviceInfo   = 0x180A;   // Device Information Service
constexpr uint16_t kChrManufacturer = 0x2A29;   // READ string
constexpr uint16_t kChrModelNumber  = 0x2A24;   // READ string
constexpr uint16_t kChrFirmwareRev  = 0x2A26;   // READ string

// Static UUID objects (avoid BLE_UUID16_DECLARE compound literals in static initialization).
const ble_uuid16_t s_uuid_voice_svc = BLE_UUID16_INIT(kSvcVoice);
const ble_uuid16_t s_uuid_voice     = BLE_UUID16_INIT(kChrVoiceDown);
const ble_uuid16_t s_uuid_file_svc = BLE_UUID16_INIT(kSvcFile);
const ble_uuid16_t s_uuid_file_cmd = BLE_UUID16_INIT(kChrFileCmd);
const ble_uuid16_t s_uuid_svc   = BLE_UUID16_INIT(kSvcDeviceControl);
const ble_uuid16_t s_uuid_cmd   = BLE_UUID16_INIT(kChrCmdRequest);
const ble_uuid16_t s_uuid_evt   = BLE_UUID16_INIT(kChrDeviceEvent);
const ble_uuid16_t s_uuid_svc_batt  = BLE_UUID16_INIT(kSvcBattery);
const ble_uuid16_t s_uuid_batt_lvl  = BLE_UUID16_INIT(kChrBatteryLevel);
const ble_uuid16_t s_uuid_svc_dis   = BLE_UUID16_INIT(kSvcDeviceInfo);
const ble_uuid16_t s_uuid_mfr       = BLE_UUID16_INIT(kChrManufacturer);
const ble_uuid16_t s_uuid_model     = BLE_UUID16_INIT(kChrModelNumber);
const ble_uuid16_t s_uuid_fwrev     = BLE_UUID16_INIT(kChrFirmwareRev);

// Stable product identity advertised for companion-app scan filtering.
// AB883C83-3FCC-4A0F-A951-E18D0C944DA4 in NimBLE little-endian byte order.
const ble_uuid128_t s_uuid_identity = BLE_UUID128_INIT(
    0xA4, 0x4D, 0x94, 0x0C, 0x8D, 0xE1, 0x51, 0xA9,
    0x0F, 0x4A, 0xCC, 0x3F, 0x83, 0x3C, 0x88, 0xAB);

char     s_name[30] = "AgentLink";  // 29-byte name + 2-byte AD header fits scan response
uint8_t  s_own_addr_type = 0;
std::atomic<bool> s_connected{false};
std::atomic<bool> s_event_notify_enabled{false};
std::atomic<bool> s_voice_notify_enabled{false};
std::atomic<uint16_t> s_conn_handle{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> s_host_running{false};
std::atomic<bool> s_stopping{false};
std::atomic<bool> s_started{false};
std::atomic<uint32_t> s_connection_epoch{0};
std::atomic<uint32_t> s_acl_epoch{0};
TaskHandle_t s_shutdown_task = nullptr;
uint32_t NowMs() { return static_cast<uint32_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS; }
bool SessionAlive(uint32_t epoch) {
    return s_connected.load() && !s_stopping.load() && epoch == s_connection_epoch.load();
}
void SleepWorker(uint32_t ms) { vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(ms))); }

// Independent of notification index 0, which belongs exclusively to host
// request completion. Storage survives every stream and task: producers never
// dereference a retired TaskHandle_t. A pending token closes the check/wait race.
class WorkerSignal {
public:
    void Init() { if (!handle_) handle_ = xSemaphoreCreateBinaryStatic(&storage_); }
    void Wake() { if (handle_) (void)xSemaphoreGive(handle_); }
    void Wait(TickType_t timeout = portMAX_DELAY) { (void)xSemaphoreTake(handle_, timeout); }
private:
    StaticSemaphore_t storage_ = {};
    SemaphoreHandle_t handle_ = nullptr;
};
WorkerSignal s_control_wake, s_voice_wake, s_shutdown_wake;
#if defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) && CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0
WorkerSignal s_rec_wake, s_img_wake;
#endif
void WakeWorkers() {
    s_control_wake.Wake(); s_voice_wake.Wake();
#if defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) && CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0
    s_rec_wake.Wake(); s_img_wake.Wake();
#endif
}

ble_npl_event s_terminate_event;
std::atomic<uint32_t> s_terminate_epoch{0};
std::atomic<bool> s_terminate_requested{false};
std::atomic<bool> s_terminate_scheduled{false};

// poll() and stream workers only publish a request. Comparing the epoch and
// terminating the handle happen together on the host, which owns GAP events.
void RequestTerminate(uint32_t epoch) {
    uint32_t pending = s_terminate_epoch.load();
    while (SessionAlive(epoch)) {
        // A delayed old worker must not overwrite a newer session's request.
        if (pending != epoch && SessionAlive(pending)) return;
        if (s_terminate_epoch.compare_exchange_weak(pending, epoch)) {
            s_terminate_requested.store(true, std::memory_order_release);
            s_control_wake.Wake();
            return;
        }
    }
}

void TerminateHostEvent(ble_npl_event*) {
    if (s_terminate_requested.exchange(false)) {
        const uint32_t epoch = s_terminate_epoch.load();
        if (SessionAlive(epoch))
            (void)ble_gap_terminate(s_conn_handle.load(), BLE_ERR_REM_USER_CONN_TERM);
    }
    s_terminate_scheduled.store(false, std::memory_order_release);
    if (s_terminate_requested) s_control_wake.Wake();
}

void ScheduleTerminate() {
    if (s_stopping || !s_terminate_requested) return;
    if (!s_terminate_scheduled.exchange(true))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_terminate_event);
}

// GATT characteristic value handles (filled in by NimBLE at registration; used for notify).
uint16_t s_h_file = 0;
uint16_t s_h_cmd   = 0; // 0xFFC1
uint16_t s_h_evt   = 0; // 0xFFC4
uint16_t s_h_voice = 0; // 0xFFA1 (voice upload)
uint16_t s_h_batt  = 0; // 0x2A19 (battery, notify)

// Standard-service values (returned by the read callback; battery notifies 0x2A19 on change).
std::atomic<uint8_t> s_batt_level{0};
std::atomic<bool> s_batt_notify_enabled{false};
std::atomic<bool> s_batt_dirty{false};
std::mutex s_device_info_mtx;              // current battery level 0-100
char    s_di_mfr[AGENT_LINK_MANUFACTURER_MAX + 1] = "Deotaland";
char    s_di_model[AGENT_LINK_MODEL_MAX + 1] = "AgentLink";
char    s_di_fw[AGENT_LINK_FIRMWARE_MAX + 1] = "1.0.0";

// Transport -> core uplink callbacks (installed by the core via set_recv/set_conn/set_stream_recv).
std::atomic<void (*)(const uint8_t*, size_t)> s_on_recv{nullptr};
std::atomic<esp_err_t (*)(const uint8_t*, size_t)> s_on_ctrl_checked{nullptr};
std::atomic<void (*)(bool)> s_on_conn{nullptr};
std::atomic<void (*)(agent_stream_t, const uint8_t*, size_t)> s_on_stream{nullptr};  // data-plane receive (L2CAP)
std::atomic<void (*)(void)> s_on_ready{nullptr};  // peer subscribed to 0xFFC4 -> notify reachable (core sends the manifest)
std::atomic<void (*)(const agent_link_status_t*)> s_on_status{nullptr};

// ── L2CAP CoC (data plane: receive App downlink TTS voice, PSM 0x0081) ──
#if defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) && CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0
#define DEOTALAND_BLE_L2CAP_COC_AVAILABLE 1
constexpr uint16_t kL2capPsm = 0x0081;  // PSM of the downlink voice channel
// NimBLE grants ceil(MTU / MPS) initial credits. A single retained RX SDU
// therefore requires a one-credit window: otherwise the peer can legally send
// another short SDU before poll() rearms its buffer. MTU is negotiated by CoC;
// it does not change the byte-stream protocol.
constexpr uint16_t kL2capMtu = std::min<unsigned>(4096, MYNEWT_VAL(BLE_L2CAP_COC_MPS));
static_assert(kL2capMtu >= 23, "NimBLE CoC MPS must support the minimum MTU");
constexpr uint16_t kL2capMps = 512;     // per-packet size (the SDU rx buffer is allocated to this)
std::atomic<struct ble_l2cap_chan*> s_l2cap_chan{nullptr};
std::atomic<bool> s_l2cap_connected{false};
std::atomic<bool> s_l2cap_stalled{false};
constexpr uint16_t kImgPsm = 0x0082;
// A host reset preserves the CoC server registry. NimBLE-Arduino's raw
// create_server allocates another entry even for an existing PSM, so each
// server must be registered once per port lifetime, rather than per sync.
bool s_voice_server_registered = false;
bool s_image_server_registered = false;
std::atomic<struct ble_l2cap_chan*> s_img_chan{nullptr};
std::atomic<bool> s_img_connected{false};
std::atomic<bool> s_img_stalled{false};
std::atomic<uint32_t> s_l2cap_epoch{0};
std::atomic<uint32_t> s_img_epoch{0};
std::atomic<uint16_t> s_l2cap_peer_mtu{0};
std::atomic<uint16_t> s_img_peer_mtu{0};
std::atomic<struct os_mbuf*> s_rx_pending{nullptr};
std::atomic<bool> s_l2cap_rearm_pending{false};
std::atomic<bool> s_img_rearm_pending{false};
std::atomic<uint32_t> s_rx_epoch{0};
size_t s_rx_offset = 0;
bool s_rx_rearm_needed = false;  // poll-owned until published to the host
size_t (*s_data_receiver)(const uint8_t*, size_t) = nullptr;
bool (*s_data_ready)(void) = nullptr;

// Only the NimBLE host may dereference CoC channel pointers. Workers hand over
// one bounded SDU at a time; shutdown keeps the host alive until they retire.
struct CocSendRequest {
    ble_npl_event event;
    TaskHandle_t waiter = nullptr;
    std::atomic<bool> done{true};
    bool image = false;
    uint32_t epoch = 0;
    uint32_t channel_epoch = 0;
    struct os_mbuf* sdu = nullptr;
    int result = BLE_HS_ENOTCONN;
};
CocSendRequest s_rec_send;
CocSendRequest s_img_send;
ble_npl_event s_rearm_event;
std::atomic<bool> s_rearm_scheduled{false};
std::atomic<bool> s_rearm_retry{false};
std::atomic<uint32_t> s_rearm_failed_at{0};
std::atomic<uint32_t> s_rearm_epoch{0};
std::atomic<uint32_t> s_img_rearm_epoch{0};
void ScheduleL2capRearm();

constexpr size_t   kRecMaxSdu       = kL2capMps;
#ifndef AGENT_LINK_BLE_AUDIO_BUFFER_BYTES
#define AGENT_LINK_BLE_AUDIO_BUFFER_BYTES (16u * 1024u)
#endif
constexpr size_t kRecQueueHardCap = AGENT_LINK_BLE_AUDIO_BUFFER_BYTES;
constexpr uint32_t kRecStallWaitMs  = 3000;
constexpr uint32_t kRecStartDelayMs = 120;
struct RecordingState {
    std::atomic<bool> running{false};
    std::atomic<bool> active{false};
    std::atomic<bool> end_req{false};
    std::atomic<bool> truncated{false};
    std::atomic<bool> complete{true};
    uint32_t transfer_id = 0;
    uint32_t epoch = 0;
    uint32_t channel_epoch = 0;
    std::atomic<uint32_t> accepted_bytes{0};
    std::atomic<uint32_t> rejected_writes{0};
    std::atomic<int> last_error{ESP_OK};
    agentlink::FixedBytes<128> start_name;
    std::atomic<uint32_t> sent_bytes{0};
    agentlink::ByteRing q;
    std::mutex mtx;
    std::atomic<size_t> queued_bytes{0};
    agentlink::FixedBytes<60> final_header;
};
RecordingState s_rec;
uint32_t s_rec_counter = 0;
#ifndef AGENT_LINK_BLE_IMAGE_BUFFER_BYTES
#define AGENT_LINK_BLE_IMAGE_BUFFER_BYTES (32u * 1024u)
#endif
constexpr size_t kImgQueueHardCap = AGENT_LINK_BLE_IMAGE_BUFFER_BYTES;
struct ImageState {
    std::atomic<bool> running{false};
    std::atomic<bool>     active{false};
    std::atomic<bool>     end_req{false};
    std::atomic<bool>     truncated{false};   // overflow / disconnect / send failure -> 0x55 status=1
    std::atomic<bool>     complete{true};     // caller's complete flag, captured at end
    uint32_t              transfer_id = 0;
    uint32_t              epoch = 0;
    uint32_t              channel_epoch = 0;
    std::atomic<uint32_t> accepted_bytes{0};
    std::atomic<uint32_t> rejected_writes{0};
    std::atomic<int> last_error{ESP_OK};
    std::atomic<uint32_t> sent_bytes{0};      // -> 0x55 valid_bytes
    agentlink::ByteRing q;
    std::mutex            mtx;
    std::atomic<size_t>   queued_bytes{0};
    uint8_t               format = 0;          // agent_image_format_t, carried in 0x54
    uint16_t              width = 0;
    uint16_t              height = 0;
    uint32_t              total_len = 0;       // total image bytes, carried in 0x54
};
ImageState s_img;
uint32_t   s_img_counter = 0;


#else
#define DEOTALAND_BLE_L2CAP_COC_AVAILABLE 0
#endif

void StartAdvertising();

// GATT access callback: command writes + standard-service reads.
int GattAccess(uint16_t /*conn*/, uint16_t /*attr*/, struct ble_gatt_access_ctxt* ctxt, void* /*arg*/) {
    const ble_uuid_t* uuid = ctxt->chr->uuid;
    if (uuid->type != BLE_UUID_TYPE_16) return BLE_ATT_ERR_UNLIKELY;
    const uint16_t chr = BLE_UUID16(uuid)->value;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        // Only the command channel 0xFFC1 takes writes -> copy the bytes out -> up to the core to parse.
        if ((chr == kChrCmdRequest || chr == kChrFileCmd) && ctxt->om) {
            uint8_t buf[512];
            if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            uint16_t n = 0;
            if (s_stopping) return BLE_ATT_ERR_UNLIKELY;
            if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0 || n == 0)
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (auto checked = s_on_ctrl_checked.load()) {
                if (checked(buf, n) != ESP_OK) return BLE_ATT_ERR_INSUFFICIENT_RES;
            } else if (auto cb = s_on_recv.load()) {
                cb(buf, n);
            } else return BLE_ATT_ERR_UNLIKELY;
        }
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        if (s_stopping) return BLE_ATT_ERR_UNLIKELY;
        std::unique_lock<std::mutex> lock(s_device_info_mtx, std::try_to_lock);
        if (!lock.owns_lock()) return BLE_ATT_ERR_INSUFFICIENT_RES;
        const uint8_t battery = s_batt_level.load();
        // Standard-service read: return battery level / device-info strings.
        const void* val = nullptr;
        uint16_t vlen = 0;
        switch (chr) {
        case kChrBatteryLevel: val = &battery; vlen = 1; break;
        case kChrManufacturer: val = s_di_mfr;   vlen = static_cast<uint16_t>(strlen(s_di_mfr));   break;
        case kChrModelNumber:  val = s_di_model; vlen = static_cast<uint16_t>(strlen(s_di_model)); break;
        case kChrFirmwareRev:  val = s_di_fw;    vlen = static_cast<uint16_t>(strlen(s_di_fw));    break;
        default: return 0;  // other reads: empty
        }
        return os_mbuf_append(ctxt->om, val, vlen) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return 0;
    }
}

// Service C (device control): 0xFFC1 command (WRITE+NOTIFY) + 0xFFC4 event (NOTIFY). Placeholders removed.
const struct ble_gatt_chr_def s_chrs[] = {
    { .uuid = &s_uuid_cmd.u, .access_cb = GattAccess,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_cmd },
    { .uuid = &s_uuid_evt.u, .access_cb = GattAccess,
      .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_evt },
    { 0 },
};
// Service A (intercom / realtime voice): only 0xFFA1 VOICE_DOWN (NOTIFY). PTT placeholder removed.
const struct ble_gatt_chr_def s_chrs_voice[] = {
    { .uuid = &s_uuid_voice.u, .access_cb = GattAccess,
      .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_voice },
    { 0 },
};
// Standard service 0x180F Battery: 0x2A19 level (READ+NOTIFY).
const struct ble_gatt_chr_def s_chrs_batt[] = {
    { .uuid = &s_uuid_batt_lvl.u, .access_cb = GattAccess,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_batt },
    { 0 },
};
// Standard service 0x180A Device Information: manufacturer / model / firmware (all READ).
const struct ble_gatt_chr_def s_chrs_dis[] = {
    { .uuid = &s_uuid_mfr.u,   .access_cb = GattAccess, .flags = BLE_GATT_CHR_F_READ },
    { .uuid = &s_uuid_model.u, .access_cb = GattAccess, .flags = BLE_GATT_CHR_F_READ },
    { .uuid = &s_uuid_fwrev.u, .access_cb = GattAccess, .flags = BLE_GATT_CHR_F_READ },
    { 0 },
};
const struct ble_gatt_chr_def s_chrs_file[] = {
    { .uuid = &s_uuid_file_cmd.u, .access_cb = GattAccess,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_file },
    { 0 },
};
const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_file_svc.u, .characteristics = s_chrs_file },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_voice_svc.u, .characteristics = s_chrs_voice },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc.u,       .characteristics = s_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc_batt.u,  .characteristics = s_chrs_batt },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc_dis.u,   .characteristics = s_chrs_dis },
    { 0 },
};

esp_err_t RegisterGatt() {
#if DEOTALAND_NIMBLE_ACTIVE
    // The bundled raw host leaves service registration to the application.
    // Register these exactly once per port_init, before our own services.
    ble_svc_gap_init();
    ble_svc_gatt_init();
#else
    // ESP-IDF's port already registers these services during port_init.
#endif
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return ESP_FAIL; }
    return ESP_OK;
}

struct NotifyRequest {
    ble_npl_event event;
    TaskHandle_t waiter = nullptr;
    std::atomic<bool> done{true};
    uint16_t handle = 0;
    uint32_t epoch = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
};
NotifyRequest s_notify_request;
std::mutex s_notify_mtx;

esp_err_t NotifyHost(uint16_t handle, const uint8_t* data, size_t len, uint32_t epoch) {
    if (!s_connected || s_stopping || handle == 0) return ESP_ERR_INVALID_STATE;
    const uint16_t conn = s_conn_handle.load();
    if (conn == BLE_HS_CONN_HANDLE_NONE || !SessionAlive(epoch)) return ESP_ERR_INVALID_STATE;
    const size_t mtu = ble_att_mtu(conn);
    if (!data || len == 0 || mtu < 3 || len > mtu - 3 || len > 512) return ESP_ERR_INVALID_SIZE;
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(conn, handle, om);  // on failure NimBLE has already freed om
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void NotifyHostEvent(ble_npl_event*) {
    auto& request = s_notify_request;
    request.result = NotifyHost(request.handle, request.data, request.len, request.epoch);
    request.done.store(true, std::memory_order_release);
    xTaskNotifyGive(request.waiter);
}

// Worker-only. The host validates both connection handle and epoch in the
// same event as notify, so a recycled handle cannot receive an old packet.
esp_err_t Notify(uint16_t handle, const uint8_t* data, size_t len, uint32_t epoch) {
    std::lock_guard<std::mutex> lock(s_notify_mtx);
    if (!SessionAlive(epoch)) return ESP_ERR_INVALID_STATE;
    s_notify_request.handle = handle;
    s_notify_request.epoch = epoch;
    s_notify_request.data = data;
    s_notify_request.len = len;
    s_notify_request.waiter=xTaskGetCurrentTaskHandle();
    s_notify_request.done.store(false, std::memory_order_release);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_notify_request.event);
    do { (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY); }
    while (!s_notify_request.done.load(std::memory_order_acquire));
    return s_notify_request.result;
}

// Control notifications may be produced both by the Arduino loop and by the
// NimBLE host callback that handles a command write.  Sending directly from
// either context loses frames whenever the controller's mbuf/notify window is
// temporarily full (and the host callback cannot block waiting for itself).
// Queue them and let one worker preserve ordering and retry with backpressure.
struct ControlPacket {
    uint16_t handle;
    uint32_t epoch;
    agentlink::FixedBytes<512> data;
    std::atomic<int>* completion = nullptr;  // optional worker-owned delivery fence
    WorkerSignal* completion_wake = nullptr;
};
#ifndef AGENT_LINK_BLE_CTRL_SLOTS
#define AGENT_LINK_BLE_CTRL_SLOTS 16
#endif
constexpr size_t kQueuePacketHardCap = AGENT_LINK_BLE_CTRL_SLOTS;
static_assert(kQueuePacketHardCap >= 4, "BLE control queue needs at least four slots");
constexpr size_t kControlQueueHardCap = kQueuePacketHardCap * 512;
std::atomic<bool> s_control_running{false};
agentlink::FixedQueue<ControlPacket,kQueuePacketHardCap> s_control_q;
std::mutex s_control_mtx;
size_t s_control_queued_bytes = 0;

void ClearControlQueue() {
    std::lock_guard<std::mutex> lock(s_control_mtx);
    while (!s_control_q.empty()) {
        if (s_control_q.front().completion) {
            s_control_q.front().completion->store(ESP_ERR_INVALID_STATE);
            s_control_q.front().completion_wake->Wake();
        }
        s_control_q.pop();
    }
    s_control_queued_bytes = 0;
}

void PumpBattery() {
    if (s_batt_notify_enabled && s_batt_dirty.exchange(false)) {
        const uint8_t level = s_batt_level.load();
        if (Notify(s_h_batt, &level, 1, s_connection_epoch.load()) != ESP_OK)
            s_batt_dirty.store(true);
    }
}

void ControlTask(void*) {
    unsigned burst = 0;
    while (!s_stopping.load(std::memory_order_acquire)) {
        ScheduleTerminate();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        ScheduleL2capRearm();
#endif
        ControlPacket packet = {};
        {
            std::lock_guard<std::mutex> lock(s_control_mtx);
            if (!s_control_q.empty()) {
                packet = std::move(s_control_q.front());
                s_control_q.pop();
                s_control_queued_bytes -= packet.data.size();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
                s_rec_wake.Wake(); s_img_wake.Wake();
#endif
            }
        }
        if (packet.data.empty()) {
            // BAS is a replaceable state, not an ordered protocol frame. Keep
            // the latest level out of the FIFO and retry without losing changes.
            PumpBattery();
            bool retry = s_batt_notify_enabled && s_batt_dirty;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
            retry = retry || s_l2cap_rearm_pending || s_img_rearm_pending;
#endif
            s_control_wake.Wait(retry ? std::max<TickType_t>(1, pdMS_TO_TICKS(10)) : portMAX_DELAY);
            continue;
        }
        const uint32_t started = NowMs();
        esp_err_t result = ESP_ERR_INVALID_STATE;
        while (SessionAlive(packet.epoch)) {
            result = Notify(packet.handle, packet.data.data(), packet.data.size(), packet.epoch);
            if (result == ESP_OK) break;
            if (NowMs() - started >= 3000) {
                ESP_LOGE(TAG, "control notification timeout; disconnecting to preserve ordering");
                RequestTerminate(packet.epoch);
                break;
            }
            s_control_wake.Wait(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
            ScheduleTerminate();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
            ScheduleL2capRearm();
#endif
        }
        if (packet.completion) {
            packet.completion->store(result, std::memory_order_release);
            packet.completion_wake->Wake();
        }
        // A continuous manifest/telemetry producer must not starve BAS or the
        // lower-priority Arduino loop on a single-core target.
        if (++burst == 16) { burst = 0; PumpBattery(); SleepWorker(1); }
    }
    ClearControlQueue();
    s_control_running.store(false);
    s_shutdown_wake.Wake();
    vTaskDelete(nullptr);
}

esp_err_t EnqueueControl(uint16_t handle, const uint8_t* data, size_t len, uint32_t epoch,
                         std::atomic<int>* completion = nullptr, WorkerSignal* completion_wake = nullptr) {
#if defined(__cpp_exceptions)
    try {
#endif
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!SessionAlive(epoch) || handle == 0 || !s_control_running.load()) return ESP_ERR_INVALID_STATE;
    if (handle == s_h_evt && !s_event_notify_enabled) return ESP_ERR_INVALID_STATE;
    std::unique_lock<std::mutex> lock(s_control_mtx, std::try_to_lock);
    if (!lock.owns_lock()) return ESP_ERR_NO_MEM;
    const uint16_t mtu = ble_att_mtu(s_conn_handle.load());
    if (mtu < 3 || len > static_cast<size_t>(mtu - 3) || len > 512) return ESP_ERR_INVALID_SIZE;
    const size_t limit = agentlink::blepolicy::ControlQueueLimit(kQueuePacketHardCap, data, len);
    if (s_control_q.size() >= limit || len > kControlQueueHardCap - s_control_queued_bytes) return ESP_ERR_NO_MEM;
    ControlPacket packet;
    packet.handle = handle;
    packet.epoch = epoch;
    packet.completion = completion;
    packet.completion_wake = completion_wake;
    packet.data.assign(data, data + len);
    s_control_q.push(std::move(packet));
    s_control_queued_bytes += len;
    s_control_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

// ── Voice upload (0x40 VoiceChunk, Notify 0xFFA1) ──
// Integrity-first queue + worker task + MTU-aware slicing.
// Frame overhead = 6 (common header) + session(4) + sequence(4) + flags(1) = 15.
esp_err_t EnqueueControl(uint16_t handle, const uint8_t* data, size_t len) {
    return EnqueueControl(handle, data, len, s_connection_epoch.load());
}

// Only stream workers call this: preserve terminal markers through temporary
// control-queue congestion. A persistent failure is made visible as disconnect.
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
bool EnqueueStreamBoundary(const uint8_t* frame, size_t len, uint32_t epoch,
                           WorkerSignal& wake, bool wait_sent = false) {
    const uint32_t started = NowMs();
    std::atomic<int> completion{INT32_MIN};
    while (SessionAlive(epoch)) {
        const esp_err_t result = EnqueueControl(s_h_evt, frame, len, epoch,
                                               wait_sent ? &completion : nullptr, &wake);
        if (result == ESP_OK) {
            if (!wait_sent) return true;
            // ControlTask completes or cancels every fence before shutdown.
            while (completion.load(std::memory_order_acquire) == INT32_MIN) wake.Wait();
            return completion.load() == ESP_OK;
        }
        if (result != ESP_ERR_NO_MEM || NowMs() - started >= 3000) break;
        wake.Wait(std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
    }
    ESP_LOGE(TAG, "stream terminal notification could not be queued");
    RequestTerminate(epoch);
    return false;
}
#endif

constexpr size_t   kVoiceOverhead      = 15;
constexpr size_t   kMaxFrameSingleMbuf = 220;  // keep a frame in a single mbuf, avoiding chain-alloc failures on high-rate notify
constexpr size_t   kMaxPcmSingleMbuf   = kMaxFrameSingleMbuf - kVoiceOverhead;  // 205
#ifndef AGENT_LINK_BLE_VOICE_BUFFER_BYTES
#define AGENT_LINK_BLE_VOICE_BUFFER_BYTES (16u * 1024u)
#endif
constexpr size_t kVoiceQueueHardCap = AGENT_LINK_BLE_VOICE_BUFFER_BYTES;
static_assert(kVoiceQueueHardCap >= 512, "Voice buffer must hold one worker chunk");
constexpr uint32_t kVoiceMaxRetries    = 300;  // per-slice congestion retry limit

struct VoiceState {
    std::atomic<bool> running{false};
    std::atomic<bool>     active{false};
    std::atomic<bool>     end_req{false};
    uint32_t              session_id = 0;
    uint32_t              epoch = 0;
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> accepted_bytes{0};
    std::atomic<uint32_t> sent_bytes{0};
    std::atomic<uint32_t> rejected_writes{0};
    std::atomic<int> last_error{ESP_OK};
    agentlink::ByteRing q;
    std::mutex            mtx;
    std::atomic<size_t>   queued_bytes{0};
};
VoiceState s_voice;
uint32_t   s_voice_counter = 0;

// Assemble one 0x40 VoiceChunk frame and notify it on 0xFFA1. len <= kMaxPcmSingleMbuf.
bool VoiceSendChunk(uint32_t sequence, const uint8_t* data, size_t len) {
    uint8_t f[kMaxFrameSingleMbuf];
    size_t i = 0;
    f[i++] = 0x01;                          // version
    f[i++] = 0x03;                          // message_type = Event
    f[i++] = 0x40;                          // command_id = VoiceChunk
    f[i++] = 0x00;                          // sequence (unused for Event)
    const uint16_t pl = static_cast<uint16_t>(4 + 4 + 1 + len);
    f[i++] = pl & 0xFF;
    f[i++] = (pl >> 8) & 0xFF;
    f[i++] = s_voice.session_id & 0xFF;     // session_id (little-endian)
    f[i++] = (s_voice.session_id >> 8) & 0xFF;
    f[i++] = (s_voice.session_id >> 16) & 0xFF;
    f[i++] = (s_voice.session_id >> 24) & 0xFF;
    f[i++] = sequence & 0xFF;               // sequence (little-endian, monotonic across slices)
    f[i++] = (sequence >> 8) & 0xFF;
    f[i++] = (sequence >> 16) & 0xFF;
    f[i++] = (sequence >> 24) & 0xFF;
    f[i++] = 0x00;                          // flags (0 = normal frame)
    memcpy(f + i, data, len);
    i += len;
    return Notify(s_h_voice, f, i, s_voice.epoch) == ESP_OK;
}

void VoiceSession() {
    ESP_LOGI(TAG, "voice task started (session=%u)", static_cast<unsigned>(s_voice.session_id));
    bool failed = false;
    while (!failed) {
        if (!SessionAlive(s_voice.epoch) || !s_voice_notify_enabled) {
            s_voice.last_error = ESP_ERR_INVALID_STATE;
            ESP_LOGW(TAG, "voice: link down — stop"); break;
        }
        const bool end_req = s_voice.end_req.load(std::memory_order_acquire);
        const size_t max_pcm = agentlink::blepolicy::VoicePayload(ble_att_mtu(s_conn_handle.load()));
        if (!max_pcm) { s_voice.last_error = ESP_ERR_INVALID_SIZE; break; }

        uint8_t chunk[512];
        size_t chunk_size=0;
        {
            std::lock_guard<std::mutex> lock(s_voice.mtx);
            const size_t count = agentlink::blepolicy::VoiceDrainBytes(s_voice.q.size(), max_pcm, sizeof(chunk));
            chunk_size=s_voice.q.pop(chunk,count);
            s_voice.queued_bytes.store(s_voice.q.size(),std::memory_order_release);
        }
        if (chunk_size == 0) {
            if (end_req) { ESP_LOGI(TAG, "voice: drained — done"); break; }
            s_voice_wake.Wait();
            continue;
        }

        for (size_t off = 0; off < chunk_size; ) {
            const size_t remaining = chunk_size - off;
            const size_t n = (remaining < max_pcm) ? remaining : max_pcm;
            const uint32_t seq = s_voice.sequence.fetch_add(1, std::memory_order_release);

            bool sent = false;
            for (uint32_t retry = 0; retry < kVoiceMaxRetries; retry++) {
                if (!SessionAlive(s_voice.epoch)) break;
                if (VoiceSendChunk(seq, chunk + off, n)) { sent = true; break; }
                // congestion backoff: 1ms x5 -> 5ms x15 -> 10ms (a failed notify signals mbuf-pool pressure).
                const uint32_t d = (retry < 5) ? 1 : (retry < 20) ? 5 : 10;
                SleepWorker(d);
            }
            if (!sent) {
                ESP_LOGE(TAG, "voice: slice send failed; closing incomplete session");
                s_voice.end_req.store(true);
                s_voice.last_error = ESP_ERR_TIMEOUT;
                RequestTerminate(s_voice.epoch);
                failed = true;
                break;
            }
            off += n;
            s_voice.sent_bytes.fetch_add(static_cast<uint32_t>(n));
            // Notify already blocks until the host completes. Yield only once
            // per burst; a tick per packet artificially limits PCM throughput.
            if ((seq & 15u) == 15u) SleepWorker(1);
        }
    }
    {
        std::lock_guard<std::mutex> lock(s_voice.mtx);
        s_voice.q.clear();
        s_voice.queued_bytes.store(0);
    }
    s_voice.active.store(false, std::memory_order_release);
}

void VoiceTask(void*) {
    while (!s_stopping.load(std::memory_order_acquire)) {
        if (s_voice.active.load(std::memory_order_acquire)) VoiceSession();
        else s_voice_wake.Wait();
    }
    if (s_voice.active.load()) s_voice.last_error = ESP_ERR_INVALID_STATE;
    s_voice.active.store(false, std::memory_order_release);
    s_voice.running.store(false, std::memory_order_release);
    s_shutdown_wake.Wake();
    vTaskDelete(nullptr);
}

esp_err_t PrepareVoice() {
    if (s_voice.running.load(std::memory_order_acquire)) return ESP_OK;
    {   // Also realize any platform mutex bookkeeping during preparation.
        std::lock_guard<std::mutex> lock(s_voice.mtx);
        if (!s_voice.q.reserve(kVoiceQueueHardCap)) return ESP_ERR_NO_MEM;
    }
    s_voice.running.store(true, std::memory_order_release);
    if (xTaskCreate(VoiceTask, "agentlink_voice", 6144, nullptr, 6, nullptr) != pdPASS) {
        s_voice.running.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t VoiceStart() {
#if defined(__cpp_exceptions)
    try {
#endif
    // Preparation may allocate/yield. Bind the caller's original peer before
    // doing any work; a reconnect must never lend its epoch to this open.
    const uint32_t epoch = s_connection_epoch.load(std::memory_order_acquire);
    if (s_voice.active.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;  // already in a session
    if (!SessionAlive(epoch) || !s_voice_notify_enabled) return ESP_ERR_INVALID_STATE;
    const esp_err_t prepared = PrepareVoice();
    if (prepared != ESP_OK) return prepared;
    {   // reset queue + counters
        std::lock_guard<std::mutex> lk(s_voice.mtx);
        s_voice.q.clear();
    }
    s_voice.queued_bytes.store(0, std::memory_order_release);
    s_voice.sequence.store(0, std::memory_order_release);
    s_voice.accepted_bytes = 0; s_voice.sent_bytes = 0;
    s_voice.rejected_writes = 0; s_voice.last_error = ESP_OK;
    s_voice.end_req.store(false, std::memory_order_release);
    s_voice.epoch = epoch;
    if (!SessionAlive(epoch) || !s_voice_notify_enabled) return ESP_ERR_INVALID_STATE;
    s_voice.session_id = ++s_voice_counter;
    s_voice.active.store(true, std::memory_order_release);
    s_voice_wake.Wake();
    ESP_LOGD(TAG, "voice session %u started (mtu=%u)",
             static_cast<unsigned>(s_voice.session_id),
             static_cast<unsigned>(s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn_handle) : 0));
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

esp_err_t VoiceEnqueue(const uint8_t* data, size_t len) {
#if defined(__cpp_exceptions)
    try {
#endif
    if (!s_voice.active.load(std::memory_order_acquire) || s_voice.end_req.load() || !SessionAlive(s_voice.epoch)) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0 || (len & 1u)) return ESP_ERR_INVALID_ARG;
    if (len > kVoiceQueueHardCap) return ESP_ERR_INVALID_SIZE;
    std::unique_lock<std::mutex> lock(s_voice.mtx, std::try_to_lock);
    if (!lock.owns_lock()) return ESP_ERR_NO_MEM;
    if (!s_voice.active.load() || s_voice.end_req.load() || !SessionAlive(s_voice.epoch)) return ESP_ERR_INVALID_STATE;
    if (len > UINT32_MAX - s_voice.accepted_bytes.load()) return ESP_ERR_INVALID_SIZE;
    // Integrity-first, with a hard cap as a backstop: when the link is bad, drop new frames to keep the head (the App at least gets the start of the voice).
    if (len > s_voice.q.available()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_voice.q.push(data,len)) return ESP_ERR_NO_MEM;
    s_voice.accepted_bytes.fetch_add(static_cast<uint32_t>(len));
    s_voice.queued_bytes.fetch_add(len, std::memory_order_release);
    s_voice_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

esp_err_t VoiceEnd(bool complete) {
    if (!s_voice.active.load(std::memory_order_acquire)) return ESP_OK;
    if (!complete) s_voice.last_error = ESP_ERR_INVALID_STATE;
    s_voice.end_req.store(true, std::memory_order_release);  // the worker drains the rest, then exits on its own
    s_voice_wake.Wake();
    return ESP_OK;
}

// ── L2CAP CoC receive (downlink TTS voice, PSM 0x0081) ──
// Replenishing the rx buffer grants the peer credit and keeps the channel able to receive.
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
bool RearmL2capRx(struct ble_l2cap_chan* chan) {
    if (!chan || s_stopping) return false;
    struct os_mbuf* rx = os_msys_get_pkthdr(kL2capMps, 0);
    if (!rx) return false;
    if (ble_l2cap_recv_ready(chan, rx) != 0) { os_mbuf_free_chain(rx); return false; }
    return true;
}

void RearmL2capHost(ble_npl_event*) {
    bool failed = false;
    if (!s_stopping) {
        if (s_l2cap_rearm_pending.exchange(false) &&
            s_rearm_epoch.load() == s_l2cap_epoch.load() && s_l2cap_connected) {
            if (!RearmL2capRx(s_l2cap_chan.load())) { s_l2cap_rearm_pending = true; failed = true; }
        }
        if (s_img_rearm_pending.exchange(false) &&
            s_img_rearm_epoch.load() == s_img_epoch.load() && s_img_connected) {
            if (!RearmL2capRx(s_img_chan.load())) { s_img_rearm_pending = true; failed = true; }
        }
    }
    if (failed) s_rearm_failed_at = NowMs();
    s_rearm_retry = failed;
    s_rearm_scheduled.store(false, std::memory_order_release);
    if (s_l2cap_rearm_pending || s_img_rearm_pending) s_control_wake.Wake();
}

// Queue insertion belongs to a worker too: FreeRTOS may wait if its event
// queue is full. poll() only publishes flags and never waits for the host.
void ScheduleL2capRearm() {
    if (s_stopping || (!s_l2cap_rearm_pending && !s_img_rearm_pending)) return;
    if (s_rearm_retry && NowMs() - s_rearm_failed_at.load() < 10) return;
    if (!s_rearm_scheduled.exchange(true))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_rearm_event);
}

void SendL2capHost(ble_npl_event* event) {
    auto* request = static_cast<CocSendRequest*>(ble_npl_event_get_arg(event));
    auto& channel = request->image ? s_img_chan : s_l2cap_chan;
    auto& channel_epoch = request->image ? s_img_epoch : s_l2cap_epoch;
    auto& connected = request->image ? s_img_connected : s_l2cap_connected;
    auto& stalled = request->image ? s_img_stalled : s_l2cap_stalled;
    struct os_mbuf* sdu = request->sdu;
    request->sdu = nullptr;
    request->result = BLE_HS_ENOTCONN;
    if (SessionAlive(request->epoch) && request->channel_epoch == channel_epoch.load() &&
        connected && channel.load()) {
        // Set before send: an immediate TX_UNSTALLED callback must win.
        stalled = true;
        request->result = ble_l2cap_send(channel.load(), sdu);
        if (request->result != BLE_HS_ESTALLED) stalled = false;
        // NimBLE rejects these before taking ownership. Other send failures
        // arise after ownership transfer and consume the SDU internally.
        if (request->result == BLE_HS_EBUSY || request->result == BLE_HS_EBADDATA)
            os_mbuf_free_chain(sdu);
    } else os_mbuf_free_chain(sdu);
    request->done.store(true, std::memory_order_release);
    xTaskNotifyGive(request->waiter);
}

int SendL2capWorker(CocSendRequest& request, bool image, uint32_t epoch,
                    uint32_t channel_epoch, const uint8_t* data, size_t len) {
    if (!SessionAlive(epoch)) return -2;
    struct os_mbuf* sdu = os_msys_get_pkthdr(len, 0);
    if (!sdu) return -1;  // allocation before send is safe to retry
    if (os_mbuf_append(sdu, data, len) != 0) { os_mbuf_free_chain(sdu); return -1; }
    request.image = image;
    request.epoch = epoch;
    request.channel_epoch = channel_epoch;
    request.sdu = sdu;
    request.waiter=xTaskGetCurrentTaskHandle();
    request.done.store(false, std::memory_order_release);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &request.event);
    // Shutdown first retires workers, then stops the host. Even canceled
    // requests must return ownership before this request slot is reused.
    do { (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY); }
    while (!request.done.load(std::memory_order_acquire));
    if (request.result == 0) return 0;
    if (request.result == BLE_HS_ESTALLED) return 1;
    return -2;  // a partial SDU may be on air; never replay it after an error
}

bool WaitL2capTx(bool image, uint32_t epoch, uint32_t channel_epoch) {
    const uint32_t started = NowMs();
    auto& stalled = image ? s_img_stalled : s_l2cap_stalled;
    auto& connected = image ? s_img_connected : s_l2cap_connected;
    auto& current_epoch = image ? s_img_epoch : s_l2cap_epoch;
    auto& truncated = image ? s_img.truncated : s_rec.truncated;
    while (stalled.load(std::memory_order_acquire)) {
        const uint32_t elapsed = NowMs() - started;
        if (!SessionAlive(epoch) || !connected || current_epoch.load() != channel_epoch ||
            truncated || elapsed >= kRecStallWaitMs) return false;
        const uint32_t remaining = kRecStallWaitMs - elapsed;
        (image ? s_img_wake : s_rec_wake).Wait(std::max<TickType_t>(1, pdMS_TO_TICKS(remaining)));
    }
    return SessionAlive(epoch) && connected && current_epoch.load() == channel_epoch && !truncated;
}

void WaitStreamStart(WorkerSignal& wake, uint32_t epoch) {
    const uint32_t started = NowMs();
    while (SessionAlive(epoch)) {
        const uint32_t elapsed = NowMs() - started;
        if (elapsed >= kRecStartDelayMs) break;
        wake.Wait(std::max<TickType_t>(1, pdMS_TO_TICKS(kRecStartDelayMs-elapsed)));
    }
}

// Retain one SDU until poll() has delivered every byte. Delaying recv_ready
// withholds credit from the peer when the application/OTA queue is full.
void PollL2capRx() {
    if (s_stopping) return;
    const uint32_t connection_epoch = s_connection_epoch.load();
    struct os_mbuf* sdu = s_rx_pending.load(std::memory_order_acquire);
    const uint32_t rx_epoch = s_rx_epoch.load();
    if (sdu && (rx_epoch != s_l2cap_epoch.load() || !s_l2cap_connected)) {
        if (s_rx_pending.exchange(nullptr) == sdu) os_mbuf_free_chain(sdu);
        s_rx_offset = 0;
        s_rx_rearm_needed = false;
        sdu = nullptr;
    }
    if (sdu) {
        const size_t total = OS_MBUF_PKTLEN(sdu);
        uint8_t buf[kL2capMps];
        for (unsigned work = 0; work < 4 && s_rx_offset < total; ++work) {
            if (rx_epoch != s_l2cap_epoch.load() || !s_l2cap_connected) return;
            if (s_data_ready && !s_data_ready()) return;
            const size_t n = std::min<size_t>(sizeof(buf), total - s_rx_offset);
            if (os_mbuf_copydata(sdu, s_rx_offset, n, buf) != 0) {
                ESP_LOGE(TAG, "L2CAP receive copy failed");
                RequestTerminate(connection_epoch);
                return;
            }
            size_t accepted = n;
            if (s_data_receiver) accepted = s_data_receiver(buf, n);
            else if (auto cb = s_on_stream.load()) cb(AGENT_STREAM_VOICE, buf, n);
            else accepted = 0;
            s_rx_offset += std::min(accepted, n);
            if (accepted < n) return;
        }
        if (s_rx_offset < total) return;
        s_rx_pending.store(nullptr, std::memory_order_release);
        os_mbuf_free_chain(sdu);
        s_rx_offset = 0;
        s_rearm_epoch.store(rx_epoch);
        s_rx_rearm_needed = true;
    }
    if (s_rx_rearm_needed && !s_rx_pending.load() && s_l2cap_connected &&
        s_rearm_epoch.load() == s_l2cap_epoch.load() && (!s_data_ready || s_data_ready()))
    {
        s_l2cap_rearm_pending.store(true);
        s_rx_rearm_needed = false;
        s_control_wake.Wake();
    }
}

int L2capEvent(struct ble_l2cap_event* event, void* arg) {
    const bool image = reinterpret_cast<uintptr_t>(arg) == kImgPsm;
    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_ACCEPT: {
        if (s_stopping || (!image && (s_l2cap_connected || s_rx_pending.load())) ||
            (image && s_img_connected)) return BLE_HS_EBUSY;
        struct os_mbuf* rx = os_msys_get_pkthdr(kL2capMps, 0);
        if (!rx) return BLE_HS_ENOMEM;
        const int rc = ble_l2cap_recv_ready(event->accept.chan, rx);
        if (rc != 0) os_mbuf_free_chain(rx);
        return rc;
    }
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status == 0) {
            ble_l2cap_chan_info info = {};
            if (ble_l2cap_get_chan_info(event->connect.chan, &info) != 0 || !info.peer_coc_mtu) {
                (void)ble_l2cap_disconnect(event->connect.chan);
                return 0;
            }
            if (image) {
                s_img_epoch.fetch_add(1); s_img_peer_mtu = info.peer_coc_mtu;
                s_img_stalled = false; s_img_chan = event->connect.chan; s_img_connected = true;
            } else {
                s_l2cap_epoch.fetch_add(1); s_l2cap_peer_mtu = info.peer_coc_mtu;
                s_l2cap_stalled = false; s_l2cap_chan = event->connect.chan; s_l2cap_connected = true;
            }
        }
        return 0;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        if (image && event->disconnect.chan == s_img_chan.load()) {
            s_img_epoch.fetch_add(1);
            s_img_chan = nullptr; s_img_connected = false; s_img_stalled = false;
            s_img_rearm_pending = false; s_img.truncated = true; s_img.end_req = true;
            s_img_wake.Wake();
        } else if (!image && event->disconnect.chan == s_l2cap_chan.load()) {
            s_l2cap_epoch.fetch_add(1);
            s_l2cap_chan = nullptr; s_l2cap_connected = false; s_l2cap_stalled = false;
            s_l2cap_rearm_pending = false; s_rec.truncated = true; s_rec.end_req = true;
            s_rec_wake.Wake();
        }
        return 0;
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
        struct os_mbuf* sdu = event->receive.sdu_rx;
        if (image || s_stopping) {
            os_mbuf_free_chain(sdu);
            if (image && !s_stopping && !RearmL2capRx(event->receive.chan)) {
                s_img_rearm_epoch = s_img_epoch.load();
                s_img_rearm_pending = true;
                s_control_wake.Wake();
            }
        } else {
            struct os_mbuf* empty = nullptr;
            if (!s_rx_pending.load()) s_rx_epoch.store(s_l2cap_epoch.load());
            if (!s_rx_pending.compare_exchange_strong(empty, sdu)) {
                os_mbuf_free_chain(sdu);
                ESP_LOGE(TAG, "unexpected second L2CAP SDU; closing channel");
                (void)ble_l2cap_disconnect(event->receive.chan);
            }
        }
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        if (image && event->tx_unstalled.chan == s_img_chan.load()) {
            if (event->tx_unstalled.status) s_img.truncated = true;
            s_img_stalled = false;
            s_img_wake.Wake();
        } else if (!image && event->tx_unstalled.chan == s_l2cap_chan.load()) {
            if (event->tx_unstalled.status) s_rec.truncated = true;
            s_l2cap_stalled = false;
            s_rec_wake.Wake();
        }
        return 0;
    default: return 0;
    }
}

esp_err_t StartL2capServer() {
    if (!s_voice_server_registered) {
        const int rc = ble_l2cap_create_server(kL2capPsm, kL2capMtu, L2capEvent,
            reinterpret_cast<void*>(static_cast<uintptr_t>(kL2capPsm)));
        if (rc != 0 && rc != BLE_HS_EALREADY) return ESP_FAIL;
        s_voice_server_registered = true;
    }
    if (!s_image_server_registered) {
        const int rc = ble_l2cap_create_server(kImgPsm, kL2capMtu, L2capEvent,
            reinterpret_cast<void*>(static_cast<uintptr_t>(kImgPsm)));
        if (rc != 0 && rc != BLE_HS_EALREADY) return ESP_FAIL;
        s_image_server_registered = true;
    }
    return ESP_OK;
}

bool RecSendStreamStart(uint32_t transfer_id, const char* name, size_t name_len) {
#if defined(__cpp_exceptions)
    try {
#endif
    if ((!name && name_len) || name_len > 128) return false;
    uint8_t frame[138];
    const size_t payload_len=4+name_len;
    frame[0]=1; frame[1]=3; frame[2]=0x52; frame[3]=0;
    frame[4]=static_cast<uint8_t>(payload_len); frame[5]=0;
    for(unsigned i=0;i<4;++i) frame[6+i]=static_cast<uint8_t>(transfer_id>>(8*i));
    if(name_len) memcpy(frame+10,name,name_len);
    return EnqueueStreamBoundary(frame,10+name_len,s_rec.epoch,s_rec_wake,true);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return false;
    }
#endif
}

bool RecSendStreamEnd(uint32_t transfer_id, uint8_t status, uint32_t valid_bytes,
                      const uint8_t* header60) {
    uint8_t frame[6 + 69];
    size_t i = 0;
    frame[i++] = 0x01; frame[i++] = 0x03; frame[i++] = 0x53; frame[i++] = 0x00;
    frame[i++] = 69; frame[i++] = 0;
    frame[i++] = static_cast<uint8_t>(transfer_id);
    frame[i++] = static_cast<uint8_t>(transfer_id >> 8);
    frame[i++] = static_cast<uint8_t>(transfer_id >> 16);
    frame[i++] = static_cast<uint8_t>(transfer_id >> 24);
    frame[i++] = status;
    frame[i++] = static_cast<uint8_t>(valid_bytes);
    frame[i++] = static_cast<uint8_t>(valid_bytes >> 8);
    frame[i++] = static_cast<uint8_t>(valid_bytes >> 16);
    frame[i++] = static_cast<uint8_t>(valid_bytes >> 24);
    if (header60) memcpy(frame + i, header60, 60); else memset(frame + i, 0, 60);
    i += 60;
    return EnqueueStreamBoundary(frame, i, s_rec.epoch, s_rec_wake);
}

int RecL2capSend(const uint8_t* data, size_t len) {
    return SendL2capWorker(s_rec_send, false, s_rec.epoch, s_rec.channel_epoch, data, len);
}

void RecordingSession() {
    if (!RecSendStreamStart(s_rec.transfer_id,
            reinterpret_cast<const char*>(s_rec.start_name.data()), s_rec.start_name.size())) {
        ESP_LOGE(TAG, "audio start notification failed");
        s_rec.truncated = true; s_rec.end_req = true;
    }
    uint32_t last_progress = NowMs();
    unsigned burst = 0;
    if (!s_rec.truncated) WaitStreamStart(s_rec_wake, s_rec.epoch);
    while (true) {
        if (!SessionAlive(s_rec.epoch) || !s_l2cap_connected ||
            s_rec.channel_epoch != s_l2cap_epoch.load() || s_rec.truncated) {
            s_rec.truncated.store(true, std::memory_order_release);
            break;
        }
        const bool end_requested = s_rec.end_req.load(std::memory_order_acquire);
        uint8_t chunk[512];
        size_t chunk_size=0;
        {
            std::lock_guard<std::mutex> lock(s_rec.mtx);
            chunk_size=s_rec.q.pop(chunk,sizeof(chunk));
            s_rec.queued_bytes.store(s_rec.q.size(),std::memory_order_release);
        }
        if (chunk_size == 0) {
            if (end_requested) break;
            last_progress = NowMs();
            s_rec_wake.Wait();
            continue;
        }
        bool abort = false;
        for (size_t offset = 0; offset < chunk_size && !abort;) {
            if (!WaitL2capTx(false, s_rec.epoch, s_rec.channel_epoch)) { abort = true; break; }
            const size_t remaining = chunk_size - offset;
            const size_t count = std::min(remaining, std::min<size_t>(kRecMaxSdu, s_l2cap_peer_mtu.load()));
            const int result = RecL2capSend(chunk + offset, count);
            if (result >= 0) {
                if (!WaitL2capTx(false, s_rec.epoch, s_rec.channel_epoch)) { abort = true; break; }
                s_rec.sent_bytes.fetch_add(static_cast<uint32_t>(count), std::memory_order_release);
                offset += count;
                last_progress = NowMs();
                if (++burst == 16) { burst=0; SleepWorker(1); }
            } else if (result == -2 || !SessionAlive(s_rec.epoch) || !s_l2cap_connected || NowMs() - last_progress >= kRecStallWaitMs) {
                abort = true;
            } else {
                SleepWorker(10);
            }
        }
        if (abort) {
            s_rec.truncated.store(true, std::memory_order_release);
            break;
        }
    }
    if (s_rec.truncated) RequestTerminate(s_rec.epoch);
    {
    const bool ok = !s_rec.truncated.load(std::memory_order_acquire) &&
                    s_rec.complete.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> finish_lock(s_rec.mtx);
    const uint8_t* header = ok && s_rec.final_header.size() >= 60
                                ? s_rec.final_header.data() : nullptr;
    if (!SessionAlive(s_rec.epoch)) s_rec.last_error = ESP_ERR_INVALID_STATE;
    else if (!RecSendStreamEnd(s_rec.transfer_id, ok ? 0 : 1,
                              s_rec.sent_bytes.load(std::memory_order_acquire), header))
        s_rec.last_error = ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "recording/ASR stream %u ended (ok=%d, bytes=%u)",
             static_cast<unsigned>(s_rec.transfer_id), ok,
             static_cast<unsigned>(s_rec.sent_bytes.load(std::memory_order_acquire)));
    s_rec.q.clear();
    s_rec.queued_bytes.store(0);
    finish_lock.unlock();
    }
    if (s_rec.truncated && s_rec.last_error == ESP_OK) s_rec.last_error = ESP_ERR_TIMEOUT;
    else if (!s_rec.complete) s_rec.last_error = ESP_ERR_INVALID_STATE;
    s_rec.active.store(false, std::memory_order_release);
}

void RecordingTask(void*) {
    while (!s_stopping.load(std::memory_order_acquire)) {
        if (s_rec.active.load(std::memory_order_acquire)) RecordingSession();
        else s_rec_wake.Wait();
    }
    if (s_rec.active.load()) s_rec.last_error = ESP_ERR_INVALID_STATE;
    s_rec.active.store(false, std::memory_order_release);
    s_rec.running.store(false, std::memory_order_release);
    s_shutdown_wake.Wake();
    vTaskDelete(nullptr);
}

esp_err_t PrepareRecording() {
    if (s_rec.running.load(std::memory_order_acquire)) return ESP_OK;
    {
        std::lock_guard<std::mutex> lock(s_rec.mtx);
        if (!s_rec.q.reserve(kRecQueueHardCap)) return ESP_ERR_NO_MEM;
    }
    s_rec.running.store(true, std::memory_order_release);
    if (xTaskCreate(RecordingTask, "agentlink_rec", 6144, nullptr, 5, nullptr) != pdPASS) {
        s_rec.running.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t RecordingStart(const char* name, size_t name_len) {
#if defined(__cpp_exceptions)
    try {
#endif
    const uint32_t epoch = s_connection_epoch.load(std::memory_order_acquire);
    const uint32_t channel_epoch = s_l2cap_epoch.load(std::memory_order_acquire);
    if (s_rec.active.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!SessionAlive(epoch) || !s_event_notify_enabled || !s_l2cap_connected) return ESP_ERR_INVALID_STATE;
    if ((!name && name_len) || name_len > 128) return ESP_ERR_INVALID_ARG;
    const uint16_t mtu = ble_att_mtu(s_conn_handle.load());
    if (mtu < 78 || name_len + 10 > static_cast<size_t>(mtu - 3)) return ESP_ERR_INVALID_SIZE;
    const esp_err_t prepared = PrepareRecording();
    if (prepared != ESP_OK) return prepared;
    {
        std::lock_guard<std::mutex> lock(s_rec.mtx);
        s_rec.q.clear();
    }
    s_rec.queued_bytes.store(0, std::memory_order_release);
    s_rec.sent_bytes.store(0, std::memory_order_release);
    s_rec.accepted_bytes = 0;
    s_rec.rejected_writes = 0; s_rec.last_error = ESP_OK;
    s_rec.end_req.store(false, std::memory_order_release);
    s_rec.truncated.store(false, std::memory_order_release);
    s_rec.complete.store(true, std::memory_order_release);
    s_rec.final_header.clear();
    s_rec.epoch = epoch;
    s_rec.channel_epoch = channel_epoch;
    s_rec.start_name.clear();
    if (name_len) s_rec.start_name.assign(reinterpret_cast<const uint8_t*>(name), reinterpret_cast<const uint8_t*>(name) + name_len);
    if (!SessionAlive(epoch) || !s_event_notify_enabled || !s_l2cap_connected ||
        channel_epoch != s_l2cap_epoch.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    s_rec.transfer_id = ++s_rec_counter;
    s_rec.active.store(true, std::memory_order_release);
    s_rec_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

esp_err_t RecordingEnqueue(const uint8_t* data, size_t len) {
#if defined(__cpp_exceptions)
    try {
#endif
    if (!s_rec.active.load(std::memory_order_acquire) || s_rec.end_req || !SessionAlive(s_rec.epoch)) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > kRecQueueHardCap) return ESP_ERR_INVALID_SIZE;
    if (s_rec.truncated.load(std::memory_order_acquire)) return ESP_ERR_NO_MEM;
    std::unique_lock<std::mutex> lock(s_rec.mtx, std::try_to_lock);
    if (!lock.owns_lock()) return ESP_ERR_NO_MEM;
    if (!s_rec.active.load() || s_rec.end_req.load() || !SessionAlive(s_rec.epoch)) return ESP_ERR_INVALID_STATE;
    if (len > UINT32_MAX - s_rec.accepted_bytes) return ESP_ERR_INVALID_SIZE;
    if (len > s_rec.q.available()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_rec.q.push(data,len)) return ESP_ERR_NO_MEM;
    s_rec.accepted_bytes += static_cast<uint32_t>(len);
    s_rec.queued_bytes.fetch_add(len, std::memory_order_release);
    s_rec_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

esp_err_t RecordingEnd(bool complete, const uint8_t* final_header, size_t header_len) {
#if defined(__cpp_exceptions)
    try {
#endif
    if (!s_rec.active.load(std::memory_order_acquire)) return ESP_OK;
    std::unique_lock<std::mutex> lock(s_rec.mtx, std::try_to_lock);
    if (!lock.owns_lock()) return ESP_ERR_NO_MEM;
    s_rec.complete.store(complete, std::memory_order_release);
    if (final_header && header_len >= 60) {
        s_rec.final_header.assign(final_header, final_header + 60);
    } else {
        s_rec.final_header.clear();
    }
    s_rec.end_req.store(true, std::memory_order_release);
    s_rec_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}
// Image snapshot uplink implementation
// Mirrors the audio path; reuses kRecMaxSdu / kRecStallWaitMs / kRecStartDelayMs (generic L2CAP params).
// 0x54 ImageStart: payload(13) = transfer_id(4) + format(1) + width(2) + height(2) + total_len(4). Notify 0xFFC4.
bool ImgSendStreamStart(uint32_t tid, uint8_t fmt, uint16_t w, uint16_t h, uint32_t total) {
    uint8_t f[6 + 13];
    size_t i = 0;
    f[i++] = 0x01; f[i++] = 0x03; f[i++] = 0x54; f[i++] = 0x00;   // ver, Event, 0x54, seq
    f[i++] = 13; f[i++] = 0x00;                                    // payload_len = 13 (LE)
    f[i++] = tid & 0xFF; f[i++] = (tid >> 8) & 0xFF; f[i++] = (tid >> 16) & 0xFF; f[i++] = (tid >> 24) & 0xFF;
    f[i++] = fmt;
    f[i++] = w & 0xFF; f[i++] = (w >> 8) & 0xFF;
    f[i++] = h & 0xFF; f[i++] = (h >> 8) & 0xFF;
    f[i++] = total & 0xFF; f[i++] = (total >> 8) & 0xFF; f[i++] = (total >> 16) & 0xFF; f[i++] = (total >> 24) & 0xFF;
    // The same FIFO as terminal events prevents a new start overtaking the
    // previous transfer's queued end. Send bytes only after this fence passes.
    return EnqueueStreamBoundary(f, i, s_img.epoch, s_img_wake, true);
}

// 0x55 ImageEnd: payload(9) = transfer_id(4) + status(1) + valid_bytes(4). Notify 0xFFC4.
bool ImgSendStreamEnd(uint32_t tid, uint8_t status, uint32_t valid_bytes) {
    uint8_t f[6 + 9];
    size_t i = 0;
    f[i++] = 0x01; f[i++] = 0x03; f[i++] = 0x55; f[i++] = 0x00;
    f[i++] = 9; f[i++] = 0x00;                                     // payload_len = 9 (LE)
    f[i++] = tid & 0xFF; f[i++] = (tid >> 8) & 0xFF; f[i++] = (tid >> 16) & 0xFF; f[i++] = (tid >> 24) & 0xFF;
    f[i++] = status;
    f[i++] = valid_bytes & 0xFF; f[i++] = (valid_bytes >> 8) & 0xFF; f[i++] = (valid_bytes >> 16) & 0xFF; f[i++] = (valid_bytes >> 24) & 0xFF;
    return EnqueueStreamBoundary(f, i, s_img.epoch, s_img_wake);
}

// Send one SDU (<= kRecMaxSdu) over the image L2CAP CoC. Returns 0 = sent, 1 = sent but now stalled, -1 = error.
int ImgL2capSend(const uint8_t* data, size_t len) {
    return SendL2capWorker(s_img_send, true, s_img.epoch, s_img.channel_epoch, data, len);
}

// Worker: drain the image queue to L2CAP in <=MPS SDUs (credit backpressure), then emit 0x55. One image at a time.
void ImageSession() {
    if (!ImgSendStreamStart(s_img.transfer_id, s_img.format, s_img.width, s_img.height, s_img.total_len)) {
        ESP_LOGE(TAG, "image start notification failed");
        s_img.truncated = true; s_img.end_req = true;
    }
    uint32_t last_progress = NowMs();
    unsigned burst = 0;
    ESP_LOGI(TAG, "image worker started (transfer_id=%u)", static_cast<unsigned>(s_img.transfer_id));
    // Head start for 0x54 ImageStart (same race as the audio stream 0x52): let the App register the session before bytes arrive.
    if (!s_img.truncated) WaitStreamStart(s_img_wake, s_img.epoch);
    while (true) {
        if (!SessionAlive(s_img.epoch) || !s_img_connected ||
            s_img.channel_epoch != s_img_epoch.load() || s_img.truncated) { s_img.truncated.store(true, std::memory_order_release); break; }
        const bool end_req = s_img.end_req.load(std::memory_order_acquire);
        uint8_t chunk[512];
        size_t chunk_size=0;
        {
            std::lock_guard<std::mutex> lock(s_img.mtx);
            chunk_size=s_img.q.pop(chunk,sizeof(chunk));
            s_img.queued_bytes.store(s_img.q.size(),std::memory_order_release);
        }
        if (chunk_size == 0) {
            if (end_req) break;                     // drained + end requested -> done
            last_progress = NowMs();
            s_img_wake.Wait();
            continue;
        }
        bool abort = false;
        for (size_t off = 0; off < chunk_size && !abort; ) {
            if (!WaitL2capTx(true, s_img.epoch, s_img.channel_epoch)) { abort = true; break; }
            const size_t n = std::min(chunk_size - off, std::min<size_t>(kRecMaxSdu, s_img_peer_mtu.load()));
            int r = ImgL2capSend(chunk + off, n);
            if (r >= 0) {                            // 0 = sent, 1 = sent then stalled: bytes are on their way
                if (!WaitL2capTx(true, s_img.epoch, s_img.channel_epoch)) { abort = true; break; }
                s_img.sent_bytes.fetch_add(static_cast<uint32_t>(n), std::memory_order_release);
                off += n;
                last_progress = NowMs();
                if (++burst == 16) { burst=0; SleepWorker(1); }
            } else {                                 // pool empty / transient: brief backoff, then retry
                if (r == -2 || !SessionAlive(s_img.epoch) || !s_img_connected || NowMs() - last_progress >= kRecStallWaitMs) { abort = true; break; }
                SleepWorker(10);
            }
        }
        if (abort) { s_img.truncated.store(true, std::memory_order_release); break; }
    }
    if (s_img.truncated) RequestTerminate(s_img.epoch);
    const bool ok = !s_img.truncated.load(std::memory_order_acquire) && s_img.complete.load(std::memory_order_acquire) &&
                   s_img.sent_bytes.load() == s_img.total_len;
    const uint8_t status = ok ? 0x00 : 0x01;
    if (!SessionAlive(s_img.epoch)) s_img.last_error = ESP_ERR_INVALID_STATE;
    else if (!ImgSendStreamEnd(s_img.transfer_id, status, s_img.sent_bytes.load(std::memory_order_acquire)))
        s_img.last_error = ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "image stream %u ended (status=%u valid_bytes=%u)",
             static_cast<unsigned>(s_img.transfer_id), status,
             static_cast<unsigned>(s_img.sent_bytes.load(std::memory_order_acquire)));
    {
        std::lock_guard<std::mutex> lock(s_img.mtx);
        s_img.q.clear();
        s_img.queued_bytes.store(0);
    }
    if (s_img.last_error == ESP_OK) {
        if (s_img.truncated) s_img.last_error = ESP_ERR_TIMEOUT;
        else if (!s_img.complete) s_img.last_error = ESP_ERR_INVALID_STATE;
        else if (!ok) s_img.last_error = ESP_ERR_INVALID_SIZE;
    }
    s_img.active.store(false, std::memory_order_release);
}

void ImageTask(void*) {
    while (!s_stopping.load(std::memory_order_acquire)) {
        if (s_img.active.load(std::memory_order_acquire)) ImageSession();
        else s_img_wake.Wait();
    }
    if (s_img.active.load()) s_img.last_error = ESP_ERR_INVALID_STATE;
    s_img.active.store(false, std::memory_order_release);
    s_img.running.store(false, std::memory_order_release);
    s_shutdown_wake.Wake();
    vTaskDelete(nullptr);
}

esp_err_t PrepareImage() {
    if (s_img.running.load(std::memory_order_acquire)) return ESP_OK;
    {
        std::lock_guard<std::mutex> lock(s_img.mtx);
        if (!s_img.q.reserve(kImgQueueHardCap)) return ESP_ERR_NO_MEM;
    }
    s_img.running.store(true, std::memory_order_release);
    if (xTaskCreate(ImageTask, "agentlink_img", 6144, nullptr, 5, nullptr) != pdPASS) {
        s_img.running.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ImageStart(uint8_t fmt, uint16_t w, uint16_t h, uint32_t total) {
#if defined(__cpp_exceptions)
    try {
#endif
    const uint32_t epoch = s_connection_epoch.load(std::memory_order_acquire);
    const uint32_t channel_epoch = s_img_epoch.load(std::memory_order_acquire);
    if (s_img.active.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;  // one image at a time
    if (!SessionAlive(epoch) || !s_event_notify_enabled) return ESP_ERR_INVALID_STATE;
    if (fmt > 1) return ESP_ERR_INVALID_ARG;
    if (!s_img_connected) {  // the App must have opened the image L2CAP CoC before we can push
        ESP_LOGD(TAG, "image: L2CAP CoC not open — App must connect PSM 0x%04X first", kImgPsm);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t prepared = PrepareImage();
    if (prepared != ESP_OK) return prepared;
    {
        std::lock_guard<std::mutex> lk(s_img.mtx);
        s_img.q.clear();
    }
    s_img.queued_bytes.store(0, std::memory_order_release);
    s_img.sent_bytes.store(0, std::memory_order_release);
    s_img.accepted_bytes = 0;
    s_img.rejected_writes = 0; s_img.last_error = ESP_OK;
    s_img.end_req.store(false, std::memory_order_release);
    s_img.truncated.store(false, std::memory_order_release);
    s_img.complete.store(true, std::memory_order_release);
    s_img.format = fmt; s_img.width = w; s_img.height = h; s_img.total_len = total;
    s_img.epoch = epoch;
    s_img.channel_epoch = channel_epoch;
    if (!SessionAlive(epoch) || !s_event_notify_enabled || !s_img_connected ||
        channel_epoch != s_img_epoch.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    s_img.transfer_id = ++s_img_counter;
    s_img.active.store(true, std::memory_order_release);
    s_img_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

esp_err_t ImageEnqueue(const uint8_t* data, size_t len) {
#if defined(__cpp_exceptions)
    try {
#endif
    if (!s_img.active.load(std::memory_order_acquire) || s_img.end_req || !SessionAlive(s_img.epoch)) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > kImgQueueHardCap) return ESP_ERR_INVALID_SIZE;
    // Once truncated, refuse new bytes: L2CAP is an ordered byte run, so we must not resume after a gap.
    if (s_img.truncated.load(std::memory_order_acquire)) return ESP_ERR_NO_MEM;
    std::unique_lock<std::mutex> lock(s_img.mtx, std::try_to_lock);
    if (!lock.owns_lock()) return ESP_ERR_NO_MEM;
    if (!s_img.active.load() || s_img.end_req.load() || !SessionAlive(s_img.epoch)) return ESP_ERR_INVALID_STATE;
    if (len > s_img.total_len - s_img.accepted_bytes) return ESP_ERR_INVALID_SIZE;
    if (len > s_img.q.available()) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_img.q.push(data,len)) return ESP_ERR_NO_MEM;
    s_img.accepted_bytes += static_cast<uint32_t>(len);
    s_img.queued_bytes.fetch_add(len, std::memory_order_release);
    s_img_wake.Wake();
    return ESP_OK;
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {
        return ESP_ERR_NO_MEM;
    }
#endif
}

esp_err_t ImageEnd(bool complete) {
    if (!s_img.active.load(std::memory_order_acquire)) return ESP_OK;
    s_img.complete.store(complete, std::memory_order_release);
    s_img.end_req.store(true, std::memory_order_release);  // worker drains, emits 0x55, exits
    s_img_wake.Wake();
    return ESP_OK;
}

#endif

// GAP events: connect / disconnect / advertising complete.
int GapEvent(struct ble_gap_event* event, void* /*arg*/) {
    if (s_stopping) {
        if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0)
            (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connection_epoch.fetch_add(1); s_acl_epoch.fetch_add(1);
            s_connected = true;
            s_event_notify_enabled = false;
            s_voice_notify_enabled = false;
            s_batt_notify_enabled = false;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (handle=%d)", s_conn_handle.load());
            if (!s_stopping) { if (auto cb = s_on_conn.load()) cb(true); }
        } else {
            ESP_LOGW(TAG, "connect failed (status=%d) — re-advertising", event->connect.status);
            if (!s_stopping.load(std::memory_order_acquire)) StartAdvertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_connection_epoch.fetch_add(1); s_acl_epoch.fetch_add(1);
        s_event_notify_enabled = false;
        s_voice_notify_enabled = false;
        s_batt_notify_enabled = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        s_l2cap_epoch.fetch_add(1); s_img_epoch.fetch_add(1);
        s_l2cap_connected = false; s_l2cap_chan = nullptr;  // L2CAP goes down with the ACL
        s_l2cap_stalled.store(false, std::memory_order_release);
        s_l2cap_rearm_pending = false;
        s_img_connected = false; s_img_chan = nullptr; s_img_stalled = false;
        s_img_rearm_pending = false; s_img.truncated = true; s_img.end_req = true;
        s_rec.truncated.store(true, std::memory_order_release);
        s_rec.end_req.store(true, std::memory_order_release);
#endif
        s_voice.end_req.store(true, std::memory_order_release);  // tell the voice worker to wrap up ASAP
        WakeWorkers();
        ESP_LOGI(TAG, "disconnected (reason=%d) — re-advertising", event->disconnect.reason);
        if (!s_stopping) { if (auto cb = s_on_conn.load()) cb(false); }
        if (!s_stopping.load(std::memory_order_acquire)) StartAdvertising();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        // Link encryption (re)established: either a fresh pairing just completed, or a bonded peer
        // reconnected and the stored LTK was restored. status==0 => encrypted, no re-pairing needed.
        ESP_LOGI(TAG, "encryption change: status=%d%s", event->enc_change.status,
                 event->enc_change.status == 0 ? " (encrypted; bond in use)" : " (failed)");
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The peer initiates pairing even though we still hold a bond for it (it wiped its keys, or
        // the user chose "forget this device"). Delete our stale bond and let the new pairing go
        // through — otherwise the stack would drop the connection.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        // Peer subscribed to the event channel 0xFFC4 (CCCD write enables notify) -> notifications
        // are delivered from now on -> tell the core it is "notify-ready" (the core sends the I/O
        // manifest from here, see agent_link.cpp OnLinkReady).
        if (event->subscribe.attr_handle == s_h_evt) {
            const bool became_ready = event->subscribe.cur_notify && !s_event_notify_enabled;
            const bool lost_ready = !event->subscribe.cur_notify && s_event_notify_enabled;
            s_event_notify_enabled = event->subscribe.cur_notify;
            if (lost_ready) {
                // Retire old outbound sessions while preserving the physical
                // CoC receive epoch (an OTA SDU may still be in progress).
                s_connection_epoch.fetch_add(1);
                WakeWorkers();
                if (auto cb = s_on_conn.load()) cb(true);
            }
            if (became_ready && !s_stopping) {
                ESP_LOGI(TAG, "peer subscribed 0xFFC4 (event channel) — notify-ready");
                if (auto cb = s_on_ready.load()) cb();
            }
        }
        if (event->subscribe.attr_handle == s_h_voice) {
            s_voice_notify_enabled = event->subscribe.cur_notify;
            s_voice_wake.Wake();
        }
        if (event->subscribe.attr_handle == s_h_batt) {
            s_batt_notify_enabled = event->subscribe.cur_notify;
            if (event->subscribe.cur_notify) s_batt_dirty.store(true);
            s_control_wake.Wake();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        StartAdvertising();
        return 0;
    default:
        return 0;
    }
}

void StartAdvertising() {
    if (s_stopping.load(std::memory_order_acquire)) return;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_uuid_identity;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = reinterpret_cast<uint8_t*>(s_name);
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof(adv));
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;   // connectable
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;   // generally discoverable
    rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &adv, GapEvent, nullptr);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as '%s'", s_name);
}

void OnSync() {
    const esp_err_t identity_result = dev_identity_load(true);
    if (identity_result != ESP_OK) {
        ESP_LOGE(TAG,"Cannot persist device identity; advertising withheld");
        agent_link_status_t status = {};
        status.phase = AGENT_LINK_PHASE_BLOCKED; status.transport = AGENT_TRANSPORT_BLE;
        status.detail = identity_result;
        snprintf(status.title,sizeof(status.title),"Identity storage failed");
        snprintf(status.hint,sizeof(status.hint),"Check NVS storage, then stop and restart the link");
        if (auto cb = s_on_status.load()) cb(&status);
        return;
    }
    if (ble_hs_util_ensure_addr(0) != 0) { ESP_LOGE(TAG, "ensure_addr failed"); return; }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) { ESP_LOGE(TAG, "infer_auto failed"); return; }
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (StartL2capServer() != ESP_OK) {
        ESP_LOGE(TAG, "CoC server registration failed; advertising withheld");
        agent_link_status_t status = {};
        status.phase = AGENT_LINK_PHASE_BLOCKED; status.transport = AGENT_TRANSPORT_BLE;
        status.detail = ESP_FAIL;
        snprintf(status.title, sizeof(status.title), "BLE data channels unavailable");
        snprintf(status.hint, sizeof(status.hint), "Stop and restart the link; check available BLE memory");
        if (auto cb = s_on_status.load()) cb(&status);
        return;
    }
#else
    ESP_LOGW(TAG, "L2CAP CoC is disabled in the selected host configuration");
#endif
    if (!s_stopping.load(std::memory_order_acquire)) StartAdvertising();
}

void OnReset(int reason) { ESP_LOGW(TAG, "nimble reset; reason=%d", reason); }

void HostTask(void* /*param*/) {
    s_host_running.store(true, std::memory_order_release);
    nimble_port_run();
    s_host_running.store(false, std::memory_order_release);
    s_shutdown_wake.Wake();
    vTaskDelete(nullptr);
}

// ── agent_transport_t interface implementation ──
// A dedicated shutdown task owns NimBLE's blocking teardown. Arduino stop()
// only requests it, and start() refuses reuse until teardown has completed.
void ShutdownTask(void*) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (s_control_running.load() || s_voice.running.load()
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
           || s_rec.running.load() || s_img.running.load()
#endif
    ) s_shutdown_wake.Wait();
    s_voice.q.release();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_rec.q.release(); s_img.q.release();
#endif
    // No host was launched if its task allocation failed during start().
    const int rc = s_host_running.load() ? nimble_port_stop() : 0;
    if (rc == 0) {
        while (s_host_running.load()) s_shutdown_wake.Wait();
        ble_npl_event_deinit(&s_notify_request.event);
        ble_npl_event_deinit(&s_terminate_event);
        s_terminate_requested = false;
        s_terminate_scheduled = false;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        struct os_mbuf* pending = s_rx_pending.exchange(nullptr);
        if (pending) os_mbuf_free_chain(pending);
        s_rx_offset = 0;
        s_rx_rearm_needed = false;
        s_l2cap_chan = nullptr; s_img_chan = nullptr;
        s_l2cap_connected = false; s_img_connected = false;
        s_l2cap_rearm_pending = false; s_img_rearm_pending = false;
        ble_npl_event_deinit(&s_rec_send.event);
        ble_npl_event_deinit(&s_img_send.event);
        ble_npl_event_deinit(&s_rearm_event);
        s_rearm_scheduled = false;
        s_rearm_retry = false;
#endif
        const esp_err_t result = nimble_port_deinit();
        if (result == ESP_OK) {
            s_h_cmd = s_h_evt = s_h_file = s_h_voice = s_h_batt = 0;
            s_connected = false;
            s_event_notify_enabled = false;
            s_voice_notify_enabled = false;
            s_batt_notify_enabled = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_shutdown_task = nullptr;
            s_started.store(false, std::memory_order_release);
        } else ESP_LOGE(TAG, "NimBLE deinit failed: %s", esp_err_to_name(result));
    } else ESP_LOGE(TAG, "NimBLE stop failed: %d (restart disabled)", rc);
    vTaskDelete(nullptr);
}

esp_err_t InitHost() {
#if DEOTALAND_NIMBLE_ACTIVE
    // NimBLE-Arduino deliberately leaves controller initialization to its
    // caller. We own a single raw host; a separately started BLE stack must
    // never be silently reused or torn down by this transport.
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = nvs_flash_init();
    // Preserve device identity and credentials if NVS needs intervention.
    if (result != ESP_OK) return result;
#if defined(CONFIG_IDF_TARGET_ESP32)
    (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
    esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32)
    controller.mode = ESP_BT_MODE_BLE;
    controller.ble_max_conn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    controller.ble_max_act = MYNEWT_VAL(BLE_MAX_CONNECTIONS) +
        MYNEWT_VAL(BLE_ROLE_BROADCASTER) + MYNEWT_VAL(BLE_ROLE_OBSERVER);
#else
    controller.nimble_max_connections = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#endif
    result = esp_bt_controller_init(&controller);
    if (result != ESP_OK) return result;
    result = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (result != ESP_OK) {
        (void)esp_bt_controller_deinit();
        return result;
    }
#if CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE
    result = esp_nimble_hci_init();
    if (result != ESP_OK) {
        (void)esp_bt_controller_disable();
        (void)esp_bt_controller_deinit();
        return result;
    }
#endif
    result = nimble_port_init();
    if (result != ESP_OK) {
#if CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE
        (void)esp_nimble_hci_deinit();
#endif
        // port_init normally tears the controller down on a host init error.
        // Check status so rollback is safe on either implementation path.
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
            (void)esp_bt_controller_disable();
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
            (void)esp_bt_controller_deinit();
    }
    return result;
#else
    return nimble_port_init();
#endif
}

esp_err_t ble_start(void* /*impl*/) {
    if (s_started.load()) return ESP_ERR_INVALID_STATE;
    s_stopping.store(false);
    s_control_wake.Init(); s_voice_wake.Init(); s_shutdown_wake.Init();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_rec_wake.Init(); s_img_wake.Init();
#endif
    esp_err_t result = InitHost();
    if (result != ESP_OK) return result;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_voice_server_registered = false;
    s_image_server_registered = false;
#endif
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    result = RegisterGatt();
    if (result != ESP_OK) { (void)nimble_port_deinit(); return result; }
    // Report a fresh IDLE/UNAVAILABLE snapshot after a full transport restart.
    // Session counters themselves stay monotonic for the wire protocol.
    s_voice.session_id = 0; s_voice.accepted_bytes = 0; s_voice.sent_bytes = 0;
    s_voice.queued_bytes = 0; s_voice.rejected_writes = 0; s_voice.last_error = ESP_OK;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_rec.transfer_id = 0; s_rec.accepted_bytes = 0; s_rec.sent_bytes = 0;
    s_rec.queued_bytes = 0; s_rec.rejected_writes = 0; s_rec.last_error = ESP_OK;
    s_img.transfer_id = 0; s_img.accepted_bytes = 0; s_img.sent_bytes = 0;
    s_img.queued_bytes = 0; s_img.rejected_writes = 0; s_img.last_error = ESP_OK;
#endif
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();
    ClearControlQueue();
    { std::lock_guard<std::mutex> lock(s_notify_mtx); }
    // Reserve the teardown task before advertising, so stop never allocates.
    if (xTaskCreate(ShutdownTask, "agentlink_stop", 4096, nullptr, 4, &s_shutdown_task) != pdPASS) {
        (void)nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    ble_npl_event_init(&s_notify_request.event, NotifyHostEvent, nullptr);
    ble_npl_event_init(&s_terminate_event, TerminateHostEvent, nullptr);
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    ble_npl_event_init(&s_rec_send.event, SendL2capHost, &s_rec_send);
    ble_npl_event_init(&s_img_send.event, SendL2capHost, &s_img_send);
    ble_npl_event_init(&s_rearm_event, RearmL2capHost, nullptr);
#endif
    s_control_running.store(true);
    if (xTaskCreate(ControlTask, "agentlink_ctrl", 4096, nullptr, 6, nullptr) != pdPASS) {
        s_control_running.store(false);
        vTaskDelete(s_shutdown_task);
        s_shutdown_task = nullptr;
        ble_npl_event_deinit(&s_notify_request.event);
        ble_npl_event_deinit(&s_terminate_event);
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        ble_npl_event_deinit(&s_rec_send.event);
        ble_npl_event_deinit(&s_img_send.event);
        ble_npl_event_deinit(&s_rearm_event);
#endif
        (void)nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_started.store(true);
    s_host_running.store(true);
    if (xTaskCreatePinnedToCore(HostTask, "agentlink_host", NIMBLE_HS_STACK_SIZE,
            nullptr, configMAX_PRIORITIES - 4, nullptr, NIMBLE_CORE) != pdPASS) {
        s_host_running.store(false);
        s_stopping.store(true);
        WakeWorkers();
        xTaskNotifyGive(s_shutdown_task);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_stop(void* /*impl*/) {
    if (!s_started.load() || s_stopping.exchange(true)) return;
    (void)ble_gap_adv_stop();
    // nimble_port_stop() in ShutdownTask closes the link after workers retire.
    s_connected = false;
    s_connection_epoch.fetch_add(1); s_acl_epoch.fetch_add(1);
    s_event_notify_enabled = false;
    s_voice_notify_enabled = false;
    s_batt_notify_enabled = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_voice.end_req = true;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_rec.truncated = true; s_rec.end_req = true;
    s_img.truncated = true; s_img.end_req = true;
#endif
    WakeWorkers();
    xTaskNotifyGive(s_shutdown_task);
}

void ble_poll(void*) {
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (s_started && !s_stopping) PollL2capRx();
#endif
}

size_t ble_ctrl_mtu(void*) {
    if (!s_connected) return 20;
    const uint16_t mtu = ble_att_mtu(s_conn_handle.load());
    return mtu > 3 ? std::min<size_t>(mtu - 3, 512) : 20;
}

// The latest app sends file/OTA commands through Service B.
bool IsFileChannelCmd(uint8_t command) {
    return command == 0x06 || command == 0x08 || command == 0x37 ||
           command == 0x55 || command == 0x56;
}
esp_err_t ble_send_ctrl(void*, const uint8_t* frame, size_t len) {
    if (!frame || len < 6) return ESP_ERR_INVALID_ARG;
    const uint8_t type = frame[1] & 0x7F;
    const uint16_t handle = type == 0x03 ? s_h_evt : IsFileChannelCmd(frame[2]) ? s_h_file : s_h_cmd;
    return EnqueueControl(handle, frame, len);
}
esp_err_t ble_stream_start(void*, agent_stream_t type, const uint8_t* meta, size_t meta_len) {
    if (type == AGENT_STREAM_VOICE) return VoiceStart();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (type == AGENT_STREAM_AUDIO) return RecordingStart(reinterpret_cast<const char*>(meta), meta_len);
    if (type == AGENT_STREAM_IMAGE) {
        if (!meta || meta_len != 9) return ESP_ERR_INVALID_ARG;
        const uint16_t w = meta[1] | (static_cast<uint16_t>(meta[2]) << 8);
        const uint16_t h = meta[3] | (static_cast<uint16_t>(meta[4]) << 8);
        const uint32_t total = static_cast<uint32_t>(meta[5]) | (static_cast<uint32_t>(meta[6]) << 8) |
                              (static_cast<uint32_t>(meta[7]) << 16) | (static_cast<uint32_t>(meta[8]) << 24);
        return ImageStart(meta[0], w, h, total);
    }
#else
    (void)meta; (void)meta_len;
#endif
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ble_send_stream(void*, agent_stream_t type, const uint8_t* data, size_t len) {
    esp_err_t result = ESP_ERR_NOT_SUPPORTED;
    if (type == AGENT_STREAM_VOICE) {
        result = VoiceEnqueue(data, len);
        if (result != ESP_OK) s_voice.rejected_writes.fetch_add(1);
    }
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (type == AGENT_STREAM_AUDIO) {
        result = RecordingEnqueue(data, len);
        if (result != ESP_OK) s_rec.rejected_writes.fetch_add(1);
    }
    if (type == AGENT_STREAM_IMAGE) {
        result = ImageEnqueue(data, len);
        if (result != ESP_OK) s_img.rejected_writes.fetch_add(1);
    }
#endif
    return result;
}
esp_err_t ble_stream_end(void*, agent_stream_t type, bool complete, const uint8_t* meta, size_t meta_len) {
    if (type == AGENT_STREAM_VOICE) return VoiceEnd(complete);
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (type == AGENT_STREAM_AUDIO) return RecordingEnd(complete, meta, meta_len);
    if (type == AGENT_STREAM_IMAGE) return ImageEnd(complete);
#else
    (void)complete; (void)meta; (void)meta_len;
#endif
    return ESP_ERR_NOT_SUPPORTED;
}
bool ble_is_ready(void* /*impl*/) { return s_started && !s_stopping && s_connected && s_event_notify_enabled; }

esp_err_t ble_prepare_streams(void*, uint32_t mask) {
    if (!s_started || s_stopping) return ESP_ERR_INVALID_STATE;
    constexpr uint32_t voice = 1u << AGENT_STREAM_VOICE;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    constexpr uint32_t supported = voice | (1u << AGENT_STREAM_AUDIO) | (1u << AGENT_STREAM_IMAGE);
#else
    constexpr uint32_t supported = voice;
#endif
    if (mask & ~supported) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t result = ESP_OK;
    if (mask & voice) result = PrepareVoice();
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (result == ESP_OK && (mask & (1u << AGENT_STREAM_AUDIO))) result = PrepareRecording();
    if (result == ESP_OK && (mask & (1u << AGENT_STREAM_IMAGE))) result = PrepareImage();
#endif
    return result;
}

esp_err_t ble_stream_status(void*, agent_stream_t type, agent_stream_status_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (type == AGENT_STREAM_VOICE) {
        agentlink::blepolicy::StreamStatus(s_voice, kVoiceQueueHardCap,
                     s_started && !s_stopping && s_connected && s_voice_notify_enabled,
                     s_voice.session_id, out);
        return ESP_OK;
    }
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    if (type == AGENT_STREAM_AUDIO) {
        agentlink::blepolicy::StreamStatus(s_rec, kRecQueueHardCap, ble_is_ready(nullptr) && s_l2cap_connected,
                     s_rec.transfer_id, out);
        return ESP_OK;
    }
    if (type == AGENT_STREAM_IMAGE) {
        agentlink::blepolicy::StreamStatus(s_img, kImgQueueHardCap, ble_is_ready(nullptr) && s_img_connected,
                     s_img.transfer_id, out);
        return ESP_OK;
    }
#endif
    *out = {}; out->state = AGENT_STREAM_UNAVAILABLE;
    return ESP_ERR_NOT_SUPPORTED;
}

// Checked ingress returns ATT insufficient-resources instead of acknowledging
// a command that could not fit in the core's bounded receive queue.
void ble_bind(void*, const agent_transport_callbacks_t* callbacks) {
    s_on_ctrl_checked.store(callbacks ? callbacks->on_ctrl : nullptr);
    s_on_conn.store(callbacks ? callbacks->on_connection : nullptr);
    s_on_ready.store(callbacks ? callbacks->on_ready : nullptr);
    s_on_status.store(callbacks ? callbacks->on_status : nullptr);
    if (!callbacks) {
        s_on_recv.store(nullptr);
        s_on_stream.store(nullptr);
    }
}

agent_transport_t s_ble = {
    ble_start, ble_stop, ble_send_ctrl,
    ble_stream_start, ble_send_stream, ble_stream_end,
    ble_is_ready, nullptr, ble_poll, ble_bind, ble_ctrl_mtu,
    ble_prepare_streams, ble_stream_status,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_ble(void) { return &s_ble; }

extern "C" void agent_transport_ble_set_name(const char* name) {
    if (name && *name) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
}
extern "C" void agent_transport_ble_set_recv(void (*cb)(const uint8_t*, size_t)) { s_on_recv = cb; }
extern "C" void agent_transport_ble_set_conn(void (*cb)(bool)) { s_on_conn = cb; }
extern "C" void agent_transport_ble_set_stream_recv(void (*cb)(agent_stream_t, const uint8_t*, size_t)) {
    s_on_stream = cb;
}
extern "C" void agent_transport_ble_set_ready(void (*cb)(void)) { s_on_ready = cb; }

// Current negotiated ATT MTU (0 if not connected). Used by the core to adapt manifest fragment size.
extern "C" uint16_t agent_transport_ble_att_mtu(void) {
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return 0;
    return ble_att_mtu(s_conn_handle);
}

// Device info (the three 0x180A standard-service strings). Called once at init; NULL/empty keeps defaults.
extern "C" void agent_transport_ble_set_device_info(const char* mfr, const char* model, const char* fw) {
    std::lock_guard<std::mutex> lock(s_device_info_mtx);
    if (mfr && *mfr)   { strncpy(s_di_mfr, mfr, sizeof(s_di_mfr) - 1);     s_di_mfr[sizeof(s_di_mfr) - 1] = '\0'; }
    if (model && *model){ strncpy(s_di_model, model, sizeof(s_di_model) - 1); s_di_model[sizeof(s_di_model) - 1] = '\0'; }
    if (fw && *fw)     { strncpy(s_di_fw, fw, sizeof(s_di_fw) - 1);        s_di_fw[sizeof(s_di_fw) - 1] = '\0'; }
}

// Update battery (standard service 0x180F). Store the value for reads; notify 0x2A19 if connected. Called by the core when the level changes.
extern "C" void agent_transport_ble_update_battery(uint8_t percent) {
    if (agentlink::blepolicy::UpdateBatteryLevel(s_batt_level, s_batt_dirty, percent))
        s_control_wake.Wake();
}

extern "C" bool agent_transport_ble_l2cap_ready(void) {
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    return s_connected && !s_stopping && s_l2cap_connected;
#else
    return false;
#endif
}
extern "C" bool agent_transport_ble_get_mac(uint8_t out[6]) {
    if (!out || !s_started || s_stopping || !ble_hs_synced()) return false;
    uint8_t address[6] = {};
    if (ble_hs_id_copy_addr(s_own_addr_type, address, nullptr) != 0) return false;
    for (unsigned i = 0; i < 6; ++i) out[i] = address[5 - i];
    return true;
}
extern "C" bool agent_transport_ble_active(void) { return s_started.load(); }
extern "C" esp_err_t agent_transport_ble_forget(void) {
    if (!s_started || s_stopping || !ble_hs_synced()) return ESP_ERR_INVALID_STATE;
    return ble_store_clear() == 0 ? ESP_OK : ESP_FAIL;
}
extern "C" void agent_transport_ble_set_data_receiver(size_t (*receive)(const uint8_t*, size_t),
                                                       bool (*ready)(void)) {
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_data_receiver = receive;
    s_data_ready = ready;
#else
    (void)receive; (void)ready;
#endif
}

#else  // no supported BLE host for this build

#define DEOTALAND_BLE_AVAILABLE 0

namespace {
esp_err_t unavailable_start(void*) { return ESP_ERR_NOT_SUPPORTED; }
void unavailable_stop(void*) {}
esp_err_t unavailable_ctrl(void*, const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t unavailable_stream_start(void*, agent_stream_t, const uint8_t*, size_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t unavailable_stream_send(void*, agent_stream_t, const uint8_t*, size_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t unavailable_stream_end(void*, agent_stream_t, bool, const uint8_t*, size_t) {
    return ESP_ERR_NOT_SUPPORTED;
}
bool unavailable_ready(void*) { return false; }

agent_transport_t s_ble = {
    unavailable_start, unavailable_stop, unavailable_ctrl,
    unavailable_stream_start, unavailable_stream_send, unavailable_stream_end,
    unavailable_ready, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_ble(void) { return &s_ble; }
extern "C" void agent_transport_ble_set_name(const char*) {}
extern "C" void agent_transport_ble_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_ble_set_conn(void (*)(bool)) {}
extern "C" void agent_transport_ble_set_stream_recv(void (*)(agent_stream_t, const uint8_t*, size_t)) {}
extern "C" void agent_transport_ble_set_ready(void (*)(void)) {}
extern "C" uint16_t agent_transport_ble_att_mtu(void) { return 0; }
extern "C" void agent_transport_ble_set_device_info(const char*, const char*, const char*) {}
extern "C" void agent_transport_ble_update_battery(uint8_t) {}
extern "C" bool agent_transport_ble_l2cap_ready(void) { return false; }
extern "C" bool agent_transport_ble_get_mac(uint8_t[6]) { return false; }
extern "C" bool agent_transport_ble_active(void) { return false; }
extern "C" esp_err_t agent_transport_ble_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void agent_transport_ble_set_data_receiver(size_t (*)(const uint8_t*, size_t), bool (*)(void)) {}

#endif  // supported BLE host
