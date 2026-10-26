/* A small SHA-256 for the host test tools only (fw_print). The firmware uses mbedTLS's hardware SHA. */
#pragma once
#include <stddef.h>
#include <stdint.h>
void sha256_tiny(const uint8_t *data, size_t len, uint8_t out[32]);
