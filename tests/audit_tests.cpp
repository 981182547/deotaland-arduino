// Production core with deterministic clock and bounded, allocation-free transport.
#include "Deotaland.h"
#include "agent_link_buffer.h"
#include "agent_link_queue.h"
#include "protocol.h"
#include "audio_downlink.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <limits>

static size_t allocations=0;
static bool fail_allocation=false;
void* operator new(size_t n) { ++allocations; if(fail_allocation) throw std::bad_alloc(); if (void* p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L || defined(_MSC_VER)
void operator delete(void* p,size_t) noexcept { std::free(p); }
void operator delete[](void* p,size_t) noexcept { std::free(p); }
#endif
static uint32_t clock_us=0;
extern "C" uint32_t agent_link_now_ms() { return clock_us/1000; }
extern "C" uint32_t agent_link_now_us() { return clock_us; }
extern "C" const char* esp_err_to_name(esp_err_t) { return "test-error"; }
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(0)

struct Link {
    agent_transport_t tx={};
    agent_transport_callbacks_t cb={};
    uint8_t last[1030]={}, metadata[32]={};
    size_t last_size=0, metadata_size=0;
    uint32_t command_cost=0, audio_cost=0, transport_cost=0;
    unsigned commands=0, sends=0, ends=0, audio_calls=0, manifests=0;
    size_t audio_bytes=0;
    uint8_t audio[4096]={};
    bool blocked=false, online=false, recording=false;
    Link() {
        tx.impl=this;
        tx.bind=[](void* p,const agent_transport_callbacks_t* c) { static_cast<Link*>(p)->cb=c?*c:agent_transport_callbacks_t{}; };
        tx.start=[](void* p)->esp_err_t { auto& l=*static_cast<Link*>(p); l.online=true; l.cb.on_connection(true); l.cb.on_ready(); return ESP_OK; };
        tx.stop=[](void* p) { static_cast<Link*>(p)->online=false; };
        tx.is_ready=[](void* p) { return static_cast<Link*>(p)->online; };
        tx.ctrl_mtu=[](void*)->size_t { return 512; };
        tx.poll=[](void* p) { clock_us+=static_cast<Link*>(p)->transport_cost; };
        tx.send_ctrl=[](void* p,const uint8_t* b,size_t n)->esp_err_t {
            auto& l=*static_cast<Link*>(p);
            if(l.blocked) return ESP_ERR_NO_MEM;
            CHECK(n<=sizeof(l.last)); std::memcpy(l.last,b,n); l.last_size=n;
            ++l.sends; if(b[1]==3 && b[2]==0x18) ++l.manifests; return ESP_OK;
        };
        tx.stream_start=[](void* p,agent_stream_t,const uint8_t* b,size_t n)->esp_err_t {
            auto& l=*static_cast<Link*>(p); l.recording=true;
            l.metadata_size=n; if(n) std::memcpy(l.metadata,b,n); return ESP_OK;
        };
        tx.send_stream=[](void*,agent_stream_t,const uint8_t*,size_t)->esp_err_t { return ESP_OK; };
        tx.stream_end=[](void* p,agent_stream_t,bool,const uint8_t*,size_t)->esp_err_t {
            auto& l=*static_cast<Link*>(p); ++l.ends; l.recording=false; return ESP_OK;
        };
    }
    void begin() {
        Deotaland.reset();
        static const uint8_t id[16]={1,2,3};
        agent_output_cb_t out={}; out.ctx=this;
        out.on_command=[](uint16_t,const uint8_t*,size_t,uint8_t*,size_t,size_t* n,void* p) {
            auto& l=*static_cast<Link*>(p); ++l.commands; clock_us+=l.command_cost; *n=0; return true;
        };
        out.on_audio_out=[](const uint8_t* b,size_t n,void* p) {
            auto& l=*static_cast<Link*>(p); ++l.audio_calls; clock_us+=l.audio_cost;
            CHECK(l.audio_bytes+n<=sizeof(l.audio)); std::memcpy(l.audio+l.audio_bytes,b,n); l.audio_bytes+=n;
        };
        agent_link_config_t c={}; c.device_name="audit"; c.transport=AGENT_TRANSPORT_CUSTOM;
        c.custom_transport=&tx; c.device_id=id; c.output=&out;
        CHECK(Deotaland.begin(c)==ESP_OK); drain(); CHECK(Deotaland.isReady());
    }
    void drain() { for(unsigned i=0;i<300;++i) Deotaland.poll(4,0); }
    void command(uint8_t id,const uint8_t* p=nullptr,size_t n=0) {
        uint8_t b[1030]; size_t size=agentlink::EncodeEvent(b,sizeof(b),id,p,n); b[1]=1;
        CHECK(size && cb.on_ctrl(b,size)==ESP_OK);
    }
    ~Link() { Deotaland.reset(); }
};
void buffers() {
    agentlink::ByteRing ring;
    fail_allocation=true; CHECK(!ring.reserve(37)); fail_allocation=false;
    CHECK(!ring.reserve(0)); CHECK(ring.reserve(37)); CHECK(ring.reserve(37)); CHECK(!ring.reserve(38));
    uint8_t model[37]={}, expected[37]={}, out[37]={}; size_t count=0; uint32_t random=17;
    for(unsigned step=0;step<20000;++step) {
        random=random*1664525u+1013904223u;
        const size_t n=(random>>8)%37+1;
        if(random&1) {
            for(size_t i=0;i<n;++i) expected[i]=static_cast<uint8_t>(step+i);
            const bool room=n<=37-count;
            CHECK(ring.push(expected,n)==room);
            if(room) { std::memcpy(model+count,expected,n); count+=n; }
        } else {
            const size_t take=count<n?count:n;
            CHECK(ring.pop(out,n)==take); CHECK(std::memcmp(out,model,take)==0);
            std::memmove(model,model+take,count-take); count-=take;
        }
        CHECK(ring.size()==count && ring.available()==37-count);
    }
    ring.clear(); CHECK(ring.size()==0 && ring.available()==37);
    ring.release(); CHECK(!ring.push(expected,1)); CHECK(ring.reserve(1)); CHECK(ring.push(expected,1));
    agentlink::FixedQueue<unsigned,3> queue;
    for(unsigned i=0;i<100;++i) {
        CHECK(queue.push(i) && queue.push(i+1) && queue.push(i+2)); CHECK(!queue.push(999));
        CHECK(queue.front()==i); queue.pop(); CHECK(queue.front()==i+1); queue.pop();
        CHECK(queue.front()==i+2); queue.pop(); CHECK(queue.empty());
    }
    agentlink::FixedText<3> text; text+="abc"; CHECK(text.good() && text.size()==3);
    text+="def"; CHECK(!text.good() && text.size()==3); text.clear(); text+='x'; CHECK(text.good());
}
void protocol_buffers() {
    uint8_t b[64]={}, body[32]; for(unsigned i=0;i<32;++i) body[i]=static_cast<uint8_t>(i);
    for(size_t n=0;n<=32;++n) {
        auto golden=agentlink::BuildResponse(0x70,91,1,1004,body,n);
        CHECK(agentlink::EncodeResponse(b,sizeof(b),0x70,91,1,1004,body,n)==golden.size());
        CHECK(std::memcmp(b,golden.data(),golden.size())==0);
        agentlink::FrameView view; CHECK(agentlink::ParseFrameView(b,golden.size(),view));
        CHECK(view.payload.data()==b+6 && view.payload.size()==n+4);
        CHECK(!agentlink::ParseFrameView(b,golden.size()-1,view));
        CHECK(agentlink::EncodeEvent(b,5,1,body,n)==0);
    }
    CHECK(agentlink::EncodeEvent(b,sizeof(b),1,body,std::numeric_limits<size_t>::max())==0);
    CHECK(agentlink::EncodeResponse(b,sizeof(b),1,0,0,0,nullptr,1)==0);
}
void time_budget_and_resume() {
    Link l; l.begin(); l.command_cost=1100;
    l.command(0x70); l.command(0x71); l.command(0x72);
    CHECK(Deotaland.poll(4,1000)==1 && l.commands==1);
    auto stats=Deotaland.stats(); CHECK(stats.poll_last_us==1100 && stats.poll_budget_exhaustions==1);
    CHECK(Deotaland.poll(4,1000)==1 && l.commands==2);
    CHECK(Deotaland.poll(4,1000)==1 && l.commands==3);
    l.command_cost=0; l.drain();
    uint8_t pcm[1024]; for(unsigned i=0;i<1024;++i) pcm[i]=static_cast<uint8_t>(i);
    l.audio_cost=1100;
    CHECK(l.cb.on_stream(AGENT_STREAM_VOICE,pcm,sizeof(pcm))==ESP_OK);
    CHECK(Deotaland.poll(4,1000)==1 && l.audio_calls==1 && l.audio_bytes==512);
    CHECK(Deotaland.poll(4,1000)==1 && l.audio_calls==2 && l.audio_bytes==1024);
    CHECK(std::memcmp(l.audio,pcm,sizeof(pcm))==0);
    // An old partial frame must not resume after connection generation changes.
    l.audio_bytes=0; CHECK(l.cb.on_stream(AGENT_STREAM_VOICE,pcm,sizeof(pcm))==ESP_OK);
    Deotaland.poll(4,1000); l.cb.on_connection(false); l.cb.on_connection(true); l.cb.on_ready();
    l.drain(); CHECK(l.audio_bytes==512);
    // Wrapping microsecond counters use unsigned subtraction.
    clock_us=UINT32_MAX-100; l.command_cost=1100; l.command(0x70);
    CHECK(Deotaland.poll(4,1000)==1 && Deotaland.stats().poll_last_us==1100);
    l.command_cost=0; l.transport_cost=1500; l.command(0x71);
    const unsigned before=l.commands;
    CHECK(Deotaland.poll(4,1000)==0 && l.commands==before);
    l.transport_cost=0; l.drain(); CHECK(l.commands==before+1);
}
void stale_stream_and_image() {
    Link l; l.begin(); agent_stream_handle_t old=nullptr,fresh=nullptr;
    const uint8_t data[2]={1,2};
    CHECK(Deotaland.openStream(AGENT_STREAM_AUDIO,nullptr,&old)==ESP_OK);
    CHECK(Deotaland.closeStream(old)==ESP_OK);
    CHECK(Deotaland.openStream(AGENT_STREAM_AUDIO,nullptr,&fresh)==ESP_OK && old!=fresh);
    CHECK(Deotaland.writeStream(old,data,2)==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.closeStream(old)==ESP_ERR_INVALID_ARG && l.recording);
    l.cb.on_connection(false); l.cb.on_connection(true); l.cb.on_ready(); l.drain();
    old=fresh; CHECK(Deotaland.openStream(AGENT_STREAM_AUDIO,nullptr,&fresh)==ESP_OK && fresh!=old);
    CHECK(Deotaland.closeStream(old)==ESP_ERR_INVALID_ARG && l.recording);
    agent_stream_opts_t image={}; image.width=2; image.height=3;
    CHECK(Deotaland.openStream(AGENT_STREAM_IMAGE,&image,&fresh)==ESP_OK);
    CHECK(l.metadata_size==9 && l.metadata[5]==12 && l.metadata[6]==0);
    CHECK(Deotaland.closeStream(fresh)==ESP_OK);
    image.encoding=AGENT_ENC_JPEG;
    CHECK(Deotaland.openStream(AGENT_STREAM_IMAGE,&image,&fresh)==ESP_ERR_INVALID_ARG);
    image.encoding=AGENT_ENC_RAW; image.width=65535; image.height=65535;
    CHECK(Deotaland.openStream(AGENT_STREAM_IMAGE,&image,&fresh)==ESP_ERR_INVALID_SIZE);
    const uint8_t bad[]={1}; l.command(0x01,bad,sizeof(bad)); l.drain();
    CHECK(l.last_size==10 && l.last[7]==1 && l.last[8]==uint8_t(1004 & 255));
}
void allocation_and_manifest() {
    Link l; l.begin();
    agent_link_io_desc_t io={}; io.id="s"; io.kind="sensor"; io.value=AGENT_VAL_U16; io.dir=AGENT_IO_IN;
    const unsigned old_manifest=l.manifests;
    CHECK(Deotaland.registerIo(io)==ESP_OK); l.drain(); CHECK(l.manifests>old_manifest);
    const size_t before=allocations;
    for(unsigned i=0;i<10000;++i) {
        const uint16_t value=static_cast<uint16_t>(i);
        CHECK(Deotaland.pushReading("s",&value,sizeof(value))==ESP_OK);
        CHECK(Deotaland.updateUInt16("s",value)==ESP_OK);
        CHECK(Deotaland.pushEvent(AGENT_EVT_BUTTON,nullptr,0)==ESP_OK);
        CHECK(Deotaland.reportBattery(static_cast<uint8_t>(i%101),false)==ESP_OK);
        l.command(0x70); Deotaland.poll(4,0);
        if(i%100==0) { CHECK(Deotaland.notifyManifestChanged()==ESP_OK); l.drain(); }
    }
    CHECK(allocations==before);
    // Per-peer OFF policy must not silently suppress a newly connected peer.
    const uint8_t off[]={1,'s',1}; l.command(0x36,off,sizeof(off)); l.drain();
    uint16_t value=2; unsigned sent=l.sends;
    CHECK(Deotaland.pushReading("s",&value,2)==ESP_OK && l.sends==sent);
    l.cb.on_connection(false); l.cb.on_connection(true); l.cb.on_ready(); l.drain(); sent=l.sends;
    CHECK(Deotaland.pushReading("s",&value,2)==ESP_OK && l.sends==sent+1);
    // Refuse to publish oversized/truncated manifest JSON, without allocation.
    static char large[AGENT_LINK_MANIFEST_BYTES+20]; std::memset(large,'x',sizeof(large)-1);
    io.desc=large; const unsigned m=l.manifests;
    CHECK(Deotaland.notifyManifestChanged()==ESP_OK); l.drain();
    CHECK(l.manifests==m && Deotaland.stats().last_error==ESP_ERR_INVALID_SIZE);
    io.desc=nullptr; CHECK(Deotaland.notifyManifestChanged()==ESP_OK); l.drain(); CHECK(l.manifests>m);
    // Borrowed descriptors remain alive until reset.
    Deotaland.reset();
}
int main() {
    buffers(); protocol_buffers(); time_budget_and_resume(); stale_stream_and_image(); allocation_and_manifest();
    std::printf("PASS: %u audit checks; 10,000 hot-path iterations without heap allocation\n",checks);
}
