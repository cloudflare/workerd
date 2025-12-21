// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.capnp.h>

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>

namespace workerd {

using kj::byte;

// Implements JsValue.ExternalPusher from worker-interface.capnp.
//
// ExternalPusher allows a remote peer to "push" certain kinds of objects into our address space
// so that they can then be embedded in `JsValue` as `External` values.
class ExternalPusherImpl: public rpc::JsValue::ExternalPusher::Server, public kj::Refcounted {
 public:
  ExternalPusherImpl(capnp::ByteStreamFactory& byteStreamFactory)
      : byteStreamFactory(byteStreamFactory) {}

  using ExternalPusher = rpc::JsValue::ExternalPusher;

  kj::Own<kj::AsyncInputStream> unwrapStream(ExternalPusher::InputStream::Client cap);

  kj::Promise<void> pushByteStream(PushByteStreamContext context) override;

 private:
  capnp::ByteStreamFactory& byteStreamFactory;

  capnp::CapabilityServerSet<ExternalPusher::InputStream> inputStreamSet;

  class InputStreamImpl;
};

}  // namespace workerd
