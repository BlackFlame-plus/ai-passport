#pragma once

// Optional HTTP CONNECT tunnel in front of the existing WebSocket client.
// Built only for the device. The direct path does not use it.

#include <stdint.h>
#include "esp_transport.h"

struct GrokTunnel {
    esp_transport_handle_t stream;
    esp_transport_handle_t ws;
};

// stream is CONNECT + one TLS session. ws is the WebSocket framing on top of it.
// headers must stay valid until grok_tunnel_destroy (the transport may keep the pointer).
bool grok_tunnel_open(GrokTunnel *out, const char *proxy_host, uint16_t proxy_port,
                      const char *path, const char *headers);
void grok_tunnel_destroy(GrokTunnel *tunnel);
