// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/capnp-kj/ffi.rs.h>

#include <capnp/serialize.h>
#include <kj/map.h>
#include <kj/mutex.h>

#include <atomic>
#include <typeinfo>

namespace workerd::rust::capnp_kj {

namespace {

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

// Every segment of `message`, unless it has just one.
kj::Array<kj::ArrayPtr<const capnp::word>> rustSegments(const RustMessage& message) {
  if (message.segment_count() == 1) {
    return nullptr;
  }
  auto segments = message.segments();
  auto result = kj::heapArray<kj::ArrayPtr<const capnp::word>>(segments.size());
  for (auto i: kj::zeroTo(segments.size())) {
    result[i] = asWords(segments[i].bytes);
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

uint firstSegmentWords(kj::Maybe<capnp::MessageSize> sizeHint) {
  KJ_IF_SOME(size, sizeHint) {
    return size.wordCount;
  }
  return capnp::SUGGESTED_FIRST_SEGMENT_WORDS;
}

// Whether `context` is a BridgeContext: an exact type check, which costs less than a failed
// dynamic_cast for every call from C++.
kj::Maybe<BridgeContext&> tryBridgeContext(capnp::CallContextHook& context) {
  if (typeid(context) == typeid(BridgeContext)) {
    return kj::downcast<BridgeContext>(context);
  }
  return kj::none;
}

BridgeContext& bridgeContext(capnp::CallContextHook& context) {
  return KJ_REQUIRE_NONNULL(tryBridgeContext(context), "not a capnp-kj call context");
}

const BridgeContext& bridgeContext(const capnp::CallContextHook& context) {
  return bridgeContext(const_cast<capnp::CallContextHook&>(context));
}

// A capnp server whose calls a Rust server handles.
class RustServerAdapter final: public capnp::Capability::Server {
 public:
  explicit RustServerAdapter(::rust::Box<RustServer> server): server(kj::mv(server)) {}

  DispatchCallResult dispatchCall(uint64_t interfaceId,
      uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    auto& hook = capnp::CallContextHook::from(context);
    // capnpc-rust does not see `$Cxx.allowCancellation`; the Rust server stated it instead.
    auto allowCancellation = server->allows_cancellation(interfaceId, methodId);
    Dispatched dispatched{};
    KJ_IF_SOME(exception,
        kj::runCatchingExceptions([&]() { dispatched = dispatch(interfaceId, methodId, hook); })) {
      // Reading the params failed before the server ran. The server's own failures arrive in
      // take_pending_call()'s promise.
      return {kj::mv(exception), false, allowCancellation};
    }
    // Most Rust servers succeed when dispatched, and their calls need no promise of a Rust
    // future.
    kj::Promise<void> promise = dispatched.complete ? kj::READY_NOW : take_pending_call(*server);
    return {kj::mv(promise), dispatched.is_streaming, allowCancellation};
  }

 private:
  Dispatched dispatch(uint64_t interfaceId, uint16_t methodId, capnp::CallContextHook& context) {
    // The bridge's own call hands its params to the server in place, and takes the server's
    // results as a Rust message.
    KJ_IF_SOME(bridge, tryBridgeContext(context)) {
      KJ_IF_SOME(params, bridge.takeRustParams()) {
        return dispatch_rust_call(*server, interfaceId, methodId, kj::mv(params), bridge.addRef());
      }
      KJ_IF_SOME(params, bridge.shareCxxParams()) {
        return dispatch_bridge_call(
            *server, interfaceId, methodId, kj::mv(params), bridge.addRef());
      }
    }
    // Any other call: a membrane's, or the RPC system's.
    auto params = CxxMessage::copy(context.getParams());
    context.releaseParams();
    return dispatch_call(*server, interfaceId, methodId, kj::mv(params), context.addRef());
  }

  ::rust::Box<RustServer> server;
};

// The client of a Rust server. KJ's LocalClient delivers its calls, so that ordering, streaming,
// cancellation and pipelining are KJ's; this client only creates the requests that C++ builds, as
// BridgeContexts whose params a Rust server can read in place.
class RustClient final: public capnp::ClientHook, public kj::Refcounted {
 public:
  explicit RustClient(kj::Own<capnp::ClientHook> local): local(kj::mv(local)) {}

  capnp::Request<capnp::AnyPointer, capnp::AnyPointer> newCall(uint64_t interfaceId,
      uint16_t methodId,
      kj::Maybe<capnp::MessageSize> sizeHint,
      CallHints hints) override {
    auto context =
        kj::refcounted<BridgeContext>(local->addRef(), interfaceId, methodId, sizeHint, hints);
    auto root = context->paramsRoot();
    return capnp::Request<capnp::AnyPointer, capnp::AnyPointer>(
        root, kj::Own<capnp::RequestHook>(kj::mv(context)));
  }

  VoidPromiseAndPipeline call(uint64_t interfaceId,
      uint16_t methodId,
      kj::Own<capnp::CallContextHook>&& context,
      CallHints hints) override {
    return local->call(interfaceId, methodId, kj::mv(context), hints);
  }

  kj::Maybe<capnp::ClientHook&> getResolved() override {
    return local->getResolved();
  }

  kj::Maybe<kj::Promise<kj::Own<capnp::ClientHook>>> whenMoreResolved() override {
    return local->whenMoreResolved();
  }

  kj::Own<capnp::ClientHook> addRef() override {
    return kj::addRef(*this);
  }

  kj::Maybe<int> getFd() override {
    return local->getFd();
  }

  void debugInfo(kj::Vector<kj::ConstString>& chain) override {
    local->debugInfo(chain);
  }

 private:
  kj::Own<capnp::ClientHook> local;
};

}  // namespace

// A standalone copy of a message, in one allocation with its first segment.
class CxxMessage::Copy final: public CxxMessage {
 public:
  // `segment` is zeroed, exactly the size of the copy, and outlives the message.
  //
  // targetSize() and set() each traverse `root`, and both count against its message's traversal
  // limit, so a message that uses more than half of that limit fails here although a C++ server
  // could read it.
  Copy(capnp::AnyPointer::Reader root, kj::ArrayPtr<capnp::word> segment): builder(segment) {
    capTable.imbue(builder.getRoot<capnp::AnyPointer>()).set(root);
    segments = builder.getSegmentsForOutput();
    caps = capTable.getTable();
  }

 private:
  // A copy of a known size fits one segment exactly, as in capnp's copyToUnchecked(), which
  // FlatMessageBuilder exists for. Unlike MallocMessageBuilder given a first segment, it doesn't
  // zero the segment again when destroyed, just before the segment is freed.
  capnp::FlatMessageBuilder builder;
  capnp::BuilderCapabilityTable capTable;
};

namespace {

// Destroys a CxxMessage::Copy and frees the allocation that holds it and its first segment.
class CopyDisposer final: public kj::Disposer {
 public:
  void disposeImpl(void* pointer) const override {
    static_cast<CxxMessage*>(pointer)->~CxxMessage();
    ::operator delete(pointer);
  }
};

}  // namespace

// The segment is sized to the copy. MallocMessageBuilder's default first segment is 1024 zeroed
// words, which costs as much as the copy itself for a small message.
kj::Own<CxxMessage> CxxMessage::copy(capnp::AnyPointer::Reader root) {
  static_assert(sizeof(Copy) % sizeof(capnp::word) == 0);
  static const CopyDisposer disposer;
  auto words = root.targetSize().wordCount + 1;
  auto bytes = sizeof(Copy) + words * sizeof(capnp::word);
  // operator new's alignment covers both the message and a word.
  auto* memory = static_cast<kj::byte*>(::operator new(bytes));
  auto* segment = reinterpret_cast<capnp::word*>(memory + sizeof(Copy));
  auto segmentWords = kj::arrayPtr(segment, words);
  segmentWords.asBytes().fill(0);
  KJ_ON_SCOPE_FAILURE(::operator delete(memory));
  CxxMessage* message = new (memory) Copy(root, segmentWords);
  return kj::Own<CxxMessage>(message, disposer);
}

kj::Maybe<kj::Own<capnp::ClientHook>> CxxMessage::getCap(size_t index) {
  KJ_REQUIRE(index < caps.size());
  return caps[index].map([](kj::Own<capnp::ClientHook>& cap) { return cap->addRef(); });
}

RustMessageReader::RustMessageReader(const RustMessage& message)
    : firstSegment(asWords(message.segment(0))),
      allSegments(rustSegments(message)),
      reader(allSegments == nullptr ? kj::arrayPtr(firstSegment) : allSegments.asPtr(),
          unlimitedReaderOptions()),
      caps(rustCaps(message)) {}

uint64_t RustMessageReader::wordCount() const {
  if (allSegments == nullptr) {
    return firstSegment.size();
  }
  uint64_t count = 0;
  for (const auto& segment: allSegments) {
    count += segment.size();
  }
  return count;
}

capnp::AnyPointer::Reader RustMessageReader::getRoot() {
  return caps.imbue(reader.getRoot<capnp::AnyPointer>());
}

CxxMessageBuilder::CxxMessageBuilder(kj::Maybe<capnp::MessageSize> sizeHint)
    : message(firstSegmentWords(sizeHint)),
      root(caps.imbue(message.getRoot<capnp::AnyPointer>())) {}

class ContextMessage::Disposer final: public kj::Disposer {
 public:
  void disposeImpl(void* pointer) const override {
    // Releasing the owner may destroy the view, so it is released last.
    auto owner = kj::mv(static_cast<ContextMessage*>(pointer)->owner);
  }
};

kj::Own<CxxMessage> ContextMessage::share(
    kj::ArrayPtr<const kj::ArrayPtr<const capnp::word>> segments,
    kj::ArrayPtr<kj::Maybe<kj::Own<capnp::ClientHook>>> caps,
    kj::Own<capnp::CallContextHook> owner) {
  static const Disposer disposer;
  KJ_REQUIRE(!isShared(), "a message of a capnp-kj call is shared twice");
  this->segments = kj::mv(segments);
  this->caps = kj::mv(caps);
  this->owner = kj::mv(owner);
  return kj::Own<CxxMessage>(this, disposer);
}

BridgeContext::BridgeContext(::rust::Box<RustMessage> params, CallHints hints)
    : hints(hints),
      rustParams(kj::mv(params)) {}

BridgeContext::BridgeContext(kj::Own<capnp::ClientHook> target,
    uint64_t interfaceId,
    uint16_t methodId,
    kj::Maybe<capnp::MessageSize> sizeHint,
    CallHints hints)
    : target(kj::mv(target)),
      interfaceId(interfaceId),
      methodId(methodId),
      hints(hints) {
  cxxParams.emplace(sizeHint);
}

BridgeContext::~BridgeContext() noexcept(false) {}

capnp::AnyPointer::Builder BridgeContext::paramsRoot() {
  return KJ_ASSERT_NONNULL(cxxParams).root;
}

capnp::AnyPointer::Reader BridgeContext::getParams() {
  KJ_IF_SOME(params, cxxParams) {
    return params.root.asReader();
  }
  KJ_IF_SOME(reader, rustParamsReader) {
    return reader.getRoot();
  }
  KJ_IF_SOME(params, rustParams) {
    return rustParamsReader.emplace(*params).getRoot();
  }
  KJ_FAIL_REQUIRE("Can't call getParams() after releaseParams().");
}

void BridgeContext::releaseParams() {
  rustParamsReader = kj::none;
  rustParams = kj::none;
  // A Rust server reading the params in place holds the context, which frees them when it goes.
  if (!paramsView.isShared()) {
    cxxParams = kj::none;
  }
}

capnp::AnyPointer::Builder BridgeContext::getResults(kj::Maybe<capnp::MessageSize> sizeHint) {
  if (tailCallResults != kj::none) {
    // As KJ's LocalCallContext does: a tail call's results are read-only.
    return nullptr;
  }
  KJ_IF_SOME(results, cxxResults) {
    return results.root;
  }
  KJ_IF_SOME(results, rustResults) {
    // C++ wants a Rust server's results to build on, as KJ's pipelining does. The Rust results
    // stay, for whoever reads them.
    copyFromRust(*results, [&](capnp::MessageSize size) { return cxxResults.emplace(size).root; });
    return KJ_ASSERT_NONNULL(cxxResults).root;
  }
  return cxxResults.emplace(sizeHint).root;
}

kj::Promise<void> BridgeContext::tailCall(kj::Own<capnp::RequestHook>&& request) {
  auto result = directTailCall(kj::mv(request));
  KJ_IF_SOME(fulfiller, tailCallPipelineFulfiller) {
    fulfiller->fulfill(capnp::AnyPointer::Pipeline(kj::mv(result.pipeline)));
  }
  return kj::mv(result.promise);
}

void BridgeContext::setPipeline(kj::Own<capnp::PipelineHook>&& pipeline) {
  KJ_IF_SOME(fulfiller, tailCallPipelineFulfiller) {
    fulfiller->fulfill(capnp::AnyPointer::Pipeline(kj::mv(pipeline)));
  }
}

kj::Promise<capnp::AnyPointer::Pipeline> BridgeContext::onTailCall() {
  auto paf = kj::newPromiseAndFulfiller<capnp::AnyPointer::Pipeline>();
  tailCallPipelineFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

// As KJ's LocalCallContext does.
capnp::ClientHook::VoidPromiseAndPipeline BridgeContext::directTailCall(
    kj::Own<capnp::RequestHook>&& request) {
  KJ_REQUIRE(cxxResults == kj::none && rustResults == kj::none,
      "Can't call tailCall() after initializing the results struct.");
  if (hints.onlyPromisePipeline) {
    return {kj::NEVER_DONE, capnp::PipelineHook::from(request->sendForPipeline())};
  }
  if (isStreaming) {
    return {request->sendStreaming(), capnp::getDisabledPipeline()};
  }
  auto promise = request->send();
  auto voidPromise = promise.then(
      [self = kj::addRef(*this)](capnp::Response<capnp::AnyPointer>&& response) mutable {
    self->tailCallResults = kj::mv(response);
  });
  return {kj::mv(voidPromise), capnp::PipelineHook::from(kj::mv(promise))};
}

kj::Own<capnp::CallContextHook> BridgeContext::addRef() {
  return kj::addRef(*this);
}

capnp::RemotePromise<capnp::AnyPointer> BridgeContext::send() {
  kj::Own<capnp::ClientHook> client;
  KJ_IF_SOME(t, target) {
    client = kj::mv(t);
  } else {
    KJ_FAIL_REQUIRE("Already called send() on this request.");
  }
  target = kj::none;
  auto call = client->call(interfaceId, methodId, kj::addRef(*this), hints);
  auto promise =
      call.promise.then([self = kj::addRef(*this)]() mutable { return self->response(); });
  return capnp::RemotePromise<capnp::AnyPointer>(
      kj::mv(promise), capnp::AnyPointer::Pipeline(kj::mv(call.pipeline)));
}

kj::Promise<void> BridgeContext::sendStreaming() {
  // As KJ's LocalRequest does: the flag only matters if the call is forwarded and tail-called.
  isStreaming = true;
  return send().ignoreResult();
}

capnp::AnyPointer::Pipeline BridgeContext::sendForPipeline() {
  hints.onlyPromisePipeline = true;
  KJ_IF_SOME(client, target) {
    auto call = client->call(interfaceId, methodId, kj::addRef(*this), hints);
    target = kj::none;
    return capnp::AnyPointer::Pipeline(kj::mv(call.pipeline));
  }
  KJ_FAIL_REQUIRE("Already called send() on this request.");
}

// The response of a request C++ sent, read in place. The context is the response's hook, so it
// holds whatever the reader reads.
capnp::Response<capnp::AnyPointer> BridgeContext::response() {
  releaseParams();
  capnp::AnyPointer::Reader reader;
  KJ_IF_SOME(results, tailCallResults) {
    reader = results;
  } else KJ_IF_SOME(results, rustResults) {
    reader = rustResultsReader.emplace(*results).getRoot();
  } else KJ_IF_SOME(results, cxxResults) {
    reader = results.root.asReader();
  }
  return capnp::Response<capnp::AnyPointer>(reader, kj::addRef(*this));
}

kj::Maybe<::rust::Box<RustMessage>> BridgeContext::takeRustParams() {
  if (rustParamsReader != kj::none) {
    return kj::none;
  }
  auto taken = kj::mv(rustParams);
  rustParams = kj::none;
  return taken;
}

kj::Maybe<kj::Own<CxxMessage>> BridgeContext::shareCxxParams() {
  KJ_IF_SOME(params, cxxParams) {
    return paramsView.share(
        params.message.getSegmentsForOutput(), params.caps.getTable(), addRef());
  }
  return kj::none;
}

void BridgeContext::setRustResults(::rust::Box<RustMessage> results) {
  KJ_REQUIRE(cxxResults == kj::none && tailCallResults == kj::none && rustResults == kj::none,
      "a Rust server set results that are already set");
  rustResults = kj::mv(results);
}

kj::Maybe<const RustMessage&> BridgeContext::getRustResults() const {
  return rustResults.map(
      [](const ::rust::Box<RustMessage>& results) -> const RustMessage& { return *results; });
}

kj::Own<CxxMessage> BridgeContext::shareResults() {
  // The same precedence as response(): a tail call's results, then Rust's, which Rust reads
  // through getRustResults() first, then C++'s.
  KJ_IF_SOME(results, tailCallResults) {
    return CxxMessage::copy(results);
  }
  KJ_IF_SOME(results, cxxResults) {
    return resultsView.share(
        results.message.getSegmentsForOutput(), results.caps.getTable(), addRef());
  }
  return resultsView.share(nullptr, nullptr, addRef());
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
// changes once added, so a view of an entry stays valid.
using PipelineHints = kj::HashMap<uint64_t, kj::Array<bool>>;

struct PipelineHintRegistry {
  kj::MutexGuarded<PipelineHints> hints;
  // Incremented whenever an interface is added, for threads to see that their caches are stale.
  std::atomic<uint64_t> version = 0;
};

PipelineHintRegistry& pipelineHintRegistry() {
  static PipelineHintRegistry registry;
  return registry;
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
    const RustMessage& params,
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
  auto& registry = pipelineHintRegistry();
  // Collected outside the lock: schemas load lazily, and collecting may be slow for an interface
  // that reaches many others. Two threads may collect the same interface; both find the same.
  PipelineHints found;
  {
    auto known = registry.hints.lockShared();
    HintCollector collector(*known);
    collector.visitInterface(schema);
    found = kj::mv(collector.found);
  }
  if (found.size() == 0) {
    return;
  }
  auto known = registry.hints.lockExclusive();
  for (auto& entry: found) {
    known->findOrCreate(
        entry.key, [&]() -> PipelineHints::Entry { return {entry.key, kj::mv(entry.value)}; });
  }
  registry.version.fetch_add(1, std::memory_order_release);
}

bool noPromisePipelining(uint64_t interfaceId, uint16_t methodId) {
  // Each thread looks hints up in its own copy of the registry's views, refreshed when an
  // interface is added, so that calls take no lock shared across threads. Interfaces are
  // registered when C++ first hands them to Rust, so refreshes stop early.
  struct Cache {
    uint64_t version = 0;
    kj::HashMap<uint64_t, kj::ArrayPtr<const bool>> methods;
  };
  static thread_local Cache cache;
  auto& registry = pipelineHintRegistry();
  auto version = registry.version.load(std::memory_order_acquire);
  if (cache.version != version) {
    auto known = registry.hints.lockShared();
    cache.methods.clear();
    for (auto& entry: *known) {
      cache.methods.insert(entry.key, entry.value.asPtr());
    }
    cache.version = version;
  }
  KJ_IF_SOME(methods, cache.methods.find(interfaceId)) {
    return methodId < methods.size() && methods[methodId];
  }
  return false;
}

kj::Own<CxxCall> client_call(const capnp::ClientHook& hook,
    uint64_t interfaceId,
    uint16_t methodId,
    ::rust::Box<RustMessage> params) {
  capnp::Capability::Client::CallHints hints;
  hints.noPromisePipelining = noPromisePipelining(interfaceId, methodId);
  auto context = kj::refcounted<BridgeContext>(kj::mv(params), hints);
  auto call = mut(hook).call(interfaceId, methodId, kj::addRef(*context), hints);
  return kj::heap<CxxCall>(CxxCall{
    .context = kj::mv(context),
    .completion = kj::mv(call.promise),
    .pipeline = kj::mv(call.pipeline),
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

kj::Promise<kj::Own<capnp::CallContextHook>> call_response(kj::Own<CxxCall> call) {
  // A continuation rather than a coroutine: KJ can place it in the completion promise's arena,
  // where a coroutine frame is an allocation of its own.
  return call->completion.then([context = kj::mv(call->context)]() mutable {
    context->releaseParams();
    return kj::Own<capnp::CallContextHook>(kj::mv(context));
  });
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
  return kj::refcounted<RustClient>(capnp::ClientHook::from(
      capnp::Capability::Client(kj::heap<RustServerAdapter>(kj::mv(server)))));
}

void context_set_results(capnp::CallContextHook& context, const RustMessage& results) {
  copyFromRust(results, [&](capnp::MessageSize size) { return context.getResults(size); });
}

void context_set_rust_results(capnp::CallContextHook& context, ::rust::Box<RustMessage> results) {
  bridgeContext(context).setRustResults(kj::mv(results));
}

bool context_has_rust_results(const capnp::CallContextHook& context) {
  return bridgeContext(context).getRustResults() != kj::none;
}

const RustMessage& context_rust_results(const capnp::CallContextHook& context) {
  return KJ_REQUIRE_NONNULL(bridgeContext(context).getRustResults());
}

kj::Own<CxxMessage> context_results_message(capnp::CallContextHook& context) {
  return bridgeContext(context).shareResults();
}

}  // namespace workerd::rust::capnp_kj
