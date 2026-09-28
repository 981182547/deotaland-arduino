// Regression tests for the 0.5.0 fixes to the five original diagnostic probes.
// Production core + fake clock/transport: no claim about on-device execution time.
#include "Deotaland.h"
#include "protocol.h"
#include "agent_link_queue.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static uint32_t now_us = 0;
extern "C" uint32_t agent_link_now_ms() { return now_us / 1000; }
extern "C" uint32_t agent_link_now_us() { return now_us; }
extern "C" const char* esp_err_to_name(esp_err_t) { return "probe-error"; }
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct Peer {
    agent_transport_t tx = {};
    agent_transport_callbacks_t cb = {};
    bool online = false, blocked = false;
    unsigned manifests = 0, readings = 0, commands = 0, ends = 0;
    uint32_t command_cost_us = 0, transport_cost_us = 0;
    size_t audio_bytes = 0, last_size = 0;
    uint8_t last[1030] = {};
    uint8_t last_reading[1024] = {};
    char manifest[AGENT_LINK_MANIFEST_BYTES+1] = {};
    size_t manifest_size=0;
    Peer() {
        tx.impl = this;
        tx.bind = [](void* p, const agent_transport_callbacks_t* c) { static_cast<Peer*>(p)->cb = c ? *c : agent_transport_callbacks_t{}; };
        tx.start = [](void* p)->esp_err_t {
            auto& q = *static_cast<Peer*>(p); q.online = true;
            q.cb.on_connection(true); q.cb.on_ready(); return ESP_OK;
        };
        tx.stop = [](void* p) { static_cast<Peer*>(p)->online = false; };
        tx.is_ready = [](void* p) { return static_cast<Peer*>(p)->online; };
        tx.ctrl_mtu = [](void*)->size_t { return 512; };
        tx.poll=[](void* p) { now_us+=static_cast<Peer*>(p)->transport_cost_us; };
        tx.send_ctrl = [](void* p, const uint8_t* b, size_t n)->esp_err_t {
            auto& q = *static_cast<Peer*>(p);
            if (q.blocked) return ESP_ERR_NO_MEM;
            CHECK(n <= sizeof(q.last)); std::memcpy(q.last, b, n); q.last_size = n;
            if (b[1] == 3 && b[2] == 0x18) {
                ++q.manifests; if(!b[6]) q.manifest_size=0;
                CHECK(q.manifest_size+n-8<sizeof(q.manifest));
                std::memcpy(q.manifest+q.manifest_size,b+8,n-8); q.manifest_size+=n-8; q.manifest[q.manifest_size]=0;
            }
            if (b[1] == 3 && b[2] == 0x19) {
                ++q.readings; const size_t offset=8+b[6];
                CHECK(offset<=n); std::memcpy(q.last_reading,b+offset,n-offset);
            }
            return ESP_OK;
        };
    }
    esp_err_t beginResult(const char* model = "probe",const char* firmware=nullptr) {
        Deotaland.reset(); now_us = 0;
        static const uint8_t id[16] = {1};
        agent_output_cb_t out = {}; out.ctx = this;
        out.on_command = [](uint16_t, const uint8_t*, size_t, uint8_t*, size_t, size_t* n, void* p) {
            auto& q = *static_cast<Peer*>(p); ++q.commands; now_us += q.command_cost_us;
            *n = 0; return true;
        };
        out.on_audio_out = [](const uint8_t*, size_t n, void* p) { static_cast<Peer*>(p)->audio_bytes += n; };
        out.on_audio_end = [](void* p) { ++static_cast<Peer*>(p)->ends; };
        agent_link_config_t c = {}; c.device_name = "probe"; c.model = model; c.caps = AGENT_CAP_SPEAKER;
        c.device_id = id; c.output = &out; c.transport = AGENT_TRANSPORT_CUSTOM; c.custom_transport = &tx;
        c.firmware_rev=firmware;
        const esp_err_t result=Deotaland.begin(c);
        if(result==ESP_OK) { drain(); CHECK(Deotaland.isReady()); }
        return result;
    }
    void begin() { CHECK(beginResult()==ESP_OK); }
    void command(uint8_t id, const uint8_t* p = nullptr, size_t n = 0) {
        uint8_t b[1030]; const size_t bytes = agentlink::EncodeEvent(b, sizeof(b), id, p, n);
        CHECK(bytes != 0); b[1] = 1; CHECK(cb.on_ctrl(b, bytes) == ESP_OK);
    }
    void drain() { for (unsigned i = 0; i < 300; ++i) Deotaland.poll(4, 0); }
    uint16_t error() const { CHECK(last_size >= 10); return uint16_t(last[8]) | (uint16_t(last[9]) << 8); }
    ~Peer() { Deotaland.reset(); }
};

int main() {
    {
        Peer p; char model[101]; std::memset(model, 'm', 100); model[100] = 0;
        CHECK(p.beginResult(model)==ESP_ERR_INVALID_SIZE);
        char valid_model[40], firmware[32]; std::memset(valid_model,'m',39); valid_model[39]=0;
        std::memset(firmware,'v',31); firmware[31]=0;
        CHECK(p.beginResult(valid_model,firmware)==ESP_OK); p.command(0x01); p.drain();
        CHECK(p.last_size==109 && p.error()==0);
        std::puts("PASS: oversized identity rejected before startup; maximum valid model/version query succeeds");
    }
    {
        Peer p; p.begin();
        const uint8_t reply[] = {1, 0, 0, 0, 2}; p.command(0x05, reply, sizeof(reply)); p.drain();
        p.blocked = true;
        p.command(0x70); uint8_t pcm[512] = {};
        CHECK(p.cb.on_stream(AGENT_STREAM_VOICE, pcm, sizeof(pcm)) == ESP_OK);
        for (unsigned i = 0; i < 100; ++i) Deotaland.poll(4, 0);
        CHECK(p.commands == 1 && p.audio_bytes == sizeof(pcm));
        p.command(0x71); p.drain(); CHECK(p.commands==1); // NEXT command still waits for ACK.
        p.blocked = false; p.drain(); CHECK(p.audio_bytes == sizeof(pcm));
        CHECK(p.commands==2);
        std::puts("PASS: ACK congestion permits ordered media progress but fences the next command");
    }
    {
        Peer p; p.begin(); const unsigned before = p.manifests;
        CHECK(Deotaland.notifyManifestChanged() == ESP_OK); p.command_cost_us = 1000;
        for (unsigned i = 0; i < 100; ++i) { p.command(0x70); Deotaland.poll(4, 1000); }
        CHECK(p.commands == 100 && p.manifests > before);
        p.command_cost_us = 0; p.drain(); CHECK(p.manifests > before);
        p.transport_cost_us=1500; const unsigned commands=p.commands;
        p.command(0x71); for(unsigned i=0;i<8;++i) Deotaland.poll(4,1000);
        CHECK(p.commands==commands+1);
        p.transport_cost_us=0;
        std::puts("PASS: manifest and RX make progress under repeated budget-consuming callbacks/backends");
    }
    {
        Peer p; p.begin();
        agent_link_io_desc_t io = {}; io.id = "s"; io.kind = "sensor"; io.value = AGENT_VAL_STR; io.dir = AGENT_IO_IN;
        CHECK(Deotaland.registerIo(io) == ESP_OK); p.drain();
        const uint8_t onchange[] = {1, 's', 3}; p.command(0x36, onchange, sizeof(onchange)); p.drain();
        const char value[] = "01234567890123456";
        CHECK(Deotaland.pushReading("s", value, 17) == ESP_OK);
        CHECK(Deotaland.pushReading("s", value, 17) == ESP_OK);
        CHECK(p.readings == 1);
        const uint8_t query[] = {1, 's'}; p.command(0x35, query, sizeof(query)); p.drain();
        CHECK(p.error() == 0 && p.last_size==30 && std::memcmp(p.last+13,value,17)==0);
        std::puts("PASS: 17-byte readings deduplicate and round-trip through GetReading");
        Deotaland.reset();
    }
    {
        Peer p; p.begin();
        agent_link_io_desc_t io = {}; io.id = "s"; io.kind = "sensor"; io.value = AGENT_VAL_U16; io.dir = AGENT_IO_IN;
        CHECK(Deotaland.registerIo(io) == ESP_OK); p.drain();
        const uint8_t periodic[] = {1, 's', 2, 10, 0}; p.command(0x36, periodic, sizeof(periodic)); p.drain();
        const uint16_t value = 7; CHECK(Deotaland.pushReading("s", &value, sizeof(value)) == ESP_OK);
        const unsigned before = p.readings;
        for (unsigned i = 0; i < 100; ++i) { now_us += 10000; Deotaland.poll(4, 0); }
        CHECK(p.readings == before+10);
        std::puts("PASS: PERIODIC 10Hz publishes the latest cached reading for one simulated second");
        Deotaland.reset();
    }
    {
        Peer p; p.begin();
        agent_link_io_desc_t io={}; io.id="s"; io.kind="sensor"; io.value=AGENT_VAL_U16; io.dir=AGENT_IO_IN;
        CHECK(Deotaland.registerIo(io)==ESP_OK); p.drain();
        p.cb.on_connection(false); p.drain(); const unsigned sent=p.readings;
        for(uint16_t value=1;value<=100;++value) CHECK(Deotaland.updateReading("s",&value,2)==ESP_OK);
        CHECK(p.readings==sent && Deotaland.stats().reading_coalesces==99);
        p.cb.on_connection(true); p.cb.on_ready(); p.drain();
        CHECK(p.readings==sent+1 && p.last_reading[0]==100 && p.last_reading[1]==0);
        p.blocked=true; uint16_t value=101; CHECK(Deotaland.updateReading("s",&value,2)==ESP_OK); p.drain();
        value=102; CHECK(Deotaland.updateReading("s",&value,2)==ESP_OK); p.blocked=false; p.drain();
        CHECK(p.readings==sent+2 && p.last_reading[0]==102);
        Deotaland.reset();
        std::puts("PASS: offline and congested sensor updates coalesce without replaying stale samples");
    }
    {
        Peer p; p.begin();
        struct Playback { uint8_t bytes[2048]; size_t size=0, accept=0; bool once=false; } sink={};
        agent_playback_config_t config={}; config.ctx=&sink;
        config.write=[](const uint8_t* b,size_t n,void* context)->size_t {
            auto& s=*static_cast<Playback*>(context); const size_t take=n<s.accept?n:s.accept;
            CHECK(s.size+take<=sizeof(s.bytes)); std::memcpy(s.bytes+s.size,b,take); s.size+=take;
            if(s.once) s.accept=0;
            return take;
        };
        CHECK(Deotaland.configurePlayback(&config)==ESP_OK);
        const uint8_t start[]={1,0,0,0,2}, end[]={1,0,0,0,3};
        p.command(0x05,start,sizeof(start)); p.drain();
        uint8_t pcm[1024]; for(unsigned i=0;i<sizeof(pcm);++i) pcm[i]=uint8_t(i);
        CHECK(p.cb.on_stream(AGENT_STREAM_VOICE,pcm,sizeof(pcm))==ESP_OK);
        p.command(0x05,end,sizeof(end));
        for(unsigned i=0;i<10;++i) Deotaland.poll(4,0);
        CHECK(p.ends==0 && sink.size==0 && Deotaland.playbackStats().pending_pcm_bytes==512);
        // Finish cannot reach the head until its complete input was staged.
        sink.once=true; sink.accept=512; Deotaland.poll(4,0); Deotaland.poll(4,0);
        CHECK(p.ends==0 && sink.size==512);
        p.command(0x03); p.drain(); CHECK(p.last[2]==0x03 && p.error()==0);
        p.command(0x05,start,sizeof(start));
        sink.once=false; sink.accept=64; p.drain();
        CHECK(p.ends==1 && sink.size==sizeof(pcm) && std::memcmp(pcm,sink.bytes,sizeof(pcm))==0);
        CHECK(Deotaland.configurePlayback(nullptr)==ESP_ERR_INVALID_STATE); // Next START is active.
        sink.size=0; sink.accept=0;
        CHECK(p.cb.on_stream(AGENT_STREAM_VOICE,pcm,sizeof(pcm))==ESP_OK); Deotaland.poll(4,0);
        p.cb.on_connection(false); p.cb.on_connection(true); p.cb.on_ready(); sink.accept=128; p.drain();
        CHECK(sink.size==0); // No pending old PCM is delivered by an early service step.
        std::puts("PASS: PCM partial acceptance, END/next START ordering, and reconnect cancellation");
    }
    {
        Peer p; p.begin();
        static char description[1801]; std::memset(description,'x',1800); description[899]=1; description[1800]=0;
        agent_link_io_desc_t io={}; io.id="long"; io.kind="sensor"; io.value=AGENT_VAL_U16; io.desc=description;
        const uint32_t before=Deotaland.stats().manifest_build_steps;
        CHECK(Deotaland.registerIo(io)==ESP_OK); p.drain();
        CHECK(Deotaland.stats().manifest_build_steps-before>=7);
        CHECK(std::strstr(p.manifest,"\\u0001") && std::strstr(p.manifest,"\"id\":\"long\""));
        CHECK(p.manifest_size>1800 && p.manifest[p.manifest_size-1]=='}');
        const unsigned fragments=p.manifests;
        for(unsigned i=0;i<50;++i) { p.command(0x34); Deotaland.poll(4,0); }
        CHECK(p.manifests>fragments && p.manifest[p.manifest_size-1]=='}');
        Deotaland.reset();
        std::puts("PASS: long manifest fields/JSON escapes resume across bounded serialization steps");
    }
    {
        Peer p; p.begin(); uint8_t pcm[2]={}; unsigned accepted=0;
        while(p.cb.on_stream(AGENT_STREAM_VOICE,pcm,sizeof(pcm))==ESP_OK) ++accepted;
        CHECK(accepted==AGENT_LINK_RX_SLOTS-2-AGENT_LINK_RX_CONTROL_RESERVE);
        for(unsigned i=0;i<AGENT_LINK_RX_CONTROL_RESERVE;++i) p.command(0x70);
        p.drain(); CHECK(p.commands==AGENT_LINK_RX_CONTROL_RESERVE);
        std::puts("PASS: media saturation preserves control ingress capacity");
    }
    std::puts("PASS: 9 optimization regression groups (simulated timing, not hardware benchmarks)");
    return 0;
}
