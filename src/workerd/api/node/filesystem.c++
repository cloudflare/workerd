// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "filesystem.h"

#include <workerd/api/filesystem.h>
#include <workerd/jsg/jsg.h>

#include <kj/string.h>

using workerd::api::filesystem::Node;

namespace workerd::api::node {

kj::Maybe<jsg::Ref<Node>> FilesystemUtil::open(jsg::Lock& js, kj::String path) {
  return kj::none;
}

}  // namespace workerd::api::node
