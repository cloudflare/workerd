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
- [x] Analysis dropdown with 6 toggleable features
- [x] Bottleneck/pattern highlighting in Waterfall, Bubble, and DAG views
- [x] Sequential fetch detection using creation time vs callback time comparison
- [x] Analysis sidebar updates correctly when switching demos
- [x] Real trace capture from workerd samples (6 traces captured)
- [x] KJ↔JS bridge tracking (`kj-to-js` and `js-to-kj` resource types)
- [x] Temporal edges feature for timing-based causality
- [x] AI analysis prompt with comprehensive trace context

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
- [x] Stream read/write operations (see below)
- [ ] Crypto async operations
- [ ] AI inference operations

### Stream Operations (Completed)
Stream operations are now instrumented in `src/workerd/api/streams/`:
- [x] `ReaderImpl::read()` → `kStreamRead` / `stream-read`
- [x] `WritableStreamDefaultWriter::write()` → `kStreamWrite` / `stream-write`
- [x] `ReadableStream::pipeTo()` → `kStreamPipeTo` / `stream-pipe-to`
- [x] `ReadableStream::pipeThrough()` → `kStreamPipeThrough` / `stream-pipe-through`

**Files modified:**
- `src/workerd/api/streams/readable.c++` - read, pipeTo, pipeThrough instrumentation
- `src/workerd/api/streams/writable.c++` - write instrumentation
- `src/workerd/io/async-trace.h` - added kStreamPipeTo, kStreamPipeThrough resource types

### KJ Promise Tracking (Completed)
KJ↔JS bridge tracking is now implemented in `io-context.h`:
- [x] KJ-to-JS promise bridging (`awaitIo()` → `kKjToJsBridge` / `kj-to-js`)
- [x] JS-to-KJ promise bridging (`awaitJs()` → `kJsToKjBridge` / `js-to-kj`)

**Implementation details:**
- `awaitIoImpl()` creates a `kKjToJsBridge` resource when called, enters callback when KJ promise completes
- `awaitJs()` creates a `kJsToKjBridge` resource when called, enters callback when JS promise resolves
- Both use `KJ_DEFER` for exception-safe callback exit
- Bridge resources capture the full async wait time between creation and callback

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

### Visualization Views (8 total)

| Key | View | Description |
|-----|------|-------------|
| 1 | **Waterfall** | Timeline view showing resource lifetimes, async wait vs sync execution |
| 2 | **Graph** | Combined view with 3 layouts: Bubble (default), Hierarchical, Force; path highlighting on hover |
| 3 | **Replay** | Animated playback of request execution; ←/→ to step, Shift+←/→ to jump to events |
| 4 | **Parallelism** | Shows concurrent resource count over time |
| 5 | **Breakdown** | Treemap showing time allocation by resource type (sync vs async) |
| 6 | **Latency** | Histogram of async wait times by resource type |
| 7 | **Gaps** | Highlights idle periods and sync activity bursts; hover over gaps to see waiting operations |
| 8 | **Heatmap** | Activity intensity over time by resource type |

### Analysis Features

All analysis features are accessible via the **🔬 Analysis** dropdown menu in the header. The button shows a count of active features (e.g., "Analysis (3)").

| Key | Feature | Description |
|-----|---------|-------------|
| C | **Critical Path** | Highlights the minimum-latency dependency chain (red glow in all views) |
| B | **Bottlenecks** | Identifies top 5 resources consuming the most time (yellow glow) |
| T | **Patterns** | Detects anti-patterns (purple glow) - see Pattern Detection below |
| F | **Click Filter** | Click any resource to filter view to its ancestors/descendants (see below) |
| G | **Stack Group** | Groups resources by creation stack trace |
| E | **Temporal Edges** | Shows timing-based causality (green dashed lines in Bubble/DAG) - see below |
| A | **High Contrast** | Accessibility mode with patterns instead of color-only |

Highlighting for Critical Path, Bottlenecks, and Patterns appears in Waterfall, Bubble, and DAG views.

### Temporal Edges

The Temporal Edges feature (`E` hotkey) shows relationships based on callback timing rather than creation-time `triggerId`. This is especially useful for visualizing what happens after KJ↔JS bridge callbacks complete.

**How it works:**
- For each resource with a callback, finds other resources whose callbacks START during this resource's callback window
- Draws green dashed edges from the "parent" callback to the "child" callback in Bubble and DAG views
- Complements the solid triggerId edges which show creation-time causality

**Use cases:**
- Understanding execution flow through bridge resources (kj-to-js, js-to-kj)
- Visualizing concurrent callback execution
- Debugging callback ordering issues

### Pattern Detection

The pattern detector identifies common anti-patterns based on concepts from "Broken Promises" and similar async performance knowledge. Detected patterns are highlighted with severity-based purple glow in Waterfall, Bubble, and DAG views, and listed in the Analysis sidebar.

**Pattern Severity Levels:**
- 🔴 **High**: Significant performance impact, should be fixed first
- 🟡 **Medium**: Moderate impact, worth investigating
- 🟢 **Low**: Minor issues or informational

**Currently Implemented Patterns (14 total):**

| Pattern | Type ID | Severity | Description |
|---------|---------|----------|-------------|
| **Sequential Fetches** | `sequential-await` | High | Fetches created after previous fetch's callback started, indicating `await` chains that could use `Promise.all()` |
| **Event Loop Blocking** | `sync-flood` | High | Callback execution >50ms, blocking other work from running |
| **Unbatched Operations** | `unbatched-ops` | High | Multiple KV/DO operations of same type within 5ms that could be batched |
| **Duplicate Fetches** | `duplicate-fetch` | Medium | Multiple fetches to the same URL that could be deduplicated or cached |
| **Deep Promise Chains** | `deep-chain` | Medium | Promise nesting exceeds 10 levels, may indicate callback hell |
| **Waterfall Fetches** | `waterfall-fetch` | Medium | Fetch triggered by another fetch's result (dependency chain includes another fetch) |
| **Long Async Gaps** | `long-async-gap` | Medium | Resources waiting >threshold before callback executes |
| **Promise Accumulation** | `promise-flood` | Medium | >20 promises created within 1ms, potential memory pressure |
| **Callback Storm** | `callback-storm` | Medium | >10 callbacks firing within 1ms |
| **Fetch Concurrency Risk** | `fetch-flood` | Medium | >6 parallel fetches, may hit connection limits |
| **Unresolved Promises** | `unresolved-promise` | Low | Promises created but never resolved by end of request |
| **Redundant Timers** | `redundant-timers` | Low | Multiple timers with similar delays created at the same time |
| **Cache Misses** | `cache-miss` | Low | Cache-get operations with >50ms wait, suggesting origin fetch |
| **Hot Callback** | `hot-callback` | Low | Single callback spawning >5 child resources |

### Configurable Thresholds

Pattern detection thresholds are configurable via sliders in the Analysis dropdown menu:

| Threshold | Default | Description |
|-----------|---------|-------------|
| Sync block | 50ms | Event loop blocking detection threshold |
| Long gap | 500ms | Long async gap detection threshold |
| Promise flood | 20 | Promises created per 1ms window |
| Callback storm | 10 | Callbacks per 1ms window |
| Unbatched ops | 3 | Operations to trigger unbatched detection |
| Fetch concurrency | 6 | Parallel fetches to trigger warning |

Thresholds are persisted across page refreshes. Click "Restore Defaults" to reset all thresholds to their default values.

### Extending Pattern Detection

To add new anti-pattern detection, edit the `detectPatterns()` function in `tools/async-trace-viewer/index.html`:

```javascript
function detectPatterns() {
  // ... existing patterns ...

  // Example: Detect slow individual operations (>100ms)
  traceData.resources.forEach(r => {
    if (r.callbackStartedAt > 0 && r.callbackEndedAt > 0) {
      const duration = (r.callbackEndedAt - r.callbackStartedAt) / 1e6; // ms
      if (duration > 100) {
        detectedPatterns.push({
          type: 'slow-operation',           // Unique type identifier
          message: `Slow ${r.type}: ${duration.toFixed(0)}ms`, // Shown in sidebar
          resources: [r.asyncId],           // Resources to highlight
          severity: 'high'                  // 'high', 'medium', or 'low'
        });
      }
    }
  });
}
```

**Pattern object structure:**
| Field | Description |
|-------|-------------|
| `type` | Unique string identifier (e.g., `'sequential-await'`, `'duplicate-fetch'`) |
| `message` | Human-readable description shown in the Analysis sidebar |
| `resources` | Array of `asyncId` values to highlight |
| `severity` | Severity level: `'high'`, `'medium'`, or `'low'` (affects highlight intensity)

### Click-to-Filter

When enabled (`F` hotkey), clicking any resource filters the view to show only:
- The clicked resource itself
- Its **ancestors** (the chain of resources that triggered it)
- Its **descendants** (all resources it triggered, recursively)

This is useful for isolating a specific async operation and understanding its full dependency chain without noise from unrelated concurrent operations.

**Per-view behavior:**
| View | Effect |
|------|--------|
| Waterfall | Hides rows for non-matching resources |
| Bubble | Recomputes layout with only matching resources |
| DAG | Recomputes layout with only matching resources |
| Heatmap | Shows only matching resource types |

To clear the filter, either click a different resource or disable click-to-filter mode.

### Stack Trace Grouping (Partial Implementation)

The `G` hotkey toggles stack trace grouping. The `getStackGroups()` function groups resources by their creation stack trace, but **visual rendering is not yet implemented**.

Intended behavior:
- **Waterfall**: Group rows by stack trace with collapsible section headers
- **Bubble**: Merge resources from same call site into larger bubbles (like clinicjs/bubbleprof)
- **DAG**: Collapse nodes with same stack trace into group nodes

### Keyboard Shortcuts

**Navigation:**
- `1`-`9`: Switch views (Waterfall, Bubble, DAG, Parallelism, Breakdown, Latency, Gaps, Replay, Heatmap)
- `?`/`H`: Show help guide
- `O`/`L`: Load file
- `P`/`V`: Toggle paste area
- `Escape`: Close modals/dropdowns

**Analysis (via dropdown):**
- `C`: Critical path highlight
- `B`: Bottleneck detection
- `T`: Pattern detection
- `F`: Click-to-filter mode
- `G`: Stack trace grouping
- `A`: High contrast / accessibility mode

**View-Specific:**
- `←`/`→`: Switch between Bubble, Hierarchical, and Force layouts (Graph view only)
- `←`/`→`: Step through timeline in 1% increments (Replay view only)
- `Shift+←`/`Shift+→`: Jump to next/previous significant event (Replay view only)
- `Space`: Play/pause (Replay view)
- `R`: Reset animation (Replay view)

**Other:**
- `I`: Open AI analysis prompt
- `Shift+R`: Refresh/reset current view (resets pan/zoom, clears cached layout)

### AI Analysis Prompt

The `I` hotkey opens an AI analysis modal that generates a detailed prompt for Claude or other AI assistants. The prompt includes:

**Trace Summary:**
- Total duration, resource count, sync/async time ratio
- Resource type breakdown with classification (typed API calls, user code, internal)

**Analysis Context:**
- Critical path (longest dependency chain with timing)
- High latency edges (>1ms gaps between operations)
- URLs/endpoints referenced
- Top stack locations by frequency

**Detected Anti-Patterns:**
- Sequential awaits (Promise.all opportunities)
- Duplicate fetches
- Waterfall fetches (chained dependencies)
- Deep promise chains

**Runtime-Specific Data:**
- Stream operations (stream-read, stream-write, stream-pipe-to, stream-pipe-through)
- KJ↔JS bridge transitions (cross-boundary calls between JS and C++ I/O layer)
- Temporal execution patterns (callbacks spawning other callbacks)
- Top bottlenecks with sync execution and async wait breakdown

**Analysis Request:**
The prompt asks the AI to:
1. Identify the primary bottleneck
2. Check for serialization issues
3. Evaluate async/sync ratio
4. Look for redundant operations
5. Assess the critical path
6. Analyze stream operations
7. Review KJ↔JS transitions
8. Provide specific code recommendations
9. Estimate potential improvement

### Sidebar Panels

The left sidebar displays key metrics and analysis results:

| Panel | Description |
|-------|-------------|
| **Stats** | Resource count, total sync/async time, request duration |
| **Time Distribution** | Bar chart showing sync vs async time allocation |
| **Runtime Overhead** | Visual breakdown of user code vs runtime overhead time |
| **Critical Path** | Details about the longest dependency chain (when enabled) |
| **Detected Patterns** | List of anti-patterns found (when enabled) |
| **Analysis** | Detailed trace analysis with recommendations |

**Sidebar Behavior:**
- Sections are collapsible - click headers to toggle
- Collapsed states are persisted across page refreshes
- Sidebar scrolls independently from the main visualization area
- Charts re-render automatically when sections are expanded

**Runtime Overhead Calculation:**

The Runtime Overhead panel estimates what percentage of async activity is dominated by runtime machinery vs user code:

- **User time** (green): Operations with identifiable user intent
  - Typed API calls (fetch, cache, KV, etc.)
  - User promises (resources with stack traces pointing to user code)
- **Runtime time** (orange): Internal runtime overhead
  - Internal promises and bridges with no user stack
  - KJ↔JS bridging overhead
  - Runtime scheduling and bookkeeping

The breakdown shows:
- Visual bar chart with percentages
- Operation counts for each category
- Detailed breakdown: API calls | User promises | Internal ops

**Proposed Future Metrics:**

The following metrics could provide additional insights:

| Category | Metric | Description |
|----------|--------|-------------|
| **Concurrency** | Peak Parallelism | Maximum concurrent operations at any point |
| | Serialization Ratio | Sequential chain time vs potential parallel time (lower = better) |
| | Parallelism Efficiency | Actual vs theoretical maximum parallelism |
| **I/O vs Compute** | I/O Wait Breakdown | Time by category: network (fetch) vs storage (KV/DO/R2) vs timers |
| | Compute vs I/O Ratio | Sync execution time vs async waiting time |
| | Blocking Time | Total event loop blocking from long sync operations |
| **Network** | Fetch Statistics | Count, parallel vs sequential ratio, duplicates detected |
| | Connection Utilization | Estimate of connection pool usage (6 concurrent = saturated) |
| | Body Consumption | Fetches where response body was/wasn't read (dangling fetch) |
| **Promise Health** | Promise Lifetime | Average/max time from creation to resolution |
| | Unresolved Count | Promises never resolved (potential leaks) |
| | Peak Concurrent | Maximum promises alive simultaneously (memory pressure) |
| **Critical Path** | Critical Path Ratio | Critical path time / total time (1.0 = fully serialized) |
| | Optimization Potential | Estimated savings if sequential patterns parallelized |
| **Workers-Specific** | Bridge Overhead | Total time in KJ↔JS transitions |
| | Batchable Operations | Count of operations that could use batch APIs |

### Usage
1. Start a local HTTP server: `python3 -m http.server 8888` from the `tools/async-trace-viewer` directory
2. Open `http://localhost:8888` in a browser
3. Either:
   - Select a demo from the dropdown
   - Click "Load" to load a trace JSON file
   - Click "Paste" and paste trace JSON directly

### Extracting Trace JSON
When running workerd with `--verbose --perfetto-trace=...`, the trace JSON is logged to stderr with prefix:
```
AsyncTrace completed; toJson() = {...}
```
Copy the JSON portion and load it into the visualization tool.

### Capturing Real Traces

To capture traces from workerd samples:

```bash
# Build workerd
bazel build //src/workerd/server:workerd

# Start a sample with tracing enabled
bazel-bin/src/workerd/server/workerd serve \
    --verbose \
    --perfetto-trace=/tmp/trace.perfetto=workerd \
    samples/helloworld_esm/config.capnp 2>&1 &

# Send a request
curl http://localhost:8080/

# Stop workerd - trace JSON is output on shutdown
pkill workerd
```

The trace JSON appears in stderr output at WARNING level. Some samples require `--experimental` flag for experimental features.

**Note:** Trace output was changed from `KJ_LOG(INFO, ...)` to `KJ_LOG(WARNING, ...)` because INFO level is only enabled for `workerd test`, not `workerd serve`.

### Available Sample Traces

**Basic Examples:**
- `sample-trace.json` - Simple timer test (11 resources)
- `sample-async-patterns.json` - Complex async patterns (61 resources)
- `sample-chat-room.json` - Durable Objects chat room (18 resources)

**Best Practices:**
- `sample-good-parallel.json` - Parallel fetches with Promise.all()

**Anti-Pattern Examples (for pattern detection testing):**
- `sample-bad-sequential.json` - Sequential fetches (triggers: `sequential-await`)
- `sample-bad-duplicates.json` - Duplicate fetches (triggers: `duplicate-fetch`)
- `sample-waterfall-fetches.json` - Fetch chains (triggers: `waterfall-fetch`)
- `sample-unresolved-promises.json` - Abandoned promises (triggers: `unresolved-promise`)
- `sample-long-async-gaps.json` - Slow operations (triggers: `long-async-gap`)
- `sample-redundant-timers.json` - Duplicate timers (triggers: `redundant-timers`)
- `sample-cache-misses.json` - Cache misses (triggers: `cache-miss`)

**Streams Examples:**
- `sample-streams-pipeline.json` - Streams processing pipeline
- `sample-pathological-streams.json` - Stress test with many stream operations

**Real Traces (captured from actual workerd samples):**
- `sample-real-helloworld.json` - ESM hello world (5 resources, 6.8ms)
- `sample-real-helloworld-sw.json` - Service Worker hello world (5 resources, 4.0ms)
- `sample-real-async-context.json` - Async context with timers (17 resources, 43.3ms)
- `sample-real-durable-objects.json` - Durable Objects chat (9 resources, 8.6ms)
- `sample-real-nodejs-compat-fs.json` - Node.js fs compat (5 resources, 14.2ms)
- `sample-real-nodejs-compat-streams.json` - Node.js streams pipeline (15 resources, 72.5ms)
- `sample-real-tcp.json` - TCP socket to gopher server (120 resources, 819ms) - includes stream-read/write operations

## Future Visualization Improvements

The following improvements are planned for the visualization tool, particularly the Waterfall view:

### Medium Impact

| Feature | Description |
|---------|-------------|
| **Minimap/Overview** | A small collapsed view of the entire trace for quick navigation when zoomed in |
| **Zoom & Pan** | Ability to zoom into specific time ranges and pan horizontally for detailed inspection |
| **Search/Find** | Search for resources by asyncId, type, or stack frame content |

### Lower Impact / Nice-to-Have

| Feature | Description |
|---------|-------------|
| **Export Options** | Export the waterfall as SVG/PNG for documentation |
| **Keyboard Navigation** | Arrow keys to move between resources, expand/collapse details |
| **Time Scale Toggle** | Switch between absolute time and relative-to-parent time |
| **Critical Path Highlighting** | Highlight the longest chain of dependencies in waterfall view |
| **Resource Grouping** | Collapse groups of related resources (e.g., all promises from same stack trace) |

### Recently Implemented (Waterfall View)

The following high-impact features were added to the Waterfall view:

| Feature | Description |
|---------|-------------|
| **Concurrency Graph** | Canvas-based graph above waterfall showing in-flight async operations over time |
| **Dependency Arrows** | SVG overlay draws curved arrows from parent to child resources (tree sort mode) |
| **Hover Highlighting** | Hovering a resource highlights its parent (blue) and children (green) |
| **Time Cursor** | Interactive cursor in concurrency graph shows timestamp and active resource count |
| **Callback Markers** | Visual markers showing when callbacks started and ended within resource bars |
| **Temporal Edge Indicators** | Badges showing timing-based causality relationships |
| **Stack Trace Integration** | Expandable stack traces inline - click ▶ button to show creation stack trace |
| **Filter by Type** | Dropdown menu to show/hide specific resource types with "Enable All" shortcut |

### Recently Implemented (Graph View - Combined Bubble+DAG)

The Bubble and DAG views have been consolidated into a single "Graph" view (key 2) with three layout options:

| Feature | Description |
|---------|-------------|
| **Layout Toggle** | Switch between Bubble, Hierarchical, and Force layouts via toggle buttons |
| **Bubble Layout** (default) | Variable-sized nodes based on sync time, latency labels on edges, auto-centered on root |
| **Hierarchical Layout** | Tree-style layout with root at top, children below, computed subtree widths |
| **Force Layout** | Physics-based simulation that optimizes node positions |
| **Path Highlighting** | Hover over any node to highlight full ancestor/descendant chain with dimmed non-path elements |
| **Arrow Key Navigation** | ←/→ hotkeys to cycle through all three layouts (Graph view only) |
| **Enhanced Tooltips** | Detailed tooltips with stack trace preview, timing info, and classification |

**View Renumbering:** Views are now 1-8 (Waterfall, Graph, Replay, Parallelism, Breakdown, Latency, Gaps, Heatmap)

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
