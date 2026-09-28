#include "wifi_platform.h"
#if !AGENT_LINK_WIFI_AVAILABLE
#include "agent_link_transport.h"
#include "wifi_provision.h"
namespace {
esp_err_t unavailable(void*) { return ESP_ERR_NOT_SUPPORTED; }
void stop(void*) {}
bool ready(void*) { return false; }
agent_transport_t wifi = {unavailable, stop, nullptr, nullptr, nullptr, nullptr, ready, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}
agent_transport_t* agent_transport_wifi(void) { return &wifi; }
void agent_transport_wifi_set_config(const struct agent_wifi_config_s*) {}
void agent_transport_wifi_set_platform(const struct agent_platform_s*, const char*) {}
void agent_transport_wifi_set_status(void (*)(const struct agent_link_status_s*)) {}
esp_err_t agent_transport_wifi_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
bool agent_transport_wifi_active(void) { return false; }
void agent_transport_wifi_set_name(const char*) {}
void agent_transport_wifi_set_recv(void (*)(const uint8_t*, size_t)) {}
void agent_transport_wifi_set_conn(void (*)(bool)) {}
void agent_transport_wifi_set_stream_recv(void (*)(agent_stream_t, const uint8_t*, size_t)) {}
esp_err_t al_wifi_prov_start(const char*, al_prov_creds_cb_t) { return ESP_ERR_NOT_SUPPORTED; }
void al_wifi_prov_set_status(al_prov_status_t, const char*, const char*) {}
void al_wifi_prov_stop(void) {}
bool al_wifi_prov_active(void) { return false; }
#endif
