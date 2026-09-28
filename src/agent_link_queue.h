#pragma once
#include "agent_link.h"
#ifndef AGENT_LINK_RX_SLOTS
#define AGENT_LINK_RX_SLOTS 16
#endif
#ifndef AGENT_LINK_RX_BYTES
#define AGENT_LINK_RX_BYTES 1024
#endif
#ifndef AGENT_LINK_RX_CONTROL_RESERVE
#define AGENT_LINK_RX_CONTROL_RESERVE ((AGENT_LINK_RX_SLOTS > 4) ? 2 : 1)
#endif
namespace agentlink {
enum class RxKind { Control, Stream, Connection, Ready };
struct RxEvent {
    RxKind kind;
    agent_stream_t stream;
    size_t length;
    uint32_t generation;
    uint8_t data[AGENT_LINK_RX_BYTES];
};
// Fixed storage; ESP32 producers can be the BLE host, consumer is loop().
void RxEnable(bool enabled);
esp_err_t RxPush(RxKind kind, agent_stream_t stream, const uint8_t* data, size_t length);
bool RxPop(RxEvent& out);
bool RxHasSpace();
uint32_t RxGeneration();
void RxStats(agent_link_stats_t* out);
void StatusReset(agent_transport_kind_t transport);
void StatusPush(const agent_link_status_t* status);
void StatusGet(agent_link_status_t* out);
bool StatusTake(agent_link_status_t* out);
}
