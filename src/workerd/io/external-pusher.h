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

  // Box which holds the reason why an AbortSignal was aborted. May be either:
  // - A serialized V8 value if the signal was aborted from JavaScript.
  // - A KJ exception if the connection from the trigger was lost.
  using PendingAbortReason = kj::RefcountedWrapper<kj::OneOf<kj::Array<byte>, kj::Exception>>;

  struct AbortSignal {
    // Resolves when `reason` has been filled in.
    kj::Promise<void> signal;

    // The abort reason box, will be uninitialized until `signal` resolves.
    kj::Own<PendingAbortReason> reason;
  };

  AbortSignal unwrapAbortSignal(ExternalPusher::AbortSignal::Client cap);

  kj::Promise<void> pushByteStream(PushByteStreamContext context) override;
  kj::Promise<void> pushAbortSignal(PushAbortSignalContext context) override;

 private:
  capnp::ByteStreamFactory& byteStreamFactory;

  capnp::CapabilityServerSet<ExternalPusher::InputStream> inputStreamSet;
  capnp::CapabilityServerSet<ExternalPusher::AbortSignal> abortSignalSet;

  class InputStreamImpl;
  class AbortSignalImpl;
};

}  // namespace workerd
