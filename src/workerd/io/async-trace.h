// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/hash.h>
#include <kj/map.h>
#include <kj/string.h>
#include <kj/time.h>
#include <kj/vector.h>

namespace workerd {

// AsyncTraceContext provides async operation tracking similar to Node.js's async_hooks,
// enabling bubbleprof-style visualization of async activity within a Worker request.
//
// This class tracks:
// - Async resource creation and destruction
// - Causality (which resource triggered which)
// - Timing (when callbacks start/end, async delays)
// - Stack traces at resource creation (for grouping in visualization)
//
// Usage:
//   1. Create an AsyncTraceContext at the start of a request
//   2. Use createResource() when a new async operation starts
//   3. Use CallbackScope when entering/exiting async callbacks
//   4. Call finalize() at request end to get the trace data
//
// The trace data can then be processed to generate bubbleprof-style visualizations.

class AsyncTraceContext {
 public:
  using AsyncId = uint64_t;
  static constexpr AsyncId ROOT_ID = 1;
  static constexpr AsyncId INVALID_ID = 0;

  // Types of async resources we track
  enum class ResourceType : uint16_t {
    kRoot = 0,             // The root context (request handler)
    kJsPromise,            // JavaScript promise
    kKjPromise,            // KJ promise (C++ side)
    kKjToJsBridge,         // KJ promise wrapped for JS
    kJsToKjBridge,         // JS promise awaited in KJ
    kFetch,                // fetch() subrequest
    kCacheGet,             // Cache API get
    kCachePut,             // Cache API put
    kKvGet,                // KV get
    kKvPut,                // KV put
    kKvDelete,             // KV delete
    kKvList,               // KV list
    kDurableObjectGet,     // DO storage get
    kDurableObjectPut,     // DO storage put
    kDurableObjectDelete,  // DO storage delete
    kDurableObjectList,    // DO storage list
    kDurableObjectCall,    // DO RPC call
    kR2Get,                // R2 get
    kR2Put,                // R2 put
    kR2Delete,             // R2 delete
    kR2List,               // R2 list
    kD1Query,              // D1 query
    kQueueSend,            // Queue send
    kTimer,                // setTimeout/setInterval
    kStreamRead,           // ReadableStream read
    kStreamWrite,          // WritableStream write
    kWebSocket,            // WebSocket operation
    kCrypto,               // Crypto operation (async)
    kAiInference,          // AI inference
    kOther,                // Unclassified
  };

  // Information about a captured stack trace
  struct StackTraceInfo {
    uint32_t id;
    kj::Array<kj::String> frames;  // Function name @ script:line:col
  };

  // Information about a single async resource
  struct ResourceInfo {
    AsyncId asyncId;
    AsyncId triggerId;
    ResourceType type;
    uint32_t stackTraceId;

    // Timing in nanoseconds relative to request start
    uint64_t createdAt;
    uint64_t callbackStartedAt;  // 0 if callback never ran
    uint64_t callbackEndedAt;    // 0 if callback never finished
    uint64_t destroyedAt;        // 0 if not yet destroyed

    // Computed metrics
    uint64_t asyncDelayNs() const {
      return callbackStartedAt > 0 ? callbackStartedAt - createdAt : 0;
    }
    uint64_t syncTimeNs() const {
      return callbackEndedAt > 0 ? callbackEndedAt - callbackStartedAt : 0;
    }
  };

  // Annotation attached to a resource (e.g., URL for fetch)
  struct Annotation {
    AsyncId asyncId;
    kj::String key;
    kj::String value;
  };

  // The complete trace for a request
  struct AsyncTrace {
    uint64_t requestDurationNs;
    kj::Array<ResourceInfo> resources;
    kj::Array<StackTraceInfo> stackTraces;
    kj::Array<Annotation> annotations;
  };

  // Constructor - doesn't require V8 isolate, private symbol created lazily
  AsyncTraceContext();
  ~AsyncTraceContext() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(AsyncTraceContext);

  // --- Resource lifecycle ---

  // Create a new async resource. Captures current stack trace if isolate provided.
  // Returns the new resource's ID. The trigger ID is automatically set to current().
  AsyncId createResource(ResourceType type, v8::Isolate* isolate = nullptr);

  // Create a new resource with an explicit trigger ID
  AsyncId createResourceWithTrigger(
      ResourceType type, AsyncId triggerId, v8::Isolate* isolate = nullptr);

  // Mark a resource as destroyed
  void destroyResource(AsyncId id);

  // --- Execution context ---

  // Get the currently executing async ID
  AsyncId current() const {
    return currentId;
  }

  // RAII scope for callback execution. Records timing.
  class CallbackScope {
   public:
    CallbackScope(AsyncTraceContext& ctx, AsyncId id);
    ~CallbackScope();
    KJ_DISALLOW_COPY_AND_MOVE(CallbackScope);

   private:
    AsyncTraceContext& ctx;
  };

  // Non-RAII enter/exit for V8 promise hooks (where RAII doesn't work)
  void enterCallback(AsyncId id);
  void exitCallback();

  // --- Annotations ---

  // Attach metadata to a resource
  void annotate(AsyncId id, kj::StringPtr key, kj::StringPtr value);

  // --- V8 Promise tracking ---

  // Store/retrieve AsyncId on a V8 promise using a private symbol.
  // These methods require the isolate to be passed in.
  void setPromiseAsyncId(v8::Isolate* isolate, v8::Local<v8::Promise> promise, AsyncId id);
  AsyncId getPromiseAsyncId(v8::Isolate* isolate, v8::Local<v8::Promise> promise);
  bool hasPromiseAsyncId(v8::Isolate* isolate, v8::Local<v8::Promise> promise);

  // --- Output ---

  // Finalize and get the complete trace data
  AsyncTrace finalize();

  // Serialize the trace data to JSON format
  kj::String toJson();

  // --- Perfetto emission ---

  // Emit trace events to Perfetto (if enabled)
  void emitResourceCreated(AsyncId id, ResourceType type, AsyncId triggerId);
  void emitCallbackStart(AsyncId id);
  void emitCallbackEnd(AsyncId id);
  void emitResourceDestroyed(AsyncId id);

  // --- Static helpers ---

  static kj::StringPtr resourceTypeName(ResourceType type);

 private:
  kj::TimePoint startTime;

  AsyncId nextId = ROOT_ID + 1;
  AsyncId currentId = ROOT_ID;

  // Stack for tracking nested callback contexts (for exitCallback)
  kj::Vector<AsyncId> contextStack;

  // Resource records indexed by AsyncId
  struct ResourceRecord {
    AsyncId triggerId;
    ResourceType type;
    uint32_t stackTraceId;
    uint64_t createdAt;
    uint64_t callbackStartedAt = 0;
    uint64_t callbackEndedAt = 0;
    uint64_t destroyedAt = 0;
  };
  kj::HashMap<AsyncId, ResourceRecord> resources;

  // Stack trace deduplication
  struct StackTraceKey {
    kj::Array<kj::String> frames;

    uint hashCode() const;
    bool operator==(const StackTraceKey& other) const;
  };
  kj::HashMap<StackTraceKey, uint32_t> stackTraceIds;
  kj::Vector<StackTraceInfo> stackTraces;
  uint32_t nextStackTraceId = 0;

  // Annotations storage
  kj::Vector<Annotation> annotations;

  // Private symbol for storing AsyncId on promises (created lazily)
  kj::Maybe<v8::Global<v8::Private>> asyncIdSymbol;

  // Ensure private symbol exists, creating it if needed
  v8::Local<v8::Private> getOrCreateAsyncIdSymbol(v8::Isolate* isolate);

  // Helper to capture and deduplicate stack trace (requires isolate for V8 stack)
  uint32_t captureStackTrace(v8::Isolate* isolate);

  // Get current time relative to request start in nanoseconds
  uint64_t nowNs() const;
};

// Static helper to get resource type name
inline kj::StringPtr AsyncTraceContext::resourceTypeName(ResourceType type) {
  switch (type) {
    case ResourceType::kRoot:
      return "root"_kj;
    case ResourceType::kJsPromise:
      return "js-promise"_kj;
    case ResourceType::kKjPromise:
      return "kj-promise"_kj;
    case ResourceType::kKjToJsBridge:
      return "kj-to-js"_kj;
    case ResourceType::kJsToKjBridge:
      return "js-to-kj"_kj;
    case ResourceType::kFetch:
      return "fetch"_kj;
    case ResourceType::kCacheGet:
      return "cache-get"_kj;
    case ResourceType::kCachePut:
      return "cache-put"_kj;
    case ResourceType::kKvGet:
      return "kv-get"_kj;
    case ResourceType::kKvPut:
      return "kv-put"_kj;
    case ResourceType::kKvDelete:
      return "kv-delete"_kj;
    case ResourceType::kKvList:
      return "kv-list"_kj;
    case ResourceType::kDurableObjectGet:
      return "do-get"_kj;
    case ResourceType::kDurableObjectPut:
      return "do-put"_kj;
    case ResourceType::kDurableObjectDelete:
      return "do-delete"_kj;
    case ResourceType::kDurableObjectList:
      return "do-list"_kj;
    case ResourceType::kDurableObjectCall:
      return "do-call"_kj;
    case ResourceType::kR2Get:
      return "r2-get"_kj;
    case ResourceType::kR2Put:
      return "r2-put"_kj;
    case ResourceType::kR2Delete:
      return "r2-delete"_kj;
    case ResourceType::kR2List:
      return "r2-list"_kj;
    case ResourceType::kD1Query:
      return "d1-query"_kj;
    case ResourceType::kQueueSend:
      return "queue-send"_kj;
    case ResourceType::kTimer:
      return "timer"_kj;
    case ResourceType::kStreamRead:
      return "stream-read"_kj;
    case ResourceType::kStreamWrite:
      return "stream-write"_kj;
    case ResourceType::kWebSocket:
      return "websocket"_kj;
    case ResourceType::kCrypto:
      return "crypto"_kj;
    case ResourceType::kAiInference:
      return "ai-inference"_kj;
    case ResourceType::kOther:
      return "other"_kj;
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd
