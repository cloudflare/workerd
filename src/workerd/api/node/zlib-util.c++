// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "zlib-util.h"

#include "zlib.h"

namespace workerd::api::node {

uint32_t ZlibUtil::crc32Sync(kj::Array<kj::byte> data, uint32_t value) {
  // Note: Bytef is defined in zlib.h
  return crc32(value, reinterpret_cast<const Bytef*>(data.begin()), data.size());
}

} // namespace workerd::api::node
