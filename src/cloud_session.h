#pragma once
// Portable state machine. step() may make ONE blocking request, so only call it
// on a dedicated worker. Arduino loop() only reads the published status mailbox.
#include "cloud_api.h"
#include "cloud_status.h"

namespace agentlink { namespace cloud {
struct Config {
    int product_id;
    const char* sn;
    const char* mac;
    const char* chip;
    const char* firmware;
};
struct Ops {
    void* ctx;
    uint32_t (*now)(void*);
    // Rechecked AFTER each request. Reject status replies from an obsolete link.
    bool (*valid)(void*);
    esp_err_t (*claim)(void*, const Config&, cloud_claim_t*, cloud_api_err_t*);
    esp_err_t (*bind)(void*, const Config&, const char*, cloud_bind_t*, cloud_api_err_t*);
    esp_err_t (*auth)(void*, const Config&, const char*, cloud_api_err_t*);
    esp_err_t (*heartbeat)(void*, const char*, cloud_api_err_t*);
    esp_err_t (*save)(void*, const char*);
    esp_err_t (*clear)(void*);
    void (*drop)(void*);
    void (*publish)(void*, const agent_cloud_status_t&);
    // A successful BOUND reply contains the credential needed on the next boot.
    // Network loss/stop must not discard it; only an explicit forget does.
    // If omitted, valid() retains its original cancellation semantics.
    bool (*accept_binding)(void*) = nullptr;
};
class Session {
public:
    void begin(const Config& config, const Ops& ops, const char* saved_key);
    void step(bool network);
    // Delay to the next useful step. UINT32_MAX means wait for a network/stop
    // event; zero means immediately continue a state transition. Wrap-safe for
    // the session's deadlines (all shorter than half the uint32_t clock range).
    uint32_t wait_ms(bool network) const;
    esp_err_t forget();
private:
    enum Stage { Claim, Bind, Persist, Auth, Heartbeat, Clear };
    Config config_ = {};
    Ops ops_ = {};
    Stage stage_ = Claim;
    char key_[65] = {};
    char code_[8] = {};
    uint32_t due_ = 0, code_at_ = 0, code_ttl_ = 0;
    bool network_ = false, refusal_ = false;
    unsigned unanswered_ = 0;
    agent_cloud_status_t status_ = {};
    void publish(agent_cloud_state_t state, const cloud_api_err_t* error = nullptr);
    void failure(const cloud_api_err_t& error, agent_cloud_state_t state);
    void accepted();
    void later(uint32_t milliseconds);
    void codeStatus();
    bool revoked(const cloud_api_err_t& error);
    void persist(bool network);
};
} }
