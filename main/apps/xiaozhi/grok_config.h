#pragma once

// Grok voice settings stored in NVS namespace "grok".
// The API key is never logged and is not returned by the setup page.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Printable ASCII, no spaces or quotes, long enough to be a real token.
static inline bool grok_key_acceptable(const char *key) {
    if (key == NULL) return false;
    const size_t n = strlen(key);
    if (n < 16 || n > 255) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)key[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    }
    return true;
}

// Empty host and port 0 means a direct connection. Otherwise a unicast IPv4
// and a TCP port, the same shape as the existing computer-proxy field.
static inline bool grok_proxy_acceptable(const char *host, int port) {
    unsigned parts[4];
    const char *p;
    int i;
    if (host == NULL) return false;
    if (host[0] == '\0') return port == 0;
    if (port < 1 || port > 65535) return false;
    p = host;
    for (i = 0; i < 4; ++i) {
        unsigned value = 0;
        int digits = 0;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') {
            value = value * 10u + (unsigned)(*p - '0');
            if (++digits > 3 || value > 255) return false;
            ++p;
        }
        parts[i] = value;
        if (i < 3) {
            if (*p != '.') return false;
            ++p;
        }
    }
    if (*p != '\0') return false;
    if (parts[0] == 0 || parts[0] == 127 || parts[0] >= 224) return false;
    if (parts[3] == 255) return false;
    return true;
}

// Copies the stored key into out. Returns false when missing or unacceptable.
// out is cleared on failure. The caller wipes out after the socket copies it.
bool grok_copy_key(char *out, size_t cap);
bool grok_has_key(void);
// Empty or null clears the key. A rejected key leaves the stored value as it was.
bool grok_set_key(const char *key);

void grok_proxy_get(char *host, size_t cap, uint16_t *port);
bool grok_proxy_set(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif
