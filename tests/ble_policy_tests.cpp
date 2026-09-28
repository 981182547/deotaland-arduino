#include "agent_link_ble_policy.h"
#include "agent_link_buffer.h"
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* text, int line) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "line %d: %s\n", line, text); std::exit(1); }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void ControlReservation() {
    uint8_t telemetry[17] = {1,3,0x31,0,11};
    uint8_t ack[10] = {1,2,1,0,4};
    uint8_t pause[7] = {1,3,0x20,0,1};
    uint8_t ota[17] = {1,3,0x38,0,11};
    using namespace agentlink::blepolicy;
    CHECK(!CriticalControl(nullptr, 6));
    CHECK(!CriticalControl(ack, 5));
    CHECK(ControlQueueLimit(16, telemetry, sizeof(telemetry)) == 14);
    CHECK(ControlQueueLimit(16, ack, sizeof(ack)) == 16);
    CHECK(ControlQueueLimit(16, pause, sizeof(pause)) == 16);
    CHECK(ControlQueueLimit(16, ota, sizeof(ota)) == 14);
    ota[6] = 2;
    CHECK(ControlQueueLimit(16, ota, sizeof(ota)) == 16);
    ota[6] = 3;
    CHECK(ControlQueueLimit(16, ota, sizeof(ota)) == 16);
    for (uint8_t command=0x52; command<=0x55; ++command) {
        telemetry[2] = command;
        CHECK(CriticalControl(telemetry, sizeof(telemetry)));
    }
    telemetry[2] = 0x31;
    agentlink::FixedQueue<unsigned,16> fifo;
    for (unsigned i=0;i<14;++i) {
        CHECK(fifo.size() < ControlQueueLimit(16,telemetry,sizeof(telemetry)));
        CHECK(fifo.push(i));
    }
    CHECK(fifo.size() == ControlQueueLimit(16,telemetry,sizeof(telemetry)));
    CHECK(fifo.size() < ControlQueueLimit(16,ack,sizeof(ack))); CHECK(fifo.push(14));
    CHECK(fifo.size() < ControlQueueLimit(16,pause,sizeof(pause))); CHECK(fifo.push(15));
    CHECK(!fifo.push(16));
    for (unsigned i=0;i<16;++i) { CHECK(fifo.front() == i); fifo.pop(); }
    CHECK(fifo.empty());
}

void BatteryCoalescing() {
    using agentlink::blepolicy::UpdateBatteryLevel;
    std::atomic<uint8_t> level{0};
    std::atomic<bool> pending{false};
    unsigned wakes = 0;
    CHECK(!UpdateBatteryLevel(level,pending,0)); CHECK(!pending.load());
    if (UpdateBatteryLevel(level,pending,80)) ++wakes;
    CHECK(wakes==1 && level.load()==80 && pending.load());
    CHECK(pending.exchange(false)); // Worker accepted the changed value.
    for (unsigned i=0;i<10000;++i)
        if (UpdateBatteryLevel(level,pending,80)) ++wakes;
    CHECK(wakes==1 && !pending.load()); // Steady reportBattery must stay idle.

    pending=true; // GAP subscription/re-subscription forces current-value sync.
    CHECK(!UpdateBatteryLevel(level,pending,80));
    CHECK(pending.exchange(false) && level.load()==80);
    pending=true; // Failed Notify retains the sample for the worker's retry.
    CHECK(!UpdateBatteryLevel(level,pending,80));
    CHECK(pending.load());
    CHECK(UpdateBatteryLevel(level,pending,79)); // Retry sends the newest sample.
    CHECK(pending.exchange(false) && level.load()==79);
    CHECK(!pending.exchange(false));
    CHECK(UpdateBatteryLevel(level,pending,255)); CHECK(level.load()==100);
    CHECK(pending.exchange(false));
    CHECK(!UpdateBatteryLevel(level,pending,200) && !pending.load());
}

void VoicePacketization() {
    using namespace agentlink::blepolicy;
    for (size_t mtu=0;mtu<=1024;++mtu) {
        const size_t payload=VoicePayload(mtu);
        CHECK((payload&1u)==0); CHECK(payload<=204);
        if (payload) CHECK(payload+18<=mtu);
    }
    CHECK(VoicePayload(23)==4); CHECK(VoicePayload(247)==204);
    CHECK(VoiceDrainBytes(512,204,512)==408);
    CHECK(VoiceDrainBytes(104,204,512)==104);
    CHECK(VoiceDrainBytes(0,204,512)==0);
    CHECK(VoiceDrainBytes(200,0,512)==0);
    const size_t mtus[] = {23,64,185,247,517};
    for (size_t mtu : mtus) {
        agentlink::ByteRing ring;
        CHECK(ring.reserve(16384));
        size_t produced=0, consumed=0, packets=0;
        const size_t total=32000, payload=VoicePayload(mtu);
        uint8_t bytes[512];
        while (consumed<total) {
            while (produced<total && ring.available()>=sizeof(bytes)) {
                const size_t count = total-produced<sizeof(bytes) ? total-produced : sizeof(bytes);
                for(size_t i=0;i<count;++i) bytes[i]=static_cast<uint8_t>((produced+i)%251);
                CHECK(ring.push(bytes,count)); produced+=count;
            }
            const size_t batch=VoiceDrainBytes(ring.size(),payload,sizeof(bytes));
            CHECK(batch>0); CHECK(batch<=sizeof(bytes)); CHECK((batch&1u)==0);
            CHECK(ring.pop(bytes,batch)==batch);
            for (size_t i=0;i<batch;++i) CHECK(bytes[i]==static_cast<uint8_t>((consumed+i)%251));
            packets+=(batch+payload-1)/payload;
            consumed+=batch;
        }
        CHECK(ring.empty()); CHECK(produced==consumed);
        CHECK(packets==(total+payload-1)/payload);
        if (mtu==247) CHECK(packets==157); // Buffered 32 kB: formerly 188 notifications.
    }
}

struct Progress {
    std::atomic<bool> active{false}, end_req{false};
    std::atomic<uint32_t> accepted_bytes{0}, sent_bytes{0}, rejected_writes{0};
    std::atomic<size_t> queued_bytes{0};
    std::atomic<int> last_error{ESP_OK};
};
void ProgressSnapshots() {
    using agentlink::blepolicy::StreamStatus;
    Progress progress; agent_stream_status_t status;
    StreamStatus(progress,1024,false,0,&status);
    CHECK(status.state==AGENT_STREAM_UNAVAILABLE); CHECK(status.writable_bytes==0);
    StreamStatus(progress,1024,true,0,&status); CHECK(status.state==AGENT_STREAM_IDLE);
    progress.active=true; progress.accepted_bytes=600; progress.sent_bytes=100;
    progress.queued_bytes=400; progress.rejected_writes=2;
    StreamStatus(progress,1024,true,1,&status);
    CHECK(status.state==AGENT_STREAM_OPEN); CHECK(status.queued_bytes==500);
    CHECK(status.writable_bytes==624); CHECK(status.rejected_writes==2);
    progress.end_req=true;
    StreamStatus(progress,1024,true,1,&status);
    CHECK(status.state==AGENT_STREAM_DRAINING); CHECK(status.writable_bytes==0);
    progress.active=false; progress.sent_bytes=600; progress.queued_bytes=0;
    StreamStatus(progress,1024,true,1,&status);
    CHECK(status.state==AGENT_STREAM_COMPLETED); CHECK(status.queued_bytes==0);
    progress.last_error=ESP_ERR_TIMEOUT;
    StreamStatus(progress,1024,false,1,&status);
    CHECK(status.state==AGENT_STREAM_FAILED); CHECK(status.last_error==ESP_ERR_TIMEOUT);
    CHECK(status.writable_bytes==0); CHECK(status.queued_bytes==0);
}
}
int main() {
    ControlReservation(); BatteryCoalescing(); VoicePacketization(); ProgressSnapshots();
    std::printf("PASS BLE queue/packetization/progress: %u assertions\n", checks);
}
