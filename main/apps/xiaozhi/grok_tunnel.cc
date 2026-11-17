#include "apps/xiaozhi/grok_tunnel.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_transport.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr const char *TAG = "GrokTunnel";
constexpr int kUpstreamPort = 443;

struct ProxyTls {
    char proxy_host[16];
    uint16_t proxy_port;
    esp_tls_t *tls;
};

int set_timeouts(int fd, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    return 0;
}

int tcp_connect(const char *host, uint16_t port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));
    addrinfo *res = nullptr;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (set_timeouts(fd, timeout_ms) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += static_cast<size_t>(n);
    }
    return 0;
}

bool read_connect_ok(int fd) {
    char resp[256];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        const ssize_t r = recv(fd, resp + n, 1, 0);
        if (r <= 0) return false;
        ++n;
        if (n >= 4 && std::memcmp(resp + n - 4, "\r\n\r\n", 4) == 0) break;
    }
    resp[n] = '\0';
    return std::strstr(resp, " 200 ") != nullptr || std::strstr(resp, " 200\r") != nullptr;
}

int proxy_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms) {
    auto *ctx = static_cast<ProxyTls *>(esp_transport_get_context_data(t));
    if (!ctx || !host || port <= 0) return -1;
    if (ctx->tls) {
        esp_tls_conn_destroy(ctx->tls);
        ctx->tls = nullptr;
    }

    const int fd = tcp_connect(ctx->proxy_host, ctx->proxy_port, timeout_ms);
    if (fd < 0) {
        ESP_LOGE(TAG, "proxy TCP connect failed");
        return -1;
    }

    char req[160];
    const int req_len = std::snprintf(
        req, sizeof(req),
        "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\nProxy-Connection: Keep-Alive\r\n\r\n",
        host, port, host, port);
    if (req_len <= 0 || req_len >= static_cast<int>(sizeof(req)) ||
        send_all(fd, req, static_cast<size_t>(req_len)) != 0) {
        ESP_LOGE(TAG, "proxy CONNECT write failed");
        close(fd);
        return -1;
    }
    if (!read_connect_ok(fd)) {
        ESP_LOGE(TAG, "proxy CONNECT rejected");
        close(fd);
        return -1;
    }

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        close(fd);
        return -1;
    }
    if (esp_tls_set_conn_sockfd(tls, fd) != ESP_OK ||
        esp_tls_set_conn_state(tls, ESP_TLS_CONNECTING) != ESP_OK) {
        esp_tls_conn_destroy(tls);
        return -1;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;
    // Socket is already connected. Drive the handshake until done or timeout.
    const int64_t deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(cfg.timeout_ms) * 1000;
    int r = 0;
    while (esp_timer_get_time() < deadline_us) {
        r = esp_tls_conn_new_async(host, static_cast<int>(std::strlen(host)), port, &cfg, tls);
        if (r != 0) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (r != 1) {
        ESP_LOGE(TAG, "TLS through proxy failed");
        esp_tls_conn_destroy(tls);
        return -1;
    }
    ctx->tls = tls;
    return 0;
}

int proxy_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms) {
    auto *ctx = static_cast<ProxyTls *>(esp_transport_get_context_data(t));
    if (!ctx || !ctx->tls || !buffer || len <= 0) return -1;
    int sock = -1;
    if (esp_tls_get_conn_sockfd(ctx->tls, &sock) == ESP_OK && sock >= 0) {
        set_timeouts(sock, timeout_ms);
    }
    const ssize_t n = esp_tls_conn_read(ctx->tls, buffer, static_cast<size_t>(len));
    if (n == 0) return 0;
    if (n < 0) {
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
        return -1;
    }
    return static_cast<int>(n);
}

int proxy_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms) {
    auto *ctx = static_cast<ProxyTls *>(esp_transport_get_context_data(t));
    if (!ctx || !ctx->tls || !buffer || len <= 0) return -1;
    int sock = -1;
    if (esp_tls_get_conn_sockfd(ctx->tls, &sock) == ESP_OK && sock >= 0) {
        set_timeouts(sock, timeout_ms);
    }
    const ssize_t n = esp_tls_conn_write(ctx->tls, buffer, static_cast<size_t>(len));
    if (n < 0) {
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
        return -1;
    }
    return static_cast<int>(n);
}

int proxy_poll(esp_transport_handle_t t, int timeout_ms, bool for_read) {
    auto *ctx = static_cast<ProxyTls *>(esp_transport_get_context_data(t));
    if (!ctx || !ctx->tls) return -1;
    int sock = -1;
    if (esp_tls_get_conn_sockfd(ctx->tls, &sock) != ESP_OK || sock < 0) return -1;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    timeval tv{};
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    const int rc = select(sock + 1, for_read ? &set : nullptr, for_read ? nullptr : &set,
                          nullptr, timeout_ms >= 0 ? &tv : nullptr);
    if (rc < 0) return -1;
    return rc;
}

int proxy_poll_read(esp_transport_handle_t t, int timeout_ms) {
    return proxy_poll(t, timeout_ms, true);
}

int proxy_poll_write(esp_transport_handle_t t, int timeout_ms) {
    return proxy_poll(t, timeout_ms, false);
}

int proxy_close(esp_transport_handle_t t) {
    auto *ctx = static_cast<ProxyTls *>(esp_transport_get_context_data(t));
    if (!ctx) return -1;
    if (ctx->tls) {
        esp_tls_conn_destroy(ctx->tls);
        ctx->tls = nullptr;
    }
    return 0;
}

int proxy_destroy(esp_transport_handle_t t) {
    auto *ctx = static_cast<ProxyTls *>(esp_transport_get_context_data(t));
    if (!ctx) return 0;
    proxy_close(t);
    delete ctx;
    esp_transport_set_context_data(t, nullptr);
    return 0;
}

} // namespace

bool grok_tunnel_open(GrokTunnel *out, const char *proxy_host, uint16_t proxy_port,
                      const char *path, const char *headers) {
    if (!out || !proxy_host || !proxy_host[0] || proxy_port == 0 || !path) return false;
    std::memset(out, 0, sizeof(*out));

    auto *ctx = new (std::nothrow) ProxyTls{};
    if (!ctx) return false;
    std::snprintf(ctx->proxy_host, sizeof(ctx->proxy_host), "%s", proxy_host);
    ctx->proxy_port = proxy_port;
    ctx->tls = nullptr;

    esp_transport_handle_t stream = esp_transport_init();
    if (!stream) {
        delete ctx;
        return false;
    }
    esp_transport_set_context_data(stream, ctx);
    esp_transport_set_func(stream, proxy_connect, proxy_read, proxy_write, proxy_close,
                           proxy_poll_read, proxy_poll_write, proxy_destroy);
    esp_transport_set_default_port(stream, kUpstreamPort);

    esp_transport_handle_t ws = esp_transport_ws_init(stream);
    if (!ws) {
        esp_transport_destroy(stream);
        return false;
    }
    esp_transport_ws_set_path(ws, path);
    if (headers && headers[0]) {
        esp_transport_ws_set_headers(ws, headers);
    }

    out->stream = stream;
    out->ws = ws;
    return true;
}

void grok_tunnel_destroy(GrokTunnel *tunnel) {
    if (!tunnel) return;
    // Destroy WS first; it does not own/destroy the parent stream.
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
