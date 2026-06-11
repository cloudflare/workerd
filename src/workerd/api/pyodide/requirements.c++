// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "requirements.h"

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/debug.h>

namespace workerd::api::pyodide {

// getField gets a field of a JSON object by key
capnp::json::Value::Reader getField(
    capnp::List<::capnp::json::Value::Field, capnp::Kind::STRUCT>::Reader &object,
    kj::StringPtr name) {
  for (const auto &ent: object) {
    if (ent.getName() == name) {
      return ent.getValue();
    }
  }

  KJ_FAIL_ASSERT("Expected key in JSON object", name);
}

kj::Own<capnp::List<capnp::json::Value::Field>::Reader> parseLockFile(
    kj::StringPtr lockFileContents) {
  capnp::JsonCodec json;
  capnp::MallocMessageBuilder message;

  auto lock = message.initRoot<capnp::JsonValue>();
  json.decodeRaw(lockFileContents, lock);

  auto object = lock.getObject().asReader();
  auto packages = getField(object, "packages").getObject();
  return capnp::clone(packages);
}

}  // namespace workerd::api::pyodide
