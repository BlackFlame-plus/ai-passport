#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_platform_zeroize(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
