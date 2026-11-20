// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/capnp-kj/ffi.rs.h>

#include <capnp/serialize.h>
#include <kj/map.h>
#include <kj/mutex.h>

namespace workerd::rust::capnp_kj {

namespace {

// The source message was already valid, and its own limits applied when it was built or read.
capnp::ReaderOptions unlimitedReaderOptions() {
  capnp::ReaderOptions options;
  options.traversalLimitInWords = kj::maxValue;
  return options;
}

// Copies a message Rust built into the builder `getDest(size)` returns, moving its capabilities
// into that builder's message. `size` is the copy's exact size, for `getDest` to pass as a size
// hint: without one, KJ allocates and zeroes 1024 words for the message, which costs more than the
// copy for a small one.
template <typename GetDest>
void copyFromRust(RustMessage& message, GetDest&& getDest) {
  auto segmentCount = message.segment_count();
  // Usually one segment, so kept off the heap.
  KJ_STACK_ARRAY(kj::ArrayPtr<const capnp::word>, segments, segmentCount, 4, 32);
  // The segments include the root pointer, so their total is the size of a message holding
  // the copy.
  uint64_t wordCount = 0;
  for (auto i: kj::zeroTo(segmentCount)) {
    auto bytes = message.segment(i);
    KJ_REQUIRE(bytes.size() % sizeof(capnp::word) == 0);
    KJ_REQUIRE(reinterpret_cast<uintptr_t>(bytes.data()) % alignof(capnp::word) == 0);
    segments[i] = kj::arrayPtr(
        reinterpret_cast<const capnp::word*>(bytes.data()), bytes.size() / sizeof(capnp::word));
    wordCount += segments[i].size();
  }
  capnp::SegmentArrayMessageReader reader(segments, unlimitedReaderOptions());

  auto capCount = message.cap_count();
  // Most messages have no capabilities; an empty table needs no allocation.
  kj::Array<kj::Maybe<kj::Own<capnp::ClientHook>>> caps;
  if (capCount > 0) {
    auto builder = kj::heapArrayBuilder<kj::Maybe<kj::Own<capnp::ClientHook>>>(capCount);
    for (auto i: kj::zeroTo(capCount)) {
      builder.add(message.take_cap(i));
    }
    caps = builder.finish();
  }
  capnp::ReaderCapabilityTable capTable(kj::mv(caps));
  capnp::AnyPointer::Builder dest =
      getDest(capnp::MessageSize{.wordCount = wordCount, .capCount = static_cast<uint>(capCount)});
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
    auto params = CxxMessage::copy(context.getParams());
    context.releaseParams();
    // The Rust server's results copy themselves into the context when the server drops them; see
    // KjResults in server.rs.
    kj::Promise<void> promise = dispatch_call(*server, interfaceId, methodId, kj::mv(params),
        capnp::CallContextHook::from(context).addRef());
    // capnpc-rust does not see `$Cxx.allowCancellation`; the Rust server stated it instead.
    return {kj::mv(promise), server->last_is_streaming(),
      server->allows_cancellation(interfaceId, methodId)};
  }

 private:
  ::rust::Box<RustServer> server;
};

}  // namespace

// The segment is sized to the copy. MallocMessageBuilder's default first segment is 1024 zeroed
// words, which costs as much as the copy itself for a small message.
//
// targetSize() and set() each traverse `root`, and both count against its message's traversal
// limit, so a message that uses more than half of that limit fails here although a C++ server
// could read it.
CxxMessage::CxxMessage(capnp::AnyPointer::Reader root, kj::ArrayPtr<capnp::word> segment)
    : builder(segment) {
  caps.imbue(builder.getRoot<capnp::AnyPointer>()).set(root);
  segments = builder.getSegmentsForOutput();
}

// Destroys a CxxMessage and frees the allocation that holds it and its first segment.
class CxxMessage::Disposer final: public kj::Disposer {
 public:
  void disposeImpl(void* pointer) const override {
    static_cast<CxxMessage*>(pointer)->~CxxMessage();
    ::operator delete(pointer);
  }
};

kj::Own<CxxMessage> CxxMessage::copy(capnp::AnyPointer::Reader root) {
  static_assert(sizeof(CxxMessage) % sizeof(capnp::word) == 0);
  static const Disposer disposer;
  auto words = root.targetSize().wordCount + 1;
  auto bytes = sizeof(CxxMessage) + words * sizeof(capnp::word);
  // operator new's alignment covers both the message and a word.
  auto* memory = static_cast<kj::byte*>(::operator new(bytes));
  auto* segment = reinterpret_cast<capnp::word*>(memory + sizeof(CxxMessage));
  auto segmentWords = kj::arrayPtr(segment, words);
  segmentWords.asBytes().fill(0);
  KJ_ON_SCOPE_FAILURE(::operator delete(memory));
  auto* message = new (memory) CxxMessage(root, segmentWords);
  return kj::Own<CxxMessage>(message, disposer);
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

// For each registered interface, by id, whether each of its methods, by index, can be called
// with `noPromisePipelining`. Interfaces are only ever added, and each interface's entry never
// changes once added.
using PipelineHints = kj::HashMap<uint64_t, kj::Array<bool>>;

kj::MutexGuarded<PipelineHints>& pipelineHints() {
  static kj::MutexGuarded<PipelineHints> hints;
  return hints;
}

// Collects the hints of an interface, its superclasses, and every interface reachable from their
// methods' params and results.
class HintCollector {
 public:
  explicit HintCollector(const PipelineHints& known): known(known) {}

  void visitInterface(capnp::InterfaceSchema schema) {
    auto id = schema.getProto().getId();
    if (known.find(id) != kj::none || !markSeen(id)) {
      return;
    }
    auto methods = schema.getMethods();
    auto noPipelining = kj::heapArray<bool>(methods.size());
    for (auto method: methods) {
      auto results = method.getResultType();
      // The same test capnpc-c++ uses for its generated clients.
      noPipelining[method.getIndex()] = !results.mayContainCapabilities();
      visitStruct(results);
      visitStruct(method.getParamType());
    }
    found.upsert(id, kj::mv(noPipelining));
    for (auto superclass: schema.getSuperclasses()) {
      visitInterface(superclass);
    }
  }

  PipelineHints found;

 private:
  // Returns false if `id` was already seen.
  bool markSeen(uint64_t id) {
    if (seen.contains(id)) {
      return false;
    }
    seen.insert(id);
    return true;
  }

  void visitStruct(capnp::StructSchema schema) {
    if (!schema.mayContainCapabilities() || !markSeen(schema.getProto().getId())) {
      return;
    }
    // Includes union members, and groups, whose type is a struct.
    for (auto field: schema.getFields()) {
      visitType(field.getType());
    }
  }

  void visitType(capnp::Type type) {
    switch (type.which()) {
      case capnp::schema::Type::STRUCT:
        visitStruct(type.asStruct());
        break;
      case capnp::schema::Type::INTERFACE:
        visitInterface(type.asInterface());
        break;
      case capnp::schema::Type::LIST:
        visitType(type.asList().getElementType());
        break;
      default:
        break;
    }
  }

  const PipelineHints& known;
  // Interface and struct ids; Cap'n Proto ids are unique across both.
  kj::HashSet<uint64_t> seen;
};

capnp::Request<capnp::AnyPointer, capnp::AnyPointer> newRequest(capnp::ClientHook& hook,
    uint64_t interfaceId,
    uint16_t methodId,
    RustMessage& params,
    capnp::Capability::Client::CallHints hints) {
  capnp::Capability::Client client(hook.addRef());
  kj::Maybe<capnp::Request<capnp::AnyPointer, capnp::AnyPointer>> request;
  copyFromRust(params, [&](capnp::MessageSize size) -> capnp::AnyPointer::Builder {
    return request.emplace(client.typelessRequest(interfaceId, methodId, size, hints));
  });
  return KJ_ASSERT_NONNULL(kj::mv(request));
}

}  // namespace

void registerInterface(capnp::InterfaceSchema schema) {
  // Collected outside the lock: schemas load lazily, and collecting may be slow for an interface
  // that reaches many others. Two threads may collect the same interface; both find the same.
  PipelineHints found;
  {
    auto known = pipelineHints().lockShared();
    HintCollector collector(*known);
    collector.visitInterface(schema);
    found = kj::mv(collector.found);
  }
  if (found.size() == 0) {
    return;
  }
  auto known = pipelineHints().lockExclusive();
  for (auto& entry: found) {
    known->findOrCreate(
        entry.key, [&]() -> PipelineHints::Entry { return {entry.key, kj::mv(entry.value)}; });
  }
}

bool noPromisePipelining(uint64_t interfaceId, uint16_t methodId) {
  auto known = pipelineHints().lockShared();
  KJ_IF_SOME(methods, known->find(interfaceId)) {
    return methodId < methods.size() && methods[methodId];
  }
  return false;
}

kj::Own<CxxCall> client_send(
    const capnp::ClientHook& hook, uint64_t interfaceId, uint16_t methodId, RustMessage& params) {
  capnp::Capability::Client::CallHints hints;
  hints.noPromisePipelining = noPromisePipelining(interfaceId, methodId);
  auto remote = newRequest(mut(hook), interfaceId, methodId, params, hints).send();
  auto pipeline = kj::mv(static_cast<capnp::AnyPointer::Pipeline&>(remote));
  auto response = kj::mv(static_cast<kj::Promise<capnp::Response<capnp::AnyPointer>>&>(remote));
  return kj::heap<CxxCall>(CxxCall{
    // A continuation rather than a coroutine in call_response(): KJ can place it in the response
    // promise's arena, where a coroutine frame is an allocation of its own.
    .response = response.then(
        [](capnp::Response<capnp::AnyPointer>&& response) { return CxxMessage::copy(response); }),
    .pipeline = pipeline.releasePipelineHook(),
    .canPipeline = !hints.noPromisePipelining,
  });
}

kj::Promise<void> client_send_streaming(kj::Own<capnp::ClientHook> hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params) {
  capnp::Capability::Client::CallHints hints;
  hints.noPromisePipelining = noPromisePipelining(interfaceId, methodId);
  auto request = newRequest(*hook, interfaceId, methodId, *params, hints);
  co_await capnp::RequestHook::from(kj::mv(request))->sendStreaming();
}

kj::Own<capnp::PipelineHook> call_pipeline(const CxxCall& call) {
  return const_cast<capnp::PipelineHook&>(*call.pipeline).addRef();
}

bool call_can_pipeline(const CxxCall& call) {
  return call.canPipeline;
}

kj::Promise<kj::Own<CxxMessage>> call_response(kj::Own<CxxCall> call) {
  return kj::mv(call->response);
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

void context_set_results(capnp::CallContextHook& context, RustMessage& results) {
  copyFromRust(results, [&](capnp::MessageSize size) { return context.getResults(size); });
}

}  // namespace workerd::rust::capnp_kj
