
// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.capnp.h>

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>
#include <kj/mutex.h>

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
  // A pending abort reason received (or synthesized on disconnect) for an AbortSignal that
  // was deserialized from RPC. A null OneOf means no abort has arrived yet.
  using PendingAbortReason = kj::OneOf<kj::Array<byte>, kj::Exception>;

  // The box holding a PendingAbortReason. It is written at most once, from the receiving
  // IoContext's thread, but may be read — under the mutex — from any thread: an AbortSignal
  // that has crossed request boundaries polls it to answer getAborted()/getReason()
  // synchronously everywhere.
  struct PendingAbortReasonBox: public kj::AtomicRefcounted {
    kj::MutexGuarded<PendingAbortReason> value;
  };

  struct AbortSignal {
    // Resolves when `reason` has been filled in.
    kj::Promise<void> signal;

    // The abort reason box, unfilled until `signal` resolves.
    kj::Arc<PendingAbortReasonBox> reason;
  };

  AbortSignal unwrapAbortSignal(ExternalPusher::AbortSignal::Client cap);

  kj::Promise<kj::Array<byte>> unwrapDelayedChannelToken(
      rpc::JsValue::ExternalPusher::DelayedChannelToken::Client cap);

  kj::Promise<void> pushByteStream(PushByteStreamContext context) override;
  kj::Promise<void> pushAbortSignal(PushAbortSignalContext context) override;
  kj::Promise<void> pushDelayedChannelToken(PushDelayedChannelTokenContext context) override;

 private:
  capnp::ByteStreamFactory& byteStreamFactory;

  capnp::CapabilityServerSet<ExternalPusher::InputStream> inputStreamSet;
  capnp::CapabilityServerSet<ExternalPusher::AbortSignal> abortSignalSet;
  capnp::CapabilityServerSet<ExternalPusher::DelayedChannelToken> delayedChannelTokenSet;

  kj::Promise<kj::Own<kj::AsyncInputStream>> unwrapStreamImpl(
      ExternalPusher::InputStream::Client cap);

  kj::Promise<void> unwrapAbortSignalImpl(
      ExternalPusher::AbortSignal::Client cap, kj::Arc<PendingAbortReasonBox> pendingReason);

  class InputStreamImpl;
  class AbortSignalImpl;
  class DelayedChannelTokenImpl;
};

}  // namespace workerd
