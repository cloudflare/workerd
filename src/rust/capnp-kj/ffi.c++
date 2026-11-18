// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/capnp-kj/ffi.rs.h>

#include <capnp/serialize.h>

namespace workerd::rust::capnp_kj {

namespace {

// The source message was already valid, and its own limits applied when it was built or read.
capnp::ReaderOptions unlimitedReaderOptions() {
  capnp::ReaderOptions options;
  options.traversalLimitInWords = kj::maxValue;
  return options;
}

// Copies a message Rust built into `dest`, moving its capabilities into `dest`'s message.
void copyFromRust(capnp::AnyPointer::Builder dest, RustMessage& message) {
  auto segmentCount = message.segment_count();
  auto segments = kj::heapArray<kj::ArrayPtr<const capnp::word>>(segmentCount);
  for (auto i: kj::zeroTo(segmentCount)) {
    auto bytes = message.segment(i);
    KJ_REQUIRE(bytes.size() % sizeof(capnp::word) == 0);
    KJ_REQUIRE(reinterpret_cast<uintptr_t>(bytes.data()) % alignof(capnp::word) == 0);
    segments[i] = kj::arrayPtr(
        reinterpret_cast<const capnp::word*>(bytes.data()), bytes.size() / sizeof(capnp::word));
  }
  capnp::SegmentArrayMessageReader reader(segments, unlimitedReaderOptions());

  auto capCount = message.cap_count();
  auto caps = kj::heapArrayBuilder<kj::Maybe<kj::Own<capnp::ClientHook>>>(capCount);
  for (auto i: kj::zeroTo(capCount)) {
    caps.add(message.take_cap(i));
  }
  capnp::ReaderCapabilityTable capTable(caps.finish());
  dest.set(capTable.imbue(reader.getRoot<capnp::AnyPointer>()));
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
    auto params = kj::heap<CxxMessage>(context.getParams());
    context.releaseParams();
    auto call = server->dispatch(interfaceId, methodId, kj::mv(params));
    bool isStreaming = call->is_streaming();
    auto promise = run_call(kj::mv(call))
                       .then([context, isStreaming](::rust::Box<RustMessage> results) mutable {
      if (!isStreaming) {
        copyFromRust(context.getResults(), *results);
      }
    });
    // capnpc-rust does not see `$Cxx.allowCancellation`; the Rust server stated it instead.
    return {kj::mv(promise), isStreaming, server->allows_cancellation(interfaceId, methodId)};
  }

 private:
  ::rust::Box<RustServer> server;
};

}  // namespace

CxxMessage::CxxMessage(capnp::AnyPointer::Reader root) {
  caps.imbue(builder.getRoot<capnp::AnyPointer>()).set(root);
  segments = builder.getSegmentsForOutput();
}

kj::Maybe<kj::Own<capnp::ClientHook>> CxxMessage::takeCap(size_t index) {
  auto table = caps.getTable();
  KJ_REQUIRE(index < table.size());
  auto cap = kj::mv(table[index]);
  table[index] = kj::none;
  return cap;
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
    capnp::ClientHook& hook, uint64_t interfaceId, uint16_t methodId, RustMessage& params) {
  capnp::Capability::Client client(hook.addRef());
  auto request = client.typelessRequest(interfaceId, methodId, kj::none, {});
  copyFromRust(request, params);
  return request;
}

}  // namespace

kj::Own<CxxCall> client_send(const capnp::ClientHook& hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params) {
  auto remote = newRequest(mut(hook), interfaceId, methodId, *params).send();
  auto pipeline = kj::mv(static_cast<capnp::AnyPointer::Pipeline&>(remote));
  return kj::heap<CxxCall>(CxxCall{
    .response = kj::mv(static_cast<kj::Promise<capnp::Response<capnp::AnyPointer>>&>(remote)),
    .pipeline = pipeline.releasePipelineHook(),
  });
}

kj::Promise<void> client_send_streaming(kj::Own<capnp::ClientHook> hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params) {
  auto request = newRequest(*hook, interfaceId, methodId, *params);
  co_await capnp::RequestHook::from(kj::mv(request))->sendStreaming();
}

kj::Own<capnp::PipelineHook> call_pipeline(const CxxCall& call) {
  return const_cast<capnp::PipelineHook&>(*call.pipeline).addRef();
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
  auto segment = message.getSegments()[index].asBytes();
  return ::rust::Slice<const uint8_t>(segment.begin(), segment.size());
}

size_t message_cap_count(const CxxMessage& message) {
  return message.getCapCount();
}

kj::Maybe<kj::Own<capnp::ClientHook>> message_take_cap(CxxMessage& message, size_t index) {
  return message.takeCap(index);
}

kj::Own<capnp::ClientHook> new_rust_server_client(::rust::Box<RustServer> server) {
  return capnp::ClientHook::from(
      capnp::Capability::Client(kj::heap<RustServerAdapter>(kj::mv(server))));
}

}  // namespace workerd::rust::capnp_kj
