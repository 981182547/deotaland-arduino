#include "ota_service.h"
#include <cstdio>
#include <cstdlib>
int main() {
    agentlink::ota::Configure(true,true);
    if (agent_link_ota_supported() || agent_link_ota_prepare()!=ESP_ERR_NOT_SUPPORTED) return 1;
    uint8_t sha[32]={}; uint16_t error=0;
    if (agentlink::ota::Start(100,sha,"2.0.0",nullptr,&error)) return 2;
    if (error!=agentlink::ota::kErrBusinessFailed || agent_link_ota_is_busy() || agentlink::ota::WorkerActive()) return 3;
    if (agentlink::ota::FeedData(sha,sizeof(sha))!=0 || agentlink::ota::OwnsDataChannel()) return 4;
    agentlink::ota::Poll(); agent_link_ota_abort();
    std::puts("PASS OTA unsupported-platform behavior");
}
