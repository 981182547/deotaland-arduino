// Tests link the production core and protocol against an in-memory transport.
// No Arduino, ESP-IDF, hardware, network, test framework, or clock shim is used.
#include "agent_link.h"
#include "Deotaland.h"
#include "agent_link_stream.h"
#include "agent_link_transport.h"
#include "adpcm.h"
#include "audio_downlink.h"
#include "ota_service.h"
#include "protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <limits>

namespace {
int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); } } while (0)

using Bytes = std::vector<uint8_t>;

struct Harness {
    agent_transport_t transport = {};
    agent_transport_callbacks_t callbacks = {};
    bool ready = false;
    size_t mtu = 256;
    int starts = 0;
    int stops = 0;
    int polls = 0;
    int custom_calls = 0;
    int audio_calls = 0;
    bool reconnect_on_audio = false;
    bool excessive_response = false;
    int stream_starts = 0;
    int stream_ends = 0;
    int fail_ctrl = 0;
    int fail_end = 0;
    esp_err_t end_error = ESP_ERR_NO_MEM;
    esp_err_t write_result = ESP_OK;
    bool ended_complete = false;
    agent_stream_t last_kind = AGENT_STREAM_VOICE;
    Bytes last_custom;
    Bytes last_audio;
    Bytes last_metadata;
    Bytes last_stream_data;
    std::vector<Bytes> attempted;
    std::vector<Bytes> sent;
    std::vector<agent_state_t> states;

    Harness() {
        transport.impl = this;
        transport.start = [](void* p) -> esp_err_t {
            Harness& h = *static_cast<Harness*>(p);
            ++h.starts;
            h.ready = true;
            h.callbacks.on_connection(true);
            h.callbacks.on_ready();
            return ESP_OK;
        };
        transport.stop = [](void* p) {
            Harness& h = *static_cast<Harness*>(p);
            ++h.stops;
            h.ready = false;
        };
        transport.is_ready = [](void* p) { return static_cast<Harness*>(p)->ready; };
        transport.bind = [](void* p, const agent_transport_callbacks_t* c) {
            static_cast<Harness*>(p)->callbacks = c ? *c : agent_transport_callbacks_t{};
        };
        transport.poll = [](void* p) { ++static_cast<Harness*>(p)->polls; };
        transport.ctrl_mtu = [](void* p) { return static_cast<Harness*>(p)->mtu; };
        transport.send_ctrl = [](void* p, const uint8_t* data, size_t len) -> esp_err_t {
            Harness& h = *static_cast<Harness*>(p);
            h.attempted.emplace_back(data, data + len);
            if (len > h.mtu) return ESP_ERR_INVALID_SIZE;
            if (h.fail_ctrl > 0) { --h.fail_ctrl; return ESP_ERR_NO_MEM; }
            h.sent.emplace_back(data, data + len);
            return ESP_OK;
        };
        transport.stream_start = [](void* p, agent_stream_t kind,
                                    const uint8_t* data, size_t len) -> esp_err_t {
            Harness& h = *static_cast<Harness*>(p);
            ++h.stream_starts;
            h.last_kind = kind;
            h.last_metadata.clear();
            if (len) h.last_metadata.assign(data, data + len);
            return ESP_OK;
        };
        transport.send_stream = [](void* p, agent_stream_t kind,
                                   const uint8_t* data, size_t len) -> esp_err_t {
            Harness& h = *static_cast<Harness*>(p);
            h.last_kind = kind;
            h.last_stream_data.assign(data, data + len);
            return h.write_result;
        };
        transport.stream_end = [](void* p, agent_stream_t kind, bool complete,
                                  const uint8_t*, size_t) -> esp_err_t {
            Harness& h = *static_cast<Harness*>(p);
            ++h.stream_ends;
            h.last_kind = kind;
            h.ended_complete = complete;
            if (h.fail_end > 0) { --h.fail_end; return h.end_error; }
            return ESP_OK;
        };
    }

    void init(const uint8_t* identity = nullptr) {
        agent_link_deinit();
        agent_output_cb_t output = {};
        output.ctx = this;
        output.on_audio_out = [](const uint8_t* data, size_t len, void* p) {
            Harness& h = *static_cast<Harness*>(p);
            ++h.audio_calls;
            h.last_audio.assign(data, data + len);
            if (h.reconnect_on_audio) {
                h.reconnect_on_audio=false;
                h.callbacks.on_connection(false);
                h.callbacks.on_connection(true);
                h.callbacks.on_ready();
            }
        };
        output.on_command = [](uint16_t id, const uint8_t* data, size_t len, uint8_t*, size_t cap,
                               size_t* length, void* p) {
            Harness& h = *static_cast<Harness*>(p);
            if (id == 0x72 && h.excessive_response) {
                *length = cap + 1; // Faulty application reports more bytes than it was given.
                return true;
            }
            if (id != 0x70 && id != 0x71) return false;
            ++h.custom_calls;
            h.last_custom.assign(data, data + len);
            *length = 0;
            return true;
        };
        agent_link_config_t config = {};
        config.device_name = "host-test";
        config.caps = AGENT_CAP_SENSOR | AGENT_CAP_SPEAKER;
        config.transport = AGENT_TRANSPORT_CUSTOM;
        config.custom_transport = &transport;
        config.output = &output;
        config.device_id = identity;
        config.state_ctx = this;
        config.on_state = [](agent_state_t state, void* p) {
            static_cast<Harness*>(p)->states.push_back(state);
        };
        CHECK(agent_link_init(&config) == ESP_OK);
        CHECK(callbacks.on_ctrl && callbacks.on_stream && callbacks.on_connection && callbacks.on_ready);
    }

    void start() {
        CHECK(agent_link_start() == ESP_OK);
        drain();
        CHECK(agent_link_state() == AGENT_STATE_READY);
    }

    void drain() {
        // Also advances the incremental manifest sender after the RX queue empties.
        for (int i = 0; i != 256; ++i) CHECK(agent_link_poll(4) <= 4);
    }

    void clear() { attempted.clear(); sent.clear(); }

    ~Harness() { agent_link_deinit(); }
};

Bytes command(uint8_t id, const Bytes& payload = {}) {
    Bytes frame = agentlink::BuildEvent(id, payload.data(), payload.size());
    frame[1] = agentlink::kMsgCommand;
    frame[3] = 7;
    return frame;
}

void receive(Harness& h, const Bytes& frame) {
    CHECK(h.callbacks.on_ctrl(frame.data(), frame.size()) == ESP_OK);
}

agent_link_io_desc_t sensor(const char* id) {
    agent_link_io_desc_t descriptor = {};
    descriptor.id = id;
    descriptor.kind = "temperature";
    descriptor.dir = AGENT_IO_IN;
    descriptor.value = AGENT_VAL_I32;
    descriptor.unit = "C";
    descriptor.desc = "Ambient temperature; host regression endpoint";
    return descriptor;
}

void protocol_validation() {
    agentlink::Frame parsed;
    const uint8_t payload[] = {1, 0, 255};
    Bytes frame = agentlink::BuildEvent(0x64, payload, sizeof(payload));
    CHECK(frame == Bytes({1, 3, 0x64, 0, 3, 0, 1, 0, 255}));
    CHECK(agentlink::ParseFrame(frame.data(), frame.size(), parsed));
    CHECK(parsed.command_id == 0x64 && parsed.payload == Bytes(payload, payload + 3));
    CHECK(!agentlink::ParseFrame(nullptr, 0, parsed));
    CHECK(!agentlink::ParseFrame(frame.data(), 5, parsed));
    CHECK(!agentlink::ParseFrame(frame.data(), frame.size() - 1, parsed));
    frame.push_back(0);
    CHECK(!agentlink::ParseFrame(frame.data(), frame.size(), parsed));
    frame.pop_back();
    frame[0] = 2;
    CHECK(!agentlink::ParseFrame(frame.data(), frame.size(), parsed));
    frame[0] = 1;
    frame[1] = 0x04;
    CHECK(!agentlink::ParseFrame(frame.data(), frame.size(), parsed));
    frame[1] = 0x83;
    CHECK(agentlink::ParseFrame(frame.data(), frame.size(), parsed) && parsed.encrypted);
    frame = agentlink::BuildResponse(0x35, 7, 1, 1004, payload, 3);
    CHECK(frame == Bytes({1, 2, 0x35, 7, 7, 0, 0x35, 1, 0xec, 3, 1, 0, 255}));
    CHECK(agentlink::BuildEvent(1, nullptr, 1).empty());
    CHECK(agentlink::BuildResponse(1, 1, 0, 0, nullptr, 1).empty());
    CHECK(agentlink::BuildEvent(1, payload, 65536).empty());
    CHECK(agentlink::BuildResponse(1, 1, 0, 0, payload, 65532).empty());
}

void queued_callbacks_and_bounded_poll() {
    Harness h;
    h.init();
    const size_t state_count = h.states.size();
    CHECK(agent_link_start() == ESP_OK);
    CHECK(h.states.size() == state_count);
    h.drain();
    CHECK(agent_link_state() == AGENT_STATE_READY);
    CHECK(h.polls > 0);
    h.clear();

    Bytes frame = command(0x70, {11, 22, 33});
    receive(h, frame);
    std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(0xff));
    CHECK(h.custom_calls == 0 && h.sent.empty());
    CHECK(agent_link_poll(1) == 1);
    CHECK(h.custom_calls == 1 && h.last_custom == Bytes({11, 22, 33}));
    CHECK(h.sent.size() == 1 && h.sent[0][3] == 7);

    Bytes audio = {0x12, 0x34, 0x56, 0x78};
    CHECK(h.callbacks.on_stream(AGENT_STREAM_VOICE, audio.data(), audio.size()) == ESP_OK);
    std::fill(audio.begin(), audio.end(), static_cast<uint8_t>(0));
    CHECK(h.audio_calls == 0);
    CHECK(agent_link_poll(1) == 1);
    CHECK(h.audio_calls == 1 && h.last_audio == Bytes({0x12, 0x34, 0x56, 0x78}));

    frame = command(0x70, {42});
    int accepted = 0;
    esp_err_t result = ESP_OK;
    while (accepted < 1024 && (result = h.callbacks.on_ctrl(frame.data(), frame.size())) == ESP_OK)
        ++accepted;
    CHECK(accepted > 0 && accepted < 1024 && result == ESP_ERR_NO_MEM);
    agent_link_stats_t stats = {};
    agent_link_get_stats(&stats);
    CHECK(stats.rx_dropped > 0 && stats.rx_high_water >= static_cast<uint32_t>(accepted));
    const int before = h.custom_calls;
    CHECK(agent_link_poll(2) == 2);
    CHECK(h.custom_calls == before + 2);
    h.drain();
    CHECK(h.custom_calls == before + accepted);

    // Encrypted payloads must not be interpreted as plaintext commands.
    frame[1] |= agentlink::kMsgEncrypted;
    const int before_encrypted = h.custom_calls;
    (void)h.callbacks.on_ctrl(frame.data(), frame.size());
    h.drain();
    CHECK(h.custom_calls == before_encrypted);

    // Retry an ACK after backpressure without repeating the hardware callback.
    h.clear();
    h.fail_ctrl = 2;
    receive(h, command(0x71, {77}));
    CHECK(agent_link_poll(1) == 1);
    CHECK(h.custom_calls == before_encrypted + 1);
    h.drain();
    CHECK(h.custom_calls == before_encrypted + 1);
    CHECK(h.sent.size() == 1 && h.sent[0][2] == 0x71);

    // Losing the link takes priority over queued commands from the old peer.
    const int before_disconnect = h.custom_calls;
    frame = command(0x70, {99});
    int saturated = 0;
    while (saturated < 1024 && h.callbacks.on_ctrl(frame.data(), frame.size()) == ESP_OK)
        ++saturated;
    CHECK(saturated > 0 && saturated < 1024);
    h.ready = false;
    h.callbacks.on_connection(false);
    h.drain();
    CHECK(agent_link_state() == AGENT_STATE_DISCONNECTED);
    CHECK(h.custom_calls == before_disconnect);
}

void identity_prompt_and_response_limits() {
    Harness h;
    uint8_t identity[16];
    for (size_t i = 0; i < sizeof(identity); ++i) identity[i] = static_cast<uint8_t>(i + 1);
    const Bytes expected_id(identity, identity + sizeof(identity));
    h.init(identity);
    std::memset(identity, 0, sizeof(identity)); // init owns a copy of the stable identity.
    h.start();
    h.clear();
    receive(h, command(0x01));
    h.drain();
    CHECK(h.sent.size() == 1);
    agentlink::Frame frame;
    CHECK(agentlink::ParseFrame(h.sent[0].data(), h.sent[0].size(), frame));
    CHECK(frame.msg_type == agentlink::kMsgResponse && frame.command_id == 0x01);
    CHECK(frame.payload.size() >= 4 + 29 && frame.payload[1] == 0);
    CHECK(Bytes(frame.payload.begin() + 4, frame.payload.begin() + 20) == expected_id);

    h.clear();
    h.excessive_response = true;
    receive(h, command(0x72));
    h.drain();
    CHECK(h.sent.size() == 1);
    CHECK(h.sent[0] == Bytes({1, 2, 0x72, 7, 4, 0, 0x72, 1, 0xec, 3}));
    CHECK(h.custom_calls == 0);

    h.clear();
    receive(h, command(0x73)); // on_command returns false for an unsupported command.
    h.drain();
    CHECK(h.sent.size() == 1);
    CHECK(h.sent[0] == Bytes({1, 2, 0x73, 7, 4, 0, 0x73, 1, 0xe9, 3}));

    h.clear();
    h.mtu = 20;
    CHECK(agent_link_push_prompt(nullptr) == ESP_ERR_INVALID_ARG);
    CHECK(agent_link_push_prompt("") == ESP_ERR_INVALID_ARG);
    CHECK(agent_link_push_prompt("123456789012345") == ESP_ERR_INVALID_SIZE);
    CHECK(h.sent.empty());
    CHECK(agent_link_push_prompt("12345678901234") == ESP_OK);
    CHECK(h.sent.size() == 1 && h.sent[0].size() == 20 && h.sent[0][2] == 0x04);
}

void incremental_manifest_retry() {
    Harness h;
    h.mtu = 20;  // Default BLE ATT payload budget, including the protocol header.
    h.init();
    const agent_link_io_desc_t descriptor = sensor("temp0");
    CHECK(agent_link_register_io(&descriptor, nullptr, nullptr) == ESP_OK);
    h.fail_ctrl = 1;
    CHECK(agent_link_start() == ESP_OK);
    bool last = false;
    std::string manifest;
    uint8_t expected_index = 0;
    for (int i = 0; i < 256 && !last; ++i) {
        const size_t before = h.attempted.size();
        CHECK(agent_link_poll(4) <= 4);
        CHECK(h.attempted.size() <= before + 1);
        if (h.sent.size() > expected_index) {
            const Bytes& frame = h.sent.back();
            CHECK(frame.size() <= 20 && frame.size() >= 8);
            CHECK(frame[2] == 0x18 && frame[6] == expected_index);
            ++expected_index;
            manifest.append(frame.begin() + 8, frame.end());
            last = frame[7] == 1;
        }
    }
    CHECK(last && expected_index > 1);
    CHECK(manifest.find("temp0") != std::string::npos);
    CHECK(manifest.front() == '{' && manifest.back() == '}');
    CHECK(h.attempted.size() == h.sent.size() + 1);
    CHECK(h.attempted[0] == h.attempted[1]);
    const size_t complete_count = h.sent.size();
    h.drain();
    CHECK(h.sent.size() == complete_count);
}

void reading_retry_and_lifecycle() {
    Harness h;
    h.init();
    const agent_link_io_desc_t descriptor = sensor("temp0");
    CHECK(agent_link_register_io(&descriptor, nullptr, nullptr) == ESP_OK);
    h.start();
    receive(h, command(0x36, {5, 't', 'e', 'm', 'p', '0', 3, 0, 0}));
    h.drain();
    h.clear();
    int32_t reading = 23;
    h.fail_ctrl = 1;
    CHECK(agent_link_push_reading("temp0", &reading, sizeof(reading)) == ESP_ERR_NO_MEM);
    CHECK(agent_link_push_reading("temp0", &reading, sizeof(reading)) == ESP_OK);
    CHECK(h.sent.size() == 1 && h.sent[0][2] == 0x19);
    CHECK(agent_link_push_reading("temp0", &reading, sizeof(reading)) == ESP_OK);
    CHECK(h.sent.size() == 1);
    CHECK(agent_link_push_reading("missing", &reading, sizeof(reading)) == ESP_ERR_NOT_FOUND);
    CHECK(agent_link_push_reading("temp0", &reading, 1) == ESP_ERR_INVALID_SIZE);

    receive(h, command(0x70, {99}));
    const int before_stop = h.custom_calls;
    agent_link_stop();
    CHECK(agent_link_state() == AGENT_STATE_DISCONNECTED);
    h.drain();
    CHECK(h.custom_calls == before_stop);
    CHECK(agent_link_push_reading("temp0", &reading, sizeof(reading)) == ESP_ERR_INVALID_STATE);
    h.start();
    h.clear();
    CHECK(agent_link_push_reading("temp0", &reading, sizeof(reading)) == ESP_OK);
    CHECK(h.sent.size() == 1);  // Reconnect must resynchronize unchanged readings.
    agent_link_deinit();
    h.init();
    h.start();
    CHECK(agent_link_push_reading("temp0", &reading, sizeof(reading)) == ESP_ERR_NOT_FOUND);
}

// Exercise wire bytes, rejected values preserving the cache, and offline retry.
void beginner_typed_readings() {
    Harness h;
    h.init();
    agent_link_io_desc_t endpoints[5];
    const agent_val_t types[] = {AGENT_VAL_BOOL, AGENT_VAL_I32, AGENT_VAL_U16, AGENT_VAL_F32, AGENT_VAL_STR};
    const char* ids[] = {"b", "i", "u", "f", "s"};
    for (size_t i=0; i<5; ++i) {
        endpoints[i]=sensor(ids[i]); endpoints[i].value=types[i];
        CHECK(Deotaland.registerIo(endpoints[i])==ESP_OK);
    }
    CHECK(Deotaland.maxPromptBytes()==0);
    CHECK(Deotaland.updateBool("b",true)==ESP_OK);
    CHECK(Deotaland.updateInt32("i",-2)==ESP_OK);
    CHECK(Deotaland.updateUInt16("u",0x1234)==ESP_OK);
    CHECK(Deotaland.updateFloat("f",1.0f)==ESP_OK);
    char temporary[]="old";
    CHECK(Deotaland.updateText("s",temporary)==ESP_OK);
    temporary[0]='X'; // input storage is borrowed only until return
    CHECK(Deotaland.updateFloat("i",2.0f)==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.updateInt32("f",2)==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.updateText("b","x")==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.updateBool("missing",true)==ESP_ERR_NOT_FOUND);
    CHECK(Deotaland.updateText("s",nullptr)==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.updateFloat("f",std::numeric_limits<float>::quiet_NaN())==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.updateFloat("f",std::numeric_limits<float>::infinity())==ESP_ERR_INVALID_ARG);
    const uint8_t invalid_bool=2;
    CHECK(Deotaland.updateReading("b",&invalid_bool,1)==ESP_ERR_INVALID_ARG);
    h.start();
    CHECK(Deotaland.maxPromptBytes()==h.mtu-6);
    const Bytes expected[] = {{1},{0xfe,0xff,0xff,0xff},{0x34,0x12},{0,0,0x80,0x3f},{'o','l','d'}};
    for (size_t i=0; i<5; ++i) {
        h.clear();
        receive(h,command(0x35,{1,static_cast<uint8_t>(ids[i][0])}));
        h.drain();
        CHECK(h.sent.size()==1);
        const Bytes& response=h.sent[0];
        CHECK(response[7]==0 && response[10]==static_cast<uint8_t>(types[i]));
        CHECK(Bytes(response.begin()+13,response.end())==expected[i]);
    }
    h.clear();
    CHECK(Deotaland.updateText("s","")==ESP_OK);
    h.drain();
    CHECK(h.sent.size()==1 && h.sent[0].size()==9 && h.sent[0][2]==0x19);
    h.clear(); receive(h,command(0x35,{1,'s'})); h.drain();
    CHECK(h.sent.size()==1 && h.sent[0][7]==0 && h.sent[0].size()==13);
    CHECK(Deotaland.updateText("s",std::string(AGENT_LINK_READING_CACHE_BYTES,'x').c_str())==ESP_OK);
    CHECK(Deotaland.updateText("s",std::string(AGENT_LINK_READING_CACHE_BYTES+1,'y').c_str())==ESP_ERR_INVALID_SIZE);
    h.mtu=20;
    CHECK(Deotaland.maxPromptBytes()==14);
    h.clear(); h.drain();
    CHECK(h.sent.empty());
    CHECK(Deotaland.stats().last_error==ESP_ERR_INVALID_SIZE);
    h.mtu=256; h.drain();
    CHECK(h.sent.size()==1 && h.sent[0].back()=='x'); // MTU growth recovers the pending sample
    h.callbacks.on_connection(false);
    CHECK(Deotaland.maxPromptBytes()==0); // immediate invalidation, even before poll
    h.drain();
    CHECK(Deotaland.updateInt32("i",99)==ESP_OK);
    CHECK(Deotaland.updateInt32("i",100)==ESP_OK);
    h.callbacks.on_connection(true); h.callbacks.on_ready(); h.clear(); h.drain();
    unsigned count=0;
    for(const auto& frame:h.sent) if(frame[2]==0x19 && frame[7]=='i') {
        ++count; CHECK(Bytes(frame.begin()+9,frame.end())==Bytes({100,0,0,0}));
    }
    CHECK(count==1);
    CHECK(Deotaland.isRetryable(ESP_ERR_NO_MEM));
    CHECK(Deotaland.isRetryable(ESP_ERR_TIMEOUT));
    CHECK(!Deotaland.isRetryable(ESP_ERR_INVALID_SIZE));
    CHECK(std::strcmp(Deotaland.stateName(AGENT_STATE_READY),"READY")==0);
    CHECK(std::strcmp(Deotaland.errorName(ESP_ERR_INVALID_ARG),"ESP_ERR_INVALID_ARG")==0);
    CHECK(Deotaland.errorHint(ESP_ERR_NOT_SUPPORTED)[0]);
}

void invalid_actuator_values() {
    Harness h; h.init();
    agent_link_io_desc_t endpoint=sensor("out");
    endpoint.dir=AGENT_IO_OUT; endpoint.value=AGENT_VAL_BOOL;
    int calls=0;
    CHECK(Deotaland.registerIo(endpoint,[](const char*,const uint8_t*,size_t,void* ctx) {
        ++*static_cast<int*>(ctx);
    },&calls)==ESP_OK);
    agent_link_io_desc_t bad=sensor("bad"); bad.range_min=10; bad.range_max=1;
    CHECK(Deotaland.registerIo(bad)==ESP_ERR_INVALID_ARG);
    CHECK(Deotaland.updateBool("out",true)==ESP_ERR_NOT_FOUND);
    h.start(); h.clear();
    receive(h,command(0x33,{3,'o','u','t',2})); h.drain();
    CHECK(calls==0 && h.sent.back()[7]==1 && h.sent.back()[8]==0xec && h.sent.back()[9]==3);
    receive(h,command(0x33,{3,'o','u','t',1})); h.drain();
    CHECK(calls==1 && h.sent.back()[7]==0);
    endpoint.value=AGENT_VAL_F32;
    CHECK(Deotaland.notifyManifestChanged()==ESP_OK); h.drain(); h.clear();
    receive(h,command(0x33,{3,'o','u','t',0,0,0x80,0x7f})); h.drain(); // +Inf
    CHECK(calls==1 && h.sent.back()[7]==1);
    receive(h,command(0x33,{3,'o','u','t',0,0,0x80,0x3f})); h.drain(); // 1.0
    CHECK(calls==2 && h.sent.back()[7]==0);
    endpoint.value=AGENT_VAL_VEC2;
    CHECK(Deotaland.notifyManifestChanged()==ESP_OK); h.drain(); h.clear();
    receive(h,command(0x33,{3,'o','u','t',0,0,0x80,0x3f,0,0,0xc0,0x7f})); h.drain();
    CHECK(calls==2 && h.sent.back()[7]==1); // second component NaN
}

void stream_metadata_and_retry() {
    static_assert(AGENT_STREAM_VOICE == 0 && AGENT_STREAM_AUDIO == 1 &&
                  AGENT_STREAM_IMAGE == 2 && AGENT_STREAM_VIDEO == 3 &&
                  AGENT_STREAM_FILE == 4 && AGENT_STREAM_KIND_COUNT == 5,
                  "Stream kinds must match the current Agent_link wire vocabulary");
    Harness h;
    h.init();
    h.start();
    agent_stream_opts_t opts = {};
    opts.encoding = AGENT_ENC_JPEG;
    opts.width = 320;
    opts.height = 240;
    opts.total_bytes = 0x12345678;
    agent_stream_handle_t handle = nullptr;
    CHECK(agent_link_stream_open(AGENT_STREAM_IMAGE, &opts, &handle) == ESP_OK);
    CHECK(handle && h.last_kind == AGENT_STREAM_IMAGE);
    CHECK(h.last_metadata == Bytes({1, 0x40, 1, 0xf0, 0, 0x78, 0x56, 0x34, 0x12}));
    agent_stream_handle_t second = nullptr;
    CHECK(agent_link_stream_open(AGENT_STREAM_IMAGE, &opts, &second) == ESP_OK);
    CHECK(second == handle && h.stream_starts == 1);
    const uint8_t data[] = {1, 2, 3, 4};
    h.write_result = ESP_ERR_NO_MEM;
    CHECK(agent_link_stream_write(handle, data, sizeof(data)) == ESP_ERR_NO_MEM);
    CHECK(agent_link_stream_is_open(AGENT_STREAM_IMAGE));
    h.write_result = ESP_OK;
    CHECK(agent_link_stream_write(handle, data, sizeof(data)) == ESP_OK);
    h.fail_end = 1;
    CHECK(agent_link_stream_close(handle, true) == ESP_ERR_NO_MEM);
    CHECK(agent_link_stream_is_open(AGENT_STREAM_IMAGE));
    CHECK(agent_link_stream_close(handle, true) == ESP_OK);
    CHECK(!agent_link_stream_is_open(AGENT_STREAM_IMAGE) && h.ended_complete);
    CHECK(h.stream_ends == 2);
    CHECK(agent_link_stream_close(handle, true) == ESP_OK && h.stream_ends == 2);
    CHECK(agent_link_stream_write(handle, data, sizeof(data)) == ESP_ERR_INVALID_STATE);

    opts = agent_stream_opts_t{};
    opts.name = "sample.pcm";
    CHECK(agent_link_stream_open(AGENT_STREAM_AUDIO, &opts, &handle) == ESP_OK);
    CHECK(h.last_metadata == Bytes({'s', 'a', 'm', 'p', 'l', 'e', '.', 'p', 'c', 'm'}));
    CHECK(agent_link_stream_open(AGENT_STREAM_FILE, &opts, &second) == ESP_OK);
    CHECK(handle != second && agent_link_stream_is_open(AGENT_STREAM_AUDIO));
    agent_link_stop();
    CHECK(!agent_link_stream_is_open(AGENT_STREAM_AUDIO));
    CHECK(!agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(agent_link_stream_write(handle, data, sizeof(data)) == ESP_ERR_INVALID_STATE);
    h.start();
    h.write_result = ESP_ERR_NO_MEM;
    CHECK(agent_link_stream_send(AGENT_STREAM_FILE, &opts, data, sizeof(data)) == ESP_ERR_NO_MEM);
    CHECK(!h.ended_complete && !agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(agent_link_stream_open(AGENT_STREAM_KIND_COUNT, nullptr, &handle) == ESP_ERR_INVALID_ARG);
    CHECK(agent_link_stream_open(AGENT_STREAM_IMAGE, nullptr, nullptr) == ESP_ERR_INVALID_ARG);
}

void one_shot_deferred_end() {
    Harness h;
    h.init();
    h.start();
    const uint8_t data[] = {1, 2, 3, 4};
    agent_stream_opts_t opts = {};
    opts.name = "one-shot";
    agent_stream_handle_t handle = nullptr;
    CHECK(agent_link_stream_open(AGENT_STREAM_FILE, &opts, &handle) == ESP_OK);
    CHECK(agent_link_stream_close(handle, false) == ESP_OK);

    const int ends_before = h.stream_ends;
    h.fail_end = 2;
    CHECK(agent_link_stream_send(AGENT_STREAM_FILE, &opts, data, sizeof(data)) == ESP_OK);
    CHECK(h.stream_ends == ends_before + 1 && agent_link_stream_is_open(AGENT_STREAM_FILE));
    agent_stream_handle_t other = nullptr;
    CHECK(agent_link_stream_open(AGENT_STREAM_FILE, &opts, &other) == ESP_ERR_INVALID_STATE);
    CHECK(other == nullptr);
    CHECK(agent_link_stream_write(handle, data, sizeof(data)) == ESP_ERR_INVALID_ARG); // stale session token
    CHECK(agent_link_stream_send(AGENT_STREAM_FILE, &opts, data, sizeof(data)) == ESP_ERR_INVALID_STATE);
    CHECK(agent_link_poll(1) == 0);
    CHECK(h.stream_ends == ends_before + 2 && agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(agent_link_poll(1) == 0);
    CHECK(h.stream_ends == ends_before + 3 && !agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(h.ended_complete);

    // An unaccepted payload must still eventually send complete=false, even when
    // the first two attempts to enqueue that abort marker time out.
    const int aborts_before = h.stream_ends;
    h.write_result = ESP_ERR_NO_MEM;
    h.end_error = ESP_ERR_TIMEOUT;
    h.fail_end = 2;
    CHECK(agent_link_stream_send(AGENT_STREAM_FILE, &opts, data, sizeof(data)) == ESP_ERR_NO_MEM);
    CHECK(h.stream_ends == aborts_before + 1 && !h.ended_complete);
    CHECK(agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(agent_link_stream_write(handle, data, sizeof(data)) == ESP_ERR_INVALID_ARG);
    CHECK(agent_link_poll(1) == 0);
    CHECK(h.stream_ends == aborts_before + 2 && agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(agent_link_poll(1) == 0);
    CHECK(h.stream_ends == aborts_before + 3 && !agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(!h.ended_complete);
    h.write_result = ESP_OK;
    CHECK(agent_link_stream_send(AGENT_STREAM_FILE, &opts, data, sizeof(data)) == ESP_OK);
    CHECK(h.ended_complete && !agent_link_stream_is_open(AGENT_STREAM_FILE));
}

void reconnect_drops_old_response() {
    Harness h;
    h.init(); h.start(); h.clear();
    CHECK(agent_link_report_battery(70,false)==ESP_OK);
    agent_stream_handle_t old_stream=nullptr;
    CHECK(agent_link_stream_open(AGENT_STREAM_FILE,nullptr,&old_stream)==ESP_OK);
    h.clear();
    h.fail_ctrl=8;
    receive(h,command(0x70,{1}));
    CHECK(agent_link_poll(1)==1);
    CHECK(h.custom_calls==1 && h.sent.empty());
    // The backend can reconnect between two loop iterations while an ACK is pending.
    h.ready=false; h.callbacks.on_connection(false);
    h.ready=true; h.callbacks.on_connection(true); h.callbacks.on_ready();
    h.fail_ctrl=0; h.clear();
    CHECK(agent_link_state()!=AGENT_STATE_READY);
    CHECK(!agent_link_stream_is_open(AGENT_STREAM_FILE));
    CHECK(agent_link_push_event(AGENT_EVT_BUTTON,nullptr,0)==ESP_ERR_INVALID_STATE);
    CHECK(agent_link_report_battery(70,false)==ESP_ERR_INVALID_STATE);
    const uint8_t byte=1;
    CHECK(agent_link_stream_write(old_stream,&byte,1)==ESP_ERR_INVALID_STATE);
    h.drain();
    CHECK(std::none_of(h.sent.begin(),h.sent.end(),[](const Bytes& b) {
        return b[1]==agentlink::kMsgResponse && b[2]==0x70;
    }));
    CHECK(h.custom_calls==1);
    receive(h,command(0x70,{2})); h.drain();
    CHECK(h.custom_calls==2 && h.last_custom==Bytes({2}));
}

void synthetic_endpoint_capacity() {
    Harness h;
    h.init();
    char ids[32][12]={};
    agent_link_io_desc_t io[32]={};
    for (int i=0; i<32; ++i) {
        std::snprintf(ids[i],sizeof(ids[i]),"sensor%d",i);
        io[i]=sensor(ids[i]);
        CHECK(agent_link_register_io(&io[i],nullptr,nullptr)==ESP_OK);
    }
    agent_output_cb_t output={};
    output.on_led=[](uint32_t,void*) {};
    agent_link_config_t config={};
    config.device_name="full-registry";
    config.transport=AGENT_TRANSPORT_CUSTOM;
    config.custom_transport=&h.transport;
    config.caps=AGENT_CAP_LED;
    config.output=&output;
    CHECK(agent_link_init(&config)==ESP_ERR_NO_MEM);
    // Failed initialization preserves the previous configuration and callbacks.
    h.start();
    receive(h,command(0x70,{9})); h.drain();
    CHECK(h.custom_calls==1 && h.last_custom==Bytes({9}));
    agent_link_deinit();
    io[0]=sensor("led0");
    for (const auto& d:io) CHECK(agent_link_register_io(&d,nullptr,nullptr)==ESP_OK);
    // A caller-owned led0 already occupies the id; no extra slot is needed.
    CHECK(agent_link_init(&config)==ESP_OK);
    CHECK(agent_link_start()==ESP_OK);
    h.drain();
    CHECK(agent_link_state()==AGENT_STATE_READY);
    agent_link_deinit(); // Descriptors must outlive the registration.
}

void subscription_recovery_drops_old_work() {
    Harness h;
    h.init(); h.start(); h.clear();
    h.fail_ctrl=8;
    receive(h,command(0x70,{3}));
    CHECK(agent_link_poll(1)==1 && h.custom_calls==1);
    // GATT can lose/recover its control subscription without an ACL disconnect.
    // Connection(true) demotes the logical link to CONNECTED until on_ready.
    for (int i=0; i<32; ++i) {
        h.callbacks.on_connection(true);
        h.callbacks.on_ready();
    }
    CHECK(agent_link_state()!=AGENT_STATE_READY);
    h.fail_ctrl=0; h.clear();
    CHECK(agent_link_poll(1)==1);
    CHECK(agent_link_state()==AGENT_STATE_CONNECTED);
    CHECK(agent_link_poll(1)==1);
    CHECK(agent_link_state()==AGENT_STATE_READY);
    CHECK(std::none_of(h.sent.begin(),h.sent.end(),[](const Bytes& b) {
        return b[1]==agentlink::kMsgResponse && b[2]==0x70;
    }));
    agent_link_stats_t stats={}; agent_link_get_stats(&stats);
    CHECK(stats.rx_dropped==0);
}

void reconnect_during_pcm_delivery() {
    Harness h;
    h.init(); h.start(); h.clear();
    const Bytes samples(1024,0);
    h.reconnect_on_audio=true;
    CHECK(h.callbacks.on_stream(AGENT_STREAM_VOICE,samples.data(),samples.size())==ESP_OK);
    CHECK(agent_link_poll(1)==1);
    // The first 512-byte callback simulates a host reconnect. Remaining PCM
    // from that old receive event must not be delivered to application code.
    CHECK(h.audio_calls==1 && h.last_audio.size()==512);
    CHECK(agent_link_state()!=AGENT_STATE_READY);
    h.drain();
    CHECK(agent_link_state()==AGENT_STATE_READY);
    const uint8_t fresh[]={9,7};
    CHECK(h.callbacks.on_stream(AGENT_STREAM_VOICE,fresh,sizeof(fresh))==ESP_OK);
    CHECK(agent_link_poll(1)==1);
    CHECK(h.audio_calls==2 && h.last_audio==Bytes({9,7}));
}

void facade_rejected_lifecycle_does_not_desync() {
    Harness h;
    Deotaland.reset();
    agent_output_cb_t output={};
    output.on_command=[](uint16_t id,const uint8_t*,size_t,uint8_t*,size_t,size_t* n,void*) {
        if (id!=0x70) return false;
        Deotaland.end();  // Invalid from a callback: must reject without corrupting facade state.
        *n=0; return true;
    };
    agent_link_config_t config={};
    config.device_name="facade-test";
    config.transport=AGENT_TRANSPORT_CUSTOM;
    config.custom_transport=&h.transport;
    config.output=&output;
    CHECK(Deotaland.begin(config)==ESP_OK);
    h.drain();
    receive(h,command(0x70));
    Deotaland.poll();
    CHECK(Deotaland.isReady());
    CHECK(Deotaland.stats().last_error==ESP_ERR_INVALID_STATE);
    Deotaland.end();
    CHECK(!h.ready && Deotaland.state()==AGENT_STATE_DISCONNECTED);
    CHECK(Deotaland.begin(config)==ESP_OK);
    h.drain();
    Deotaland.reset();
    CHECK(!h.ready);
}

void status_snapshot_copy_and_lifecycle() {
    Harness h;
    std::vector<agent_link_status_t> delivered;
    agent_link_config_t config = {};
    config.device_name = "status-test";
    config.transport = AGENT_TRANSPORT_CUSTOM;
    config.custom_transport = &h.transport;
    config.status_ctx = &delivered;
    config.on_status = [](const agent_link_status_t* value, void* context) {
        static_cast<std::vector<agent_link_status_t>*>(context)->push_back(*value);
    };
    CHECK(Deotaland.begin(config) == ESP_OK);
    h.drain();
    CHECK(h.callbacks.on_status != nullptr);
    delivered.clear();

    agent_link_status_t source = {};
    source.transport = AGENT_TRANSPORT_WIFI;
    source.phase = AGENT_LINK_PHASE_SETUP;
    source.wifi_connected = true;
    source.wifi_portal_active = true;
    std::strcpy(source.title, "Set up WiFi");
    std::strcpy(source.hint, "Connected; waiting for portal shutdown");
    // A faulty/custom backend can fill its whole fixed-width IP field. The
    // public mailbox must copy it and always append a terminator of its own.
    std::memset(source.wifi_ip, '1', sizeof(source.wifi_ip));
    h.callbacks.on_status(&source);
    CHECK(delivered.empty()); // Backend thread never invokes application callbacks.
    agent_link_status_t snapshot = {};
    agent_link_get_status(&snapshot);
    CHECK(snapshot.transport == source.transport && snapshot.phase == source.phase);
    CHECK(snapshot.wifi_connected && snapshot.wifi_portal_active);
    CHECK(std::strcmp(snapshot.title, source.title) == 0);
    CHECK(std::strcmp(snapshot.hint, source.hint) == 0);
    CHECK(snapshot.wifi_ip[sizeof(snapshot.wifi_ip) - 1] == 0);
    CHECK(std::strlen(snapshot.wifi_ip) == sizeof(snapshot.wifi_ip) - 1);
    CHECK(std::strncmp(snapshot.wifi_ip, source.wifi_ip, sizeof(snapshot.wifi_ip) - 1) == 0);
    CHECK(source.wifi_ip[sizeof(source.wifi_ip) - 1] == '1'); // Caller memory is unchanged.
    const agent_link_status_t copied = snapshot;
    source = {}; // The backend may reuse or release its source immediately.
    snapshot = Deotaland.status();
    CHECK(snapshot.wifi_connected && snapshot.wifi_portal_active);
    CHECK(std::strcmp(snapshot.wifi_ip, copied.wifi_ip) == 0);
    h.drain();
    CHECK(delivered.size() == 1);
    const agent_link_status_t& callback = delivered.back();
    CHECK(callback.transport == copied.transport && callback.phase == copied.phase);
    CHECK(callback.wifi_connected == copied.wifi_connected);
    CHECK(callback.wifi_portal_active == copied.wifi_portal_active);
    CHECK(std::strcmp(callback.wifi_ip, copied.wifi_ip) == 0);
    CHECK(std::strcmp(callback.title, copied.title) == 0 && std::strcmp(callback.hint, copied.hint) == 0);
    CHECK(callback.expires_s == copied.expires_s && callback.detail == copied.detail);
    CHECK(std::strcmp(callback.code, copied.code) == 0);

    const auto check_cleared = []() {
        const agent_link_status_t value = Deotaland.status();
        CHECK(value.phase == AGENT_LINK_PHASE_IDLE);
        CHECK(!value.wifi_connected && !value.wifi_portal_active);
        CHECK(value.wifi_ip[0] == 0);
        CHECK(value.title[0] == 0 && value.hint[0] == 0);
        CHECK(value.code[0] == 0 && value.expires_s == 0 && value.detail == 0);
    };
    // End must clear both the current snapshot and an undelivered update.
    h.callbacks.on_status(&copied);
    Deotaland.end();
    check_cleared();
    h.callbacks.on_status(&copied); // A late transport callback is ignored after end.
    check_cleared();
    CHECK(Deotaland.poll() == 0);
    CHECK(delivered.size() == 1);

    CHECK(agent_link_start() == ESP_OK);
    h.drain();
    h.callbacks.on_status(&copied);
    CHECK(Deotaland.status().wifi_connected);
    const size_t callbacks_before_reset = delivered.size();
    const auto late_callback = h.callbacks.on_status;
    Deotaland.reset();
    check_cleared();
    CHECK(h.callbacks.on_status == nullptr);
    late_callback(&copied); // Captured worker callback cannot revive reset state.
    check_cleared();
    CHECK(Deotaland.poll() == 0);
    CHECK(delivered.size() == callbacks_before_reset);
}

Bytes decoded_audio;
std::vector<Bytes> audio_flow;

void audio_decode_and_portable_ota() {
    using namespace agentlink;
    audio::OnLinkState(false);
    decoded_audio.clear();
    audio_flow.clear();
    audio::SetSinks([](const uint8_t* data, size_t bytes) {
        decoded_audio.insert(decoded_audio.end(), data, data + bytes);
    }, [](uint8_t id, const uint8_t* data, size_t bytes) {
        CHECK(id == 0x20 && bytes == 4);
        audio_flow.emplace_back(data, data + bytes);
    });
    CHECK(!audio::SetCodec(2));
    CHECK(audio::SetCodec(audio::kCodecImaAdpcm));
    audio::Arm();
    CHECK(audio::SetCodec(audio::kCodecPcm16));
    CHECK(audio::CodecValue() == audio::kCodecImaAdpcm); // Mid-stream changes are ignored.
    Bytes block(adpcm::kBlockBytes, 0);
    audio::Feed(block.data(), 100);
    CHECK(decoded_audio.empty());
    audio::Feed(block.data() + 100, block.size() - 100);
    CHECK(decoded_audio.size() == adpcm::kSamplesPerBlock * sizeof(int16_t));
    CHECK(std::all_of(decoded_audio.begin(), decoded_audio.end(), [](uint8_t b) { return b == 0; }));
    audio::OnLinkState(false);
    CHECK(!audio::IsArmed() && audio::CodecValue() == audio::kCodecPcm16);
    audio::SetBufferMs(400);
    audio::Arm();
    Bytes burst(32000, 0); // One second of PCM exceeds the default 250 ms high-water mark.
    audio::Feed(burst.data(), burst.size());
    CHECK(!audio_flow.empty() && audio_flow.back()[0] == 1);
    audio::Disarm();
    CHECK(audio_flow.back()[0] == 0);
    audio::SetSinks(nullptr, nullptr);

    ota::Configure(true, true);
    CHECK(!agent_link_ota_supported());
    uint8_t hash[32] = {};
    uint16_t error = 0;
    CHECK(!ota::Start(1024, hash, "1.0.0", "host", &error));
    CHECK(error == ota::kErrBusinessFailed);
    CHECK(!agent_link_ota_is_busy());
    CHECK(agent_link_ota_mark_valid() == ESP_ERR_NOT_SUPPORTED);
    ota::Configure(false, false);
}
}  // namespace

int main() {
    protocol_validation();
    queued_callbacks_and_bounded_poll();
    identity_prompt_and_response_limits();
    incremental_manifest_retry();
    reading_retry_and_lifecycle();
    beginner_typed_readings();
    invalid_actuator_values();
    stream_metadata_and_retry();
    one_shot_deferred_end();
    reconnect_drops_old_response();
    facade_rejected_lifecycle_does_not_desync();
    status_snapshot_copy_and_lifecycle();
    synthetic_endpoint_capacity();
    subscription_recovery_drops_old_work();
    reconnect_during_pcm_delivery();
    audio_decode_and_portable_ota();
    std::printf("PASS: %d checks across 16 production-core regression groups\n", checks);
    return 0;
}
