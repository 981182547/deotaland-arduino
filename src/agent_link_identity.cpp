#include "agent_link_identity.h"
#include "device_identity.h"
bool agent_link_default_identity(uint8_t out[16]) {
    return dev_identity_load(false) == ESP_OK && dev_identity_uuid(out);
}
