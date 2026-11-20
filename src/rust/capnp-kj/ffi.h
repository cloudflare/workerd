// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of the capnp-kj bridge: KJ's RPC system as capnp-rust's capability hooks see it.
// See ffi.rs.

#include <rust/cxx.h>

#include <capnp/any.h>
#include <capnp/capability.h>
#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/async.h>
#include <kj/memory.h>

namespace workerd::rust::capnp_kj {

// Rust types, defined in the generated bridge header (which includes this file).
struct RustMessage;
struct RustServer;

// A standalone message with its own capability table, copied from part of another message.
// Rust reads its segments in place and takes its capabilities.
class CxxMessage {
 public:
  // Copies `root`, in one allocation that holds both the message and its first segment, sized
  // to fit the copy.
  static kj::Own<CxxMessage> copy(capnp::AnyPointer::Reader root);

  KJ_DISALLOW_COPY_AND_MOVE(CxxMessage);

  kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> getSegments() const {
    return segments;
  }
  size_t getCapCount() const {
    return caps.getTable().size();
  }
  kj::Maybe<kj::Own<capnp::ClientHook>> takeCap(size_t index);

 private:
  class Disposer;

  // `segment` is zeroed, exactly the size of the copy, and outlives the message.
  CxxMessage(capnp::AnyPointer::Reader root, kj::ArrayPtr<capnp::word> segment);

  // A copy of a known size fits one segment exactly, as in capnp's copyToUnchecked(), which
  // FlatMessageBuilder exists for. Unlike MallocMessageBuilder given a first segment, it doesn't
  // zero the segment again when destroyed, just before the segment is freed.
  capnp::FlatMessageBuilder builder;
  mutable capnp::BuilderCapabilityTable caps;
  kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> segments;
};

// A call sent from Rust.
struct CxxCall {
  // The response, copied for Rust to read.
  kj::Promise<kj::Own<CxxMessage>> response;
  kj::Own<capnp::PipelineHook> pipeline;
  // False if the call was sent with `noPromisePipelining`, so `pipeline` is KJ's disabled one.
  bool canPipeline;
};

kj::Own<capnp::ClientHook> client_add_ref(const capnp::ClientHook& hook);
size_t client_ptr(const capnp::ClientHook& hook);
kj::Maybe<kj::Own<capnp::ClientHook>> client_get_resolved(const capnp::ClientHook& hook);
bool client_is_settled(const capnp::ClientHook& hook);
kj::Promise<kj::Own<capnp::ClientHook>> client_when_more_resolved(kj::Own<capnp::ClientHook> hook);
kj::Promise<void> client_when_resolved(kj::Own<capnp::ClientHook> hook);
kj::Own<capnp::ClientHook> new_broken_cap(::rust::Str description);

kj::Own<CxxCall> client_send(
    const capnp::ClientHook& hook, uint64_t interfaceId, uint16_t methodId, RustMessage& params);
kj::Promise<void> client_send_streaming(kj::Own<capnp::ClientHook> hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params);

kj::Own<capnp::PipelineHook> call_pipeline(const CxxCall& call);
bool call_can_pipeline(const CxxCall& call);
kj::Promise<kj::Own<CxxMessage>> call_response(kj::Own<CxxCall> call);

kj::Own<capnp::PipelineHook> pipeline_add_ref(const capnp::PipelineHook& pipeline);
kj::Own<capnp::ClientHook> pipeline_get_cap(
    const capnp::PipelineHook& pipeline, ::rust::Slice<const uint16_t> pointerIndices);

size_t message_segment_count(const CxxMessage& message);
::rust::Slice<const uint8_t> message_segment(const CxxMessage& message, size_t index);
size_t message_cap_count(const CxxMessage& message);
kj::Maybe<kj::Own<capnp::ClientHook>> message_take_cap(CxxMessage& message, size_t index);

kj::Own<capnp::ClientHook> new_rust_server_client(::rust::Box<RustServer> server);
void context_set_results(capnp::CallContextHook& context, RustMessage& results);

// Promise pipelining hints for calls from Rust.
//
// capnpc-c++'s generated clients send a call with `CallHints::noPromisePipelining` when the
// method's results can't contain a capability, which spares KJ setting up a pipeline that can't be
// used. capnpc-rust's clients pass no hints, and the bridge sees only interface and method ids, so
// the bridge looks the hint up here, in the C++ schemas of the interfaces that C++ hands to Rust or
// takes from it (and of every interface reachable from their methods). A call to an interface not
// registered keeps pipelining, which is always correct, only slower.
void registerInterface(capnp::InterfaceSchema schema);
bool noPromisePipelining(uint64_t interfaceId, uint16_t methodId);

template <typename Interface>
void registerInterfaceOnce() {
  if constexpr (!kj::isSameType<Interface, capnp::Capability>()) {
    static const bool registered KJ_UNUSED =
        (registerInterface(capnp::Schema::from<Interface>()), true);
  }
}

// Typed conveniences for C++ code that hands capabilities to Rust or takes them back.

template <typename Client>
kj::Own<capnp::ClientHook> clientToRust(Client client) {
  registerInterfaceOnce<typename Client::Calls>();
  return capnp::ClientHook::from(kj::mv(client));
}

template <typename Interface>
Interface::Client clientFromRust(kj::Own<capnp::ClientHook> hook) {
  registerInterfaceOnce<Interface>();
  return typename Interface::Client(kj::mv(hook));
}

}  // namespace workerd::rust::capnp_kj
