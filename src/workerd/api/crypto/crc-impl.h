// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include <stdint.h>

// crc32-c implementation according to the spec:
// https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-32-iscsi
uint32_t crc32c(uint32_t crc, const uint8_t *data, unsigned int length);

// crc64-nvme implementation according to the spec:
// https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-64-nvme
uint64_t crc64nvme(uint64_t crc, const uint8_t *data, unsigned int length);
