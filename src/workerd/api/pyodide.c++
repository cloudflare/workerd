// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "pyodide.h"

#include <capnp/common.h>
#include <capnp/compat/json.h>

namespace workerd::api::pyodide {

kj::String generatePyodideMetadata(server::config::Worker::Reader conf) {
  capnp::JsonCodec jsonCodec;
  jsonCodec.setPrettyPrint(false);
  return jsonCodec.encode(conf);
}

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules) {
  for (auto module: modules) {
    if (module.isPythonModule()) {
      return true;
    }
  }
  return false;
}

}  // namespace workerd
