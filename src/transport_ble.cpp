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
//   - TTS downlink and recording/ASR uplink: L2CAP CoC PSM 0x0081 when enabled by the Core.
// Not done: generic file/OTA transfer, Service B, encryption.
#include "agent_link_transport.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// Arduino-ESP32 3.3+ releases the controller's BLE memory during initArduino()
// unless a linked BLE library marks it as in use before app_main().  Direct
// NimBLE C API users must include the same marker as the bundled BLE libraries.
// Older Arduino cores do not provide this header and do not need the marker.
#if defined(ARDUINO_ARCH_ESP32) && __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#endif

#if defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED && \
    defined(SOC_BLE_SUPPORTED) && SOC_BLE_SUPPORTED

#define DEOTALAND_BLE_AVAILABLE 1

#include <cstring>
#include <queue>
#include <vector>
#include <mutex>
#include <atomic>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

extern "C" void ble_store_config_init(void);

namespace {
constexpr const char* TAG = "agent_link.ble";

// ── Service A (intercom / realtime voice): device -> App PCM voice stream ──
constexpr uint16_t kSvcVoice      = 0xFFA0;
constexpr uint16_t kChrVoiceDown  = 0xFFA1;  // NOTIFY: device -> App PCM (event 0x40 VoiceChunk)
// ── Service C (device control) ──
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

char     s_name[31] = "AgentLink";
uint8_t  s_own_addr_type = 0;
bool     s_connected = false;
bool     s_event_notify_enabled = false;
bool     s_voice_notify_enabled = false;
uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
std::atomic<bool> s_host_running{false};
std::atomic<bool> s_stopping{false};
bool s_started = false;

// GATT characteristic value handles (filled in by NimBLE at registration; used for notify).
uint16_t s_h_cmd   = 0; // 0xFFC1
uint16_t s_h_evt   = 0; // 0xFFC4
uint16_t s_h_voice = 0; // 0xFFA1 (voice upload)
uint16_t s_h_batt  = 0; // 0x2A19 (battery, notify)

// Standard-service values (returned by the read callback; battery notifies 0x2A19 on change).
uint8_t s_batt_level = 0;              // current battery level 0-100
char    s_di_mfr[32]   = "Deotaland";  // 0x2A29 manufacturer
char    s_di_model[32] = "AgentLink";  // 0x2A24 model (overridden by init with the device name)
char    s_di_fw[24]    = "1.0.0";      // 0x2A26 firmware revision

// Transport -> core uplink callbacks (installed by the core via set_recv/set_conn/set_stream_recv).
void (*s_on_recv)(const uint8_t*, size_t) = nullptr;
void (*s_on_conn)(bool) = nullptr;
void (*s_on_stream)(agent_stream_t, const uint8_t*, size_t) = nullptr;  // data-plane receive (L2CAP)
void (*s_on_ready)(void) = nullptr;  // peer subscribed to 0xFFC4 -> notify reachable (core sends the manifest)

// ── L2CAP CoC (data plane: receive App downlink TTS voice, PSM 0x0081) ──
#if defined(CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM) && CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0
#define DEOTALAND_BLE_L2CAP_COC_AVAILABLE 1
constexpr uint16_t kL2capPsm = 0x0081;  // PSM of the downlink voice channel
constexpr uint16_t kL2capMtu = 4096;    // our_coc_mtu (receive)
constexpr uint16_t kL2capMps = 512;     // per-packet size (the SDU rx buffer is allocated to this)
struct ble_l2cap_chan* s_l2cap_chan = nullptr;
bool s_l2cap_connected = false;
std::atomic<bool> s_l2cap_stalled{false};

constexpr size_t   kRecMaxSdu       = kL2capMps;
constexpr size_t   kRecQueueHardCap = 128 * 1024;
constexpr uint32_t kRecStallWaitMs  = 3000;
constexpr uint32_t kRecStartDelayMs = 120;
struct RecordingState {
    std::atomic<bool> active{false};
    std::atomic<bool> end_req{false};
    std::atomic<bool> truncated{false};
    std::atomic<bool> complete{true};
    uint32_t transfer_id = 0;
    std::atomic<uint32_t> sent_bytes{0};
    std::queue<std::vector<uint8_t>> q;
    std::mutex mtx;
    std::atomic<size_t> queued_bytes{0};
    std::vector<uint8_t> final_header;
    TaskHandle_t task = nullptr;
};
RecordingState s_rec;
uint32_t s_rec_counter = 0;
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
        if (chr == kChrCmdRequest && ctxt->om) {
            uint8_t buf[512];
            uint16_t n = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) == 0 && n > 0 && s_on_recv) {
                s_on_recv(buf, n);
            }
        }
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        // Standard-service read: return battery level / device-info strings.
        const void* val = nullptr;
        uint16_t vlen = 0;
        switch (chr) {
        case kChrBatteryLevel: val = &s_batt_level; vlen = 1; break;
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
const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_voice_svc.u, .characteristics = s_chrs_voice },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc.u,       .characteristics = s_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc_batt.u,  .characteristics = s_chrs_batt },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_uuid_svc_dis.u,   .characteristics = s_chrs_dis },
    { 0 },
};

esp_err_t RegisterGatt() {
    // Arduino-ESP32 3.x initializes GAP/GATT services inside nimble_port_init().
    // Calling ble_svc_gap_init()/ble_svc_gatt_init() again corrupts the controller's
    // internal memory on ESP32-S3 and ends in an interrupt-watchdog reset.
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t Notify(uint16_t handle, const uint8_t* data, size_t len) {
    if (!s_connected || handle == 0) return ESP_ERR_INVALID_STATE;
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);  // on failure NimBLE has already freed om
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// Control notifications may be produced both by the Arduino loop and by the
// NimBLE host callback that handles a command write.  Sending directly from
// either context loses frames whenever the controller's mbuf/notify window is
// temporarily full (and the host callback cannot block waiting for itself).
// Queue them and let one worker preserve ordering and retry with backpressure.
struct ControlPacket {
    uint16_t handle;
    std::vector<uint8_t> data;
};
constexpr size_t kControlQueueHardCap = 64 * 1024;
std::queue<ControlPacket> s_control_q;
std::mutex s_control_mtx;
size_t s_control_queued_bytes = 0;
TaskHandle_t s_control_task = nullptr;

void ClearControlQueue() {
    std::lock_guard<std::mutex> lock(s_control_mtx);
    std::queue<ControlPacket> empty;
    std::swap(s_control_q, empty);
    s_control_queued_bytes = 0;
}

void ControlTask(void*) {
    while (!s_stopping.load(std::memory_order_acquire)) {
        ControlPacket packet = {};
        {
            std::lock_guard<std::mutex> lock(s_control_mtx);
            if (!s_control_q.empty()) {
                packet = std::move(s_control_q.front());
                s_control_q.pop();
                s_control_queued_bytes -= packet.data.size();
            }
        }
        if (packet.data.empty()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        while (s_connected && !s_stopping.load(std::memory_order_acquire)) {
            if (Notify(packet.handle, packet.data.data(), packet.data.size()) == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    ClearControlQueue();
    s_control_task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t EnqueueControl(uint16_t handle, const uint8_t* data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_connected || handle == 0 || !s_control_task) return ESP_ERR_INVALID_STATE;
    if (handle == s_h_evt && !s_event_notify_enabled) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lock(s_control_mtx);
    if (s_control_queued_bytes + len > kControlQueueHardCap) return ESP_ERR_NO_MEM;
    ControlPacket packet;
    packet.handle = handle;
    packet.data.assign(data, data + len);
    s_control_queued_bytes += packet.data.size();
    s_control_q.push(std::move(packet));
    return ESP_OK;
}

// ── Voice upload (0x40 VoiceChunk, Notify 0xFFA1) ──
// Integrity-first queue + worker task + MTU-aware slicing.
// Frame overhead = 6 (common header) + session(4) + sequence(4) + flags(1) = 15.
constexpr size_t   kVoiceOverhead      = 15;
constexpr size_t   kMaxFrameSingleMbuf = 220;  // keep a frame in a single mbuf, avoiding chain-alloc failures on high-rate notify
constexpr size_t   kMaxPcmSingleMbuf   = kMaxFrameSingleMbuf - kVoiceOverhead;  // 205
constexpr size_t   kFallbackPcmBytes   = 80;   // conservative slice when the MTU is abnormal
constexpr size_t   kVoiceQueueHardCap  = 96 * 1024;  // hard cap ~3s @ 32KB/s: when the link is bad, drop new frames to keep the head
constexpr uint32_t kVoiceMaxRetries    = 300;  // per-slice congestion retry limit

struct VoiceState {
    std::atomic<bool>     active{false};
    std::atomic<bool>     end_req{false};
    uint32_t              session_id = 0;
    std::atomic<uint32_t> sequence{0};
    std::queue<std::vector<uint8_t>> q;
    std::mutex            mtx;
    std::atomic<size_t>   queued_bytes{0};
    TaskHandle_t          task = nullptr;
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
    return Notify(s_h_voice, f, i) == ESP_OK;
}

void VoiceTask(void*) {
    ESP_LOGI(TAG, "voice task started (session=%u)", static_cast<unsigned>(s_voice.session_id));
    while (true) {
        if (!s_connected) { ESP_LOGW(TAG, "voice: link down — stop"); break; }
        const bool end_req = s_voice.end_req.load(std::memory_order_acquire);

        std::vector<uint8_t> chunk;
        {
            std::lock_guard<std::mutex> lk(s_voice.mtx);
            if (!s_voice.q.empty()) {
                chunk = std::move(s_voice.q.front());
                s_voice.q.pop();
                s_voice.queued_bytes.fetch_sub(chunk.size(), std::memory_order_release);
            }
        }
        if (chunk.empty()) {
            if (end_req) { ESP_LOGI(TAG, "voice: drained — done"); break; }
            vTaskDelay(pdMS_TO_TICKS(10));  // empty queue: yield at least 1 tick (FREERTOS_HZ=100)
            continue;
        }

        // MTU-aware slicing: per-notify PCM = ATT_MTU - 3 - 15, clamped to a single mbuf (205) and even-aligned.
        const uint16_t mtu = (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(s_conn_handle) : 0;
        size_t max_pcm = (static_cast<size_t>(mtu) > 3 + kVoiceOverhead + 2)
                             ? (static_cast<size_t>(mtu) - 3 - kVoiceOverhead)
                             : kFallbackPcmBytes;
        if (max_pcm > kMaxPcmSingleMbuf) max_pcm = kMaxPcmSingleMbuf;
        max_pcm &= ~static_cast<size_t>(1);           // even-aligned (int16 samples)
        if (max_pcm == 0) max_pcm = 2;

        for (size_t off = 0; off < chunk.size(); ) {
            const size_t remaining = chunk.size() - off;
            const size_t n = (remaining < max_pcm) ? remaining : max_pcm;
            const uint32_t seq = s_voice.sequence.fetch_add(1, std::memory_order_release);

            bool sent = false;
            for (uint32_t retry = 0; retry < kVoiceMaxRetries; retry++) {
                if (!s_connected) break;
                if (VoiceSendChunk(seq, chunk.data() + off, n)) { sent = true; break; }
                // congestion backoff: 1ms x5 -> 5ms x15 -> 10ms (a failed notify signals mbuf-pool pressure).
                const uint32_t d = (retry < 5) ? 1 : (retry < 20) ? 5 : 10;
                vTaskDelay(pdMS_TO_TICKS(d));
            }
            if (!sent) { ESP_LOGE(TAG, "voice: slice send failed — stop"); s_voice.end_req.store(true); break; }
            off += n;
            vTaskDelay(pdMS_TO_TICKS(1));  // small yield per slice to give the BLE host a chance
        }
    }
    s_voice.active.store(false, std::memory_order_release);
    s_voice.task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t VoiceStart() {
    if (s_voice.active.load(std::memory_order_acquire)) return ESP_OK;  // already in a session
    if (!s_connected || !s_voice_notify_enabled) return ESP_ERR_INVALID_STATE;
    {   // reset queue + counters
        std::lock_guard<std::mutex> lk(s_voice.mtx);
        std::queue<std::vector<uint8_t>> empty;
        std::swap(s_voice.q, empty);
    }
    s_voice.queued_bytes.store(0, std::memory_order_release);
    s_voice.sequence.store(0, std::memory_order_release);
    s_voice.end_req.store(false, std::memory_order_release);
    s_voice.session_id = ++s_voice_counter;
    s_voice.active.store(true, std::memory_order_release);
    // Use the internal stack (NimBLE's notify call chain is deep, so leave headroom; do not assume PSRAM, for portability). Priority 6.
    if (xTaskCreate(VoiceTask, "agentlink_voice", 6144, nullptr, 6, &s_voice.task) != pdPASS) {
        s_voice.active.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "voice: task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice session %u started (mtu=%u)",
             static_cast<unsigned>(s_voice.session_id),
             static_cast<unsigned>(s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn_handle) : 0));
    return ESP_OK;
}

esp_err_t VoiceEnqueue(const uint8_t* data, size_t len) {
    if (!s_voice.active.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(s_voice.mtx);
    // Integrity-first, with a hard cap as a backstop: when the link is bad, drop new frames to keep the head (the App at least gets the start of the voice).
    if (s_voice.queued_bytes.load(std::memory_order_acquire) + len > kVoiceQueueHardCap) {
        ESP_LOGW(TAG, "voice queue hard cap (%uKB) — drop %uB (link stalled)",
                 static_cast<unsigned>(kVoiceQueueHardCap / 1024), static_cast<unsigned>(len));
        return ESP_ERR_NO_MEM;
    }
    s_voice.q.emplace(data, data + len);
    s_voice.queued_bytes.fetch_add(len, std::memory_order_release);
    return ESP_OK;
}

esp_err_t VoiceEnd() {
    if (!s_voice.active.load(std::memory_order_acquire)) return ESP_OK;
    s_voice.end_req.store(true, std::memory_order_release);  // the worker drains the rest, then exits on its own
    return ESP_OK;
}

// ── L2CAP CoC receive (downlink TTS voice, PSM 0x0081) ──
// Replenishing the rx buffer grants the peer credit and keeps the channel able to receive.
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
bool RearmL2capRx(struct ble_l2cap_chan* chan) {
    struct os_mbuf* rx = os_msys_get_pkthdr(kL2capMps, 0);
    if (!rx) return false;
    if (ble_l2cap_recv_ready(chan, rx) != 0) { os_mbuf_free_chain(rx); return false; }
    return true;
}

// L2CAP event callback (runs on the NimBLE host task; do not block).
int L2capEvent(struct ble_l2cap_event* event, void* /*arg*/) {
    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_ACCEPT: {
        // Critical: allocate the SDU rx buffer + recv_ready first, or the credit handshake never
        // completes (NimBLE rejects with "No Resources"; iOS reports a didOpen error).
        struct os_mbuf* rx = os_msys_get_pkthdr(kL2capMps, 0);
        if (!rx) return BLE_HS_ENOMEM;
        int rc = ble_l2cap_recv_ready(event->accept.chan, rx);
        if (rc != 0) { os_mbuf_free_chain(rx); return rc; }
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status == 0) {
            s_l2cap_chan = event->connect.chan;
            s_l2cap_connected = true;
            ESP_LOGI(TAG, "L2CAP CoC connected (PSM 0x%04X)", kL2capPsm);
        } else {
            ESP_LOGW(TAG, "L2CAP connect failed status=%d", event->connect.status);
        }
        return 0;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        s_l2cap_chan = nullptr;
        s_l2cap_connected = false;
        s_l2cap_stalled.store(false, std::memory_order_release);
        s_rec.truncated.store(true, std::memory_order_release);
        s_rec.end_req.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "L2CAP CoC disconnected");
        return 0;
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
        struct os_mbuf* sdu = event->receive.sdu_rx;
        const uint16_t total = OS_MBUF_PKTLEN(sdu);
        // Copy out in chunks and forward to the core. The only downlink data plane right now is TTS voice -> AGENT_STREAM_VOICE.
        // Do not block here (NimBLE host task) — the core/board should enqueue quickly and play in another task.
        uint8_t buf[kL2capMps];
        for (uint16_t off = 0; off < total; ) {
            const uint16_t n = static_cast<uint16_t>((total - off < kL2capMps) ? (total - off) : kL2capMps);
            if (os_mbuf_copydata(sdu, off, n, buf) == 0 && s_on_stream) {
                s_on_stream(AGENT_STREAM_VOICE, buf, n);
            }
            off += n;
        }
        os_mbuf_free_chain(sdu);            // NimBLE does not auto-free; must free explicitly
        RearmL2capRx(event->receive.chan);  // replenish the rx buffer to keep receiving
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        s_l2cap_stalled.store(false, std::memory_order_release);
        return 0;
    default:
        return 0;
    }
}

esp_err_t StartL2capServer() {
    int rc = ble_l2cap_create_server(kL2capPsm, kL2capMtu, L2capEvent, nullptr);
    if (rc != 0) { ESP_LOGE(TAG, "l2cap_create_server rc=%d", rc); return ESP_FAIL; }
    ESP_LOGI(TAG, "L2CAP CoC server on PSM 0x%04X (MTU=%d MPS=%d) — App must explicitly open this PSM after connecting",
             kL2capPsm, kL2capMtu, kL2capMps);
    return ESP_OK;
}

bool RecSendStreamStart(uint32_t transfer_id, const char* name, size_t name_len) {
    if (name_len > UINT16_MAX - 4) return false;
    std::vector<uint8_t> frame;
    const uint16_t payload_len = static_cast<uint16_t>(4 + name_len);
    frame.reserve(6 + payload_len);
    frame.insert(frame.end(), {0x01, 0x03, 0x52, 0x00,
                               static_cast<uint8_t>(payload_len),
                               static_cast<uint8_t>(payload_len >> 8),
                               static_cast<uint8_t>(transfer_id),
                               static_cast<uint8_t>(transfer_id >> 8),
                               static_cast<uint8_t>(transfer_id >> 16),
                               static_cast<uint8_t>(transfer_id >> 24)});
    if (name && name_len) frame.insert(frame.end(), name, name + name_len);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (Notify(s_h_evt, frame.data(), frame.size()) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
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
    return Notify(s_h_evt, frame, i) == ESP_OK;
}

int RecL2capSend(const uint8_t* data, size_t len) {
    if (!s_l2cap_connected || !s_l2cap_chan) return -1;
    struct os_mbuf* sdu = os_msys_get_pkthdr(len, 0);
    if (!sdu) return -1;
    if (os_mbuf_append(sdu, data, len) != 0) {
        os_mbuf_free_chain(sdu);
        return -1;
    }
    const int result = ble_l2cap_send(s_l2cap_chan, sdu);
    if (result == 0) return 0;
    if (result == BLE_HS_ESTALLED) {
        s_l2cap_stalled.store(true, std::memory_order_release);
        return 1;
    }
    return -1;
}

void RecordingTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(kRecStartDelayMs));
    while (true) {
        if (!s_connected) {
            s_rec.truncated.store(true, std::memory_order_release);
            break;
        }
        const bool end_requested = s_rec.end_req.load(std::memory_order_acquire);
        std::vector<uint8_t> chunk;
        {
            std::lock_guard<std::mutex> lock(s_rec.mtx);
            if (!s_rec.q.empty()) {
                chunk = std::move(s_rec.q.front());
                s_rec.q.pop();
                s_rec.queued_bytes.fetch_sub(chunk.size(), std::memory_order_release);
            }
        }
        if (chunk.empty()) {
            if (end_requested) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        bool abort = false;
        for (size_t offset = 0; offset < chunk.size() && !abort;) {
            uint32_t waited = 0;
            while (s_l2cap_stalled.load(std::memory_order_acquire)) {
                if (!s_connected || !s_l2cap_connected || waited >= kRecStallWaitMs) {
                    abort = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                waited += 5;
            }
            if (abort) break;
            const size_t remaining = chunk.size() - offset;
            const size_t count = remaining < kRecMaxSdu ? remaining : kRecMaxSdu;
            const int result = RecL2capSend(chunk.data() + offset, count);
            if (result >= 0) {
                s_rec.sent_bytes.fetch_add(static_cast<uint32_t>(count), std::memory_order_release);
                offset += count;
                if (result == 0) vTaskDelay(pdMS_TO_TICKS(1));
            } else if (!s_connected || !s_l2cap_connected) {
                abort = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (abort) {
            s_rec.truncated.store(true, std::memory_order_release);
            break;
        }
    }
    const bool ok = !s_rec.truncated.load(std::memory_order_acquire) &&
                    s_rec.complete.load(std::memory_order_acquire);
    const uint8_t* header = ok && s_rec.final_header.size() >= 60
                                ? s_rec.final_header.data() : nullptr;
    (void)RecSendStreamEnd(s_rec.transfer_id, ok ? 0 : 1,
                           s_rec.sent_bytes.load(std::memory_order_acquire), header);
    ESP_LOGI(TAG, "recording/ASR stream %u ended (ok=%d, bytes=%u)",
             static_cast<unsigned>(s_rec.transfer_id), ok,
             static_cast<unsigned>(s_rec.sent_bytes.load(std::memory_order_acquire)));
    s_rec.active.store(false, std::memory_order_release);
    s_rec.task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t RecordingStart(const char* name, size_t name_len) {
    if (s_rec.active.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!s_connected || !s_l2cap_connected) return ESP_ERR_INVALID_STATE;
    {
        std::lock_guard<std::mutex> lock(s_rec.mtx);
        std::queue<std::vector<uint8_t>> empty;
        std::swap(s_rec.q, empty);
    }
    s_rec.queued_bytes.store(0, std::memory_order_release);
    s_rec.sent_bytes.store(0, std::memory_order_release);
    s_rec.end_req.store(false, std::memory_order_release);
    s_rec.truncated.store(false, std::memory_order_release);
    s_rec.complete.store(true, std::memory_order_release);
    s_rec.final_header.clear();
    s_rec.transfer_id = ++s_rec_counter;
    if (!RecSendStreamStart(s_rec.transfer_id, name, name_len)) return ESP_FAIL;
    s_rec.active.store(true, std::memory_order_release);
    if (xTaskCreate(RecordingTask, "agentlink_rec", 6144, nullptr, 5, &s_rec.task) != pdPASS) {
        s_rec.active.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t RecordingEnqueue(const uint8_t* data, size_t len) {
    if (!s_rec.active.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_rec.truncated.load(std::memory_order_acquire)) return ESP_ERR_NO_MEM;
    std::lock_guard<std::mutex> lock(s_rec.mtx);
    if (s_rec.queued_bytes.load(std::memory_order_acquire) + len > kRecQueueHardCap) {
        s_rec.truncated.store(true, std::memory_order_release);
        s_rec.end_req.store(true, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    s_rec.q.emplace(data, data + len);
    s_rec.queued_bytes.fetch_add(len, std::memory_order_release);
    return ESP_OK;
}

esp_err_t RecordingEnd(bool complete, const uint8_t* final_header, size_t header_len) {
    if (!s_rec.active.load(std::memory_order_acquire)) return ESP_OK;
    s_rec.complete.store(complete, std::memory_order_release);
    if (final_header && header_len >= 60) {
        s_rec.final_header.assign(final_header, final_header + 60);
    } else {
        s_rec.final_header.clear();
    }
    s_rec.end_req.store(true, std::memory_order_release);
    return ESP_OK;
}
#endif

// GAP events: connect / disconnect / advertising complete.
int GapEvent(struct ble_gap_event* event, void* /*arg*/) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_event_notify_enabled = false;
            s_voice_notify_enabled = false;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (handle=%d)", s_conn_handle);
            if (s_on_conn) s_on_conn(true);
        } else {
            ESP_LOGW(TAG, "connect failed (status=%d) — re-advertising", event->connect.status);
            if (!s_stopping.load(std::memory_order_acquire)) StartAdvertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_event_notify_enabled = false;
        s_voice_notify_enabled = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        s_l2cap_connected = false; s_l2cap_chan = nullptr;  // L2CAP goes down with the ACL
        s_l2cap_stalled.store(false, std::memory_order_release);
        s_rec.truncated.store(true, std::memory_order_release);
        s_rec.end_req.store(true, std::memory_order_release);
#endif
        s_voice.end_req.store(true, std::memory_order_release);  // tell the voice worker to wrap up ASAP
        ESP_LOGI(TAG, "disconnected (reason=%d) — re-advertising", event->disconnect.reason);
        if (s_on_conn) s_on_conn(false);
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
            s_event_notify_enabled = event->subscribe.cur_notify;
            if (became_ready && s_on_ready) {
                ESP_LOGI(TAG, "peer subscribed 0xFFC4 (event channel) — notify-ready");
                s_on_ready();
            }
        }
        if (event->subscribe.attr_handle == s_h_voice) {
            s_voice_notify_enabled = event->subscribe.cur_notify;
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
    if (ble_hs_util_ensure_addr(0) != 0) { ESP_LOGE(TAG, "ensure_addr failed"); return; }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) { ESP_LOGE(TAG, "infer_auto failed"); return; }
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    (void)StartL2capServer();  // once the stack is ready, create the L2CAP CoC server (downlink TTS voice channel)
#else
    ESP_LOGW(TAG, "L2CAP CoC is disabled by this Arduino-ESP32 core; TTS downlink is unavailable");
#endif
    if (!s_stopping.load(std::memory_order_acquire)) StartAdvertising();
}

void OnReset(int reason) { ESP_LOGW(TAG, "nimble reset; reason=%d", reason); }

void HostTask(void* /*param*/) {
    s_host_running.store(true, std::memory_order_release);
    nimble_port_run();
    s_host_running.store(false, std::memory_order_release);
    nimble_port_freertos_deinit();
}

// ── agent_transport_t interface implementation ──
esp_err_t ble_start(void* /*impl*/) {
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_stopping.store(false, std::memory_order_release);
    // Arduino-ESP32 owns the controller-memory and PHY initialization sequence.
    // The BLE memory-use marker above must run before initArduino(); after that,
    // NimBLE performs the remaining controller/host initialization here.
    esp_err_t r = nimble_port_init();
    if (r != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(r)); return r; }

    ble_hs_cfg.sync_cb  = OnSync;
    ble_hs_cfg.reset_cb = OnReset;

    ble_hs_cfg.sm_bonding = 1;                          // keep the keys after pairing
    ble_hs_cfg.sm_sc      = 1;                          // LE Secure Connections
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;

    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    if (RegisterGatt() != ESP_OK) return ESP_FAIL;
    ble_svc_gap_device_name_set(s_name);

    ble_store_config_init();  // install the key store
    nimble_port_freertos_init(HostTask);
    s_started = true;
    ClearControlQueue();
    if (xTaskCreate(ControlTask, "agentlink_ctrl", 4096, nullptr, 6, &s_control_task) != pdPASS) {
        s_control_task = nullptr;
        ESP_LOGE(TAG, "control notification task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BLE started — Service C 0xFFC0 registered; advertising on sync");
    return ESP_OK;
}

void ble_stop(void* /*impl*/) {
    if (!s_started) return;
    s_stopping.store(true, std::memory_order_release);
    (void)ble_gap_adv_stop();
    if (s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_connected = false;
    s_event_notify_enabled = false;
    s_voice_notify_enabled = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_voice.end_req.store(true, std::memory_order_release);
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
    s_rec.truncated.store(true, std::memory_order_release);
    s_rec.end_req.store(true, std::memory_order_release);
#endif
    for (int i = 0; i < 100 && s_control_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const int stop_result = nimble_port_stop();
    if (stop_result == 0) {
        for (int i = 0; i < 100 && s_host_running.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        const esp_err_t deinit_result = nimble_port_deinit();
        if (deinit_result != ESP_OK) {
            ESP_LOGW(TAG, "nimble_port_deinit: %s", esp_err_to_name(deinit_result));
        }
    } else {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d", stop_result);
    }
    s_h_cmd = s_h_evt = s_h_voice = s_h_batt = 0;
    s_started = false;
}

// Control-plane send: route by frame message_type. response (0x02) -> 0xFFC1; event (0x03) -> 0xFFC4.
esp_err_t ble_send_ctrl(void* /*impl*/, const uint8_t* frame, size_t len) {
    if (!frame || len < 2) return ESP_ERR_INVALID_ARG;
    const uint8_t msg_type = frame[1] & 0x7F;
    const uint16_t handle = (msg_type == 0x03 /*Event*/) ? s_h_evt : s_h_cmd;
    return EnqueueControl(handle, frame, len);
}

// Data plane: voice over GATT Notify 0xFFA1 (implemented); recording/file over L2CAP, video WiFi-only (to do).
esp_err_t ble_stream_start(void* /*impl*/, agent_stream_t type, const uint8_t* meta, size_t meta_len) {
    if (type == AGENT_STREAM_VOICE) return VoiceStart();
    if (type == AGENT_STREAM_RECORDING) {
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        return RecordingStart(reinterpret_cast<const char*>(meta), meta_len);
#else
        (void)meta; (void)meta_len;
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ble_send_stream(void* /*impl*/, agent_stream_t type, const uint8_t* data, size_t len) {
    if (type == AGENT_STREAM_VOICE) return VoiceEnqueue(data, len);
    if (type == AGENT_STREAM_RECORDING) {
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        return RecordingEnqueue(data, len);
#else
        (void)data; (void)len;
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ble_stream_end(void* /*impl*/, agent_stream_t type, bool complete,
                         const uint8_t* meta, size_t meta_len) {
    if (type == AGENT_STREAM_VOICE) return VoiceEnd();
    if (type == AGENT_STREAM_RECORDING) {
#if DEOTALAND_BLE_L2CAP_COC_AVAILABLE
        return RecordingEnd(complete, meta, meta_len);
#else
        (void)complete; (void)meta; (void)meta_len;
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
}
bool ble_is_ready(void* /*impl*/) { return s_connected && s_event_notify_enabled; }

agent_transport_t s_ble = {
    ble_start, ble_stop, ble_send_ctrl,
    ble_stream_start, ble_send_stream, ble_stream_end,
    ble_is_ready, nullptr,
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
    if (mfr && *mfr)   { strncpy(s_di_mfr, mfr, sizeof(s_di_mfr) - 1);     s_di_mfr[sizeof(s_di_mfr) - 1] = '\0'; }
    if (model && *model){ strncpy(s_di_model, model, sizeof(s_di_model) - 1); s_di_model[sizeof(s_di_model) - 1] = '\0'; }
    if (fw && *fw)     { strncpy(s_di_fw, fw, sizeof(s_di_fw) - 1);        s_di_fw[sizeof(s_di_fw) - 1] = '\0'; }
}

// Update battery (standard service 0x180F). Store the value for reads; notify 0x2A19 if connected. Called by the core when the level changes.
extern "C" void agent_transport_ble_update_battery(uint8_t percent) {
    s_batt_level = (percent > 100) ? 100 : percent;
    if (s_connected && s_h_batt) (void)Notify(s_h_batt, &s_batt_level, 1);
}

#else  // CONFIG_BT_NIMBLE_ENABLED && SOC_BLE_SUPPORTED

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
    unavailable_ready, nullptr,
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

#endif  // CONFIG_BT_NIMBLE_ENABLED && SOC_BLE_SUPPORTED
