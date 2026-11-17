// Host-only RFC 4648 Base64 matching the mbedtls encode/decode API shape.
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

#include <string.h>

void mbedtls_platform_zeroize(void *buf, size_t len) {
    if (buf == NULL || len == 0) return;
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) *p++ = 0;
}

static const unsigned char kEnc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int decode_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen) {
    if (olen == NULL) return -1;
    size_t need = 4 * ((slen + 2) / 3);
    if (need / 4 != (slen + 2) / 3) {
        *olen = (size_t)-1;
        return -1;
    }
    if (dst == NULL || dlen < need + 1) {
        *olen = need + 1;
        return dst == NULL ? 0 : MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }
    size_t i = 0, o = 0;
    while (i + 3 <= slen) {
        unsigned n = ((unsigned)src[i] << 16) | ((unsigned)src[i + 1] << 8) | src[i + 2];
        dst[o++] = kEnc[(n >> 18) & 63];
        dst[o++] = kEnc[(n >> 12) & 63];
        dst[o++] = kEnc[(n >> 6) & 63];
        dst[o++] = kEnc[n & 63];
        i += 3;
    }
    if (i < slen) {
        unsigned n = (unsigned)src[i] << 16;
        dst[o++] = kEnc[(n >> 18) & 63];
        if (i + 1 < slen) {
            n |= (unsigned)src[i + 1] << 8;
            dst[o++] = kEnc[(n >> 12) & 63];
            dst[o++] = kEnc[(n >> 6) & 63];
            dst[o++] = '=';
        } else {
            dst[o++] = kEnc[(n >> 12) & 63];
            dst[o++] = '=';
            dst[o++] = '=';
        }
    }
    dst[o] = '\0';
    *olen = o;
    return 0;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen) {
    if (olen == NULL) return -1;
    *olen = 0;
    if (src == NULL) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    while (slen > 0 && (src[slen - 1] == ' ' || src[slen - 1] == '\t' ||
                        src[slen - 1] == '\r' || src[slen - 1] == '\n')) {
        --slen;
    }
    if (slen == 0) {
        *olen = 0;
        return 0;
    }
    if ((slen % 4) != 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;

    size_t pad = 0;
    if (src[slen - 1] == '=') {
        pad = 1;
        if (slen >= 2 && src[slen - 2] == '=') pad = 2;
    }
    size_t out_len = (slen / 4) * 3 - pad;
    if (dst == NULL) {
        *olen = out_len;
        return 0;
    }
    if (dlen < out_len) {
        *olen = out_len;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }

    size_t o = 0;
    for (size_t i = 0; i < slen; i += 4) {
        int v0 = decode_value(src[i]);
        int v1 = decode_value(src[i + 1]);
        int v2 = src[i + 2] == '=' ? 0 : decode_value(src[i + 2]);
        int v3 = src[i + 3] == '=' ? 0 : decode_value(src[i + 3]);
        if (v0 < 0 || v1 < 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        if (src[i + 2] != '=' && v2 < 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        if (src[i + 3] != '=' && v3 < 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        if (src[i + 2] == '=' && src[i + 3] != '=') return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        if ((src[i + 2] == '=' || src[i + 3] == '=') && i + 4 != slen) {
            return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        }
        unsigned n = ((unsigned)v0 << 18) | ((unsigned)v1 << 12) |
                     ((unsigned)v2 << 6) | (unsigned)v3;
        if (o < out_len) dst[o++] = (unsigned char)((n >> 16) & 0xff);
        if (src[i + 2] != '=' && o < out_len) dst[o++] = (unsigned char)((n >> 8) & 0xff);
        if (src[i + 3] != '=' && o < out_len) dst[o++] = (unsigned char)(n & 0xff);
    }
    *olen = o;
    return 0;
}
