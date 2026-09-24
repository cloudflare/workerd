// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "snapshot.h"

#include "worker-rpc.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

DurableObjectSnapshot::DurableObjectSnapshot(capnp::Capability::Client client)
    : client(IoContext::current().addObject(kj::heap<capnp::Capability::Client>(kj::mv(client)))) {}

capnp::Capability::Client DurableObjectSnapshot::getClient() {
  return *client;
}

void DurableObjectSnapshot::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "DurableObjectSnapshots cannot be cloned or stored. They can only be transferred over RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHandler*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "DurableObjectSnapshots cannot be cloned or stored. They can only be transferred over RPC.");

  externalHandler->write([cap = getClient()](rpc::JsValue::External::Builder builder) mutable {
    builder.initDurableObjectSnapshot().setCapability(kj::mv(cap));
  });
}

jsg::Ref<DurableObjectSnapshot> DurableObjectSnapshot::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto& handler = KJ_REQUIRE_NONNULL(
      deserializer.getExternalHandler(), "got Snapshot on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHandler*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got Snapshot on non-RPC serialized object?");

  auto reader = externalHandler->read();
  KJ_REQUIRE(
      reader.isDurableObjectSnapshot(), "external table slot type doesn't match serialization tag");

  return js.alloc<DurableObjectSnapshot>(reader.getDurableObjectSnapshot().getCapability());
}

}  // namespace workerd::api
