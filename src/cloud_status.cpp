#include "cloud_status.h"
#include "agent_link.h"
#include <cstdio>
#include <cstring>
void agent_cloud_compose(const agent_cloud_status_t& c, bool configured, bool portal,
                         const char* ap_ssid, const char* station_ip, agent_link_status_t* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->transport = AGENT_TRANSPORT_WIFI;
    out->wifi_connected = station_ip && station_ip[0];
    out->wifi_portal_active = portal;
    if (out->wifi_connected) snprintf(out->wifi_ip, sizeof out->wifi_ip, "%s", station_ip);
    auto say = [out](agent_link_phase_t phase, const char* title, const char* hint) {
        out->phase = phase;
        snprintf(out->title, sizeof out->title, "%s", title);
        snprintf(out->hint,  sizeof out->hint,  "%s", hint);
    };

    // Network state takes precedence over stale cloud worker snapshots. A
    // station-only sketch must expose the same provisioning/joining progress.
    if (portal) {
        say(AGENT_LINK_PHASE_SETUP, "Set up WiFi", "");
        snprintf(out->hint, sizeof out->hint,
                 "Join the %s hotspot from your phone, then pick your network",
                 ap_ssid && ap_ssid[0] ? ap_ssid : "device");
        return;
    }
    if (!out->wifi_connected) {
        say(AGENT_LINK_PHASE_CONNECTING, "Joining WiFi", "Connecting to the configured or saved network");
        return;
    }
    if (!configured) {
        say(AGENT_LINK_PHASE_NETWORK_READY, "WiFi connected",
            "Set config.platform to enable platform activation and authentication");
        return;
    }

    out->detail = c.api_code;
    switch (c.state) {
    case AGENT_CLOUD_IDLE:
    case AGENT_CLOUD_NO_NETWORK:
        say(AGENT_LINK_PHASE_CONNECTING, "Connecting platform", "WiFi connected; waiting for platform activation or authentication");
        break;

    case AGENT_CLOUD_CLAIMING:
        say(AGENT_LINK_PHASE_PAIRING, "Activation code", "");
        snprintf(out->code, sizeof out->code, "%.7s", c.code);
        out->expires_s = c.expires_s;
        snprintf(out->hint, sizeof out->hint, "Enter it in the Agent Link console - %us left",
                 static_cast<unsigned>(c.expires_s));
        break;

    case AGENT_CLOUD_BOUND:
        say(AGENT_LINK_PHASE_CONNECTING, "Signing in", "Authenticating with the platform");
        break;

    case AGENT_CLOUD_ONLINE:
        // Not READY: platform authentication is not a runtime data connection.
        say(AGENT_LINK_PHASE_CONNECTED, "Platform online", "Authenticated; WiFi control and media transport are not implemented");
        break;

    case AGENT_CLOUD_ALREADY_BOUND:
        say(AGENT_LINK_PHASE_BLOCKED, "Already bound",
            "This device is claimed by an account. Unbind it in the console to activate it again.");
        break;

    case AGENT_CLOUD_NO_AGENT:
        say(AGENT_LINK_PHASE_BLOCKED, "No agent",
            "Bound, but no agent is attached. Pick one for this device in the console.");
        break;

    case AGENT_CLOUD_FAULT:
    default:
        if (c.api_code < 0) {
            // No response (socket or TLS failure); the session is retrying.
            say(AGENT_LINK_PHASE_CONNECTING, "Reconnecting", "Cannot reach the platform - retrying");
        } else if (c.api_code == 401) {
            say(AGENT_LINK_PHASE_BLOCKED, "Platform error",
                "The platform gateway is refusing device requests");
        } else if (c.api_code == 1 || c.api_code == 400) {
            say(AGENT_LINK_PHASE_BLOCKED, "Platform error",
                "The firmware sent a request the platform rejected");
        } else {
            say(AGENT_LINK_PHASE_BLOCKED, "Platform error",
                "The platform refused this device - retrying");
        }
        break;
    }
}
