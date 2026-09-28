#pragma once
// A single NVS value keeps the SSID and password from different submissions
// from being mixed if a write fails. Legacy separate keys remain read-only.
#include "agent_link_platform.h"
#include <cstring>

namespace agentlink { namespace wifi_detail {
struct CredentialRecord {
    char version[4];
    char ssid[33];
    char password[65];
};
inline bool ValidCredentials(const CredentialRecord& value) {
    return std::memcmp(value.version, "ALW1", 4) == 0 && value.ssid[0] &&
        std::memchr(value.ssid, 0, sizeof(value.ssid)) &&
        std::memchr(value.password, 0, sizeof(value.password));
}
// Store owns an opened handle. Always close it, including each failure path.
// Kept portable so the actual write/commit error behavior can be fault-tested.
template<class Store>
esp_err_t SaveCredentials(Store& store, const char* ssid, const char* password) {
    if (!ssid || !ssid[0] || std::strlen(ssid) > 32 ||
        (password && std::strlen(password) > 64)) return ESP_ERR_INVALID_ARG;
    CredentialRecord value = {};
    std::memcpy(value.version, "ALW1", 4);
    std::memcpy(value.ssid, ssid, std::strlen(ssid));
    if (password) std::memcpy(value.password, password, std::strlen(password));
    esp_err_t result = store.open();
    if (result != ESP_OK) return result;
    result = store.write(value);
    if (result == ESP_OK) result = store.commit();
    store.close();
    return result;
}
} }
