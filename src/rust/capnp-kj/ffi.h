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
#include <kj/refcount.h>

namespace workerd::rust::capnp_kj {

// Rust types, defined in the generated bridge header (which includes this file).
struct RustMessage;
struct RustServer;
struct SentCall;

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
};

// A Rust message, read in place by C++.
class RustMessageReader {
 public:
  explicit RustMessageReader(const RustMessage& message);
  capnp::AnyPointer::Reader getRoot();
  // The size of the segments, which include the root pointer: the size of a message that holds a
  // copy of this one.
  uint64_t wordCount() const;

 private:
  kj::ArrayPtr<const capnp::word> firstSegment;
  // Every segment, when there are more than one.
  kj::Array<kj::ArrayPtr<const capnp::word>> allSegments;
  capnp::SegmentArrayMessageReader reader;
  capnp::ReaderCapabilityTable caps;
};

// A message built in C++, with its capability table.
struct CxxMessageBuilder {
  explicit CxxMessageBuilder(kj::Maybe<capnp::MessageSize> sizeHint);
  capnp::MallocMessageBuilder message;
  capnp::BuilderCapabilityTable caps;
  capnp::AnyPointer::Builder root;
};

// A view of a message a BridgeContext owns, which Rust holds as a kj::Own<CxxMessage> that keeps
// the context alive.
class ContextMessage final: public CxxMessage {
 public:
  ContextMessage() = default;
  // Points the view at `segments` and `caps`, and returns it, holding `owner`. Rust holds at most
  // one at a time.
  kj::Own<CxxMessage> share(kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> segments,
      kj::ArrayPtr<kj::Maybe<kj::Own<capnp::ClientHook>>> caps,
      kj::Own<capnp::CallContextHook> owner);
  bool isShared() const {
    return owner.get() != nullptr;
  }

 private:
  class Disposer;
  kj::Own<capnp::CallContextHook> owner;
};

// A call that crosses the bridge, as KJ sees it: its request, its context and its response.
//
// A call from Rust has Rust params; a call from C++ to a Rust server's client (see RustClient in
// ffi.c++) has params C++ builds in the context. A Rust server's results are a Rust message, and
// any other server's a C++ message it builds in the context. Each side reads the other's message
// in place. A message is copied only to forward the call through a context the bridge doesn't
// know, for a tail call's response, which Rust can't read in place, and when C++ asks for a Rust
// server's results, as KJ's pipelining does when the results can contain capabilities. Results
// are never moved out of the context once set, so readers on both sides can take them in any
// order; a Rust server takes a Rust caller's params only if C++ hasn't read them.
class BridgeContext final: public capnp::CallContextHook,
                           public capnp::ResponseHook,
                           public capnp::RequestHook,
                           public kj::Refcounted {
 public:
  using CallHints = capnp::ClientHook::CallHints;

  // A call from Rust.
  BridgeContext(::rust::Box<RustMessage> params, CallHints hints);
  // A request to `target`, whose params C++ builds in `paramsRoot()`.
  BridgeContext(kj::Own<capnp::ClientHook> target,
      uint64_t interfaceId,
      uint16_t methodId,
      kj::Maybe<capnp::MessageSize> sizeHint,
      CallHints hints);
  ~BridgeContext() noexcept(false);

  capnp::AnyPointer::Builder paramsRoot();

  // CallContextHook
  capnp::AnyPointer::Reader getParams() override;
  void releaseParams() override;
  capnp::AnyPointer::Builder getResults(kj::Maybe<capnp::MessageSize> sizeHint) override;
  kj::Promise<void> tailCall(kj::Own<capnp::RequestHook>&& request) override;
  void setPipeline(kj::Own<capnp::PipelineHook>&& pipeline) override;
  kj::Promise<capnp::AnyPointer::Pipeline> onTailCall() override;
  capnp::ClientHook::VoidPromiseAndPipeline directTailCall(
      kj::Own<capnp::RequestHook>&& request) override;
  kj::Own<capnp::CallContextHook> addRef() override;

  // RequestHook
  capnp::RemotePromise<capnp::AnyPointer> send() override;
  kj::Promise<void> sendStreaming() override;
  capnp::AnyPointer::Pipeline sendForPipeline() override;

  // For a Rust server: takes Rust params, or shares C++ params, unless C++ has already read them.
  kj::Maybe<::rust::Box<RustMessage>> takeRustParams();
  kj::Maybe<kj::Own<CxxMessage>> shareCxxParams();
  // For a Rust server, before the call completes.
  void setRustResults(::rust::Box<RustMessage> results);

  // For a Rust caller, once the call completes. Without Rust results, shareResults() is the C++
  // results, which have no segments if the server wrote none.
  kj::Maybe<const RustMessage&> getRustResults() const;
  kj::Own<CxxMessage> shareResults();

  // For a Rust caller: the call that client_call() sent, until call_response() takes it.
  void setCompletion(kj::Promise<void> promise);
  kj::Promise<void> takeCompletion();

 private:
  capnp::Response<capnp::AnyPointer> response();

  // Where send() sends the call, until it does.
  kj::Maybe<kj::Own<capnp::ClientHook>> target;
  uint64_t interfaceId = 0;
  uint16_t methodId = 0;
  CallHints hints;
  bool isStreaming = false;

  // The params, until released: Rust's, which C++ reads through `rustParamsReader`, or C++'s.
  kj::Maybe<::rust::Box<RustMessage>> rustParams;
  kj::Maybe<RustMessageReader> rustParamsReader;
  kj::Maybe<CxxMessageBuilder> cxxParams;

  // The results: Rust's, which C++ reads through `rustResultsReader` or copies into `cxxResults`;
  // C++'s, in `cxxResults`; or a tail call's, which Rust reads from a copy.
  kj::Maybe<::rust::Box<RustMessage>> rustResults;
  kj::Maybe<RustMessageReader> rustResultsReader;
  kj::Maybe<CxxMessageBuilder> cxxResults;
  kj::Maybe<capnp::Response<capnp::AnyPointer>> tailCallResults;

  kj::Maybe<kj::Own<kj::PromiseFulfiller<capnp::AnyPointer::Pipeline>>> tailCallPipelineFulfiller;

  // Declared after what they view, so that they are destroyed first.
  ContextMessage paramsView;
  ContextMessage resultsView;

  // Holds the call, which holds a reference to this context, so call_response() takes it at
  // once.
  kj::Maybe<kj::Promise<void>> completion;
};

kj::Own<capnp::ClientHook> client_add_ref(const capnp::ClientHook& hook);
size_t client_ptr(const capnp::ClientHook& hook);
kj::Maybe<kj::Own<capnp::ClientHook>> client_get_resolved(const capnp::ClientHook& hook);
bool client_is_settled(const capnp::ClientHook& hook);
kj::Promise<kj::Own<capnp::ClientHook>> client_when_more_resolved(kj::Own<capnp::ClientHook> hook);
kj::Promise<void> client_when_resolved(kj::Own<capnp::ClientHook> hook);
kj::Own<capnp::ClientHook> new_broken_cap(::rust::Str description);

SentCall client_call(const capnp::ClientHook& hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params);
kj::Promise<void> client_send_streaming(kj::Own<capnp::ClientHook> hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params);

kj::Promise<kj::Own<capnp::CallContextHook>> call_response(kj::Own<capnp::CallContextHook> context);

kj::Own<capnp::PipelineHook> pipeline_add_ref(const capnp::PipelineHook& pipeline);
kj::Own<capnp::ClientHook> pipeline_get_cap(
    const capnp::PipelineHook& pipeline, ::rust::Slice<const uint16_t> pointerIndices);

size_t message_segment_count(const CxxMessage& message);
::rust::Slice<const uint8_t> message_segment(const CxxMessage& message, size_t index);
size_t message_cap_count(const CxxMessage& message);
kj::Maybe<kj::Own<capnp::ClientHook>> message_get_cap(CxxMessage& message, size_t index);

kj::Own<capnp::ClientHook> new_rust_server_client(::rust::Box<RustServer> server);
void context_set_results(capnp::CallContextHook& context, const RustMessage& results);
// `context` is a BridgeContext; see ffi.rs. Each throws if it isn't.
void context_set_rust_results(capnp::CallContextHook& context, ::rust::Box<RustMessage> results);
bool context_has_rust_results(const capnp::CallContextHook& context);
const RustMessage& context_rust_results(const capnp::CallContextHook& context);
kj::Own<CxxMessage> context_results_message(capnp::CallContextHook& context);

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
