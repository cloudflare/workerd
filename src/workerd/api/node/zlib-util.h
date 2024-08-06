// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include <kj/array.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// Implements utilities in support of the Node.js Zlib
class ZlibUtil final : public jsg::Object {
public:
  ZlibUtil() = default;
  ZlibUtil(jsg::Lock&, const jsg::Url&) {}

  uint32_t crc32Sync(kj::Array<kj::byte> data, uint32_t value);

  JSG_RESOURCE_TYPE(ZlibUtil) {
    JSG_METHOD_NAMED(crc32, crc32Sync);
  }
};

#define EW_NODE_ZLIB_ISOLATE_TYPES api::node::ZlibUtil

} // namespace workerd::api::node
