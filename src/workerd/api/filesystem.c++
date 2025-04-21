// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "filesystem.h"

#include <kj/filesystem.h>
#include <kj/string.h>

#include <cstdint>

namespace workerd::api::filesystem {

void Handle::close() {
  // Implement this.
}

Node::Stat Node::getStat() {
  uint64_t modifiedAt_ = static_cast<uint64_t>((modifiedAt - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  uint64_t createdAt_ = static_cast<uint64_t>((createdAt - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  return Stat{
    .name = name,
    .path = path.toString(),
    .modifiedAt = modifiedAt_,
    .createdAt = createdAt_,
    .type = static_cast<kj::uint>(type),
  };
}

}  // namespace workerd::api::filesystem
