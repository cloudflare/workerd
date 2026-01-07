# Async Trace Implementation Progress

This file tracks progress on implementing async tracing for Workers, enabling clinicjs/bubbleprof-style visualization of async activity.

## Goal

Enable the ability to implement a clinicjs-style bubbleprof visualization of async activity in workers. This involves tracking:
- Async resource creation and destruction
- Causality chains (which async operation triggered which)
- Timing (when callbacks start/end, async delays between creation and execution)
- Stack traces at resource creation (for grouping in visualization)

## Architecture Overview

The implementation consists of:

1. **AsyncTraceContext** (`src/workerd/io/async-trace.h/.c++`): Core tracking data structure
   - Tracks async resources with unique IDs
   - Records parent/child relationships (trigger IDs)
   - Captures stack traces (deduplicated)
   - Records timing at nanosecond precision
   - Supports annotations (e.g., URLs for fetch operations)
   - Emits Perfetto trace events when enabled
   - Outputs JSON trace data at request completion

2. **AsyncTracePromiseHook** (`src/workerd/io/async-trace-hooks.h/.c++`): V8 Promise integration
   - Uses V8's PromiseHook API to intercept promise lifecycle
   - Tracks: init (creation), before (callback about to run), after (callback done), resolve
   - Stores AsyncId on promises using V8 private symbols

3. **IoContext Integration** (`src/workerd/io/io-context.h/.c++`):
   - `asyncTrace` member holds optional AsyncTraceContext per request
   - `enableAsyncTrace()` method to activate tracing
   - `getAsyncTrace()` accessor used throughout codebase
   - Currently auto-enabled when Perfetto tracing is active

4. **Worker Isolate Setup** (`src/workerd/io/worker.c++`):
   - Promise hook installed during isolate creation
   - Hook checks `IoContext::tryCurrent()->getAsyncTrace()` for each event

5. **API Instrumentation** (partial):
   - `fetch()` in `src/workerd/api/http.c++` - creates kFetch resource with URL/method annotations
   - `cache.match/put/delete` in `src/workerd/api/cache.c++` - creates kCacheGet/kCachePut resources
   - `setTimeout/setInterval` in `src/workerd/api/global-scope.c++` - creates kTimer resources

## Resource Types Defined

```cpp
enum class ResourceType : uint16_t {
  kRoot,              // Root context (request handler)
  kJsPromise,         // JavaScript promise
  kKjPromise,         // KJ promise (C++ side)
  kKjToJsBridge,      // KJ promise wrapped for JS
  kJsToKjBridge,      // JS promise awaited in KJ
  kFetch,             // fetch() subrequest
  kCacheGet,          // Cache API get
  kCachePut,          // Cache API put
  kKvGet/Put/Delete/List,      // KV operations
  kDurableObjectGet/Put/Delete/List/Call,  // DO operations
  kR2Get/Put/Delete/List,      // R2 operations
  kD1Query,           // D1 query
  kQueueSend,         // Queue send
  kTimer,             // setTimeout/setInterval
  kStreamRead/Write,  // Stream operations
  kWebSocket,         // WebSocket operations
  kCrypto,            // Crypto operations
  kAiInference,       // AI inference
  kOther              // Unclassified
};
```

## Current Status

### Completed
- [x] Core AsyncTraceContext class with resource tracking
- [x] Stack trace capture and deduplication
- [x] Timing tracking (creation, callback start/end, destroy)
- [x] Annotation support
- [x] JSON output format
- [x] Perfetto trace event emission (when WORKERD_USE_PERFETTO defined)
- [x] V8 Promise hook integration
- [x] IoContext integration
- [x] Promise hook installation in Worker isolate
- [x] Basic API instrumentation (fetch, cache, timers)
- [x] BUILD.bazel updated

### Verified Working
- [x] Build verification - compiles successfully
- [x] Integration test with actual request flow
- [x] JSON output validation - correct structure and timing
- [x] Perfetto trace file generation
- [x] HTML visualization tool created

### Not Yet Done
- [ ] Unit tests for AsyncTraceContext
- [ ] Perfetto UI visualization validation

### TODO - More Instrumentation Needed
- [ ] KV API operations (`src/workerd/api/kv.c++`)
- [ ] Durable Object storage operations
- [ ] R2 operations
- [ ] D1 queries
- [ ] Queue operations
- [ ] WebSocket operations
- [ ] Stream read/write operations
- [ ] Crypto async operations
- [ ] AI inference operations

### TODO - KJ Promise Tracking
The current implementation only tracks JS promises via V8 hooks. For complete causality tracking, we also need to track KJ promises, especially:
- [ ] KJ-to-JS promise bridging (when C++ async results are awaited in JS)
- [ ] JS-to-KJ promise bridging (when JS promises are awaited in C++)

This would involve instrumenting `awaitIo()`, `awaitJs()`, and related methods in IoContext.

### TODO - Output/Visualization
- [ ] Define output format for bubbleprof-compatible visualization
- [ ] Consider exposing trace via response header or separate endpoint
- [ ] Consider sampling strategy for production use
- [ ] Integration with existing tracing infrastructure

## Key Files

| File | Purpose |
|------|---------|
| `src/workerd/io/async-trace.h` | Core AsyncTraceContext class definition |
| `src/workerd/io/async-trace.c++` | AsyncTraceContext implementation |
| `src/workerd/io/async-trace-hooks.h` | V8 Promise hook declaration |
| `src/workerd/io/async-trace-hooks.c++` | V8 Promise hook implementation |
| `src/workerd/io/io-context.h` | IoContext integration (asyncTrace member) |
| `src/workerd/io/io-context.c++` | enableAsyncTrace() implementation |
| `src/workerd/io/worker.c++` | Promise hook installation |
| `src/workerd/io/BUILD.bazel` | Build configuration |
| `src/workerd/api/http.c++` | fetch() instrumentation |
| `src/workerd/api/cache.c++` | Cache API instrumentation |
| `src/workerd/api/global-scope.c++` | setTimeout/setInterval instrumentation |

## JSON Output Format

The AsyncTraceContext outputs JSON with this structure:
```json
{
  "requestDurationNs": 123456789,
  "resources": [
    {
      "asyncId": 1,
      "triggerId": 0,
      "type": "root",
      "stackTraceId": 0,
      "createdAt": 0,
      "callbackStartedAt": 0,
      "callbackEndedAt": 123456789,
      "destroyedAt": 0
    },
    ...
  ],
  "stackTraces": [
    {
      "id": 0,
      "frames": ["functionName @ script.js:10:5", ...]
    },
    ...
  ],
  "annotations": [
    {"asyncId": 2, "key": "url", "value": "https://example.com"},
    ...
  ]
}
```

## Notes for Resuming Work

1. **First step**: Run `bazel build //src/workerd/io:io` to verify the code compiles
2. **If compilation fails**: Check for missing includes or type errors
3. **Testing**: Create a simple .wd-test that exercises fetch/cache/timers and verify JSON output in logs
4. **For more instrumentation**: Follow the pattern in http.c++ - get asyncTrace from IoContext, create resource, add annotations
5. **For KJ promise tracking**: Look at `awaitIo()` implementation in io-context.h - this is where KJ promises get bridged to JS

## Visualization Tool

A bubbleprof-style HTML visualization tool is available at:
`tools/async-trace-viewer/index.html`

### Features
- **Bubble View**: Groups resources by stack trace, sizes by sync time, shows causality links
- **Flame Graph**: Timeline view showing resource lifetimes and callback execution
- **Tree View**: Hierarchical view of resource causality chains

### Usage
1. Start a local HTTP server: `python3 -m http.server 8888` from the `tools/async-trace-viewer` directory
2. Open `http://localhost:8888` in a browser
3. Either:
   - Click "Load Demo" to see sample trace data
   - Click "Load JSON File" to load a trace JSON file
   - Click "Paste JSON" and paste trace JSON directly

### Extracting Trace JSON
When running workerd with `--verbose --perfetto-trace=...`, the trace JSON is logged to stderr with prefix:
```
AsyncTrace completed; toJson() = {...}
```
Copy the JSON portion and load it into the visualization tool.

### Available Sample Traces
- `sample-trace.json` - Simple timer test (11 resources)
- `sample-async-patterns.json` - Complex async patterns (61 resources) showing sequential, parallel, race, nested, and iteration patterns
- `sample-chat-room.json` - Durable Objects chat room (18 resources) with internal fetch

## Reference: clinicjs/bubbleprof

Bubbleprof visualization shows:
- "Bubbles" representing groups of async operations (grouped by stack trace)
- Bubble size = time spent
- Lines between bubbles = causality
- Timeline showing when things happened
- Helps identify:
  - Async bottlenecks
  - Unexpected sequential operations
  - Long async delays

The goal is to generate trace data compatible with (or convertible to) this visualization.
