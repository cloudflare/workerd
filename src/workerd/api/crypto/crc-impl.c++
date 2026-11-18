// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crc-impl.h"

#include <array>
#include <bit>
#include <cstring>
#include <type_traits>

// crc64nvme() folds long inputs with carry-less multiplication where the CPU has it.
#if (defined(__x86_64__) || defined(__aarch64__)) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define WD_CRC64NVME_CLMUL 1
#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#else
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif
#endif
#endif

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

#if WD_CRC64NVME_CLMUL
// CRC-64/NVME by folding with carry-less multiplication (PCLMULQDQ on x86-64, PMULL on aarch64),
// as described in Intel's "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ
// Instruction".
//
// The input is read as 128-bit lanes of message polynomial. Multiplying a lane's two 64-bit halves
// by x^(d+64) mod P and x^d mod P gives a 128-bit value congruent to the lane moved d bits further
// along the message, which can then be xored into the lane d bits later without changing the CRC.
// Eight lanes are folded in parallel, 1024 bits at a time, then folded into one lane, whose 16
// bytes have the same CRC as everything read so far. The table code finishes from there.
//
// The register and the lanes are bit-reflected: bit 0 holds the highest power of x. A 64x64-bit
// carry-less product of reflected values comes out one bit position short, so each constant is
// x^(n-1) mod P to multiply by x^n.

// x^n mod P for the CRC-64/NVME polynomial P, bit-reflected like the CRC register.
constexpr uint64_t crc64nvmeXPowMod(int n) {
  // P without its x^64 term; bit i holds the coefficient of x^i.
  constexpr uint64_t polynomial = 0xad93d23594c93659;
  uint64_t remainder = 1;
  for (int i = 0; i < n; ++i) {
    remainder = (remainder >> 63) ? (remainder << 1) ^ polynomial : remainder << 1;
  }
  return reverse(remainder);
}

constexpr size_t crc64nvmeLaneCount = 8;
constexpr size_t crc64nvmeBlockSize = crc64nvmeLaneCount * 16;

// Constants that move a lane 1024 bits (to the same lane of the next block) and 128 bits (to the
// next lane). The first multiplies the lane's low half, which holds its first 8 bytes.
constexpr uint64_t crc64nvmeFoldBlockLow = crc64nvmeXPowMod(1024 + 64 - 1);
constexpr uint64_t crc64nvmeFoldBlockHigh = crc64nvmeXPowMod(1024 - 1);
constexpr uint64_t crc64nvmeFoldLaneLow = crc64nvmeXPowMod(128 + 64 - 1);
constexpr uint64_t crc64nvmeFoldLaneHigh = crc64nvmeXPowMod(128 - 1);

#if defined(__x86_64__)
#define WD_CLMUL_TARGET __attribute__((target("pclmul")))
using Lane = __m128i;

inline Lane loadLane(const uint8_t *data) {
  return _mm_loadu_si128(reinterpret_cast<const __m128i *>(data));
}
inline void storeLane(uint8_t *data, Lane lane) {
  _mm_storeu_si128(reinterpret_cast<__m128i *>(data), lane);
}
inline Lane makeLane(uint64_t low, uint64_t high) {
  return _mm_set_epi64x(static_cast<long long>(high), static_cast<long long>(low));
}
inline Lane xorLanes(Lane a, Lane b) {
  return _mm_xor_si128(a, b);
}

// Returns lane.low * k.low ^ lane.high * k.high ^ next.
WD_CLMUL_TARGET inline Lane foldLane(Lane lane, Lane k, Lane next) {
  Lane low = _mm_clmulepi64_si128(lane, k, 0x00);
  Lane high = _mm_clmulepi64_si128(lane, k, 0x11);
  return _mm_xor_si128(_mm_xor_si128(low, high), next);
}

bool cpuHasClmul() {
  unsigned int eax, ebx, ecx, edx;
  return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_PCLMUL) != 0;
}
#else
#define WD_CLMUL_TARGET __attribute__((target("aes")))
using Lane = uint64x2_t;

inline Lane loadLane(const uint8_t *data) {
  return vreinterpretq_u64_u8(vld1q_u8(data));
}
inline void storeLane(uint8_t *data, Lane lane) {
  vst1q_u8(data, vreinterpretq_u8_u64(lane));
}
inline Lane makeLane(uint64_t low, uint64_t high) {
  return vcombine_u64(vcreate_u64(low), vcreate_u64(high));
}
inline Lane xorLanes(Lane a, Lane b) {
  return veorq_u64(a, b);
}

// Returns lane.low * k.low ^ lane.high * k.high ^ next.
WD_CLMUL_TARGET inline Lane foldLane(Lane lane, Lane k, Lane next) {
  poly64x2_t a = vreinterpretq_p64_u64(lane);
  poly64x2_t b = vreinterpretq_p64_u64(k);
  Lane low = vreinterpretq_u64_p128(vmull_p64(vgetq_lane_p64(a, 0), vgetq_lane_p64(b, 0)));
  Lane high = vreinterpretq_u64_p128(vmull_high_p64(a, b));
  return veorq_u64(veorq_u64(low, high), next);
}

bool cpuHasClmul() {
#if defined(__ARM_FEATURE_AES) || defined(__APPLE__)
  // Guaranteed by the compilation target, or by every Apple arm64 CPU.
  return true;
#elif defined(__linux__)
#ifndef HWCAP_PMULL
#define HWCAP_PMULL (1 << 4)
#endif
  return (getauxval(AT_HWCAP) & HWCAP_PMULL) != 0;
#else
  return false;
#endif
}
#endif

// Like crc64nvmeRaw(), for length >= crc64nvmeBlockSize on a CPU where cpuHasClmul().
WD_CLMUL_TARGET uint64_t crc64nvmeClmulRaw(uint64_t crc, const uint8_t *data, size_t length) {
  const Lane foldBlock = makeLane(crc64nvmeFoldBlockLow, crc64nvmeFoldBlockHigh);
  const Lane foldOneLane = makeLane(crc64nvmeFoldLaneLow, crc64nvmeFoldLaneHigh);

  Lane lanes[crc64nvmeLaneCount];
  for (size_t i = 0; i < crc64nvmeLaneCount; ++i) {
    lanes[i] = loadLane(data + 16 * i);
  }
  // As in crc64nvmeRaw(), the register is xored into the first 8 bytes of the message.
  lanes[0] = xorLanes(lanes[0], makeLane(crc, 0));
  data += crc64nvmeBlockSize;
  length -= crc64nvmeBlockSize;

  while (length >= crc64nvmeBlockSize) {
    for (size_t i = 0; i < crc64nvmeLaneCount; ++i) {
      lanes[i] = foldLane(lanes[i], foldBlock, loadLane(data + 16 * i));
    }
    data += crc64nvmeBlockSize;
    length -= crc64nvmeBlockSize;
  }

  Lane folded = lanes[0];
  for (size_t i = 1; i < crc64nvmeLaneCount; ++i) {
    folded = foldLane(folded, foldOneLane, lanes[i]);
  }

  // `folded` has the same CRC, from a zero register, as the input read so far.
  uint8_t foldedBytes[16];
  storeLane(foldedBytes, folded);
  return crc64nvmeRaw(crc64nvmeRaw(0, foldedBytes, sizeof(foldedBytes)), data, length);
}
#endif  // WD_CRC64NVME_CLMUL
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
  crc ^= 0xffffffffffffffff;
#if WD_CRC64NVME_CLMUL
  // Short inputs stay on the table code, which has no lanes to set up and fold.
  static const bool useClmul = cpuHasClmul();
  if (useClmul && length >= 2 * crc64nvmeBlockSize) {
    return crc64nvmeClmulRaw(crc, data, length) ^ 0xffffffffffffffff;
  }
#endif
  return crc64nvmeRaw(crc, data, length) ^ 0xffffffffffffffff;
}
