#include "wifi_platform.h"
#if AGENT_LINK_WIFI_AVAILABLE
// agent_link WiFi transport backend — station (STA) bring-up + captive-portal provisioning (配网).
//
// What this file owns:
//    1.The ESP-IDF WiFi stack for the WiFi transport: esp_netif + driver init, STA connect,
//     auto-reconnect with backoff, and credential persistence in NVS
//    2.First-run provisioning: with no stored credentials it brings up an open SoftAP and hands
//     off to the captive portal (wifi_provision.cpp) — the user joins the device's WiFi, a page
//     opens automatically, and they enter their home SSID/password. Credentials are saved only
//     once the device confirms it can actually join (got an IP), so a wrong password is never kept
//    3.Self-healing: if stored credentials stop working (AP moved / password changed), it reopens
//     the portal after a bounded number of failed joins
#include "agent_link_transport.h"
#include "agent_link.h"          // STA credentials, platform configuration and display status
#include "wifi_provision.h"
#include "wifi_credentials.h"
#include "wifi_network_state.h"
#include "worker_wake.h"
#include "cloud_worker.h"
#include "device_identity.h"

#include <cstring>
#include <cstdio>
#include <atomic>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace {
constexpr const char* TAG = "agent_link.wifi";

// NVS store for provisioned station credentials.
constexpr const char* kNvsNs   = "al_wifi";
constexpr const char* kNvsSsid = "ssid";
constexpr const char* kNvsPass = "pass";
constexpr const char* kNvsCredentials = "credentials";

// Reconnect / provisioning-fallback thresholds.
constexpr int      kProvFailLimit   = 3;      // provisioning: give up an attempt after 3 disconnects
constexpr int      kBootFailLimit   = 8;      // stored creds: after 8 failed joins without ever getting IP ,then open the portal
constexpr uint32_t kReconnectBaseMs = 2000;   // base station reconnect backoff
constexpr uint32_t kReconnectMaxMs  = 30000;  // backoff cap
constexpr uint32_t kProvTeardownMs  = 4000;   // linger on the portal after success so the page can show it, then drop the AP
constexpr uint32_t kSaveRetryMs     = 5000;

enum class Phase { kIdle, kProvisioning, kStaConnecting, kStaConnected };

char  s_name[25]    = "AgentLink";   // device name,SoftAP SSID prefix
char  s_ap_ssid[33] = {0};           // "<name>-XXXX" (XXXX from the SoftAP MAC)

const agent_wifi_config_s* s_cfg = nullptr;
const agent_platform_t* s_platform = nullptr;
const char* s_firmware = nullptr;
void (*s_on_status)(const agent_link_status_t*) = nullptr;
// Copy event addresses; invalidate delayed work at every network loss.
agentlink::wifi_detail::NetworkState s_network;
std::atomic<esp_err_t> s_service_error{ESP_OK};
agent_link_status_t s_last_status = {};
bool s_reported_status = false, s_reported_connected = false; // loop only

// Transport -> core uplink callbacks
void (*s_on_recv)(const uint8_t*, size_t) = nullptr;
void (*s_on_conn)(bool) = nullptr;
void (*s_on_stream)(agent_stream_t, const uint8_t*, size_t) = nullptr;

bool  s_wifi_inited = false;
bool  s_started     = false;
Phase s_phase       = Phase::kIdle;
bool  s_want_connect = false;    // gate STA_START/reconnect auto-connect (off while waiting for portal input)
bool  s_ever_got_ip  = false;    // once true, keep retrying forever instead of falling back to the portal
int   s_retry        = 0;        // consecutive failed joins in the current phase

char       s_ssid[33] = {0};     // credentials currently being tried
char       s_pass[65] = {0};

bool s_reconnect_pending = false;
bool s_teardown_pending = false;
bool s_save_pending = false;
uint32_t s_save_at = 0;
char s_provisioned_ip[16] = {};
uint32_t s_reconnect_at = 0;
uint32_t s_teardown_at = 0;
bool s_credentials_pending = false;
uint32_t s_credentials_at = 0;
uint32_t s_credentials_started = 0;

esp_err_t StartProvisioning();
std::atomic<bool> s_service_alive{false};
std::atomic<bool> s_stop_requested{true};
enum class WorkKind : uint8_t { kStaStart, kDisconnected, kGotIp, kCredentials };
struct Work {
    WorkKind kind;
    uint32_t generation;
    uint32_t network_ticket;
    uint8_t reason;
    esp_netif_ip_info_t ip;
    char ssid[33];
    char password[65];
};
QueueHandle_t s_work = nullptr;
agentlink::detail::WorkerWake s_service_wake;
char s_initial_ssid[33] = {};
char s_initial_pass[65] = {};
std::atomic<uint32_t> s_service_generation{0};
std::atomic<uint32_t> s_fence_seen{0};
ESP_EVENT_DEFINE_BASE(AL_WIFI_FENCE_EVENT);
esp_event_handler_instance_t s_fence_handler = nullptr;
void ApplyPendingCredentials();
bool Due(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }
void FenceEvent(void*, esp_event_base_t, int32_t, void* data) {
    if (data) s_fence_seen.store(*static_cast<const uint32_t*>(data), std::memory_order_release);
}
esp_event_handler_instance_t s_wifi_handler = nullptr;
esp_event_handler_instance_t s_ip_handler = nullptr;
esp_netif_t* s_owned_sta = nullptr;
esp_netif_t* s_owned_ap = nullptr;

void RequestStop(esp_err_t error) {
    esp_err_t expected = ESP_OK;
    s_service_error.compare_exchange_strong(expected, error); // preserve the first cause
    s_network.clear();
    s_stop_requested.store(true);
    s_service_wake.wake();
}

bool PostWork(const Work& work) {
    Work copy = work;
    copy.generation = s_service_generation.load(std::memory_order_acquire);
    if (!s_service_alive.load() || s_stop_requested.load() || !s_work) return false;
    const bool sent = xQueueSend(s_work, &copy, 0) == pdTRUE;
    if (sent) s_service_wake.wake();
    return sent;
}
void PostKind(WorkKind kind) {
    Work work = {}; work.kind = kind;
    if (!PostWork(work) && !s_stop_requested.load()) RequestStop(ESP_ERR_NO_MEM);
}


// NVS credential store
esp_err_t EnsureNvs() {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // A library must not erase Preferences/NVS owned by the user's sketch.
        ESP_LOGE(TAG, "NVS requires recovery (%s); refusing to erase application data",
                 esp_err_to_name(r));
    }
    return r;
}

bool LoadCreds(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    agentlink::wifi_detail::CredentialRecord record = {};
    size_t size = sizeof(record);
    const esp_err_t result = nvs_get_blob(h, kNvsCredentials, &record, &size);
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        if (result != ESP_OK || size != sizeof(record) ||
            !agentlink::wifi_detail::ValidCredentials(record) ||
            std::strlen(record.ssid) >= ssid_sz || std::strlen(record.password) >= pass_sz) return false;
        std::strcpy(ssid, record.ssid);
        std::strcpy(pass, record.password);
        return true;
    }
    // Read existing installations without rewriting flash during startup.
    size_t sl = sizeof(record.ssid), pl = sizeof(record.password);
    const esp_err_t ssid_result = nvs_get_str(h, kNvsSsid, record.ssid, &sl);
    const esp_err_t pass_result = nvs_get_str(h, kNvsPass, record.password, &pl);
    nvs_close(h);
    if (ssid_result != ESP_OK || !record.ssid[0] ||
        (pass_result != ESP_OK && pass_result != ESP_ERR_NVS_NOT_FOUND) ||
        std::strlen(record.ssid) >= ssid_sz || std::strlen(record.password) >= pass_sz) return false;
    std::strcpy(ssid, record.ssid);
    std::strcpy(pass, record.password);
    return true;
}

esp_err_t SaveCreds(const char* ssid, const char* pass) {
    struct Store {
        nvs_handle_t handle = 0;
        esp_err_t open() { return nvs_open(kNvsNs, NVS_READWRITE, &handle); }
        esp_err_t write(const agentlink::wifi_detail::CredentialRecord& value) {
            return nvs_set_blob(handle, kNvsCredentials, &value, sizeof(value));
        }
        esp_err_t commit() { return nvs_commit(handle); }
        void close() { nvs_close(handle); }
    } store;
    return agentlink::wifi_detail::SaveCredentials(store, ssid, pass);
}

void PersistProvisionedCredentials() {
    const esp_err_t result = SaveCreds(s_ssid, s_pass);
    s_save_pending = result != ESP_OK;
    if (result != ESP_OK) {
        s_save_at = agent_link_now_ms() + kSaveRetryMs;
        s_teardown_pending = false;
        al_wifi_prov_set_status(AL_PROV_SAVE_FAILED, s_provisioned_ip, "storage");
        ESP_LOGW(TAG, "WiFi joined but credentials were not saved: %s; retrying", esp_err_to_name(result));
        return;
    }
    al_wifi_prov_set_status(AL_PROV_CONNECTED, s_provisioned_ip, nullptr);
    ESP_LOGI(TAG, "provisioning succeeded: joined '%s', ip=%s (credentials saved)", s_ssid, s_provisioned_ip);
    s_teardown_at = agent_link_now_ms() + kProvTeardownMs;
    s_teardown_pending = true;
}

//  Helpers 
// Returns a short machine code (not prose) so the portal page can localize it — see connecting.html.
const char* ReasonStr(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:              return "notfound";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   return "badpass";
    default:                                   return "fail";
    }
}

void BuildApSsid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s-%02X%02X", s_name, mac[4], mac[5]);
}

esp_err_t ApplyStaConfig(const char* ssid, const char* pass) {
    wifi_config_t wc = {};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    if (pass) memcpy(wc.sta.password, pass, strlen(pass));
    wc.sta.pmf_cfg.capable = true;   // allow WPA3/PMF APs
    return esp_wifi_set_config(WIFI_IF_STA, &wc);
}

esp_err_t ApplyApConfig() {
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char*>(wc.ap.ssid), s_ap_ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = strlen(s_ap_ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_OPEN;   // open network
    wc.ap.beacon_interval = 100;
    return esp_wifi_set_config(WIFI_IF_AP, &wc);
}

void ScheduleReconnect(uint32_t ms) {
    s_reconnect_at = agent_link_now_ms() + ms;
    s_reconnect_pending = true;
}

// Station reached the network
void OnGotIp(const esp_netif_ip_info_t& ip, uint32_t ticket) {
    wifi_ap_record_t associated = {};
    if (s_credentials_pending || esp_wifi_sta_get_ap_info(&associated) != ESP_OK ||
        strncmp(reinterpret_cast<const char*>(associated.ssid), s_ssid, sizeof(associated.ssid)) != 0) return;
    if (!s_network.accept(ticket, ip.ip.addr)) return;
    char ip_str[16];
    esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof ip_str);
    s_ever_got_ip = true;
    s_retry = 0;

    if (al_wifi_prov_active()) {
        // First successful join from the portal: persist the credentials now
        std::strcpy(s_provisioned_ip, ip_str);
        PersistProvisionedCredentials();
    } else {
        ESP_LOGI(TAG, "WiFi connected: ip=%s", ip_str);
    }
    s_phase = Phase::kStaConnected;
}

void OnStaDisconnected(uint8_t reason) {
    cloud_worker_network(false);
    if (s_save_pending && al_wifi_prov_active()) al_wifi_prov_set_status(AL_PROV_CONNECTING, nullptr, nullptr);
    if (s_credentials_pending) { ApplyPendingCredentials(); return; }
    if (!s_want_connect) return;
    if (s_phase == Phase::kProvisioning) {
        // A join attempt during provisioning failed,retry a couple times, then tell the page.
        if (++s_retry >= kProvFailLimit) {
            s_want_connect = false;                    // stop hammering; wait for a fresh submit from the page
            al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, ReasonStr(reason));
            ESP_LOGW(TAG, "provisioning join failed (reason=%d) after %d tries — awaiting retry", reason, s_retry);
        } else {
            ESP_LOGD(TAG, "provisioning join retry %d (reason=%d)", s_retry, reason);
            ScheduleReconnect(1500);
        }
        return;
    }

    // Normal station path
    s_phase = Phase::kStaConnecting;
    if (s_retry < 16) ++s_retry;
    if (!s_ever_got_ip && s_retry >= kBootFailLimit) {
        ESP_LOGW(TAG, "cannot join '%s' after %d tries — starting provisioning portal", s_ssid, s_retry);
        const esp_err_t result = StartProvisioning();
        if (result != ESP_OK) RequestStop(result);
        return;
    }
    uint32_t backoff = kReconnectBaseMs << (s_retry < 4 ? s_retry : 4);
    if (backoff > kReconnectMaxMs) backoff = kReconnectMaxMs;
    ESP_LOGD(TAG, "station disconnected (reason=%d) — reconnect in %ums", reason, static_cast<unsigned>(backoff));
    ScheduleReconnect(backoff);
}

// ESP event/timer/HTTP callbacks only post bounded work. All driver state and
// blocking portal teardown belong to ServiceTask, never the Arduino loop.
void WifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_STA_START) PostKind(WorkKind::kStaStart);
    else if (id == WIFI_EVENT_STA_STOP) {
        s_network.clear();
        cloud_worker_network(false);
    }
    else if (id == WIFI_EVENT_STA_DISCONNECTED && data) {
        s_network.clear();
        cloud_worker_network(false);
        Work work = {};
        work.kind = WorkKind::kDisconnected;
        work.reason = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
        if (!PostWork(work) && !s_stop_requested.load()) {
            ESP_LOGE(TAG, "WiFi event queue full; stopping session");
            RequestStop(ESP_ERR_NO_MEM);
        }
    }
}
void IpEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base != IP_EVENT) return;
    if (id == IP_EVENT_STA_LOST_IP) {
        s_network.clear();
        cloud_worker_network(false);
        s_service_wake.wake();
        return;
    }
    if (id != IP_EVENT_STA_GOT_IP || !data) return;
    Work work = {};
    work.kind = WorkKind::kGotIp;
    work.network_ticket = s_network.ticket();
    work.ip = static_cast<const ip_event_got_ip_t*>(data)->ip_info;
    if (!PostWork(work) && !s_stop_requested.load()) RequestStop(ESP_ERR_NO_MEM);
}

// One-time WiFi stack bring-up: netif + event loop + default STA/AP interfaces + driver + handlers.
esp_err_t WifiInitOnce() {
    if (s_wifi_inited) return ESP_OK;
    esp_err_t result = EnsureNvs();
    if (result != ESP_OK) return result;
    wifi_mode_t previous_mode;
    if (esp_wifi_get_mode(&previous_mode) == ESP_OK) {
        ESP_LOGE(TAG, "WiFi driver already belongs to another library");
        return ESP_ERR_INVALID_STATE;
    }
    result = esp_netif_init();
    if (result != ESP_OK) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    const bool own_sta = !sta, own_ap = !ap;
    if (!sta) sta = esp_netif_create_default_wifi_sta();
    if (!ap) ap = esp_netif_create_default_wifi_ap();
    bool driver_initialized = false;
    auto fail = [&](esp_err_t error) {
        if (s_wifi_handler) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler); s_wifi_handler = nullptr; }
        if (s_ip_handler) { esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler); s_ip_handler = nullptr; }
        if (s_fence_handler) { esp_event_handler_instance_unregister(AL_WIFI_FENCE_EVENT, ESP_EVENT_ANY_ID, s_fence_handler); s_fence_handler = nullptr; }
        if (driver_initialized) esp_wifi_deinit();
        if (own_sta && sta) esp_netif_destroy_default_wifi(sta);
        if (own_ap && ap) esp_netif_destroy_default_wifi(ap);
        return error;
    };
    if (!sta || !ap) return fail(ESP_ERR_NO_MEM);
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&config);
    if (result != ESP_OK) return fail(result);
    driver_initialized = true;
    result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEvent, nullptr, &s_wifi_handler);
    if (result != ESP_OK) return fail(result);
    result = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, IpEvent, nullptr, &s_ip_handler);
    if (result != ESP_OK) return fail(result);
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) return fail(result);
    result = esp_event_handler_instance_register(AL_WIFI_FENCE_EVENT, ESP_EVENT_ANY_ID, FenceEvent, nullptr, &s_fence_handler);
    if (result != ESP_OK) return fail(result);
    s_owned_sta = own_sta ? sta : nullptr;
    s_owned_ap = own_ap ? ap : nullptr;
    s_wifi_inited = true;
    return ESP_OK;
}

// Join a network as a station (boot path with known credentials).
esp_err_t StartSta(const char* ssid, const char* pass) {
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, pass ? pass : "", sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = '\0';
    s_phase = Phase::kStaConnecting;
    s_want_connect = true;
    s_retry = 0;
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) return result;
    result = ApplyStaConfig(ssid, pass);
    if (result != ESP_OK) return result;
    result = esp_wifi_start();
    if (result == ESP_OK) s_started = true;
    return result;
}

// The user submitted credentials on the portal
void ApplyPendingCredentials() {
    if (!s_credentials_pending || s_stop_requested.load()) return;
    const esp_err_t result = ApplyStaConfig(s_ssid, s_pass);
    if (result == ESP_ERR_WIFI_STATE && agent_link_now_ms() - s_credentials_started < 3000) {
        s_credentials_at = agent_link_now_ms() + 100;
        return;
    }
    s_credentials_pending = false;
    if (result != ESP_OK) {
        s_want_connect = false;
        al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, "config");
        return;
    }
    s_want_connect = true;
    if (esp_wifi_connect() != ESP_OK) ScheduleReconnect(1500);
}

void ApplyProvCreds(const char* ssid, const char* pass) {
    s_phase = Phase::kProvisioning;
    s_teardown_pending = false;
    s_save_pending = false;
    s_network.clear();
    cloud_worker_network(false);
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = 0;
    strncpy(s_pass, pass ? pass : "", sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = 0;
    s_retry = 0;
    s_reconnect_pending = false;
    s_want_connect = false;
    s_credentials_pending = true;
    s_credentials_started = agent_link_now_ms();
    s_credentials_at = s_credentials_started + 100;
    al_wifi_prov_set_status(AL_PROV_CONNECTING, nullptr, nullptr);
    // IDF rejects set_config during a connection attempt. The disconnect event
    // applies the new values; a bounded deferred retry also covers an idle STA.
    (void)esp_wifi_disconnect();
}

void OnProvCreds(const char* ssid, const char* pass) {
    Work work = {};
    work.kind = WorkKind::kCredentials;
    strncpy(work.ssid, ssid, sizeof(work.ssid) - 1);
    strncpy(work.password, pass ? pass : "", sizeof(work.password) - 1);
    if (!PostWork(work)) al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, "busy");
}

// Bring up the SoftAP + captive portal and wait for the user to enter their WiFi
esp_err_t StartProvisioning() {
    s_want_connect = false;
    s_retry = 0;
    s_phase = Phase::kProvisioning;
    s_teardown_pending = false;
    s_save_pending = false;

    s_reconnect_pending = false;
    if (s_started) (void)esp_wifi_disconnect();
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result != ESP_OK) return result;
    result = ApplyApConfig();
    if (result != ESP_OK) return result;
    if (!s_started) {
        result = esp_wifi_start();
        if (result != ESP_OK) return result;
        s_started = true;
    }
    return al_wifi_prov_start(s_ap_ssid, OnProvCreds);
}

esp_err_t ReleaseDriver() {
    const esp_err_t result = esp_wifi_deinit();
    if (result != ESP_OK) return result;
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler);
    esp_event_handler_instance_unregister(AL_WIFI_FENCE_EVENT, ESP_EVENT_ANY_ID, s_fence_handler);
    s_wifi_handler = nullptr; s_ip_handler = nullptr; s_fence_handler = nullptr;
    if (s_owned_sta) { esp_netif_destroy_default_wifi(s_owned_sta); s_owned_sta = nullptr; }
    if (s_owned_ap) { esp_netif_destroy_default_wifi(s_owned_ap); s_owned_ap = nullptr; }
    s_wifi_inited = false;
    return ESP_OK;
}

void ServiceTask(void*) {
    s_service_wake.attach();
    esp_err_t result = ESP_OK;
    if (!s_stop_requested.load()) result = s_initial_ssid[0] ? StartSta(s_initial_ssid, s_initial_pass) : StartProvisioning();
    if (result == ESP_OK && !s_stop_requested.load()) result = dev_identity_load(true);
    if (result == ESP_OK && !s_stop_requested.load() && s_platform)
        result = cloud_worker_start(s_platform,s_firmware);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(result));
        RequestStop(result);
    }
    const uint32_t generation = s_service_generation.load();
    while (!s_stop_requested.load()) {
        const uint32_t now = agent_link_now_ms();
        if (s_credentials_pending && Due(now, s_credentials_at)) ApplyPendingCredentials();
        if (s_reconnect_pending && Due(now, s_reconnect_at)) {
            s_reconnect_pending = false;
            if (s_want_connect && esp_wifi_connect() != ESP_OK) ScheduleReconnect(1500);
        }
        if (s_teardown_pending && Due(now, s_teardown_at)) {
            s_teardown_pending = false;
            if (s_phase == Phase::kStaConnected) {
                al_wifi_prov_stop();
                const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_STA);
                if (mode_result != ESP_OK) RequestStop(mode_result);
            }
        }
        if (s_save_pending && s_network.address() && Due(now, s_save_at)) PersistProvisionedCredentials();
        cloud_worker_network(s_network.address() != 0 && !al_wifi_prov_active() && !s_stop_requested.load());
        Work work = {};
        if (xQueueReceive(s_work, &work, 0) != pdTRUE) {
            // Producers latch notifications, including posts between this empty
            // check and wait(). An idle connected device needs no timer polling.
            uint32_t wait = UINT32_MAX;
            const uint32_t idle_now = agent_link_now_ms();
            const auto deadline = [&](bool pending, uint32_t at) {
                if (!pending) return;
                const uint32_t remaining = Due(idle_now, at) ? 0 : at - idle_now;
                if (remaining < wait) wait = remaining;
            };
            deadline(s_credentials_pending, s_credentials_at);
            deadline(s_reconnect_pending, s_reconnect_at);
            deadline(s_teardown_pending, s_teardown_at);
            deadline(s_save_pending && s_network.address(), s_save_at);
            if (!s_stop_requested.load()) s_service_wake.wait(wait);
            continue;
        }
        if (s_stop_requested.load()) break;
        if (work.generation != generation) continue;
        switch (work.kind) {
        case WorkKind::kStaStart:
            if (s_want_connect && esp_wifi_connect() != ESP_OK) ScheduleReconnect(1500);
            break;
        case WorkKind::kDisconnected: OnStaDisconnected(work.reason); break;
        case WorkKind::kGotIp:
            if (s_want_connect) OnGotIp(work.ip, work.network_ticket);
            break;
        case WorkKind::kCredentials:
            if (al_wifi_prov_active()) ApplyProvCreds(work.ssid, work.password);
            break;
        }
    }
    s_want_connect = false;
    cloud_worker_stop();
    // Keep driver/config ownership until the last HTTP request has retired.
    // This wait is in the worker, never in loop()/end().
    while (cloud_worker_active()) vTaskDelay(pdMS_TO_TICKS(50));
    s_network.clear();
    s_reconnect_pending = false;
    s_teardown_pending = false;
    al_wifi_prov_stop();
    if (s_started) {
        (void)esp_wifi_disconnect();
        const esp_err_t stopped = esp_wifi_stop();
        if (stopped != ESP_OK) {
            RequestStop(stopped);
            ESP_LOGE(TAG, "WiFi stop failed: %s; restart disabled", esp_err_to_name(stopped));
            s_service_wake.detach();
            vTaskDelete(nullptr);
            return;
        }
        s_started = false;
    }
    s_phase = Phase::kIdle;
    s_credentials_pending = false;
    // The default event loop is FIFO. A fence after driver stop drains old SDK
    // callbacks before the next service generation may start.
    const uint32_t fence = generation;
    const esp_err_t fence_result = esp_event_post(AL_WIFI_FENCE_EVENT, 0, &fence, sizeof(fence), pdMS_TO_TICKS(100));
    const uint32_t waiting_since = agent_link_now_ms();
    while (fence_result == ESP_OK && s_fence_seen.load(std::memory_order_acquire) != fence &&
           agent_link_now_ms() - waiting_since < 2000) vTaskDelay(pdMS_TO_TICKS(10));
    if (fence_result != ESP_OK || s_fence_seen.load() != fence) {
        RequestStop(fence_result == ESP_OK ? ESP_ERR_TIMEOUT : fence_result);
        ESP_LOGE(TAG, "WiFi event-loop shutdown fence failed; restart disabled");
        s_service_wake.detach();
        vTaskDelete(nullptr);
        return;
    }
    const esp_err_t deinitialized = ReleaseDriver();
    if (deinitialized != ESP_OK) {
        RequestStop(deinitialized);
        ESP_LOGE(TAG, "WiFi deinit failed: %s; restart disabled", esp_err_to_name(deinitialized));
        s_service_wake.detach();
        vTaskDelete(nullptr);
        return;
    }
    vQueueDelete(s_work); s_work = nullptr;
    s_service_wake.detach();
    s_service_alive.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

// API never waits for an AP, an HTTP handler, or DNS/scan task shutdown.
esp_err_t wifi_start(void*) {
    if (s_service_alive.load()) return ESP_ERR_INVALID_STATE;
    if (s_platform && !AGENT_LINK_CLOUD_AVAILABLE) return ESP_ERR_NOT_SUPPORTED;
    if (s_cfg && ((s_cfg->ssid && strlen(s_cfg->ssid) > 32) ||
                  (s_cfg->password && strlen(s_cfg->password) > 64))) return ESP_ERR_INVALID_ARG;
    if (!s_work) s_work = xQueueCreate(16, sizeof(Work));
    if (!s_work) return ESP_ERR_NO_MEM;
    const esp_err_t result = WifiInitOnce();
    if (result != ESP_OK) { vQueueDelete(s_work); s_work = nullptr; return result; }
    xQueueReset(s_work);
    s_initial_ssid[0] = s_initial_pass[0] = 0;
    if (s_cfg && s_cfg->ssid && s_cfg->ssid[0]) {
        strncpy(s_initial_ssid, s_cfg->ssid, sizeof(s_initial_ssid) - 1);
        strncpy(s_initial_pass, s_cfg->password ? s_cfg->password : "", sizeof(s_initial_pass) - 1);
    } else (void)LoadCreds(s_initial_ssid, sizeof(s_initial_ssid), s_initial_pass, sizeof(s_initial_pass));
    BuildApSsid();
    s_network.clear(); s_service_error = ESP_OK;
    s_last_status = {}; s_reported_status = false; s_reported_connected = false;
    s_ever_got_ip = false; s_retry = 0;
    s_reconnect_pending = false; s_teardown_pending = false; s_credentials_pending = false; s_save_pending = false;
    s_service_generation.fetch_add(1);
    s_stop_requested.store(false);
    s_service_alive.store(true);
    if (xTaskCreate(ServiceTask, "agentlink_wifi", 6144, nullptr, 4, nullptr) != pdPASS) {
        s_stop_requested.store(true);
        const esp_err_t released = ReleaseDriver();
        if (released == ESP_OK) {
            vQueueDelete(s_work); s_work = nullptr;
            s_service_alive.store(false);
        } else ESP_LOGE(TAG, "WiFi failed-start cleanup failed; restart disabled");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void wifi_stop(void*) {
    s_stop_requested.store(true, std::memory_order_release);
    s_service_wake.wake();
    cloud_worker_stop();
}

void wifi_poll(void*) {
    agent_cloud_status_t cloud = {};
    if (s_platform) cloud_worker_status(&cloud);
    const bool stopping = s_stop_requested.load();
    const bool portal = !stopping && al_wifi_prov_active();
    esp_ip4_addr_t ip = {};
    ip.addr = stopping ? 0 : s_network.address();
    char address[16] = {};
    if (ip.addr) esp_ip4addr_ntoa(&ip, address, sizeof address);
    agent_link_status_t status;
    agent_cloud_compose(cloud,s_platform != nullptr,portal,s_ap_ssid,address,&status);
    const esp_err_t error = s_service_error.load();
    if (error != ESP_OK) {
        status = {}; status.transport = AGENT_TRANSPORT_WIFI;
        status.phase = AGENT_LINK_PHASE_BLOCKED; status.detail = error;
        snprintf(status.title,sizeof(status.title),"WiFi service failed");
        snprintf(status.hint,sizeof(status.hint),"%s; check configuration and restart the board",esp_err_to_name(error));
    }
    const bool connected = status.phase == AGENT_LINK_PHASE_CONNECTED;
    if (connected != s_reported_connected) {
        s_reported_connected = connected;
        if (s_on_conn) s_on_conn(connected);
    }
    if (!s_reported_status || memcmp(&s_last_status,&status,sizeof(status)) != 0) {
        s_reported_status = true; s_last_status = status;
        if (s_on_status) s_on_status(&status);
    }
}

// Control plane
esp_err_t wifi_send_ctrl(void* /*impl*/, const uint8_t* /*frame*/, size_t /*len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_stream_start(void* /*impl*/, agent_stream_t /*type*/, const uint8_t* /*meta*/, size_t /*meta_len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_send_stream(void* /*impl*/, agent_stream_t /*type*/, const uint8_t* /*data*/, size_t /*len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_stream_end(void* /*impl*/, agent_stream_t /*type*/, bool /*complete*/,
                          const uint8_t* /*meta*/, size_t /*meta_len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

bool wifi_is_ready(void* /*impl*/) { return false; }

agent_transport_t s_wifi = {
    wifi_start, wifi_stop, wifi_send_ctrl,
    wifi_stream_start, wifi_send_stream, wifi_stream_end,
    wifi_is_ready, nullptr, wifi_poll, nullptr, nullptr, nullptr, nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_wifi(void) { return &s_wifi; }

extern "C" void agent_transport_wifi_set_config(const struct agent_wifi_config_s* cfg) { s_cfg = cfg; }
extern "C" void agent_transport_wifi_set_platform(const agent_platform_t* platform, const char* firmware) {
    s_platform = platform; s_firmware = firmware;
}
extern "C" void agent_transport_wifi_set_status(void (*cb)(const agent_link_status_t*)) { s_on_status = cb; }
extern "C" esp_err_t agent_transport_wifi_forget(void) {
    if (s_stop_requested.load()) return ESP_ERR_INVALID_STATE;
    return s_platform ? cloud_worker_forget() : ESP_ERR_NOT_SUPPORTED;
}
extern "C" bool agent_transport_wifi_active(void) { return s_service_alive.load(); }

extern "C" void agent_transport_wifi_set_name(const char* name) {
    if (name && *name) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
}

extern "C" void agent_transport_wifi_set_recv(void (*cb)(const uint8_t*, size_t)) { s_on_recv = cb; }
extern "C" void agent_transport_wifi_set_conn(void (*cb)(bool)) { s_on_conn = cb; }
extern "C" void agent_transport_wifi_set_stream_recv(void (*cb)(agent_stream_t, const uint8_t*, size_t)) {
    s_on_stream = cb;
}


#endif // ARDUINO_ARCH_ESP32
