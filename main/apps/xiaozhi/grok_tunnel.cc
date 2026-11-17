#include "apps/xiaozhi/grok_tunnel.h"

#include "esp_log.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr const char *TAG = "GrokTunnel";
constexpr const char *kUpstreamHost = "api.x.ai";
constexpr int kUpstreamPort = 443;
constexpr int kConnectTimeoutMs = 10000;
} // namespace

bool grok_tunnel_open(GrokTunnel *out, const char *proxy_host, uint16_t proxy_port,
                      const char *path, const char *headers) {
    if (!out || !proxy_host || !proxy_host[0] || proxy_port == 0 || !path) return false;
    std::memset(out, 0, sizeof(*out));

    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) return false;
    if (esp_transport_connect(tcp, proxy_host, proxy_port, kConnectTimeoutMs) != 0) {
        ESP_LOGE(TAG, "proxy TCP connect failed");
        esp_transport_destroy(tcp);
        return false;
    }

    char req[160];
    std::snprintf(req, sizeof(req),
                  "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\nProxy-Connection: Keep-Alive\r\n\r\n",
                  kUpstreamHost, kUpstreamPort, kUpstreamHost, kUpstreamPort);
    if (esp_transport_write(tcp, req, (int)std::strlen(req), kConnectTimeoutMs) < 0) {
        ESP_LOGE(TAG, "proxy CONNECT write failed");
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }

    char resp[256];
    int n = esp_transport_read(tcp, resp, (int)sizeof(resp) - 1, kConnectTimeoutMs);
    if (n <= 0) {
        ESP_LOGE(TAG, "proxy CONNECT read failed");
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }
    resp[n] = '\0';
    if (std::strstr(resp, " 200 ") == nullptr && std::strstr(resp, " 200\r") == nullptr) {
        ESP_LOGE(TAG, "proxy CONNECT rejected");
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }

    esp_transport_handle_t ssl = esp_transport_ssl_init();
    if (!ssl) {
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }
    esp_transport_ssl_set_underlying_transport(ssl, tcp);
    esp_transport_ssl_enable_global_ca_store(ssl);
    if (esp_transport_connect(ssl, kUpstreamHost, kUpstreamPort, kConnectTimeoutMs) != 0) {
        ESP_LOGE(TAG, "TLS through proxy failed");
        esp_transport_close(ssl);
        esp_transport_destroy(ssl);
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }

    esp_transport_handle_t ws = esp_transport_ws_init(ssl);
    if (!ws) {
        esp_transport_close(ssl);
        esp_transport_destroy(ssl);
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }
    esp_transport_ws_set_path(ws, path);
    if (headers && headers[0]) {
        esp_transport_ws_set_headers(ws, headers);
    }
    if (esp_transport_connect(ws, kUpstreamHost, kUpstreamPort, kConnectTimeoutMs) != 0) {
        ESP_LOGE(TAG, "WebSocket through proxy failed");
        esp_transport_close(ws);
        esp_transport_destroy(ws);
        esp_transport_close(ssl);
        esp_transport_destroy(ssl);
        esp_transport_close(tcp);
        esp_transport_destroy(tcp);
        return false;
    }

    out->stream = ssl;
    out->ws = ws;
    // Keep tcp alive under ssl; destroy order is ws → ssl → tcp in destroy.
    // The TCP handle is owned by SSL's underlying transport in some IDF builds;
    // we only track stream+ws and close them in reverse.
    (void)tcp;
    return true;
}

void grok_tunnel_destroy(GrokTunnel *tunnel) {
    if (!tunnel) return;
    if (tunnel->ws) {
        esp_transport_close(tunnel->ws);
        esp_transport_destroy(tunnel->ws);
        tunnel->ws = nullptr;
    }
    if (tunnel->stream) {
        esp_transport_close(tunnel->stream);
        esp_transport_destroy(tunnel->stream);
        tunnel->stream = nullptr;
    }
}
