#pragma once
#include <Arduino.h>
#include <agent_link_transport.h>
#include <string.h>

// Example adapter, not a stock Deotaland App transport. A UART bridge is required.
// Fixed 4-frame TX queue, 512-byte frame limit, 64 RX/TX bytes per poll.
// Stream transfers intentionally return NOT_SUPPORTED (null operation pointers).
// The Stream implementation must report free TX capacity via availableForWrite()
// and honor it without blocking. Stream's default availableForWrite() returns 0.
// The bridge must pace RX or provide hardware flow control; this queue cannot
// prevent an unrelated UART driver's hardware RX buffer from overflowing.
class SerialAgentTransport {
public:
    explicit SerialAgentTransport(Stream& serial) : port(serial) {
        transport = {};
        transport.start = start;
        transport.stop = stop;
        transport.send_ctrl = send;
        transport.is_ready = ready;
        transport.impl = this;
        transport.poll = poll;
        transport.bind = bind;
        transport.ctrl_mtu = mtu;
    }
    agent_transport_t transport;
private:
    struct Packet { uint8_t bytes[514]; size_t length; } tx[4];
    Stream& port;
    agent_transport_callbacks_t callbacks = {};
    bool running = false;
    size_t head = 0, count = 0, sent = 0;
    uint8_t rx[512], prefix[2];
    size_t prefixCount = 0, needed = 0, received = 0;
    size_t discard = 0;
    uint32_t lastByte = 0;

    static SerialAgentTransport& self(void* p) { return *static_cast<SerialAgentTransport*>(p); }
    static void bind(void* p,const agent_transport_callbacks_t* cb) { self(p).callbacks = cb ? *cb : agent_transport_callbacks_t{}; }
    static size_t mtu(void*) { return 512; }
    static bool ready(void* p) { return self(p).running; }
    static esp_err_t start(void* p) {
        auto& s=self(p);
        if (s.running) return ESP_ERR_INVALID_STATE;
        if (!s.callbacks.on_ctrl || !s.callbacks.on_connection || !s.callbacks.on_ready)
            return ESP_ERR_INVALID_STATE;
        s.running=true;
        // UART has no connection detection. A real network adapter signals these
        // only after its peer and event channel are actually usable.
        s.callbacks.on_connection(true);
        s.callbacks.on_ready();
        return ESP_OK;
    }
    static void stop(void* p) {
        auto& s=self(p);
        s.running=false; s.head=s.count=s.sent=0;
        s.prefixCount=s.needed=s.received=s.discard=0;
    }
    static esp_err_t send(void* p,const uint8_t* data,size_t n) {
        auto& s=self(p);
        if (!s.running) return ESP_ERR_INVALID_STATE;
        if (!data || n<6 || n>512) return ESP_ERR_INVALID_SIZE;
        if (s.count==4) return ESP_ERR_NO_MEM;
        auto& packet=s.tx[(s.head+s.count)%4];
        packet.bytes[0]=uint8_t(n); packet.bytes[1]=uint8_t(n>>8);
        memcpy(packet.bytes+2,data,n); packet.length=n+2; ++s.count;
        return ESP_OK;
    }
    static void poll(void* p) {
        auto& s=self(p);
        if (!s.running || !s.callbacks.on_ctrl) return;
        // Retain a complete RX frame when the core's queue is full.
        if (s.needed && s.received==s.needed) {
            const esp_err_t e=s.callbacks.on_ctrl(s.rx,s.needed);
            if (e!=ESP_ERR_NO_MEM) s.prefixCount=s.needed=s.received=0;
        }
        if ((!s.needed || s.received<s.needed) && (s.prefixCount || s.discard) &&
            uint32_t(millis()-s.lastByte)>1000)
            s.prefixCount=s.needed=s.received=s.discard=0;
        for (unsigned budget=0; budget<64 && s.port.available()>0; ++budget) {
            if (s.needed && s.received==s.needed) break;
            const int value=s.port.read();
            if (value<0) break;
            s.lastByte=millis();
            if (s.discard) { --s.discard; continue; }
            if (s.prefixCount<2) {
                s.prefix[s.prefixCount++]=uint8_t(value);
                if (s.prefixCount==2) {
                    s.needed=s.prefix[0] | (size_t(s.prefix[1])<<8);
                    if (s.needed<6 || s.needed>sizeof(s.rx)) {
                        // Drain this declared payload, rather than parsing its
                        // contents as new commands. An idle gap resets truncation.
                        s.discard=s.needed;
                        s.prefixCount=s.needed=s.received=0;
                    }
                }
            } else s.rx[s.received++]=uint8_t(value);
        }
        if (s.count) {
            auto& packet=s.tx[s.head];
            const int room=s.port.availableForWrite();
            size_t n=packet.length-s.sent;
            if (n>64) n=64;
            if (room<=0) return;
            if (n>size_t(room)) n=size_t(room);
            s.sent+=s.port.write(packet.bytes+s.sent,n);
            if (s.sent==packet.length) { s.sent=0; s.head=(s.head+1)%4; --s.count; }
        }
    }
};
