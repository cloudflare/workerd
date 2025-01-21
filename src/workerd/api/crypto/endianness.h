// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include <stdint.h>

// This file provides cross platform support for endianness conversions.
// It is intended to hide away the polluting includes of system headers to provide the functions
// without polluting the global namespace.

uint16_t htobe16(uint16_t x);
uint16_t htole16(uint16_t x);
uint16_t be16toh(uint16_t x);
uint16_t le16toh(uint16_t x);

uint32_t htobe32(uint32_t x);
uint32_t htole32(uint32_t x);
uint32_t be32toh(uint32_t x);
uint32_t le32toh(uint32_t x);

uint64_t htobe64(uint64_t x);
uint64_t htole64(uint64_t x);
uint64_t be64toh(uint64_t x);
uint64_t le64toh(uint64_t x);
