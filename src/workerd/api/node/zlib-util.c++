// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "zlib-util.h"

#include "zlib.h"

namespace workerd::api::node {

namespace {
// This function is required to avoid function name clash.
uint32_t crc32Impl(uint32_t value, unsigned char* bytes, size_t size) {
  // Note: Bytef is defined in zlib.h
  return crc32(value, reinterpret_cast<const Bytef*>(bytes), size);
}
} // namespace

uint32_t ZlibUtil::crc32(kj::Array<kj::byte> data, uint32_t value) {
  return crc32Impl(value, data.begin(), data.size());
}

} // namespace workerd::api::node
