#include "audio_downlink.h"
#include "adpcm.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
uint32_t clock_ms=0;
unsigned checks=0;
#define CHECK(v) do { ++checks; if (!(v)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#v); std::exit(1); } } while (0)
struct Sink {
    std::vector<uint8_t> received;
    size_t allowance=0;
    size_t queued=0;
    unsigned calls=0;
    unsigned cancel=0;
    bool invalid=false;
    bool odd=false;
    bool recurse=false;
} sink;
std::vector<std::vector<uint8_t>> events;
size_t Write(const uint8_t* bytes,size_t n,void* ctx) {
    CHECK(ctx==&sink);
    CHECK(n && !(n&1u));
    CHECK(reinterpret_cast<uintptr_t>(bytes)%alignof(int16_t)==0);
    ++sink.calls;
    if (sink.recurse) {
        CHECK(agentlink::audio::Feed(bytes,n)==0);
        agentlink::audio::Tick();
    }
    if (sink.cancel) {
        const unsigned cancel=sink.cancel;
        sink.cancel=0;
        if (cancel==1) agentlink::audio::OnLinkState(false);
        else if (cancel==2) agentlink::audio::Arm();
        else agentlink::audio::Disarm();
        return n;
    }
    if (sink.invalid) return sink.odd ? 1 : n+2;
    const size_t accepted=std::min(n,sink.allowance)&~static_cast<size_t>(1);
    sink.received.insert(sink.received.end(),bytes,bytes+accepted);
    sink.queued+=accepted;
    return accepted;
}
size_t Queued(void* ctx) { CHECK(ctx==&sink); return sink.queued; }
void Event(uint8_t id,const uint8_t* data,size_t n) {
    CHECK(id==0x20 && n==4);
    events.emplace_back(data,data+n);
}
void Reset(bool real_watermark=true) {
    using namespace agentlink::audio;
    OnLinkState(false);
    SetSinks(nullptr,Event);
    sink=Sink{}; events.clear(); clock_ms=0;
    SetPlaybackSink(Write,real_watermark ? Queued : nullptr,&sink);
    SetBufferMs(400); Arm();
}
agent_playback_stats_t Stats() {
    agent_playback_stats_t result={};
    agentlink::audio::GetStats(&result);
    return result;
}
std::vector<uint8_t> Sequence(size_t n) {
    std::vector<uint8_t> data(n);
    for (size_t i=0;i<n;++i) data[i]=static_cast<uint8_t>((i*47u+i/251u)&255u);
    return data;
}
void TestBoundedRetry() {
    using namespace agentlink::audio;
    Reset(); const auto data=Sequence(1536);
    CHECK(Feed(data.data(),data.size())==512);
    CHECK(sink.calls==1 && PendingBytes()==512);
    CHECK(events.size()==1 && events.back()[0]==1);
    CHECK(Feed(data.data()+512,1024)==0 && sink.calls==1);
    for (unsigned i=0;i<10;++i) { Tick(); CHECK(sink.calls==i+2); CHECK(PendingBytes()==512); }
    sink.allowance=6; Tick();
    CHECK(PendingBytes()==506 && sink.received.size()==6);
    CHECK(Feed(data.data()+512,1024)==0 && sink.calls==12);
    sink.allowance=SIZE_MAX; Tick();
    CHECK(PendingBytes()==0);
    CHECK(Feed(data.data()+512,1024)==1024);
    CHECK(sink.received==data);
    const auto stats=Stats();
    CHECK(stats.accepted_pcm_bytes==data.size());
    CHECK(stats.backpressure_events==12 && stats.max_pending_pcm_bytes==512);
}
void TestRandomPcmFragments() {
    using namespace agentlink::audio;
    const auto data=Sequence(40000);
    for (unsigned seed=1;seed<=12;++seed) {
        Reset(false); uint32_t random=seed; size_t input=0;
        unsigned iterations=0;
        while (input<data.size() || PendingBytes()) {
            CHECK(++iterations<100000);
            random=random*1664525u+1013904223u;
            sink.allowance=(random>>16)%258u;
            Tick();
            const size_t offered=std::min(data.size()-input,static_cast<size_t>((random>>3)%1100u+1u));
            const size_t consumed=Feed(data.data()+input,offered);
            CHECK(consumed<=offered);
            input+=consumed;
            ++clock_ms;
        }
        Finish(); CHECK(IsDrained() && !IsArmed()); CHECK(sink.received==data);
        CHECK(Stats().accepted_pcm_bytes==data.size());
    }
}
void TestAdpcmBackpressure() {
    using namespace agentlink;
    Reset(); audio::Disarm(); CHECK(audio::SetCodec(audio::kCodecImaAdpcm)); audio::Arm();
    const auto encoded=Sequence(adpcm::kBlockBytes*20);
    std::vector<uint8_t> expected;
    for (size_t i=0;i<encoded.size();i+=adpcm::kBlockBytes) {
        int16_t decoded[adpcm::kSamplesPerBlock];
        adpcm::DecodeBlock(encoded.data()+i,decoded);
        const uint8_t* ptr=reinterpret_cast<const uint8_t*>(decoded);
        expected.insert(expected.end(),ptr,ptr+sizeof(decoded));
    }
    CHECK(audio::Feed(encoded.data(),encoded.size())==adpcm::kBlockBytes);
    CHECK(audio::PendingBytes()==adpcm::kPcmBytesPerBlock && sink.calls==1);
    CHECK(audio::Feed(encoded.data()+adpcm::kBlockBytes,100)==0);
    size_t consumed=adpcm::kBlockBytes; unsigned iteration=0;
    while (consumed<encoded.size() || audio::PendingBytes()) {
        CHECK(++iteration<10000);
        sink.allowance=iteration%5 ? 74 : 0;
        audio::Tick();
        const size_t n=std::min(encoded.size()-consumed,static_cast<size_t>((iteration*11)%299+1));
        consumed+=audio::Feed(encoded.data()+consumed,n);
    }
    CHECK(sink.received==expected);
    CHECK(Stats().max_pending_pcm_bytes==adpcm::kPcmBytesPerBlock);
}
void TestFinishAndTruncation() {
    using namespace agentlink::audio;
    Reset(); const auto data=Sequence(15);
    CHECK(Feed(data.data(),data.size())==15);
    CHECK(PendingBytes()==14 && !IsDrained());
    Finish(); CHECK(IsArmed() && !IsDrained());
    CHECK(Feed(data.data(),2)==0);
    CHECK(Stats().truncated_input_bytes==1);
    sink.allowance=4;
    for (unsigned i=0;i<3;++i) { Tick(); CHECK(IsArmed() && !IsDrained()); }
    Tick(); CHECK(!IsArmed() && IsDrained());
    CHECK(sink.received==std::vector<uint8_t>(data.begin(),data.begin()+14));
    CHECK(Feed(data.data(),2)==0);
    const auto stats=Stats(); Tick(); Finish(); CHECK(Stats().truncated_input_bytes==stats.truncated_input_bytes);
    Arm(); sink.allowance=SIZE_MAX; CHECK(Feed(data.data(),2)==2);
    Reset(); Disarm(); SetCodec(kCodecImaAdpcm); Arm();
    CHECK(Feed(data.data(),data.size())==15); Finish();
    CHECK(IsDrained() && !IsArmed() && Stats().truncated_input_bytes==15);
}
void TestRealWatermark() {
    using namespace agentlink::audio;
    Reset(); sink.allowance=SIZE_MAX; const auto data=Sequence(8000);
    CHECK(Feed(data.data(),data.size())==data.size());
    CHECK(events.size()==1 && events.back()[0]==1);
    CHECK((static_cast<unsigned>(events.back()[2])<<8 | events.back()[1])==250);
    clock_ms=10000; Tick(); CHECK(events.back()[0]==1); // A stopped DAC cannot drain by wall clock.
    sink.queued=2400; Tick(); CHECK(events.back()[0]==0);
    sink.queued=SIZE_MAX; Tick(); CHECK(events.back()[0]==1 && events.back()[1]==255 && events.back()[2]==255);
    sink.queued=0; Tick(); CHECK(events.back()[0]==0);
    // Even less than one millisecond of retained data must keep pause asserted.
    sink.allowance=0; CHECK(Feed(data.data(),2)==2); CHECK(events.back()[0]==1);
    clock_ms+=30000; Tick(); CHECK(events.back()[0]==1);
    sink.allowance=2; Tick(); CHECK(events.back()[0]==0);
}
void TestGenerationAndContracts() {
    using namespace agentlink::audio;
    const auto data=Sequence(1024);
    for (unsigned mode=1;mode<=3;++mode) {
        Reset(); CHECK(Feed(data.data(),data.size())==512);
        sink.cancel=mode; Tick();
        CHECK(PendingBytes()==0 && sink.received.empty());
        Arm(); sink.allowance=SIZE_MAX; CHECK(Feed(data.data()+512,512)==512);
        CHECK(sink.received==std::vector<uint8_t>(data.begin()+512,data.end()));
    }
    Reset(); CHECK(Feed(data.data(),1)==1); OnLinkState(false); Arm(); sink.allowance=SIZE_MAX;
    CHECK(Feed(data.data()+2,2)==2 && sink.received==std::vector<uint8_t>(data.begin()+2,data.begin()+4));
    Reset(); sink.invalid=true; CHECK(Feed(data.data(),4)==4);
    CHECK(PendingBytes()==4 && Stats().invalid_sink_returns==1 && Stats().accepted_pcm_bytes==0);
    sink.odd=true; Tick();
    CHECK(PendingBytes()==4 && Stats().invalid_sink_returns==2 && Stats().accepted_pcm_bytes==0);
    sink.invalid=false; sink.allowance=SIZE_MAX; Tick(); CHECK(PendingBytes()==0 && sink.received.size()==4);
    Reset(); sink.recurse=true; sink.allowance=SIZE_MAX;
    CHECK(Feed(data.data(),4)==4 && sink.calls==1 && sink.received.size()==4);
    SetPlaybackSink(nullptr,nullptr,nullptr); CHECK(Stats().sink_calls==0 && PendingBytes()==0);
    // Neither a partial encoded block nor fully decoded retained PCM may cross
    // reconnection, even when the new session negotiates the same codec.
    Reset(); Disarm(); SetCodec(kCodecImaAdpcm); Arm();
    CHECK(Feed(data.data(),100)==100); OnLinkState(false); SetCodec(kCodecImaAdpcm); Arm();
    sink.allowance=SIZE_MAX;
    CHECK(Feed(data.data()+256,256)==256);
    int16_t decoded[agentlink::adpcm::kSamplesPerBlock];
    agentlink::adpcm::DecodeBlock(data.data()+256,decoded);
    CHECK(sink.received.size()==sizeof(decoded) && std::memcmp(sink.received.data(),decoded,sizeof(decoded))==0);
    sink.received.clear(); sink.allowance=0;
    CHECK(Feed(data.data()+512,256)==256 && PendingBytes()==sizeof(decoded));
    OnLinkState(false); SetCodec(kCodecImaAdpcm); Arm(); sink.allowance=SIZE_MAX;
    CHECK(Feed(data.data()+768,256)==256);
    agentlink::adpcm::DecodeBlock(data.data()+768,decoded);
    CHECK(sink.received.size()==sizeof(decoded) && std::memcmp(sink.received.data(),decoded,sizeof(decoded))==0);
}
unsigned reliable_calls=0;
esp_err_t RejectFlow(uint8_t,const uint8_t*,size_t) { ++reliable_calls; return ESP_ERR_NO_MEM; }
void TestFinishFlowRetry() {
    using namespace agentlink::audio;
    Reset(); SetReliableEventSink(RejectFlow); reliable_calls=0;
    const uint8_t sample[]={1,2};
    CHECK(Feed(sample,sizeof(sample))==2 && reliable_calls==1);
    Finish(); CHECK(reliable_calls==1);
    sink.allowance=SIZE_MAX; Tick(); CHECK(reliable_calls==2 && IsDrained());
    Tick(); CHECK(reliable_calls==3);
    OnLinkState(false); Tick(); CHECK(reliable_calls==3);
}
}
extern "C" uint32_t agent_link_now_ms(void) { return clock_ms; }
int main() {
    TestBoundedRetry(); TestRandomPcmFragments(); TestAdpcmBackpressure();
    TestFinishAndTruncation(); TestRealWatermark(); TestGenerationAndContracts(); TestFinishFlowRetry();
    std::printf("PASS %u playback checks\n",checks);
}
