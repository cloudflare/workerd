// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace.h"

#include <workerd/util/use-perfetto-categories.h>

#include <v8-profiler.h>

namespace workerd {

namespace {

// Get the monotonic clock for timing
kj::TimePoint now() {
  return kj::systemPreciseMonotonicClock().now();
}

}  // namespace

// ============================================================================
// StackTraceKey implementation

uint AsyncTraceContext::StackTraceKey::hashCode() const {
  uint h = 0;
  for (const auto& frame: frames) {
    h = h * 31 + kj::hashCode(frame);
  }
  return h;
}

bool AsyncTraceContext::StackTraceKey::operator==(const StackTraceKey& other) const {
  if (frames.size() != other.frames.size()) return false;
  for (size_t i = 0; i < frames.size(); i++) {
    if (frames[i] != other.frames[i]) return false;
  }
  return true;
}

// ============================================================================
// AsyncTraceContext implementation

AsyncTraceContext::AsyncTraceContext(): startTime(now()) {
  // Create the root resource - represents the entire request scope
  // The "callback" for root starts immediately at time 0
  ResourceRecord root{
    .triggerId = INVALID_ID,
    .type = ResourceType::kRoot,
    .stackTraceId = 0,  // No stack trace for root
    .createdAt = 0,
    .callbackStartedAt = 0,  // Root callback starts at creation
    .callbackEndedAt = 0,    // Will be set in destructor
    .destroyedAt = 0,
  };
  resources.insert(ROOT_ID, kj::mv(root));

  // Push root onto context stack
  contextStack.add(ROOT_ID);

  // Emit Perfetto event for root
  emitResourceCreated(ROOT_ID, ResourceType::kRoot, INVALID_ID);
  emitCallbackStart(ROOT_ID);
}

v8::Local<v8::Private> AsyncTraceContext::getOrCreateAsyncIdSymbol(v8::Isolate* isolate) {
  KJ_IF_SOME(symbol, asyncIdSymbol) {
    return symbol.Get(isolate);
  }

  // Create the symbol lazily
  v8::HandleScope handleScope(isolate);
  auto symbolName = v8::String::NewFromUtf8Literal(isolate, "asyncTraceId");
  auto symbol = v8::Private::New(isolate, symbolName);
  asyncIdSymbol = v8::Global<v8::Private>(isolate, symbol);
  return symbol;
}

AsyncTraceContext::~AsyncTraceContext() noexcept(false) {
  // Record final timing for root resource
  KJ_IF_SOME(record, resources.find(ROOT_ID)) {
    record.callbackEndedAt = nowNs();
    record.destroyedAt = nowNs();
  }

  // End the root callback
  emitCallbackEnd(ROOT_ID);
  emitResourceDestroyed(ROOT_ID);

#ifdef WORKERD_USE_PERFETTO
  // Emit a trace summary event with statistics
  TRACE_EVENT_INSTANT("workerd", "AsyncTrace::Summary", "resourceCount", resources.size(),
      "stackTraceCount", stackTraces.size(), "annotationCount", annotations.size(), "durationNs",
      nowNs());
#endif

  // Log trace JSON at INFO level (useful for debugging)
  KJ_LOG(INFO, "AsyncTrace completed", toJson());
}

uint64_t AsyncTraceContext::nowNs() const {
  auto elapsed = now() - startTime;
  return elapsed / kj::NANOSECONDS;
}

uint32_t AsyncTraceContext::captureStackTrace(v8::Isolate* isolate) {
  // If no isolate provided, return a placeholder stack trace ID
  if (isolate == nullptr) {
    return 0;
  }

  // Capture V8 stack trace
  constexpr int kMaxFrames = 16;
  auto stackTrace = v8::StackTrace::CurrentStackTrace(isolate, kMaxFrames);

  kj::Vector<kj::String> frames;
  int frameCount = stackTrace->GetFrameCount();
  for (int i = 0; i < frameCount; i++) {
    auto frame = stackTrace->GetFrame(isolate, i);

    kj::String functionName;
    auto fnName = frame->GetFunctionName();
    if (!fnName.IsEmpty()) {
      v8::String::Utf8Value utf8(isolate, fnName);
      functionName = kj::str(*utf8);
    } else {
      functionName = kj::str("<anonymous>");
    }

    kj::String scriptName;
    auto sName = frame->GetScriptName();
    if (!sName.IsEmpty()) {
      v8::String::Utf8Value utf8(isolate, sName);
      scriptName = kj::str(*utf8);
    } else {
      scriptName = kj::str("<unknown>");
    }

    int line = frame->GetLineNumber();
    int col = frame->GetColumn();

    frames.add(kj::str(functionName, " @ ", scriptName, ":", line, ":", col));
  }

  // Check if we've seen this stack trace before
  StackTraceKey key{frames.releaseAsArray()};

  KJ_IF_SOME(existingId, stackTraceIds.find(key)) {
    return existingId;
  }

  // New stack trace - assign an ID and store it
  uint32_t id = nextStackTraceId++;

  // Clone the frames array for storage (can't copy kj::String)
  auto framesCopy = kj::heapArrayBuilder<kj::String>(key.frames.size());
  for (auto& frame: key.frames) {
    framesCopy.add(kj::str(frame));
  }

  stackTraces.add(StackTraceInfo{
    .id = id,
    .frames = framesCopy.finish(),
  });
  stackTraceIds.insert(kj::mv(key), id);

  return id;
}

AsyncTraceContext::AsyncId AsyncTraceContext::createResource(
    ResourceType type, v8::Isolate* isolate) {
  return createResourceWithTrigger(type, currentId, isolate);
}

AsyncTraceContext::AsyncId AsyncTraceContext::createResourceWithTrigger(
    ResourceType type, AsyncId triggerId, v8::Isolate* isolate) {
  AsyncId id = nextId++;
  uint64_t timestamp = nowNs();
  uint32_t stackId = captureStackTrace(isolate);

  ResourceRecord record{
    .triggerId = triggerId,
    .type = type,
    .stackTraceId = stackId,
    .createdAt = timestamp,
    .callbackStartedAt = 0,
    .callbackEndedAt = 0,
    .destroyedAt = 0,
  };

  resources.insert(id, kj::mv(record));

  // Emit Perfetto event
  emitResourceCreated(id, type, triggerId);

  return id;
}

void AsyncTraceContext::destroyResource(AsyncId id) {
  KJ_IF_SOME(record, resources.find(id)) {
    record.destroyedAt = nowNs();
    emitResourceDestroyed(id);
  }
}

AsyncTraceContext::CallbackScope::CallbackScope(AsyncTraceContext& ctx, AsyncId id): ctx(ctx) {
  ctx.enterCallback(id);
}

AsyncTraceContext::CallbackScope::~CallbackScope() {
  ctx.exitCallback();
}

void AsyncTraceContext::enterCallback(AsyncId id) {
  // Record callback start time
  KJ_IF_SOME(record, resources.find(id)) {
    if (record.callbackStartedAt == 0) {
      record.callbackStartedAt = nowNs();
    }
  }

  // Push onto context stack
  contextStack.add(currentId);
  currentId = id;

  emitCallbackStart(id);
}

void AsyncTraceContext::exitCallback() {
  // Record callback end time
  KJ_IF_SOME(record, resources.find(currentId)) {
    record.callbackEndedAt = nowNs();
  }

  emitCallbackEnd(currentId);

  // Pop from context stack
  KJ_ASSERT(contextStack.size() > 0, "exitCallback called without matching enterCallback");
  currentId = contextStack.back();
  contextStack.removeLast();
}

void AsyncTraceContext::annotate(AsyncId id, kj::StringPtr key, kj::StringPtr value) {
  annotations.add(Annotation{
    .asyncId = id,
    .key = kj::str(key),
    .value = kj::str(value),
  });
}

void AsyncTraceContext::setPromiseAsyncId(
    v8::Isolate* isolate, v8::Local<v8::Promise> promise, AsyncId id) {
  v8::HandleScope handleScope(isolate);
  auto context = isolate->GetCurrentContext();
  auto symbol = getOrCreateAsyncIdSymbol(isolate);

  // Store the ID as a BigInt to preserve full 64-bit precision
  auto value = v8::BigInt::NewFromUnsigned(isolate, id);
  promise->SetPrivate(context, symbol, value).Check();
}

AsyncTraceContext::AsyncId AsyncTraceContext::getPromiseAsyncId(
    v8::Isolate* isolate, v8::Local<v8::Promise> promise) {
  v8::HandleScope handleScope(isolate);
  auto context = isolate->GetCurrentContext();
  auto symbol = getOrCreateAsyncIdSymbol(isolate);

  auto maybeValue = promise->GetPrivate(context, symbol);
  v8::Local<v8::Value> value;
  if (!maybeValue.ToLocal(&value) || !value->IsBigInt()) {
    return INVALID_ID;
  }

  auto bigint = value.As<v8::BigInt>();
  return bigint->Uint64Value();
}

bool AsyncTraceContext::hasPromiseAsyncId(v8::Isolate* isolate, v8::Local<v8::Promise> promise) {
  v8::HandleScope handleScope(isolate);
  auto context = isolate->GetCurrentContext();
  auto symbol = getOrCreateAsyncIdSymbol(isolate);

  auto maybeResult = promise->HasPrivate(context, symbol);
  bool result = false;
  return maybeResult.To(&result) && result;
}

AsyncTraceContext::AsyncTrace AsyncTraceContext::finalize() {
  uint64_t duration = nowNs();

  // Convert resources map to array
  kj::Vector<ResourceInfo> resourceList;
  for (auto& entry: resources) {
    auto& record = entry.value;
    resourceList.add(ResourceInfo{
      .asyncId = entry.key,
      .triggerId = record.triggerId,
      .type = record.type,
      .stackTraceId = record.stackTraceId,
      .createdAt = record.createdAt,
      .callbackStartedAt = record.callbackStartedAt,
      .callbackEndedAt = record.callbackEndedAt,
      .destroyedAt = record.destroyedAt,
    });
  }

  // Copy stack traces
  kj::Vector<StackTraceInfo> stackTraceList;
  for (auto& st: stackTraces) {
    auto framesCopy = kj::heapArrayBuilder<kj::String>(st.frames.size());
    for (auto& frame: st.frames) {
      framesCopy.add(kj::str(frame));
    }
    stackTraceList.add(StackTraceInfo{
      .id = st.id,
      .frames = framesCopy.finish(),
    });
  }

  return AsyncTrace{
    .requestDurationNs = duration,
    .resources = resourceList.releaseAsArray(),
    .stackTraces = stackTraceList.releaseAsArray(),
    .annotations = annotations.releaseAsArray(),
  };
}

kj::String AsyncTraceContext::toJson() {
  // Build JSON representation of the trace data
  // Note: This is a simple implementation; for production use, consider using
  // a proper JSON library.

  kj::Vector<char> json;
  auto append = [&](kj::StringPtr s) {
    for (char c: s) json.add(c);
  };
  auto appendQuoted = [&](kj::StringPtr s) {
    json.add('"');
    for (char c: s) {
      // Escape special characters
      if (c == '"' || c == '\\') {
        json.add('\\');
        json.add(c);
      } else if (c == '\n') {
        json.add('\\');
        json.add('n');
      } else if (c == '\r') {
        json.add('\\');
        json.add('r');
      } else if (c == '\t') {
        json.add('\\');
        json.add('t');
      } else {
        json.add(c);
      }
    }
    json.add('"');
  };

  append("{\n");
  append("  \"requestDurationNs\": ");
  append(kj::str(nowNs()));
  append(",\n");

  // Resources
  append("  \"resources\": [\n");
  bool firstResource = true;
  for (auto& entry: resources) {
    if (!firstResource) append(",\n");
    firstResource = false;
    auto& record = entry.value;
    append("    {");
    append("\"asyncId\": ");
    append(kj::str(entry.key));
    append(", \"triggerId\": ");
    append(kj::str(record.triggerId));
    append(", \"type\": ");
    appendQuoted(resourceTypeName(record.type));
    append(", \"stackTraceId\": ");
    append(kj::str(record.stackTraceId));
    append(", \"createdAt\": ");
    append(kj::str(record.createdAt));
    append(", \"callbackStartedAt\": ");
    append(kj::str(record.callbackStartedAt));
    append(", \"callbackEndedAt\": ");
    append(kj::str(record.callbackEndedAt));
    append(", \"destroyedAt\": ");
    append(kj::str(record.destroyedAt));
    append("}");
  }
  append("\n  ],\n");

  // Stack traces
  append("  \"stackTraces\": [\n");
  bool firstStackTrace = true;
  for (auto& st: stackTraces) {
    if (!firstStackTrace) append(",\n");
    firstStackTrace = false;
    append("    {\"id\": ");
    append(kj::str(st.id));
    append(", \"frames\": [");
    bool firstFrame = true;
    for (auto& frame: st.frames) {
      if (!firstFrame) append(", ");
      firstFrame = false;
      appendQuoted(frame);
    }
    append("]}");
  }
  append("\n  ],\n");

  // Annotations
  append("  \"annotations\": [\n");
  bool firstAnnotation = true;
  for (auto& ann: annotations) {
    if (!firstAnnotation) append(",\n");
    firstAnnotation = false;
    append("    {\"asyncId\": ");
    append(kj::str(ann.asyncId));
    append(", \"key\": ");
    appendQuoted(ann.key);
    append(", \"value\": ");
    appendQuoted(ann.value);
    append("}");
  }
  append("\n  ]\n");

  append("}\n");

  // Add NUL terminator for kj::String
  json.add('\0');

  return kj::String(json.releaseAsArray());
}

// ============================================================================
// Perfetto emission

void AsyncTraceContext::emitResourceCreated(AsyncId id, ResourceType type, AsyncId triggerId) {
#ifdef WORKERD_USE_PERFETTO
  TRACE_EVENT_INSTANT("workerd", "AsyncResource::Create", "asyncId", id, "type",
      resourceTypeName(type).cStr(), "triggerId", triggerId);

  // Begin an async slice for this resource
  TRACE_EVENT_BEGIN("workerd", perfetto::DynamicString{resourceTypeName(type).cStr()},
      perfetto::Track(id), "asyncId", id, "triggerId", triggerId);

  // If there's a trigger, add a flow event to show causality
  if (triggerId != INVALID_ID) {
    // Flow from trigger to this resource
    TRACE_EVENT_INSTANT("workerd", "AsyncFlow", perfetto::Track(id),
        PERFETTO_FLOW_FROM_POINTER(reinterpret_cast<void*>(triggerId)));
  }
#endif
}

void AsyncTraceContext::emitCallbackStart(AsyncId id) {
#ifdef WORKERD_USE_PERFETTO
  TRACE_EVENT_BEGIN("workerd", "Callback",
      perfetto::Track(id + 0x100000000),  // Offset to avoid track collision
      "asyncId", id);
#endif
}

void AsyncTraceContext::emitCallbackEnd(AsyncId id) {
#ifdef WORKERD_USE_PERFETTO
  TRACE_EVENT_END("workerd", perfetto::Track(id + 0x100000000));
#endif
}

void AsyncTraceContext::emitResourceDestroyed(AsyncId id) {
#ifdef WORKERD_USE_PERFETTO
  TRACE_EVENT_END("workerd", perfetto::Track(id));

  TRACE_EVENT_INSTANT("workerd", "AsyncResource::Destroy", "asyncId", id,
      PERFETTO_TERMINATING_FLOW_FROM_POINTER(reinterpret_cast<void*>(id)));
#endif
}

}  // namespace workerd
