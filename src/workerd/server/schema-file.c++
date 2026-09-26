// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "schema-file.h"

#include <workerd/server/cpp-capnp-schema.embed.h>
#include <workerd/server/workerd-capnp-schema.embed.h>

namespace workerd::server {

kj::Maybe<kj::Own<capnp::SchemaFile>> tryImportBulitin(kj::StringPtr name) {
  if (name == "/capnp/c++.capnp") {
    return kj::heap<BuiltinSchemaFileImpl>("/capnp/c++.capnp", CPP_CAPNP_SCHEMA);
  } else if (name == "/workerd/workerd.capnp") {
    return kj::heap<BuiltinSchemaFileImpl>("/workerd/workerd.capnp", WORKERD_CAPNP_SCHEMA);
  } else {
    return kj::none;
  }
}

}  // namespace workerd::server
