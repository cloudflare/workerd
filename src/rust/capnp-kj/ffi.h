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
#include <capnp/serialize.h>
#include <kj/async.h>
#include <kj/memory.h>
#include <kj/refcount.h>

namespace workerd::rust::capnp_kj {

// Rust types, defined in the generated bridge header (which includes this file).
struct RustMessage;
struct RustServer;

// A C++ message that Rust reads in place: its segments, and its capability table, from which Rust
// takes a reference to each capability.
class CxxMessage {
 public:
  // Copies `root` into a standalone message, in one allocation that holds both the message and its
  // first segment, sized to fit the copy.
  static kj::Own<CxxMessage> copy(capnp::AnyPointer::Reader root);

  KJ_DISALLOW_COPY_AND_MOVE(CxxMessage);
  // Virtual so that kj::Own can upcast to it.
  virtual ~CxxMessage() noexcept(false) = default;

  kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> getSegments() const {
    return segments;
  }
  size_t getCapCount() const {
    return caps.size();
  }
  // Returns a new reference to capability `index`.
  kj::Maybe<kj::Own<capnp::ClientHook>> getCap(size_t index);

 protected:
  CxxMessage() = default;

  // Views into the message, which the derived class owns.
  kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> segments;
  kj::ArrayPtr<kj::Maybe<kj::Own<capnp::ClientHook>>> caps;

 private:
  class Copy;
  friend class RustCallContext;
};

// A call from Rust, as the capability it is sent to sees it.
//
// The params are a Rust message. A C++ server reads them in place; a Rust server takes them. The
// results are a C++ message the context owns, which Rust reads in place once the call completes,
// or a Rust message from a Rust server, which Rust takes back. Either way neither side copies
// the other's message, except to forward the call elsewhere, or to give C++ a Rust server's
// results, as KJ's pipelining does when the results can contain capabilities.
class RustCallContext final: public capnp::CallContextHook,
                             public CxxMessage,
                             public kj::Refcounted {
 public:
  explicit RustCallContext(::rust::Box<RustMessage> params);
  ~RustCallContext() noexcept(false);

  capnp::AnyPointer::Reader getParams() override;
  void releaseParams() override;
  capnp::AnyPointer::Builder getResults(kj::Maybe<capnp::MessageSize> sizeHint) override;
  kj::Promise<void> tailCall(kj::Own<capnp::RequestHook>&& request) override;
  void setPipeline(kj::Own<capnp::PipelineHook>&& pipeline) override;
  kj::Promise<capnp::AnyPointer::Pipeline> onTailCall() override;
  capnp::ClientHook::VoidPromiseAndPipeline directTailCall(
      kj::Own<capnp::RequestHook>&& request) override;
  kj::Own<capnp::CallContextHook> addRef() override;

  // For a Rust server: takes the params, unless C++ has already read them.
  kj::Maybe<::rust::Box<RustMessage>> takeRustParams();
  // For a Rust server, before the call completes.
  void setRustResults(::rust::Box<RustMessage> results);

  // Once the call completes: whether the results are a Rust message, to take with
  // takeRustResults(); if not, the context reads as the results' C++ message, which is empty if
  // the server wrote none.
  void finish();
  bool hasRustResults() const {
    return rustResults != kj::none;
  }
  ::rust::Box<RustMessage> takeRustResults();

 private:
  // The params, read in place from C++.
  struct ParamsReader {
    explicit ParamsReader(RustMessage& message);
    kj::ArrayPtr<const capnp::word> firstSegment;
    // Every segment, when there are more than one.
    kj::Array<kj::ArrayPtr<const capnp::word>> allSegments;
    capnp::SegmentArrayMessageReader reader;
    capnp::ReaderCapabilityTable caps;
  };

  // Results built in C++.
  struct CxxResults {
    explicit CxxResults(kj::Maybe<capnp::MessageSize> sizeHint);
    capnp::MallocMessageBuilder message;
    capnp::BuilderCapabilityTable caps;
    capnp::AnyPointer::Builder root;
  };

  kj::Maybe<::rust::Box<RustMessage>> params;
  kj::Maybe<ParamsReader> paramsReader;

  // At most one of these holds the results.
  kj::Maybe<::rust::Box<RustMessage>> rustResults;
  kj::Maybe<CxxResults> cxxResults;
  kj::Maybe<capnp::Response<capnp::AnyPointer>> tailCallResults;

  // A copy of `tailCallResults`, which Rust can't read in place.
  kj::Maybe<kj::Own<CxxMessage>> resultsCopy;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<capnp::AnyPointer::Pipeline>>> tailCallPipelineFulfiller;
};

// A call sent from Rust.
struct CxxCall {
  kj::Own<RustCallContext> context;
  kj::Promise<void> completion;
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

kj::Own<CxxCall> client_call(const capnp::ClientHook& hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params);
kj::Promise<void> client_send_streaming(kj::Own<capnp::ClientHook> hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params);

kj::Own<capnp::PipelineHook> call_pipeline(const CxxCall& call);
bool call_can_pipeline(const CxxCall& call);
kj::Promise<kj::Own<capnp::CallContextHook>> call_response(kj::Own<CxxCall> call);

kj::Own<capnp::PipelineHook> pipeline_add_ref(const capnp::PipelineHook& pipeline);
kj::Own<capnp::ClientHook> pipeline_get_cap(
    const capnp::PipelineHook& pipeline, ::rust::Slice<const uint16_t> pointerIndices);

size_t message_segment_count(const CxxMessage& message);
::rust::Slice<const uint8_t> message_segment(const CxxMessage& message, size_t index);
size_t message_cap_count(const CxxMessage& message);
kj::Maybe<kj::Own<capnp::ClientHook>> message_get_cap(CxxMessage& message, size_t index);

kj::Own<capnp::ClientHook> new_rust_server_client(::rust::Box<RustServer> server);
void context_set_results(capnp::CallContextHook& context, RustMessage& results);
// `context` is a RustCallContext; see ffi.rs.
void context_set_rust_results(capnp::CallContextHook& context, ::rust::Box<RustMessage> results);
bool context_has_rust_results(const capnp::CallContextHook& context);
::rust::Box<RustMessage> context_take_rust_results(capnp::CallContextHook& context);
kj::Own<CxxMessage> context_into_message(kj::Own<capnp::CallContextHook> context);

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
