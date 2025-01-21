// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "endianness.h"

#if (defined(_WIN16) || defined(_WIN32) || defined(_WIN64)) && !defined(__WINDOWS__)

#define __WINDOWS__

#endif

#if defined(__linux__) || defined(__CYGWIN__)

#include <endian.h>

#elif defined(__APPLE__)

#include <libkern/OSByteOrder.h>

uint16_t htobe16(uint16_t x) {
  return OSSwapHostToBigInt16(x);
}
uint16_t htole16(uint16_t x) {
  return OSSwapHostToLittleInt16(x);
}
uint16_t be16toh(uint16_t x) {
  return OSSwapBigToHostInt16(x);
}
uint16_t le16toh(uint16_t x) {
  return OSSwapLittleToHostInt16(x);
}

uint32_t htobe32(uint32_t x) {
  return OSSwapHostToBigInt32(x);
}
uint32_t htole32(uint32_t x) {
  return OSSwapHostToLittleInt32(x);
}
uint32_t be32toh(uint32_t x) {
  return OSSwapBigToHostInt32(x);
}
uint32_t le32toh(uint32_t x) {
  return OSSwapLittleToHostInt32(x);
}

uint64_t htobe64(uint64_t x) {
  return OSSwapHostToBigInt64(x);
}
uint64_t htole64(uint64_t x) {
  return OSSwapHostToLittleInt64(x);
}
uint64_t be64toh(uint64_t x) {
  return OSSwapBigToHostInt64(x);
}
uint64_t le64toh(uint64_t x) {
  return OSSwapLittleToHostInt64(x);
}

#elif defined(__OpenBSD__)

#include <sys/endian.h>

#elif defined(__NetBSD__) || defined(__FreeBSD__) || defined(__DragonFly__)

#include <sys/endian.h>

uint16_t be16toh(uint16_t x) {
  return betoh16(x);
}
uint16_t le16toh(uint16_t x) {
  return letoh16(x);
}

uint32_t be32toh(uint32_t x) {
  return betoh32(x);
}
uint32_t le32toh(uint32_t x) {
  return letoh32(x);
}

uint64_t be64toh(uint64_t x) {
  return betoh64(x);
}
uint64_t le64toh(uint64_t x) {
  return letoh64(x);
}

#elif defined(__WINDOWS__)

#include <winsock2.h>

#if BYTE_ORDER == LITTLE_ENDIAN

uint16_t htobe16(uint16_t x) {
  return htons(x);
}
uint16_t htole16(uint16_t x) {
  return x;
}
uint16_t be16toh(uint16_t x) {
  return ntohs(x);
}
uint16_t le16toh(uint16_t x) {
  return x;
}

uint32_t htobe32(uint32_t x) {
  return htonl(x);
}
uint32_t htole32(uint32_t x) {
  return x;
}
uint32_t be32toh(uint32_t x) {
  return ntohl(x);
}
uint32_t le32toh(uint32_t x) {
  return x;
}

uint64_t htobe64(uint64_t x) {
  return htonll(x);
}
uint64_t htole64(uint64_t x) {
  return x;
}
uint64_t be64toh(uint64_t x) {
  return ntohll(x);
}
uint64_t le64toh(uint64_t x) {
  return x;
}

#elif BYTE_ORDER == BIG_ENDIAN

/* that would be xbox 360 */
uint16_t htobe16(uint16_t x) {
  return x;
}
uint16_t htole16(uint16_t x) {
  return __builtin_bswap16(x);
}
uint16_t be16toh(uint16_t x) {
  return x;
}
uint16_t le16toh(uint16_t x) {
  return __builtin_bswap16(x);
}

uint32_t htobe32(uint32_t x) {
  return x;
}
uint32_t htole32(uint32_t x) {
  return __builtin_bswap32(x);
}
uint32_t be32toh(uint32_t x) {
  return x;
}
uint32_t le32toh(uint32_t x) {
  return __builtin_bswap32(x);
}

uint64_t htobe64(uint64_t x) {
  return x;
}
uint64_t htole64(uint64_t x) {
  return __builtin_bswap64(x);
}
uint64_t be64toh(uint64_t x) {
  return x;
}
uint64_t le64toh(uint64_t x) {
  return __builtin_bswap64(x);
}

#else

#error byte order not supported

#endif

#else

#error platform not supported

#endif
