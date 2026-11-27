// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/capnp-kj/ffi.rs.h>

#include <capnp/serialize.h>

namespace workerd::rust::capnp_kj {

namespace {

// A Rust message, read in place by C++.
class RustMessageReader {
 public:
  explicit RustMessageReader(const RustMessage& message);
  capnp::AnyPointer::Reader getRoot();
  // The size of the segments, which include the root pointer: the size of a message that holds a
  // copy of this one.
  uint64_t wordCount() const;

 private:
  kj::Array<kj::ArrayPtr<const capnp::word>> segments;
  capnp::SegmentArrayMessageReader reader;
  capnp::ReaderCapabilityTable caps;
};

// For messages that need no limits: a copy of a message that was already valid, whose own limits
// applied when it was read, or a message built in this process.
capnp::ReaderOptions unlimitedReaderOptions() {
  capnp::ReaderOptions options;
  options.traversalLimitInWords = kj::maxValue;
  options.nestingLimit = kj::maxValue;
  return options;
}

kj::ArrayPtr<const capnp::word> asWords(::rust::Slice<const uint8_t> bytes) {
  KJ_REQUIRE(bytes.size() % sizeof(capnp::word) == 0);
  KJ_REQUIRE(reinterpret_cast<uintptr_t>(bytes.data()) % alignof(capnp::word) == 0);
  return kj::arrayPtr(
      reinterpret_cast<const capnp::word*>(bytes.data()), bytes.size() / sizeof(capnp::word));
}

// Every segment of `message`.
kj::Array<kj::ArrayPtr<const capnp::word>> rustSegments(const RustMessage& message) {
  auto result = kj::heapArray<kj::ArrayPtr<const capnp::word>>(message.segment_count());
  for (auto i: kj::indices(result)) {
    result[i] = asWords(message.segment(i));
  }
  return result;
}

// New references to the capabilities of `message`.
kj::Array<kj::Maybe<kj::Own<capnp::ClientHook>>> rustCaps(const RustMessage& message) {
  auto count = message.cap_count();
  // Most messages have no capabilities; an empty table needs no allocation.
  if (count == 0) {
    return nullptr;
  }
  auto builder = kj::heapArrayBuilder<kj::Maybe<kj::Own<capnp::ClientHook>>>(count);
  for (auto i: kj::zeroTo(count)) {
    builder.add(message.get_cap(i));
  }
  return builder.finish();
}

RustMessageReader::RustMessageReader(const RustMessage& message)
    : segments(rustSegments(message)),
      reader(segments, unlimitedReaderOptions()),
      caps(rustCaps(message)) {}

uint64_t RustMessageReader::wordCount() const {
  uint64_t count = 0;
  for (const auto& segment: segments) {
    count += segment.size();
  }
  return count;
}

capnp::AnyPointer::Reader RustMessageReader::getRoot() {
  return caps.imbue(reader.getRoot<capnp::AnyPointer>());
}

// Copies a message Rust built into the builder `getDest(size)` returns, with new references to its
// capabilities. `size` is the copy's exact size, for `getDest` to pass as a size hint: without
// one, KJ allocates and zeroes 1024 words for the message, which costs more than the copy for a
// small one.
template <typename GetDest>
void copyFromRust(const RustMessage& message, GetDest&& getDest) {
  RustMessageReader reader(message);
  capnp::AnyPointer::Builder dest = getDest(capnp::MessageSize{
    .wordCount = reader.wordCount(), .capCount = static_cast<uint>(message.cap_count())});
  dest.set(reader.getRoot());
}

capnp::ClientHook& mut(const capnp::ClientHook& hook) {
  // capnp's hooks are reference counted and their methods are not const; Rust holds them as
  // shared references.
  return const_cast<capnp::ClientHook&>(hook);
}

// A capnp server whose calls a Rust server handles.
class RustServerAdapter final: public capnp::Capability::Server {
 public:
  explicit RustServerAdapter(::rust::Box<RustServer> server): server(kj::mv(server)) {}

  DispatchCallResult dispatchCall(uint64_t interfaceId,
      uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    auto& hook = capnp::CallContextHook::from(context);
    bool allowCancellation = false;
    Dispatched dispatched{};
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      // capnpc-rust does not see `$Cxx.allowCancellation`; the Rust server stated it instead.
      allowCancellation = server->allows_cancellation(interfaceId, methodId);
      dispatched = dispatch(interfaceId, methodId, hook);
    })) {
      // The server's Cancellation panicked, or copying the params failed, before the server saw
      // the call. Whatever the server does with it, including panicking before its method
      // returns a future, arrives in take_pending_call()'s promise.
      return {kj::mv(exception), false, allowCancellation};
    }
    // Most Rust servers succeed when dispatched, and their calls need no promise of a Rust
    // future.
    kj::Promise<void> promise = dispatched.complete ? kj::READY_NOW : take_pending_call(*server);
    return {kj::mv(promise), dispatched.is_streaming, allowCancellation};
  }

 private:
  Dispatched dispatch(uint64_t interfaceId, uint16_t methodId, capnp::CallContextHook& context) {
    auto params = kj::heap<CxxMessage>(context.getParams());
    context.releaseParams();
    return dispatch_call(*server, interfaceId, methodId, kj::mv(params), context.addRef());
  }

  ::rust::Box<RustServer> server;
};

}  // namespace

CxxMessage::CxxMessage(capnp::AnyPointer::Reader root) {
  caps.imbue(builder.getRoot<capnp::AnyPointer>()).set(root);
  segments = builder.getSegmentsForOutput();
}

kj::Maybe<kj::Own<capnp::ClientHook>> CxxMessage::getCap(size_t index) {
  auto table = caps.getTable();
  KJ_REQUIRE(index < table.size());
  return table[index].map([](kj::Own<capnp::ClientHook>& cap) { return cap->addRef(); });
}

kj::Own<capnp::ClientHook> client_add_ref(const capnp::ClientHook& hook) {
  return mut(hook).addRef();
}

size_t client_ptr(const capnp::ClientHook& hook) {
  return reinterpret_cast<size_t>(&hook);
}

kj::Maybe<kj::Own<capnp::ClientHook>> client_get_resolved(const capnp::ClientHook& hook) {
  return mut(hook).getResolved().map([](capnp::ClientHook& resolved) { return resolved.addRef(); });
}

bool client_is_settled(const capnp::ClientHook& hook) {
  return mut(hook).whenMoreResolved() == kj::none;
}

kj::Promise<kj::Own<capnp::ClientHook>> client_when_more_resolved(kj::Own<capnp::ClientHook> hook) {
  KJ_IF_SOME(promise, hook->whenMoreResolved()) {
    co_return co_await promise;
  }
  co_return kj::mv(hook);
}

kj::Promise<void> client_when_resolved(kj::Own<capnp::ClientHook> hook) {
  co_await hook->whenResolved();
}

kj::Own<capnp::ClientHook> new_broken_cap(::rust::Str description) {
  return capnp::newBrokenCap(kj::str(kj::arrayPtr(description.data(), description.size())));
}

namespace {

capnp::Request<capnp::AnyPointer, capnp::AnyPointer> newRequest(
    capnp::ClientHook& hook, uint64_t interfaceId, uint16_t methodId, const RustMessage& params) {
  capnp::Capability::Client client(hook.addRef());
  kj::Maybe<capnp::Request<capnp::AnyPointer, capnp::AnyPointer>> request;
  copyFromRust(params, [&](capnp::MessageSize size) -> capnp::AnyPointer::Builder {
    return request.emplace(client.typelessRequest(interfaceId, methodId, size, {}));
  });
  return KJ_ASSERT_NONNULL(kj::mv(request));
}

}  // namespace

SentCall client_send(const capnp::ClientHook& hook,
    uint64_t interfaceId,
    uint16_t methodId,
    const RustMessage& params) {
  auto remote = newRequest(mut(hook), interfaceId, methodId, params).send();
  auto pipeline = kj::mv(static_cast<capnp::AnyPointer::Pipeline&>(remote));
  auto response = kj::mv(static_cast<kj::Promise<capnp::Response<capnp::AnyPointer>>&>(remote));
  return SentCall{
    .call = kj::heap<CxxCall>(CxxCall{.response = kj::mv(response)}),
    .pipeline = pipeline.releasePipelineHook(),
  };
}

kj::Promise<void> client_send_streaming(kj::Own<capnp::ClientHook> hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params) {
  // Not a coroutine, so that the params and the client are freed here rather than when the call
  // completes: the request is a copy, sent before this returns, and the promise holds what the
  // call needs, as capnp's StreamingRequest::send() assumes when it drops the request.
  return capnp::RequestHook::from(newRequest(*hook, interfaceId, methodId, *params))
      ->sendStreaming();
}

kj::Promise<kj::Own<CxxMessage>> call_response(kj::Own<CxxCall> call) {
  auto response = co_await call->response;
  co_return kj::heap<CxxMessage>(response);
}

kj::Own<capnp::PipelineHook> pipeline_add_ref(const capnp::PipelineHook& pipeline) {
  return const_cast<capnp::PipelineHook&>(pipeline).addRef();
}

kj::Own<capnp::ClientHook> pipeline_get_cap(
    const capnp::PipelineHook& pipeline, ::rust::Slice<const uint16_t> pointerIndices) {
  auto ops = kj::heapArray<capnp::PipelineOp>(pointerIndices.size());
  for (auto i: kj::indices(ops)) {
    ops[i].type = capnp::PipelineOp::GET_POINTER_FIELD;
    ops[i].pointerIndex = pointerIndices[i];
  }
  return const_cast<capnp::PipelineHook&>(pipeline).getPipelinedCap(kj::mv(ops));
}

size_t message_segment_count(const CxxMessage& message) {
  return message.getSegments().size();
}

::rust::Slice<const uint8_t> message_segment(const CxxMessage& message, size_t index) {
  auto segments = message.getSegments();
  KJ_REQUIRE(index < segments.size());
  auto segment = segments[index].asBytes();
  return ::rust::Slice<const uint8_t>(segment.begin(), segment.size());
}

size_t message_cap_count(const CxxMessage& message) {
  return message.getCapCount();
}

kj::Maybe<kj::Own<capnp::ClientHook>> message_get_cap(CxxMessage& message, size_t index) {
  return message.getCap(index);
}

kj::Own<capnp::ClientHook> new_rust_server_client(::rust::Box<RustServer> server) {
  return capnp::ClientHook::from(
      capnp::Capability::Client(kj::heap<RustServerAdapter>(kj::mv(server))));
}

void context_set_results(capnp::CallContextHook& context, const RustMessage& results) {
  copyFromRust(results, [&](capnp::MessageSize size) { return context.getResults(size); });
}

}  // namespace workerd::rust::capnp_kj
