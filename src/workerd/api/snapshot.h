// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Opaque snapshot handle transferable over Workers RPC. IoOwn ensures its capability is destroyed
// in the owning IoContext, even if the JS object is collected later.
class DurableObjectSnapshot final: public jsg::Object {
 public:
  explicit DurableObjectSnapshot(capnp::Capability::Client client);

  capnp::Capability::Client getClient();

  JSG_RESOURCE_TYPE(DurableObjectSnapshot) {
    // No methods exposed to JS.
  }
  JSG_SERIALIZABLE(rpc::SerializationTag::DURABLE_OBJECT_SNAPSHOT);

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<DurableObjectSnapshot> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

 private:
  IoOwn<capnp::Capability::Client> client;
};

#define EW_SNAPSHOT_ISOLATE_TYPES api::DurableObjectSnapshot

}  // namespace workerd::api
