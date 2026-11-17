#include "apps/xiaozhi/grok_config.h"
#include "apps/xiaozhi/xiaozhi_store.h"

#include <nvs.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kNs = "grok";
constexpr const char *kKey = "api_key";
constexpr const char *kProxyHost = "px_host";
constexpr const char *kProxyPort = "px_port";

bool erase_ok(const char *key) {
    nvs_handle_t handle;
    if (nvs_open(kNs, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

} // namespace

bool grok_copy_key(char *out, size_t cap) {
    if (out == nullptr || cap < 2) return false;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(kNs, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t len = cap;
    const esp_err_t err = nvs_get_str(handle, kKey, out, &len);
    nvs_close(handle);
    if (err != ESP_OK || !grok_key_acceptable(out)) {
        std::memset(out, 0, cap);
        return false;
    }
    return true;
}

bool grok_has_key(void) {
    char key[256];
    const bool ok = grok_copy_key(key, sizeof(key));
    std::memset(key, 0, sizeof(key));
    return ok;
}

bool grok_set_key(const char *key) {
    if (key == nullptr || key[0] == '\0') return erase_ok(kKey);
    if (!grok_key_acceptable(key)) return false;
    return store_set_string(kNs, kKey, key);
}

void grok_proxy_get(char *host, size_t cap, uint16_t *port) {
    if (port) *port = 0;
    if (host && cap) host[0] = '\0';
    const std::string stored = store_get_string(kNs, kProxyHost, "");
    const int stored_port = store_get_int(kNs, kProxyPort, 0);
    if (!grok_proxy_acceptable(stored.c_str(), stored_port)) return;
    if (host && cap) std::snprintf(host, cap, "%s", stored.c_str());
    if (port) *port = static_cast<uint16_t>(stored_port);
}

bool grok_proxy_set(const char *host, uint16_t port) {
    const char *value = host ? host : "";
    if (!grok_proxy_acceptable(value, port)) return false;
    if (value[0] == '\0') {
        const bool host_ok = erase_ok(kProxyHost);
        const bool port_ok = erase_ok(kProxyPort);
        return host_ok && port_ok;
    }
    return store_set_string(kNs, kProxyHost, value) &&
           store_set_int(kNs, kProxyPort, port);
}
