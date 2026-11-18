// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crc-impl.h"

#include <array>
#include <bit>
#include <cstring>
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
// NOLINTNEXTLINE(edgeworker-multiple-bool-args)
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
// Slicing-by-16 tables for CRC-64/NVME
// (https://reveng.sourceforge.io/crc-catalogue/all.htm#crc.cat.crc-64-nvme).
// crc64nvme_tables[0] is the usual byte-at-a-time table. crc64nvme_tables[k][b] is the CRC of byte
// value b followed by k zero bytes, so a 16-byte block can be folded into the CRC with 16
// independent lookups, one per byte at its distance from the end of the block, instead of a chain
// of 16 dependent ones.
constexpr auto crc64nvmeSlices = 16;
using Crc64nvmeTables = std::array<std::array<uint64_t, crcTableSize>, crc64nvmeSlices>;

constexpr Crc64nvmeTables gen_crc64nvme_tables() {
  Crc64nvmeTables tables{};
  tables[0] = gen_crc_table(static_cast<uint64_t>(0xad93d23594c93659), true, true);
  for (int k = 1; k < crc64nvmeSlices; ++k) {
    for (int b = 0; b < crcTableSize; ++b) {
      uint64_t prev = tables[k - 1][b];
      tables[k][b] = (prev >> 8) ^ tables[0][prev & 0xff];
    }
  }
  return tables;
}

constexpr auto crc64nvme_tables = gen_crc64nvme_tables();

// Reads 8 bytes at a possibly unaligned address as a little-endian integer.
inline uint64_t loadLittleEndian64(const uint8_t *data) {
  uint64_t value;
  memcpy(&value, data, sizeof(value));
  if constexpr (std::endian::native == std::endian::big) {
    value = __builtin_bswap64(value);
  }
  return value;
}

// CRC-64/NVME without the initial and final inversion: `crc` is the raw CRC register.
uint64_t crc64nvmeRaw(uint64_t crc, const uint8_t *data, size_t length) {
  const auto &t = crc64nvme_tables;
  while (length >= 16) {
    // The register is xored into the first 8 bytes of the block, as in the byte-at-a-time loop.
    uint64_t low = crc ^ loadLittleEndian64(data);
    uint64_t high = loadLittleEndian64(data + 8);
    crc = 0;
    for (int i = 0; i < 8; ++i) {
      crc ^= t[15 - i][(low >> (8 * i)) & 0xff] ^ t[7 - i][(high >> (8 * i)) & 0xff];
    }
    data += 16;
    length -= 16;
  }
  while (length--) {
    crc = t[0][(crc ^ *data++) & 0xff] ^ (crc >> 8);
  }
  return crc;
}
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
    uint64_t val = *reinterpret_cast<const uint64_t *>(data);
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
  return crc64nvmeRaw(crc ^ 0xffffffffffffffff, data, length) ^ 0xffffffffffffffff;
}
