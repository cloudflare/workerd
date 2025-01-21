// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crc-impl.h"

#include <array>
#include <type_traits>

namespace {
constexpr auto crcTableSize = 256;

template <typename T>
concept Uint64OrUint32 = std::unsigned_integral<T> && (sizeof(T) == 8 || sizeof(T) == 4);

template <Uint64OrUint32 T>
constexpr T reverse(T value) {
  if constexpr (sizeof(T) == 4) {
    value = ((value & 0xaaaaaaaa) >> 1) | ((value & 0x55555555) << 1);
    value = ((value & 0xcccccccc) >> 2) | ((value & 0x33333333) << 2);
    value = ((value & 0xf0f0f0f0) >> 4) | ((value & 0x0f0f0f0f) << 4);
    value = ((value & 0xff00ff00) >> 8) | ((value & 0x00ff00ff) << 8);
    value = (value >> 16) | (value << 16);
    return value;
  } else {
    value = ((value & 0xaaaaaaaaaaaaaaaa) >> 1) | ((value & 0x5555555555555555) << 1);
    value = ((value & 0xcccccccccccccccc) >> 2) | ((value & 0x3333333333333333) << 2);
    value = ((value & 0xf0f0f0f0f0f0f0f0) >> 4) | ((value & 0x0f0f0f0f0f0f0f0f) << 4);
    value = ((value & 0xff00ff00ff00ff00) >> 8) | ((value & 0x00ff00ff00ff00ff) << 8);
    value = ((value & 0xffff0000ffff0000) >> 16) | ((value & 0x0000ffff0000ffff) << 16);
    value = (value >> 32) | (value << 32);
    return value;
  }
}

template <Uint64OrUint32 T>
constexpr std::array<T, crcTableSize> gen_crc_table(T polynomial, bool reflectIn, bool reflectOut) {
  constexpr auto numIterations = sizeof(polynomial) * 8;  // number of bits in polynomial
  auto crcTable = std::array<T, crcTableSize>{};

  for (T byte = 0u; byte < crcTableSize; ++byte) {
    T crc = (reflectIn ? (reverse(T(byte)) >> (numIterations - 8)) : byte);

    for (int i = 0; i < numIterations; ++i) {
      if (crc & (static_cast<T>(1) << (numIterations - 1))) {
        crc = (crc << 1) ^ polynomial;
      } else {
        crc <<= 1;
      }
    }

    crcTable[byte] = (reflectOut ? reverse(crc) : crc);
  }

  return crcTable;
}

#if !(__CRC32__ || __ARM_FEATURE_CRC32)
// https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-32-iscsi
constexpr auto crc32c_table = gen_crc_table(static_cast<uint32_t>(0x1edc6f41), true, true);
#endif
// https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-64-nvme
constexpr auto crc64nvme_table =
    gen_crc_table(static_cast<uint64_t>(0xad93d23594c93659), true, true);
}  // namespace

uint32_t crc32c(uint32_t crc, const uint8_t *data, unsigned int length) {
  if (data == nullptr) {
    return 0;
  }
  crc ^= 0xffffffff;
#if __CRC32__ || __ARM_FEATURE_CRC32
  // Using hardware acceleration, process data in 8-byte chunks. Any remaining bytes are processed
  // one-by-one in the main loop.
  while (length >= 8) {
    // 8-byte unaligned read
    uint64_t val = *(uint64_t *)data;
#if __ARM_FEATURE_CRC32
    crc = __builtin_arm_crc32cd(crc, val);
#else
    crc = __builtin_ia32_crc32di(crc, val);
#endif
    length -= 8;
    data += 8;
  }
#endif

  while (length--) {
#if __ARM_FEATURE_CRC32
    crc = __builtin_arm_crc32cb(crc, *data++);
#elif __CRC32__
    crc = __builtin_ia32_crc32qi(crc, *data++);
#else
    crc = crc32c_table[(crc ^ *data++) & 0xffL] ^ (crc >> 8);
#endif
  }
  return crc ^ 0xffffffff;
}

uint64_t crc64nvme(uint64_t crc, const uint8_t *data, unsigned int length) {
  if (data == nullptr) {
    return 0;
  }
  crc ^= 0xffffffffffffffff;
  while (length--) {
    crc = crc64nvme_table[(crc ^ *data++) & 0xffL] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffffffffff;
}
