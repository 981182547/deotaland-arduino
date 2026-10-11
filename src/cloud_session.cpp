#include "cloud_session.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace agentlink { namespace cloud {
void Session::begin(const Config& config, const Ops& ops, const char* saved_key) {
    *this = Session();
    config_ = config; ops_ = ops;
    snprintf(key_, sizeof(key_), "%s", saved_key ? saved_key : "");
    stage_ = key_[0] ? Auth : Claim;
    due_ = ops_.now(ops_.ctx);
    publish(AGENT_CLOUD_NO_NETWORK);
}
void Session::publish(agent_cloud_state_t state, const cloud_api_err_t* error) {
    status_.state = state;
    status_.api_code = error ? error->code : 0;
    snprintf(status_.message, sizeof(status_.message), "%s", error ? error->message : "");
    if (state != AGENT_CLOUD_CLAIMING) { status_.code[0] = 0; status_.expires_s = 0; }
    ops_.publish(ops_.ctx, status_);
}
void Session::later(uint32_t milliseconds) { due_ = ops_.now(ops_.ctx) + milliseconds; }
void Session::accepted() { unanswered_ = 0; refusal_ = false; }
void Session::failure(const cloud_api_err_t& error, agent_cloud_state_t state) {
    if (error.code < 0) {
        if (unanswered_ < 3) ++unanswered_;
        if (refusal_ && unanswered_ < 3) { later(5000); return; }
        refusal_ = false;
    } else { unanswered_ = 0; refusal_ = true; }
    publish(state, &error);
    later(error.code > 0 ? 30000 : 5000);
}
void Session::codeStatus() {
    const uint32_t elapsed = (ops_.now(ops_.ctx) - code_at_) / 1000;
    status_.expires_s = elapsed < code_ttl_ ? code_ttl_ - elapsed : 0;
    snprintf(status_.code, sizeof(status_.code), "%s", code_);
    publish(AGENT_CLOUD_CLAIMING);
}
bool Session::revoked(const cloud_api_err_t& error) {
    if (error.code != CLOUD_API_KEY_REVOKED) return false;
    stage_ = Clear;
    key_[0] = 0;
    publish(AGENT_CLOUD_BOUND);
    later(0);
    return true;
}
esp_err_t Session::forget() {
    ops_.drop(ops_.ctx);
    key_[0] = code_[0] = 0;
    stage_ = Clear;
    accepted();
    const esp_err_t result = ops_.clear(ops_.ctx);
    if (result == ESP_OK) {
        stage_ = Claim; later(0);
        publish(AGENT_CLOUD_NO_NETWORK);
    } else {
        cloud_api_err_t error = {}; error.code = result;
        snprintf(error.message,sizeof(error.message),"Credential erase failed");
        failure(error,AGENT_CLOUD_FAULT);
    }
    return result;
}
void Session::persist(bool network) {
    const bool clearing = stage_ == Clear;
    const esp_err_t result = clearing ? ops_.clear(ops_.ctx) : ops_.save(ops_.ctx,key_);
    if (result != ESP_OK) {
        cloud_api_err_t error = {}; error.code = result;
        snprintf(error.message,sizeof(error.message),"Credential storage failed");
        failure(error,AGENT_CLOUD_FAULT); return;
    }
    accepted(); stage_ = clearing ? Claim : Auth;
    later(0);
    publish(network ? AGENT_CLOUD_BOUND : AGENT_CLOUD_NO_NETWORK);
}
uint32_t Session::wait_ms(bool network) const {
    // Storage retries survive network loss; all other work requires a link.
    if (stage_ != Persist && stage_ != Clear) {
        if (network != network_) return 0;
        if (!network) return UINT32_MAX;
    }
    const uint32_t now = ops_.now(ops_.ctx);
    return static_cast<int32_t>(now - due_) >= 0 ? 0 : due_ - now;
}
void Session::step(bool network) {
    const uint32_t now = ops_.now(ops_.ctx);
    // Never lose a just-issued key because persistence failed or WiFi went away.
    // Keep retrying the same key; the platform need not issue it a second time.
    if (stage_ == Persist || stage_ == Clear) {
        if (static_cast<int32_t>(now - due_) < 0) return;
        persist(network);
        return;
    }
    if (!network) {
        if (network_ || status_.state != AGENT_CLOUD_NO_NETWORK) {
            ops_.drop(ops_.ctx);
            publish(AGENT_CLOUD_NO_NETWORK);
        }
        network_ = false;
        stage_ = key_[0] ? Auth : Claim;
        return;
    }
    if (!network_) { network_ = true; later(0); }
    if (static_cast<int32_t>(now - due_) < 0) return;
    if (!ops_.valid(ops_.ctx)) return;
    cloud_api_err_t error = {}; error.code = -1;
    switch (stage_) {
    case Claim: {
        cloud_claim_t claim = {};
        const esp_err_t result = ops_.claim(ops_.ctx,config_,&claim,&error);
        if (!ops_.valid(ops_.ctx)) return;
        if (result != ESP_OK) {
            failure(error, error.code == CLOUD_API_ALREADY_BOUND ? AGENT_CLOUD_ALREADY_BOUND : AGENT_CLOUD_FAULT);
            return;
        }
        accepted();
        snprintf(code_,sizeof(code_),"%s",claim.code);
        code_at_ = ops_.now(ops_.ctx); code_ttl_ = std::min(claim.expires_in,uint32_t(86400));
        stage_ = Bind; codeStatus(); later(3000);
        break;
    }
    case Bind: {
        const uint32_t elapsed = (now - code_at_) / 1000;
        if (elapsed >= code_ttl_ || code_ttl_ - elapsed <= 20) { stage_ = Claim; later(0); return; }
        cloud_bind_t bind = {};
        const esp_err_t result = ops_.bind(ops_.ctx,config_,code_,&bind,&error);
        const bool current = ops_.valid(ops_.ctx);
        if (result == ESP_OK && bind.state == CLOUD_BIND_BOUND) {
            // This key belongs to our device identity even if WiFi disappeared
            // while the request completed. Forget still rejects obsolete keys.
            if (ops_.accept_binding ? !ops_.accept_binding(ops_.ctx) : !current) return;
            accepted();
            snprintf(key_,sizeof(key_),"%s",bind.auth_key);
            if (!key_[0]) { error.code=-1; failure(error,AGENT_CLOUD_FAULT); return; }
            code_[0] = 0; stage_ = Persist; later(0);
            // Save once before returning: stop() may prevent another step.
            // Failed writes retain this same key in Persist for retry.
            persist(current);
            return;
        }
        if (!current) return;
        if (result != ESP_OK) {
            // Account for request latency using the real clock, not polling count.
            if (error.code > 0) failure(error,AGENT_CLOUD_FAULT);
            else { codeStatus(); later(3000); }
            return;
        }
        accepted();
        if (bind.state == CLOUD_BIND_EXPIRED) { stage_ = Claim; later(0); }
        else {
            if (bind.remaining_ttl) {
                const uint32_t spent = (ops_.now(ops_.ctx)-code_at_)/1000;
                const uint32_t local = spent < code_ttl_ ? code_ttl_-spent : 0;
                code_ttl_ = std::min(local,bind.remaining_ttl); code_at_ = ops_.now(ops_.ctx);
            }
            codeStatus(); later(3000);
        }
        break;
    }
    case Auth: {
        if (!refusal_) publish(AGENT_CLOUD_BOUND);
        const esp_err_t result = ops_.auth(ops_.ctx,config_,key_,&error);
        if (!ops_.valid(ops_.ctx)) return;
        if (result != ESP_OK) {
            if (!revoked(error)) failure(error,error.code == CLOUD_API_NO_AGENT ? AGENT_CLOUD_NO_AGENT : AGENT_CLOUD_FAULT);
            return;
        }
        accepted(); stage_ = Heartbeat;
        publish(AGENT_CLOUD_ONLINE); later(60000);
        break;
    }
    case Heartbeat: {
        const esp_err_t result = ops_.heartbeat(ops_.ctx,key_,&error);
        if (!ops_.valid(ops_.ctx)) return;
        if (result == ESP_OK) { accepted(); later(60000); }
        else if (!revoked(error)) {
            // Re-authenticate after a failed heartbeat; do not claim to be online forever.
            stage_ = Auth;
            failure(error,error.code == CLOUD_API_NO_AGENT ? AGENT_CLOUD_NO_AGENT : AGENT_CLOUD_FAULT);
        }
        break;
    }
    default: break;
    }
}
} }
