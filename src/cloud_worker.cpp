#include "wifi_platform.h"
#if AGENT_LINK_CLOUD_AVAILABLE
#include "cloud_worker.h"
#include "cloud_session.h"
#include "worker_wake.h"
#include "cloud_credential.h"
#include "device_identity.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
std::atomic<bool> active{false}, stopping{true}, network{false}, forgetting{false};
std::atomic<bool> initialized{false};
std::atomic<uint32_t> epoch{0};
uint32_t request_epoch = 0; // worker only
char base_url[96], chip[32], firmware_rev[256], sn[37], mac[18];
int product_id = 0;
agent_cloud_status_t status = {};
portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
agentlink::detail::WorkerWake worker_wake;
void Publish(void*, const agent_cloud_status_t& value) {
    taskENTER_CRITICAL(&status_lock); status = value; taskEXIT_CRITICAL(&status_lock);
}
void StorageFailure(esp_err_t result) {
    agent_cloud_status_t failed = {};
    failed.state = AGENT_CLOUD_FAULT; failed.api_code = result;
    snprintf(failed.message,sizeof(failed.message),"Cloud initialization or credential storage failed");
    Publish(nullptr,failed);
}
bool Valid(void*) {
    return !stopping.load() && !forgetting.load() && network.load() && epoch.load() == request_epoch;
}
uint32_t Now(void*) { return agent_link_now_ms(); }
esp_err_t Claim(void*, const agentlink::cloud::Config& c, cloud_claim_t* out, cloud_api_err_t* error) {
    return cloud_api_claim_code(c.product_id,c.sn,c.mac,c.chip,c.firmware,out,error);
}
esp_err_t Bind(void*, const agentlink::cloud::Config& c, const char* code, cloud_bind_t* out, cloud_api_err_t* error) {
    return cloud_api_bind_status(c.sn,code,out,error);
}
esp_err_t Auth(void*, const agentlink::cloud::Config& c, const char* key, cloud_api_err_t* error) {
    // Runtime configuration has no consumer until upstream defines the data plane.
    return cloud_api_auth(key,c.mac,c.firmware,nullptr,0,error);
}
esp_err_t Heartbeat(void*, const char* key, cloud_api_err_t* error) { return cloud_api_heartbeat(key,error); }
esp_err_t Save(void*, const char* key) { return cloud_cred_set_auth_key(key); }
esp_err_t Clear(void*) { return cloud_cred_clear(); }
void Drop(void*) { cloud_api_drop_connection(); }

void Worker(void*) {
    worker_wake.attach();
    esp_err_t result = cloud_cred_load();
    if (result == ESP_OK) result = cloud_api_init(base_url);
    if (result != ESP_OK) {
        StorageFailure(result);
        // Keep ownership until stop so a failed worker cannot silently restart.
        while (!stopping.load()) worker_wake.wait(UINT32_MAX);
    } else {
        agentlink::cloud::Session session;
        const agentlink::cloud::Config config = {product_id,sn,mac,chip,firmware_rev};
        const agentlink::cloud::Ops ops = {nullptr,Now,Valid,Claim,Bind,Auth,Heartbeat,Save,Clear,Drop,Publish};
        session.begin(config,ops,cloud_cred_auth_key());
        initialized.store(true);
        while (!stopping.load()) {
            if (forgetting.exchange(false)) (void)session.forget();
            if (stopping.load()) break;
            request_epoch = epoch.load();
            session.step(network.load());
            // Notifications remain pending if an event arrives between step()
            // and wait(), so disconnect/forget/stop cannot be lost here.
            if (!stopping.load()) worker_wake.wait(session.wait_ms(network.load()));
        }
    }
    // Only this task touches HTTP handles and credentials, including cleanup.
    cloud_api_deinit();
    initialized.store(false);
    worker_wake.detach();
    active.store(false,std::memory_order_release);
    vTaskDelete(nullptr);
}
}
esp_err_t cloud_worker_start(const agent_platform_t* platform, const char* firmware) {
    if (active.load()) return ESP_ERR_INVALID_STATE;
    if (!platform || !platform->base_url || strlen(platform->base_url) >= sizeof(base_url) ||
        !platform->chip_type || strlen(platform->chip_type) >= sizeof(chip) ||
        !firmware || strlen(firmware) >= sizeof(firmware_rev)) return ESP_ERR_INVALID_ARG;
    if (!dev_identity_sn()[0]) return ESP_ERR_INVALID_STATE;
    snprintf(base_url,sizeof(base_url),"%s",platform->base_url);
    snprintf(chip,sizeof(chip),"%s",platform->chip_type);
    snprintf(firmware_rev,sizeof(firmware_rev),"%s",firmware);
    snprintf(sn,sizeof(sn),"%s",dev_identity_sn());
    uint8_t address[6];
    const esp_err_t result = esp_read_mac(address,ESP_MAC_WIFI_STA);
    if (result != ESP_OK) return result;
    snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",address[0],address[1],address[2],address[3],address[4],address[5]);
    product_id = platform->product_id;
    agent_cloud_status_t initial = {}; initial.state = AGENT_CLOUD_NO_NETWORK; Publish(nullptr,initial);
    network = false; forgetting = false; stopping = false; initialized = false; ++epoch; active = true;
    if (xTaskCreate(Worker,"al_cloud",8192,nullptr,4,nullptr) != pdPASS) {
        stopping = true; active = false; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void cloud_worker_network(bool ready) {
    if (network.exchange(ready) != ready) { ++epoch; worker_wake.wake(); }
}
void cloud_worker_stop() { stopping = true; network = false; ++epoch; worker_wake.wake(); }
bool cloud_worker_active() { return active.load(std::memory_order_acquire); }
void cloud_worker_status(agent_cloud_status_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&status_lock); *out = status; taskEXIT_CRITICAL(&status_lock);
}
esp_err_t cloud_worker_forget() {
    if (!active.load() || stopping.load() || !initialized.load()) return ESP_ERR_INVALID_STATE;
    forgetting = true; ++epoch; worker_wake.wake(); return ESP_OK;
}
#elif AGENT_LINK_WIFI_AVAILABLE
#include "cloud_worker.h"
esp_err_t cloud_worker_start(const agent_platform_t*,const char*) { return ESP_ERR_NOT_SUPPORTED; }
void cloud_worker_network(bool) {}
void cloud_worker_stop() {}
bool cloud_worker_active() { return false; }
void cloud_worker_status(agent_cloud_status_t* out) { if (out) *out = {}; }
esp_err_t cloud_worker_forget() { return ESP_ERR_NOT_SUPPORTED; }
#endif
