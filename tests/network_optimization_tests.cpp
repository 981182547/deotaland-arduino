#include "wifi_credentials.h"
#include "wifi_network_state.h"
#include "cloud_session.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)
using agentlink::wifi_detail::CredentialRecord;
using agentlink::wifi_detail::SaveCredentials;
using agentlink::wifi_detail::ValidCredentials;

struct Store {
    esp_err_t open_error = ESP_OK, write_error = ESP_OK, commit_error = ESP_OK;
    unsigned opens = 0, writes = 0, commits = 0, closes = 0;
    CredentialRecord saved = {}, pending = {};
    esp_err_t open() { ++opens; return open_error; }
    esp_err_t write(const CredentialRecord& value) { ++writes; if (write_error == ESP_OK) pending = value; return write_error; }
    esp_err_t commit() { ++commits; if (commit_error == ESP_OK) saved = pending; return commit_error; }
    void close() { ++closes; }
};
void TestCredentials() {
    Store store;
    CHECK(SaveCredentials(store, "old-network", "old-password") == ESP_OK);
    CHECK(ValidCredentials(store.saved));
    CHECK(std::strcmp(store.saved.ssid, "old-network") == 0);
    CHECK(std::strcmp(store.saved.password, "old-password") == 0);
    const CredentialRecord original = store.saved;
    for (unsigned failing = 0; failing != 3; ++failing) {
        store.open_error = failing == 0 ? ESP_ERR_NO_MEM : ESP_OK;
        store.write_error = failing == 1 ? ESP_ERR_INVALID_SIZE : ESP_OK;
        store.commit_error = failing == 2 ? ESP_FAIL : ESP_OK;
        const unsigned before_writes = store.writes, before_commits = store.commits, before_closes = store.closes;
        const esp_err_t expected = failing == 0 ? ESP_ERR_NO_MEM : failing == 1 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
        CHECK(SaveCredentials(store, "new-network", "new-password") == expected);
        CHECK(std::memcmp(&original, &store.saved, sizeof(original)) == 0);
        CHECK(store.writes - before_writes == (failing > 0 ? 1u : 0u));
        CHECK(store.commits - before_commits == (failing > 1 ? 1u : 0u));
        CHECK(store.closes - before_closes == (failing > 0 ? 1u : 0u));
    }
    store.open_error = store.write_error = store.commit_error = ESP_OK;
    CHECK(SaveCredentials(store, "new-network", "new-password") == ESP_OK);
    CHECK(std::strcmp(store.saved.ssid, "new-network") == 0);
    CHECK(std::strcmp(store.saved.password, "new-password") == 0);
    CHECK(SaveCredentials(store, "open-network", nullptr) == ESP_OK);
    CHECK(store.saved.password[0] == 0);
    char max_ssid[33], max_password[65];
    std::memset(max_ssid, 's', 32); max_ssid[32] = 0;
    std::memset(max_password, 'p', 64); max_password[64] = 0;
    CHECK(SaveCredentials(store, max_ssid, max_password) == ESP_OK);
    CHECK(ValidCredentials(store.saved));
    CHECK(std::strlen(store.saved.ssid) == 32 && std::strlen(store.saved.password) == 64);
    const unsigned before = store.opens;
    CHECK(SaveCredentials(store, "", "p") == ESP_ERR_INVALID_ARG);
    CHECK(SaveCredentials(store, nullptr, "p") == ESP_ERR_INVALID_ARG);
    CHECK(SaveCredentials(store, "012345678901234567890123456789012", "p") == ESP_ERR_INVALID_ARG);
    CHECK(SaveCredentials(store, "s", "0123456789012345678901234567890123456789012345678901234567890123456") == ESP_ERR_INVALID_ARG);
    CHECK(store.opens == before);
    CredentialRecord invalid = store.saved;
    invalid.version[0] = 'X'; CHECK(!ValidCredentials(invalid));
    invalid = store.saved; std::memset(invalid.ssid, 'X', sizeof(invalid.ssid)); CHECK(!ValidCredentials(invalid));
    invalid = store.saved; std::memset(invalid.password, 'X', sizeof(invalid.password)); CHECK(!ValidCredentials(invalid));
    invalid = store.saved; invalid.ssid[0] = 0; CHECK(!ValidCredentials(invalid));
}

struct Cloud {
    uint32_t now = 0, latency = 0;
    bool valid = true;
    unsigned claims = 0, binds = 0, auths = 0, heartbeats = 0, saves = 0, clears = 0, drops = 0;
    esp_err_t transport = ESP_OK, storage = ESP_OK;
    int code = -1;
    cloud_bind_state_t bind_state = CLOUD_BIND_PENDING;
    agent_cloud_status_t status = {};
    char saved_key[65] = {};
    agentlink::cloud::Session session;
    void begin(const char* key = "") {
        const agentlink::cloud::Config config = {1, "sn", "mac", "chip", "fw"};
        agentlink::cloud::Ops ops = {};
        ops.ctx = this;
        ops.now = [](void* p) { return static_cast<Cloud*>(p)->now; };
        ops.valid = [](void* p) { return static_cast<Cloud*>(p)->valid; };
        ops.claim = [](void* p, const agentlink::cloud::Config&, cloud_claim_t* out, cloud_api_err_t* error) {
            Cloud& c = *static_cast<Cloud*>(p); ++c.claims; c.now += c.latency;
            std::strcpy(out->code, "123456"); out->expires_in = 600;
            error->code = c.code; return c.transport;
        };
        ops.bind = [](void* p, const agentlink::cloud::Config&, const char*, cloud_bind_t* out, cloud_api_err_t* error) {
            Cloud& c = *static_cast<Cloud*>(p); ++c.binds; c.now += c.latency;
            out->state = c.bind_state; std::strcpy(out->auth_key, "issued-key");
            error->code = c.code; return c.transport;
        };
        ops.auth = [](void* p, const agentlink::cloud::Config&, const char*, cloud_api_err_t* error) {
            Cloud& c = *static_cast<Cloud*>(p); ++c.auths; c.now += c.latency;
            error->code = c.code; return c.transport;
        };
        ops.heartbeat = [](void* p, const char*, cloud_api_err_t* error) {
            Cloud& c = *static_cast<Cloud*>(p); ++c.heartbeats; c.now += c.latency;
            error->code = c.code; return c.transport;
        };
        ops.save = [](void* p, const char* key) {
            Cloud& c = *static_cast<Cloud*>(p); ++c.saves;
            if (c.storage == ESP_OK) std::strcpy(c.saved_key, key);
            return c.storage;
        };
        ops.clear = [](void* p) { Cloud& c = *static_cast<Cloud*>(p); ++c.clears; return c.storage; };
        ops.drop = [](void* p) { ++static_cast<Cloud*>(p)->drops; };
        ops.publish = [](void* p, const agent_cloud_status_t& status) { static_cast<Cloud*>(p)->status = status; };
        session.begin(config, ops, key);
    }
};

void TestCloudDeadlines() {
    Cloud c; c.begin();
    CHECK(c.session.wait_ms(false) == UINT32_MAX);
    CHECK(c.session.wait_ms(true) == 0);
    c.session.step(true);
    CHECK(c.claims == 1 && c.status.state == AGENT_CLOUD_CLAIMING);
    CHECK(c.session.wait_ms(true) == 3000);
    c.now = 2999; CHECK(c.session.wait_ms(true) == 1);
    c.session.step(true); CHECK(c.binds == 0);
    c.now = 3000; CHECK(c.session.wait_ms(true) == 0);
    c.session.step(true); CHECK(c.binds == 1 && c.session.wait_ms(true) == 3000);
    // Network edges interrupt a pending HTTP deadline immediately.
    CHECK(c.session.wait_ms(false) == 0);
    c.session.step(false);
    CHECK(c.drops == 1 && c.session.wait_ms(false) == UINT32_MAX);
    CHECK(c.session.wait_ms(true) == 0);
    c.session.step(true); CHECK(c.claims == 2);
    c.now += 3000; c.bind_state = CLOUD_BIND_BOUND;
    c.session.step(true);
    CHECK(c.session.wait_ms(true) == 0);
    // The issued key is saved even if the network disappears before the next step.
    c.session.step(false);
    CHECK(c.saves == 1 && std::strcmp(c.saved_key, "issued-key") == 0);
    CHECK(c.session.wait_ms(false) == 0);
    c.session.step(false); CHECK(c.session.wait_ms(false) == UINT32_MAX);
    c.session.step(true);
    CHECK(c.auths == 1 && c.status.state == AGENT_CLOUD_ONLINE);
    CHECK(c.session.wait_ms(true) == 60000);
    c.now += 60000; c.session.step(true);
    CHECK(c.heartbeats == 1 && c.session.wait_ms(true) == 60000);
    CHECK(c.session.forget() == ESP_OK);
    CHECK(c.clears == 1 && c.session.wait_ms(true) == 0);
    c.session.step(true); CHECK(c.claims == 3);
}

void TestCloudFailuresAndWrap() {
    Cloud c; c.now = UINT32_MAX - 2000; c.latency = 50; c.begin("saved-key");
    c.session.step(true);
    CHECK(c.session.wait_ms(true) == 60000);
    c.now += 59999; CHECK(c.session.wait_ms(true) == 1);
    c.now += 1; c.transport = ESP_FAIL; c.code = -1;
    c.session.step(true);
    CHECK(c.heartbeats == 1 && c.session.wait_ms(true) == 5000);
    c.now += 5000; c.code = CLOUD_API_NO_AGENT;
    c.session.step(true);
    CHECK(c.auths == 2 && c.session.wait_ms(true) == 30000);
    CHECK(c.status.state == AGENT_CLOUD_NO_AGENT);
    c.now += 30000; c.code = CLOUD_API_KEY_REVOKED;
    c.session.step(true);
    CHECK(c.session.wait_ms(true) == 0);
    c.storage = ESP_ERR_NO_MEM; c.session.step(false);
    CHECK(c.clears == 1 && c.session.wait_ms(false) == 30000);
    c.now += 29999; c.session.step(false); CHECK(c.clears == 1);
    c.now += 1; c.storage = ESP_OK; c.session.step(false);
    CHECK(c.clears == 2);
    c.session.step(false); CHECK(c.session.wait_ms(false) == UINT32_MAX);
    c.storage = ESP_FAIL; CHECK(c.session.forget() == ESP_FAIL);
    CHECK(c.session.wait_ms(false) == 5000);
    c.now += 5000; c.storage = ESP_OK; c.session.step(false);
    CHECK(c.session.wait_ms(false) == UINT32_MAX);
}

void TestNetworkAndCloudStatusMatrix() {
    // A stale cloud snapshot must never hide the current router/portal state.
    // Exercise every cloud state with absent, empty and assigned station IPs.
    struct CloudCase {
        agent_cloud_state_t state;
        agent_link_phase_t phase;
        const char* title;
    };
    const CloudCase cloud_cases[] = {
        {AGENT_CLOUD_IDLE, AGENT_LINK_PHASE_CONNECTING, "Connecting platform"},
        {AGENT_CLOUD_NO_NETWORK, AGENT_LINK_PHASE_CONNECTING, "Connecting platform"},
        {AGENT_CLOUD_CLAIMING, AGENT_LINK_PHASE_PAIRING, "Activation code"},
        {AGENT_CLOUD_BOUND, AGENT_LINK_PHASE_CONNECTING, "Signing in"},
        {AGENT_CLOUD_ONLINE, AGENT_LINK_PHASE_CONNECTED, nullptr},
        {AGENT_CLOUD_FAULT, AGENT_LINK_PHASE_BLOCKED, "Platform error"},
        {AGENT_CLOUD_ALREADY_BOUND, AGENT_LINK_PHASE_BLOCKED, "Already bound"},
        {AGENT_CLOUD_NO_AGENT, AGENT_LINK_PHASE_BLOCKED, "No agent"},
    };
    const char* station_ips[] = {nullptr, "", "192.168.20.33"};
    agent_link_status_t status = {}; // Reuse to detect stale fields after transitions.
    for (const CloudCase& item : cloud_cases) {
        agent_cloud_status_t cloud = {};
        cloud.state = item.state;
        cloud.api_code = 401;
        std::strcpy(cloud.code, "123456");
        cloud.expires_s = 57;
        for (unsigned configured = 0; configured != 2; ++configured) {
            for (unsigned portal = 0; portal != 2; ++portal) {
                for (const char* ip : station_ips) {
                    const bool network = ip && ip[0];
                    const bool cloud_visible = configured && !portal && network;
                    agent_cloud_compose(cloud, configured != 0, portal != 0,
                                        "Deotaland-Test-ABCD", ip, &status);
                    CHECK(status.transport == AGENT_TRANSPORT_WIFI);
                    CHECK(status.wifi_connected == network);
                    CHECK(status.wifi_portal_active == (portal != 0));
                    CHECK(std::strcmp(status.wifi_ip, network ? ip : "") == 0);
                    CHECK(status.phase != AGENT_LINK_PHASE_READY);
                    CHECK(status.title[sizeof(status.title) - 1] == 0);
                    CHECK(status.hint[sizeof(status.hint) - 1] == 0);
                    CHECK(status.code[sizeof(status.code) - 1] == 0);
                    CHECK(status.wifi_ip[sizeof(status.wifi_ip) - 1] == 0);
                    if (portal) {
                        CHECK(status.phase == AGENT_LINK_PHASE_SETUP);
                        CHECK(std::strstr(status.hint, "Deotaland-Test-ABCD") != nullptr);
                    } else if (!network) {
                        CHECK(status.phase == AGENT_LINK_PHASE_CONNECTING);
                        CHECK(std::strcmp(status.title, "Joining WiFi") == 0);
                    } else if (!configured) {
                        CHECK(status.phase == AGENT_LINK_PHASE_NETWORK_READY);
                        CHECK(status.phase != AGENT_LINK_PHASE_CONNECTED);
                    } else {
                        CHECK(status.phase == item.phase);
                        if (item.title) CHECK(std::strcmp(status.title, item.title) == 0);
                    }
                    CHECK(status.detail == (cloud_visible ? cloud.api_code : 0));
                    const bool pairing = cloud_visible && item.state == AGENT_CLOUD_CLAIMING;
                    CHECK(std::strcmp(status.code, pairing ? "123456" : "") == 0);
                    CHECK(status.expires_s == (pairing ? 57u : 0u));
                }
            }
        }
    }

    // A TLS/network request error means reconnecting to the platform, while the
    // independently reported router connection and assigned IP remain visible.
    agent_cloud_status_t cloud = {};
    cloud.state = AGENT_CLOUD_FAULT;
    cloud.api_code = -1;
    agent_cloud_compose(cloud, true, false, nullptr, "10.0.0.8", &status);
    CHECK(status.phase == AGENT_LINK_PHASE_CONNECTING);
    CHECK(std::strcmp(status.title, "Reconnecting") == 0);
    CHECK(status.detail == -1);
    CHECK(status.wifi_connected && std::strcmp(status.wifi_ip, "10.0.0.8") == 0);

    // Reuse a live activation snapshot across a physical disconnect; neither
    // the code, countdown, platform error nor old IP may survive the transition.
    cloud.state = AGENT_CLOUD_CLAIMING;
    std::strcpy(cloud.code, "654321"); cloud.expires_s = 100;
    agent_cloud_compose(cloud, true, false, nullptr, "10.0.0.8", &status);
    CHECK(status.phase == AGENT_LINK_PHASE_PAIRING && status.code[0] != 0);
    agent_cloud_compose(cloud, true, false, nullptr, nullptr, &status);
    CHECK(status.phase == AGENT_LINK_PHASE_CONNECTING);
    CHECK(!status.wifi_connected && status.wifi_ip[0] == 0);
    CHECK(status.code[0] == 0 && status.expires_s == 0 && status.detail == 0);
}

void TestNetworkStatusBounds() {
    struct GuardedStatus {
        uint32_t before;
        agent_link_status_t value;
        uint32_t after;
    } guarded = {0x12345678u, {}, 0x87654321u};
    agent_cloud_status_t cloud = {};
    cloud.state = AGENT_CLOUD_CLAIMING;
    cloud.expires_s = 42;
    // Defensive copying must terminate fixed-width input even if every byte of
    // the source code array is occupied (for example after a malformed reply).
    std::memset(cloud.code, '9', sizeof(cloud.code));
    const char* long_ip = "255.255.255.255-unexpected-suffix";
    agent_cloud_compose(cloud, true, false, nullptr, long_ip, &guarded.value);
    CHECK(std::strcmp(guarded.value.code, "9999999") == 0);
    CHECK(std::strlen(guarded.value.wifi_ip) == sizeof(guarded.value.wifi_ip) - 1);
    CHECK(std::strncmp(guarded.value.wifi_ip, long_ip, sizeof(guarded.value.wifi_ip) - 1) == 0);
    CHECK(guarded.before == 0x12345678u && guarded.after == 0x87654321u);

    char long_ssid[300]; std::memset(long_ssid, 's', sizeof(long_ssid) - 1); long_ssid[sizeof(long_ssid) - 1] = 0;
    agent_cloud_compose(cloud, false, true, long_ssid, nullptr, &guarded.value);
    CHECK(guarded.value.phase == AGENT_LINK_PHASE_SETUP);
    CHECK(guarded.value.hint[sizeof(guarded.value.hint) - 1] == 0);
    CHECK(guarded.before == 0x12345678u && guarded.after == 0x87654321u);
    agent_cloud_compose(cloud, false, true, nullptr, nullptr, &guarded.value);
    CHECK(guarded.value.phase == AGENT_LINK_PHASE_SETUP);
    CHECK(guarded.value.wifi_portal_active && !guarded.value.wifi_connected);
    CHECK(guarded.value.code[0] == 0 && guarded.value.detail == 0);
    CHECK(guarded.value.hint[sizeof(guarded.value.hint) - 1] == 0);
    agent_cloud_compose(cloud, true, true, nullptr, nullptr, nullptr);
    CHECK(guarded.before == 0x12345678u && guarded.after == 0x87654321u);
}

void TestNetworkEventOrdering() {
    using agentlink::wifi_detail::NetworkState;
    constexpr uint32_t first_ip = 0x0100000Au, second_ip = 0x0200000Au;
    NetworkState network;
    CHECK(network.address() == 0);
    const uint32_t initial_ticket = network.ticket();
    CHECK(network.ticket() == initial_ticket);
    CHECK(!network.accept(initial_ticket, 0));
    CHECK(network.address() == 0);
    CHECK(network.accept(initial_ticket, first_ip));
    CHECK(network.address() == first_ip);
    CHECK(network.ticket() == initial_ticket);
    CHECK(!network.accept(initial_ticket, 0));
    CHECK(network.address() == first_ip); // Invalid updates must not erase a valid IP.

    // GOT_IP was consumed before DISCONNECTED: loss immediately clears it.
    network.clear();
    CHECK(network.address() == 0);
    CHECK(network.ticket() != initial_ticket);
    CHECK(!network.accept(initial_ticket, first_ip));
    CHECK(network.address() == 0);

    // GOT_IP was queued but not consumed before DISCONNECTED: even a quick
    // reconnect to the same SSID cannot make that old address current again.
    const uint32_t queued_ticket = network.ticket();
    network.clear();
    CHECK(!network.accept(queued_ticket, first_ip));
    CHECK(network.address() == 0);
    const uint32_t reconnected_ticket = network.ticket();
    CHECK(network.accept(reconnected_ticket, second_ip));
    CHECK(network.address() == second_ip);
    CHECK(!network.accept(queued_ticket, first_ip));
    CHECK(!network.accept(initial_ticket, first_ip));
    CHECK(network.address() == second_ip);

    // DISCONNECTED and LOST_IP may both describe the same outage. Every old
    // ticket stays invalid, and a subsequent successful join still works.
    network.clear();
    const uint32_t disconnected_ticket = network.ticket();
    network.clear();
    CHECK(network.ticket() != disconnected_ticket);
    CHECK(!network.accept(disconnected_ticket, first_ip));
    CHECK(!network.accept(reconnected_ticket, second_ip));
    CHECK(network.address() == 0);

    // Repeated reconnects and DHCP changes exercise more than a one-time flag.
    uint32_t previous_ticket = reconnected_ticket;
    for (uint32_t cycle = 0; cycle != 128; ++cycle) {
        const uint32_t ticket = network.ticket();
        const uint32_t current_ip = first_ip + (cycle << 24);
        CHECK(network.accept(ticket, current_ip));
        CHECK(network.address() == current_ip);
        CHECK(!network.accept(previous_ticket, second_ip));
        CHECK(network.address() == current_ip);
        CHECK(network.accept(ticket, second_ip)); // Address renewal without a disconnect.
        CHECK(network.address() == second_ip);
        network.clear();
        CHECK(network.address() == 0);
        CHECK(!network.accept(ticket, current_ip));
        CHECK(network.address() == 0);
        previous_ticket = ticket;
    }
}
}
int main() {
    TestCredentials(); TestCloudDeadlines(); TestCloudFailuresAndWrap();
    TestNetworkAndCloudStatusMatrix(); TestNetworkStatusBounds();
    TestNetworkEventOrdering();
    std::printf("Network optimization tests: %u assertions passed.\n", checks);
}
