#pragma once
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

namespace agentlink { namespace wifi_detail {
// The event task invalidates queued GOT_IP work when an address is lost.
// Protect both the generation and IP together: a delayed worker must never
// restore an address from before a disconnect (even to the same SSID).
class NetworkState {
public:
    uint32_t ticket() { Guard guard(*this); return generation_; }
    uint32_t address() { Guard guard(*this); return address_; }
    void clear() { Guard guard(*this); ++generation_; address_ = 0; }
    bool accept(uint32_t ticket, uint32_t address) {
        Guard guard(*this);
        if (ticket != generation_ || !address) return false;
        address_ = address;
        return true;
    }
private:
#if defined(ARDUINO_ARCH_ESP32)
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    struct Guard {
        NetworkState& state;
        explicit Guard(NetworkState& s) : state(s) { portENTER_CRITICAL(&state.lock_); }
        ~Guard() { portEXIT_CRITICAL(&state.lock_); }
    };
#else
    // Host tests model ordering deterministically; this backend has no workers.
    struct Guard { explicit Guard(NetworkState&) {} };
#endif
    uint32_t generation_ = 0;
    uint32_t address_ = 0;
};
} }
