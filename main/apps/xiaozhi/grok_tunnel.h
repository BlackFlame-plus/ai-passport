#pragma once

// Optional HTTP CONNECT + TLS parent transport for the WebSocket client.
// Built only for the device. The direct path does not use it.
//
// grok_tunnel_open builds an unconnected WS transport whose parent does
// CONNECT to the LAN proxy, then TLS to api.x.ai. The websocket client
// owns the connect call; destroy the handles only after the client stops.

#include <stdint.h>
#include "esp_transport.h"

struct GrokTunnel {
    esp_transport_handle_t stream; // CONNECT + TLS parent (owned)
    esp_transport_handle_t ws;     // WebSocket framing over stream (owned)
};

// headers must stay valid until grok_tunnel_destroy (WS may keep the pointer).
bool grok_tunnel_open(GrokTunnel *out, const char *proxy_host, uint16_t proxy_port,
                      const char *path, const char *headers);
void grok_tunnel_destroy(GrokTunnel *tunnel);
