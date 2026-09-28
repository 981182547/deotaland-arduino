#include "audio_downlink.h"
#include "ota_service.h"
#include "wifi_text.h"
#include "adpcm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <stdint.h>

namespace {
uint32_t clock_ms = 0;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)
std::vector<uint8_t> pcm;
std::vector<std::vector<uint8_t>> flow;
bool cancel_on_pcm = false;
void Pcm(const uint8_t* data, size_t bytes) {
    CHECK(bytes % 2 == 0);
    CHECK(reinterpret_cast<uintptr_t>(data) % alignof(int16_t) == 0);
    pcm.insert(pcm.end(), data, data + bytes);
    if (cancel_on_pcm) { cancel_on_pcm = false; agentlink::audio::Disarm(); }
}
void Event(uint8_t type, const uint8_t* payload, size_t bytes) {
    CHECK(type == 0x20);
    CHECK(bytes == 4 && payload[3] == 0);
    flow.emplace_back(payload, payload + bytes);
}
void Reset() {
    using namespace agentlink::audio;
    OnLinkState(false);
    SetSinks(Pcm, Event);
    SetCodec(kCodecPcm16);
    SetBufferMs(400);
    clock_ms = 0; pcm.clear(); flow.clear(); cancel_on_pcm = false;
    Arm();
}
void TestPcmFragments() {
    using namespace agentlink::audio;
    uint8_t storage[34];
    for (size_t i = 0; i < sizeof(storage); ++i) storage[i] = static_cast<uint8_t>(i);
    for (size_t split = 1; split < 32; ++split) {
        Reset();
        Feed(storage + 1, split);
        Feed(storage + 1 + split, 32 - split);
        CHECK(pcm == std::vector<uint8_t>(storage + 1, storage + 33));
    }
    Reset();
    for (size_t i = 0; i < 31; ++i) Feed(storage + i, 1);
    CHECK(pcm.size() == 30);
    Feed(storage + 31, 1);
    CHECK(pcm == std::vector<uint8_t>(storage, storage + 32));
    Reset(); Feed(storage, 1); Disarm(); Arm(); Feed(storage + 2, 2);
    CHECK(pcm == std::vector<uint8_t>(storage + 2, storage + 4));
    Reset(); Feed(storage, 1); OnLinkState(false); Arm(); Feed(storage + 4, 2);
    CHECK(pcm == std::vector<uint8_t>(storage + 4, storage + 6));
    Reset(); Feed(storage, 1); Disarm(); SetCodec(kCodecImaAdpcm); SetCodec(kCodecPcm16); Arm(); Feed(storage + 6, 2);
    CHECK(pcm == std::vector<uint8_t>(storage + 6, storage + 8));
}
void TestFlowAndReentrancy() {
    using namespace agentlink::audio;
    Reset();
    const uint8_t sample[2] = {1, 2};
    for (unsigned i = 0; i < 4000; ++i) Feed(sample, 2);
    CHECK(flow.size() == 1);
    CHECK(flow.back()[0] == 1 && flow.back()[1] == 250 && flow.back()[2] == 0);
    clock_ms = 200; Tick();
    CHECK(flow.back()[0] == 0);
    Reset();
    std::vector<uint8_t> burst(32000, 0);
    Feed(burst.data(), burst.size());
    CHECK(flow.size() == 1 && flow[0][0] == 1);
    clock_ms = 199; Tick(); CHECK(flow.size() == 1);
    clock_ms = 200; Tick(); CHECK(flow.size() == 2 && flow[1][0] == 1);
    OnLinkState(false); flow.clear(); clock_ms += 3000; Tick(); CHECK(flow.empty());
    Reset(); cancel_on_pcm = true;
    Feed(burst.data(), 1024);
    CHECK(pcm.size() == 512 && !IsArmed());
}
void TestAdpcm() {
    using namespace agentlink;
    Reset(); audio::Disarm(); audio::SetCodec(audio::kCodecImaAdpcm); audio::Arm();
    std::vector<uint8_t> block(adpcm::kBlockBytes, 0);
    audio::Feed(block.data(), 127); CHECK(pcm.empty());
    audio::Feed(block.data() + 127, block.size() - 127);
    CHECK(pcm.size() == adpcm::kPcmBytesPerBlock);
    for (uint8_t value : pcm) CHECK(value == 0);
}
bool flow_blocked=false;
esp_err_t ReliableFlow(uint8_t id,const uint8_t* bytes,size_t n) {
    if(flow_blocked) return ESP_ERR_NO_MEM;
    Event(id,bytes,n); return ESP_OK;
}
void TestReliableFlow() {
    using namespace agentlink::audio;
    Reset(); SetReliableEventSink(ReliableFlow); flow_blocked=true;
    std::vector<uint8_t> burst(8000,0);
    Feed(burst.data(),burst.size()); CHECK(flow.empty());
    // A failed pause is retried on the next tick, before the 200 ms repeat.
    flow_blocked=false; Tick(); CHECK(flow.size()==1 && flow.back()[0]==1);
    flow_blocked=true; clock_ms=200; Tick(); CHECK(flow.size()==1);
    flow_blocked=false; Tick(); CHECK(flow.size()==2 && flow.back()[0]==0);
    Reset(); SetReliableEventSink(ReliableFlow); flow_blocked=true;
    Feed(burst.data(),burst.size()); clock_ms=300; Tick();
    flow_blocked=false; Tick(); CHECK(flow.size()==1 && flow.back()[0]==0);
    // Cancellation must not send the previous peer's resume or retain retries.
    Reset(); SetReliableEventSink(ReliableFlow); flow_blocked=false;
    Feed(burst.data(),burst.size()); CHECK(flow.back()[0]==1);
    const size_t before=flow.size(); OnLinkState(false); Tick(); CHECK(flow.size()==before);
    Reset(); SetReliableEventSink(ReliableFlow); flow_blocked=true;
    Feed(burst.data(),burst.size()); OnLinkState(false); flow_blocked=false; Tick(); CHECK(flow.empty());
    SetSinks(nullptr,nullptr);
}
void TestOtaWire() {
    const agent_ota_state_t states[] = {AGENT_OTA_IDLE, AGENT_OTA_RECEIVING, AGENT_OTA_VERIFYING, AGENT_OTA_SUCCESS, AGENT_OTA_FAILED};
    const uint8_t phases[] = {0, 0, 1, 2, 3};
    for (unsigned i = 0; i < 5; ++i) {
        agent_ota_status_t status = {states[i], 0x89abcdefu, 0xfedcba98u, 0x2037};
        uint8_t payload[11] = {};
        agentlink::ota::EncodeProgress(status, payload);
        const uint8_t expected[] = {phases[i], 0xef,0xcd,0xab,0x89, 0x98,0xba,0xdc,0xfe, 0x37,0x20};
        CHECK(std::memcmp(payload, expected, sizeof(payload)) == 0);
    }
}
void TestPortalText() {
    using namespace agentlink::wifi_detail;
    char output[33] = {};
    CHECK(DecodeFormValue(output, sizeof(output), "Room%20%22A%22%5C+WiFi"));
    CHECK(std::string(output) == "Room \"A\"\\ WiFi");
    CHECK(!DecodeFormValue(output, sizeof(output), "bad%00tail"));
    CHECK(!DecodeFormValue(output, sizeof(output), "bad%G0"));
    CHECK(!DecodeFormValue(output, sizeof(output), "bad%"));
    CHECK(DecodeFormValue(output, sizeof(output), "12345678901234567890123456789012"));
    CHECK(!DecodeFormValue(output, sizeof(output), "123456789012345678901234567890123"));
    char password[65] = {};
    std::string full_password(64, 'a');
    CHECK(DecodeFormValue(password, sizeof(password), full_password.c_str()));
    CHECK(std::strlen(password) == 64);
    std::string escaped;
    AppendJsonEscaped(escaped, "A\"\\\n\x01");
    CHECK(escaped == "A\\\"\\\\\\n\\u0001");
}
}
extern "C" uint32_t agent_link_now_ms(void) { return clock_ms; }
int main() {
    TestPcmFragments(); TestFlowAndReentrancy(); TestAdpcm(); TestReliableFlow(); TestOtaWire(); TestPortalText();
    std::printf("PASS %u service checks\n", checks);
}
