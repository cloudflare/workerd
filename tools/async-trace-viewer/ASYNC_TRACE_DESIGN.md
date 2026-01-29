# Async Trace Viewer: Design and Implementation

## Table of Contents

1. [Motivation and Problem Statement](#motivation-and-problem-statement)
2. [Overall Approach](#overall-approach)
3. [The Visualizer Tool](#the-visualizer-tool)
4. [Instrumentation Model and JSON Format](#instrumentation-model-and-json-format)
5. [Next Steps and Future Directions](#next-steps-and-future-directions)

---

## Motivation and Problem Statement

### The Challenge of Understanding Async Execution

Modern JavaScript applications, particularly those running on the Cloudflare Workers platform, are fundamentally asynchronous. A single HTTP request can spawn dozens or hundreds of async operations—promises, fetch requests, stream operations, timers, and storage calls—that execute concurrently and trigger cascading chains of further operations.

When performance problems occur in these applications, developers face several challenges:

**Invisible Causality**: Standard debugging tools show individual operations in isolation, but don't reveal the causal relationships between them. A fetch request that takes 200ms might look slow, but without knowing that it was waiting on three other operations to complete first, developers can't identify the true bottleneck.

**Hidden Parallelism (or Lack Thereof)**: Code that *appears* parallel may actually execute sequentially due to accidental `await` placement or promise chain construction. Similarly, operations that should be sequential might inadvertently race. These patterns are invisible in standard logs.

**Timing Opacity**: Console timestamps show when log statements execute, but don't capture the crucial distinction between *async delay* (time spent waiting for an operation to complete) and *sync time* (time spent executing JavaScript). A slow request might be CPU-bound or I/O-bound—the optimization approach differs dramatically.

**Promise Chain Complexity**: JavaScript promises create implicit dependency graphs. A `.then()` chain creates sequential execution; `Promise.all()` creates parallel execution. In complex applications with hundreds of promises, understanding the actual execution flow requires visualizing the entire graph structure.

### Workers-Specific Challenges

Beyond general JavaScript async complexity, Cloudflare Workers introduce unique challenges that make async tracing particularly valuable:

**Dual Promise Systems**: Workers run on the workerd runtime, which uses KJ (a C++ toolkit library) for its internal async operations. When JavaScript code calls a Workers API like `fetch()` or `cache.match()`, the operation creates a KJ promise in C++ that must be bridged back to a JavaScript promise. This KJ↔JS boundary creates "bridge" resources that appear in traces—understanding these bridges is essential for diagnosing where time is actually spent (in JavaScript, in C++ runtime code, or waiting for external I/O).

**Distributed Storage Primitives**: Workers applications routinely interact with globally distributed storage systems—KV for key-value storage, R2 for object storage, D1 for SQL databases, Durable Objects for coordinated state, and Queues for async messaging. Each of these has different latency characteristics:
- KV reads from edge cache are fast; cache misses require origin fetches
- Durable Object operations may require routing to a specific datacenter
- R2 operations involve object storage with variable latency based on object size
- D1 queries depend on database location and query complexity

Without visibility into which storage operations are happening and how they're sequenced, developers can't optimize their data access patterns.

**Durable Objects Complexity**: Durable Objects introduce particularly complex async patterns. A single DO can handle concurrent requests, each spawning its own async operations. DO storage operations (`state.storage.get/put`) create async work that interleaves with WebSocket message handling and alarm callbacks. The DO's transactional storage model means operations may be batched or serialized in non-obvious ways. Tracing these patterns is essential for building performant stateful applications.

**The I/O Gate Model**: Workers enforce a strict I/O model where certain operations (like responding to the client) can only happen at specific points in the request lifecycle. The runtime uses "I/O gates" to control when async work can proceed. Misunderstanding this model leads to subtle bugs where operations appear to hang or complete in unexpected order. Visualizing the actual execution flow makes these patterns visible.

**Edge Computing Latency Concerns**: Workers run at the edge, close to users, but their backend dependencies (origins, storage systems, third-party APIs) may be far away. A request to a Worker in Tokyo that fetches from an origin in Virginia adds significant network latency. Async tracing helps developers understand whether their request latency is dominated by:
- JavaScript execution time (optimize the code)
- Sequential async operations (parallelize them)
- Network round-trips to distant services (cache, colocate, or reduce calls)

**No Traditional Profiling**: The Workers sandbox environment doesn't provide access to traditional profiling tools. There's no `perf`, no sampling profiler, no ability to attach a debugger to production traffic. Console logging provides timestamps but not causality. The async trace viewer fills this gap by providing deep visibility into async behavior without requiring privileged system access.

### Prior Art: Node.js async_hooks and Bubbleprof

Node.js introduced the `async_hooks` API to address similar challenges in server-side JavaScript. This API provides hooks into the async resource lifecycle, tracking when resources are created, when their callbacks execute, and when they're destroyed.

The [Clinic.js Bubbleprof](https://clinicjs.org/bubbleprof/) tool built on this foundation, creating visualizations that group async operations by their call stack and show how time flows through the application. Bubbleprof demonstrated that proper visualization can make complex async behavior comprehensible.

However, Bubbleprof targets Node.js and doesn't work with Cloudflare Workers. Workers have a different async model (KJ promises in C++ interoperating with JavaScript promises), different APIs (fetch, KV, Durable Objects, R2, D1, etc.), and run in a more constrained environment where traditional profiling approaches don't apply.

### Goals

The async trace viewer aims to:

1. **Make async behavior visible**: Transform invisible causality chains into clear visual representations
2. **Enable performance diagnosis**: Help developers identify sequential operations that should be parallel, duplicate operations that should be cached, and bottleneck operations that dominate request latency
3. **Support the Workers ecosystem**: Track Workers-specific operations (KV, DO, R2, Queues) alongside standard web APIs
4. **Optimize for insight density**: Given the inherent overhead of Promise hooks and stack capture, maximize the diagnostic value extracted from each traced request
5. **Work with existing tooling**: Integrate with Perfetto for timeline analysis while providing specialized visualization for async-specific insights

---

## Overall Approach

### Architecture Overview

The system consists of two main components:

```
┌───────────────────────────────────────────────────────────────────┐
│                        Cloudflare Worker                          │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                      workerd Runtime                        │  │
│  │                                                             │  │
│  │  ┌────────────────┐    ┌─────────────────────────────────┐  │  │
│  │  │ AsyncTrace     │    │ API Instrumentation             │  │  │
│  │  │ Context        │◄───┤ - fetch(), cache.match(), etc.  │  │  │
│  │  │                │    │ - Timers, streams, sockets      │  │  │
│  │  │ • Resources    │    │ - KJ↔JS promise bridges         │  │  │
│  │  │ • Stack traces │    └─────────────────────────────────┘  │  │
│  │  │ • Timing       │                                         │  │
│  │  │ • Annotations  │    ┌─────────────────────────────────┐  │  │
│  │  │                │◄───┤ V8 Promise Hook                 │  │  │
│  │  └───────┬────────┘    │ - Promise lifecycle tracking    │  │  │
│  │          │             └─────────────────────────────────┘  │  │
│  │          ▼                                                  │  │
│  │  ┌────────────────┐                                         │  │
│  │  │ JSON Output    │─────────────────────────────────────────┼──┼──►
│  │  └────────────────┘                                         │  │
│  └─────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌───────────────────────────────────────────────────────────────────┐
│                       Async Trace Viewer                          │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │  Single-file HTML/CSS/JavaScript application                │  │
│  │                                                             │  │
│  │  Views:                    Analysis:                        │  │
│  │  • Waterfall timeline      • Critical path detection        │  │
│  │  • Dependency graph        • Bottleneck identification      │  │
│  │  • Animated replay         • Anti-pattern detection         │  │
│  │  • Parallelism analysis    • Sibling/cousin grouping        │  │
│  │  • Latency distribution    • Stack trace grouping           │  │
│  │  • Gap analysis                                             │  │
│  └─────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────┘
```

### Design Principles

**Development-Time Tracing**: The tracing infrastructure is designed for development and debugging, not production use. V8's Promise hooks, which are essential for tracking JavaScript promise lifecycles, introduce significant runtime overhead. Stack trace capture adds further cost. The infrastructure optimizes where it can—using monotonic nanosecond timestamps, deduplicating stack traces, and storing data in compact structures—but tracing should be enabled selectively during development or investigation, not on production traffic.

**Causality-First Model**: Every async resource tracks its `triggerId`—the ID of the resource whose callback was executing when this resource was created. This builds a directed acyclic graph (DAG) of causality that forms the foundation for all visualizations.

**Separation of Capture and Analysis**: The runtime produces simple, structured JSON. All sophisticated analysis (critical path detection, pattern recognition, grouping) happens in the viewer. This keeps the runtime simple and allows the viewer to evolve independently.

**Single-File Deployment**: The viewer is a self-contained HTML file with embedded CSS and JavaScript. This simplifies deployment, enables offline use, and eliminates build tooling requirements.

### Perfetto Integration

Async tracing integrates with workerd's existing Perfetto tracing infrastructure in two ways:

**Activation**: Async trace collection is enabled when Perfetto tracing is active. The `--perfetto-trace` command-line flag serves as the trigger—when present, an `AsyncTraceContext` is created for each request. This piggybacks on Perfetto's existing mechanism rather than introducing a separate activation flag.

**Dual Output**: As async events occur, the `AsyncTraceContext` both:
1. Accumulates data in memory for JSON serialization at request end
2. Emits Perfetto trace events (`emitResourceCreated`, `emitCallbackStart`, `emitCallbackEnd`, `emitResourceDestroyed`) for real-time timeline capture

The two outputs serve complementary purposes:

| Output | Purpose | Strengths |
|--------|---------|-----------|
| **Perfetto trace** | Timeline visualization in Perfetto UI | Correlates with other system events (I/O, CPU, memory); standard tracing ecosystem; supports very long traces |
| **JSON output** | Async-specific visualization in async-trace-viewer | Causality-aware analysis (critical path, patterns); specialized views (graph, replay); async-specific grouping and filtering |

The Perfetto trace shows *when* things happened on a timeline alongside other system activity. The JSON trace captures the *causality structure*—which operation triggered which—enabling the dependency-aware analysis that makes the async-trace-viewer valuable.

### Trace Capture Flow

1. **Request Start**: An `AsyncTraceContext` is created and associated with the `IoContext` for the request. A root resource (ID=1) is created with trigger ID=0.

2. **API Calls**: When instrumented APIs (fetch, cache, timers, etc.) are called, they create async resources via `AsyncTraceContext::createResource()`. This records:
   - A unique `asyncId`
   - The `triggerId` (copied from `context.current()`)
   - The resource type
   - A captured and deduplicated stack trace
   - The creation timestamp

3. **Promise Tracking**: V8's PromiseHook API notifies us of promise lifecycle events. We use private symbols to associate async IDs with promises, enabling tracking across the JavaScript/C++ boundary.

4. **Callback Execution**: When callbacks execute, `enterCallback()`/`exitCallback()` or `CallbackScope` record the start and end times. This tracks which resource is "current" for proper trigger attribution.

5. **Request End**: `finalize()` is called, producing the complete `AsyncTrace` structure, which is serialized to JSON.

---

## The Visualizer Tool

### Overview

The async trace viewer is a single-file HTML application located at `tools/async-trace-viewer/index.html`. It provides six complementary views for analyzing async execution, along with multiple analysis features accessible via keyboard shortcuts or the Analysis dropdown menu.

### Views

#### 1. Waterfall View (Key: `1`)

The waterfall view provides a timeline-based visualization similar to browser DevTools network panels, but for async operations.

**Features:**
- Horizontal bars showing operation duration with distinct colors for async delay vs sync execution
- Concurrency graph above the timeline showing active operations over time
- Expandable dependency arrows in tree mode
- Hover highlighting of entire causality chains
- Expandable stack traces via ▶ buttons
- Sort options: tree hierarchy, start time, duration, or type
- Type filtering to focus on specific operation types

**When to Use:** Initial exploration, understanding overall request flow, identifying sequential vs parallel execution.

#### 2. Graph View (Key: `2`)

The graph view renders the causality DAG directly, showing how operations trigger other operations.

**Features:**
- Three layout algorithms: Bubble (hierarchical), Hierarchical (strict layers), Force-directed
- Switch layouts with ←/→ arrow keys
- Path highlighting on hover showing ancestors and descendants
- Drag nodes to reposition
- Edge labels showing latency between operations (positive values only)
- Sibling grouping collapses repetitive patterns into compound nodes

**When to Use:** Understanding causality relationships, identifying deep chains, seeing the overall dependency structure.

#### 3. Replay View (Key: `3`)

The replay view animates the trace execution in real-time (or at adjustable speed), showing operations as they're created, execute callbacks, and complete.

**Features:**
- Three layout modes: Grid, Bubble (hierarchical tree), and Rings (time-based concentric circles)
- Play/pause with Space, speed control with +/−
- Step forward/backward with arrow keys
- Ghost mode shows pulses when state changes occur
- Trail mode leaves fading afterimages
- Loop mode for continuous replay
- Node size grows during callback execution
- Lifecycle badges (⏳ waiting, ▶ running, ✓ done)

**When to Use:** Building intuition about execution flow, presentations, identifying timing-based issues.

#### 4. Parallelism View (Key: `4`)

The parallelism view quantifies concurrency over time, showing how many operations were active simultaneously.

**Features:**
- Stacked bar chart with solid bars for sync execution, faded for async waiting
- Cyan "ideal parallelism" line showing theoretical maximum
- Orange critical path line showing the minimum serial work
- Efficiency metrics in sidebar
- Hover to see specific operations active at each time bucket

**When to Use:** Quantifying parallelism, comparing against ideal, measuring efficiency.

#### 5. Latency View (Key: `5`)

The latency view provides statistical analysis of operation wait times (time from creation to callback execution).

**Features:**
- Multiple visualization modes:
  - **Histogram**: Distribution of latencies with outlier detection (>3σ)
  - **CDF**: Cumulative distribution showing percentiles
  - **Birth Order**: Latency vs sibling position (reveals queuing effects)
  - **By Type**: Violin plots grouped by resource type (latency-weighted)
  - **By User Code Location**: Violin plots grouped by originating code location
- Violin plots show distribution shape, not just quartiles
- Auto log scale when range exceeds 100×
- Critical path resources highlighted

**When to Use:** Statistical performance analysis, identifying slow operation types, finding code locations that introduce the most wait time.

#### 6. Gaps View (Key: `6`)

The gaps view identifies and classifies idle periods where no JavaScript was executing.

**Features:**
- Gap classification by cause (fetch waiting, timer waiting, I/O, promise chain)
- Color-coded gap bars
- Threshold slider to filter minor gaps
- Optimization recommendations based on gap patterns

**When to Use:** Finding optimization opportunities, understanding where time goes between operations.

### Analysis Features

Accessible via the 🔬 Analysis dropdown or keyboard shortcuts:

| Feature | Key | Description |
|---------|-----|-------------|
| **Critical Path** | `C` | Highlights the minimum-latency dependency chain that determined request duration |
| **Bottlenecks** | `B` | Identifies top 5 resources consuming the most time |
| **Patterns** | `T` | Detects 14 anti-patterns (sequential fetches, blocking, unbatched operations) |
| **Click Filter** | `F` | Filters to ancestors/descendants of clicked resource |
| **Stack Group** | `G` | Groups resources by creation stack trace |
| **Temporal Edges** | `E` | Shows timing-based causality (dashed lines) |
| **Hide Internal** | `H` | Hides internal runtime machinery (bridges, internal modules) |
| **Group Siblings** | `S` | Collapses repetitive sibling resources into compound nodes |
| **High Contrast** | `A` | Accessibility mode with enhanced colors |

### Pattern Detection

The viewer detects 14 anti-patterns with configurable severity thresholds:

**High Severity:**
- Sequential fetches that could be parallelized
- Event loop blocking (callbacks >50ms)
- Unbatched storage operations

**Medium Severity:**
- Duplicate fetches to the same URL
- Deep promise chains (>10 levels)
- Waterfall fetch patterns
- Long idle gaps
- Promise/callback floods
- Excessive fetch concurrency (>6)

**Low Severity:**
- Unresolved promises
- Redundant timers
- Cache misses
- Hot callbacks (high invocation count)

### Sibling and Cousin Grouping

Complex traces often contain repetitive patterns—for example, a loop that makes 10 fetch requests, or a stream that's read in 50 chunks. Sibling grouping collapses these patterns to reduce visual noise.

**Siblings** are resources that:
1. Share the same stack trace (or both have internal-only stacks)
2. Were triggered by the same parent
3. Were created within a time window (during parent's callback, or within configurable proximity threshold)

**Cousins** extend this: resources with the same stack whose *parents* are siblings.

Example: Reading a stream in chunks:
```
Parent #26 spawns read operations: #38, #39, #40, #41 (siblings)
  ├─ #38 spawns promises: #44, #45
  ├─ #39 spawns promises: #46, #47  ← These 8 are "cousins"
  ├─ #40 spawns promises: #48, #50
  └─ #41 spawns promises: #49, #51
```

With cousin grouping enabled, all 8 child promises collapse into one compound node.

**Configurable Options:**
- **Proximity threshold** (1-5ms): How close in time for sibling grouping when parent timing is unavailable
- **Cousin grouping** (Y/N): Whether to merge cousin groups

### Implementation Notes

The viewer is structured as:

1. **State Variables**: Each view maintains its own state (hover index, cached data, render parameters)

2. **Render Functions**: `render<ViewName>()` functions handle drawing and store data for hit detection

3. **Event Handlers**: Hover, click, and keyboard handlers set up once per canvas via `_<view>HandlersSet` flag

4. **Tooltips**: Dynamically created `<div>` elements positioned near mouse pointer

5. **Keyboard Navigation**: Global handler at `document.addEventListener('keydown', ...)` with view-specific routing

**Testing Changes:**
```bash
cd tools/async-trace-viewer
python3 -m http.server 8888
# Open http://localhost:8888, use demo dropdown or load trace JSON
```

---

## Instrumentation Model and JSON Format

### Resource Lifecycle

Each async resource goes through a defined lifecycle tracked by the `AsyncTraceContext`:

```
       createResource()
              │
              ▼
    ┌─────────────────┐
    │     Created     │  createdAt = now()
    └────────┬────────┘
             │
             │ (async delay - waiting for I/O, timer, etc.)
             │
             ▼
    ┌─────────────────┐
    │  enterCallback  │  callbackStartedAt = now()
    └────────┬────────┘
             │
             │ (sync execution - JavaScript running)
             │
             ▼
    ┌─────────────────┐
    │  exitCallback   │  callbackEndedAt = now()
    └────────┬────────┘
             │
             │ (cleanup - waiting for GC)
             │
             ▼
    ┌─────────────────┐
    │ destroyResource │  destroyedAt = now()
    └─────────────────┘
```

### Resource Types

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
  kMicrotask,         // queueMicrotask
  kStreamRead/Write/PipeTo/PipeThrough,  // Stream operations
  kSocketConnect/StartTls/Close,         // Socket operations
  kWebSocket,         // WebSocket operations
  kCrypto,            // Crypto operations
  kAiInference,       // AI inference
  kOther              // Unclassified
};
```

### Instrumentation Pattern

APIs are instrumented following this pattern:

```cpp
// In an API implementation (e.g., http.c++, cache.c++)
auto& ioContext = IoContext::current();
if (auto* trace = ioContext.getAsyncTrace()) {
  // Create a resource, capturing current stack and trigger
  auto asyncId = trace->createResource(
      AsyncTraceContext::ResourceType::kFetch,
      isolate);  // V8 isolate for stack capture

  // Add contextual annotations
  trace->annotate(asyncId, "url", url.toString());
  trace->annotate(asyncId, "method", "GET");

  // When the async operation completes and callback runs:
  trace->enterCallback(asyncId);
  KJ_DEFER(trace->exitCallback());  // Exception-safe exit

  // ... execute callback ...
}
```

### JSON Schema

The trace is serialized to JSON with this structure:

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
      "destroyedAt": 123456800
    },
    {
      "asyncId": 2,
      "triggerId": 1,
      "type": "fetch",
      "stackTraceId": 1,
      "createdAt": 1000000,
      "callbackStartedAt": 50000000,
      "callbackEndedAt": 51000000,
      "destroyedAt": 0
    }
  ],
  "stackTraces": [
    {"id": 0, "frames": []},
    {"id": 1, "frames": ["fetchUser @ worker:10:5", "handleRequest @ worker:25:12"]}
  ],
  "annotations": [
    {"asyncId": 2, "key": "url", "value": "https://api.example.com/user"},
    {"asyncId": 2, "key": "method", "value": "GET"}
  ]
}
```

### Key Fields

**Resources:**
- `asyncId`: Unique identifier (root is always 1)
- `triggerId`: Parent resource (0 = no parent)
- `type`: Resource type string
- `stackTraceId`: Reference to deduplicated stack trace
- Timing fields: All in nanoseconds relative to request start; 0 = not occurred

**Stack Traces:**
- Deduplicated by content to save space
- Frame format: `functionName @ module:line:column`
- Innermost frame first

**Annotations:**
- Key-value metadata attached to resources
- Common keys: `url`, `method`, `delay`, `type`, `address`

### Computed Metrics

The viewer computes these derived values:

- **Async delay**: `callbackStartedAt - createdAt` (wait time)
- **Sync time**: `callbackEndedAt - callbackStartedAt` (CPU time)
- **Total time**: `callbackEndedAt - createdAt`
- **Latency**: Another term for async delay

### Capturing Traces

```bash
bazel-bin/src/workerd/server/workerd serve \
    --verbose --perfetto-trace=/tmp/trace.perfetto=workerd \
    your-config.capnp 2>&1 &
curl http://localhost:8080/
pkill workerd  # Trace JSON output on shutdown
```

The trace JSON appears in stderr with prefix: `AsyncTrace completed; toJson() = {...}`

---

## Next Steps and Future Directions

### Immediate Concrete Tasks

#### Instrumentation Completion

The following APIs need instrumentation following established patterns:

| API | File | Resource Types | Priority |
|-----|------|----------------|----------|
| KV | `src/workerd/api/kv.c++` | kKvGet, kKvPut, kKvDelete, kKvList | High |
| Durable Objects | `src/workerd/api/actor-state.c++` | kDurableObjectGet/Put/Delete/List/Call | High |
| R2 | `src/workerd/api/r2*.c++` | kR2Get, kR2Put, kR2Delete, kR2List | Medium |
| D1 | `src/workerd/api/sql.c++` | kD1Query | Medium |
| Queues | `src/workerd/api/queue.c++` | kQueueSend | Medium |
| WebSocket | `src/workerd/api/web-socket.c++` | kWebSocket | Low |
| Crypto | `src/workerd/api/crypto*.c++` | kCrypto | Low |
| AI | `src/workerd/api/ai.c++` | kAiInference | Low |

#### Testing Infrastructure

- [ ] Unit tests for `AsyncTraceContext` class
- [ ] Integration tests generating known traces
- [ ] Perfetto UI validation for emitted events
- [ ] Viewer automated testing with sample traces

#### Deployment and Integration

- [ ] Wrangler dev integration for local development
- [ ] Trace output to response header or separate endpoint option
- [ ] On-demand trace capture mechanism (e.g., header-triggered for specific requests)
- [ ] Integration with existing Cloudflare tracing infrastructure

#### Viewer Enhancements

- [ ] Cross-view linking (selections sync across views)
- [ ] Search by asyncId, type, or stack frame
- [ ] Export/share functionality
- [ ] Minimap/overview for navigation in large traces

### Speculative Future Directions

#### Advanced Visualizations

**Phase Space Plot**: X-axis = waiting time, Y-axis = execution time. Each resource as a point. Instantly reveals whether problems are "waiting too long" vs "executing too long" and clusters operations by behavior pattern.

**Sankey Diagram**: Resources flow through states (created → waiting → executing → done/leaked). Width = count or duration. Shows where resources pile up, leak, or get stuck.

**Arc Diagram**: Linear timeline with arcs connecting parent→child relationships. Arc height = time distance. Shows dependency depth and cascade risk.

**Flame Graph Animation**: Build a flame graph in real-time during replay. Width = time consumed, stacking = async nesting. Watch flames grow as callbacks execute.

#### Analysis Improvements

**Latency by Depth**: Are deeper async operations slower? Scatter plot or box plots grouped by depth level. Reveals if delays cascade/accumulate.

**Critical Path Latency Breakdown**: Dedicated view showing only resources on the critical path with waterfall-style visualization of where time actually went.

**Wait vs Execution Time Scatter**: 2D scatter distinguishing "blocked on I/O" from "CPU-bound callback taking too long."

**Trace Comparison Mode**: Compare two traces (before/after optimization). Overlaid histograms or CDFs with statistical significance indicators.

#### Production Features

**Streaming Traces**: Real-time visualization as requests execute, not just post-hoc analysis.

**Aggregate Analysis**: Combine traces from multiple requests to identify patterns across a workload, not just individual requests.

**Anomaly Detection**: Machine learning to identify unusual trace patterns that might indicate problems.

**Performance Regression Detection**: Compare traces across deployments to catch performance regressions automatically.

#### Integration Points

**Perfetto Trace Augmentation**: The viewer currently consumes only the JSON async trace. Accepting Perfetto traces as a supplementary input could significantly enhance analysis by providing lower-level system context.

*Additional data available in Perfetto traces:*

- **I/O Event Decomposition**: A `fetch()` appears as a single resource in the async trace, but Perfetto could reveal DNS resolution, TCP connection, TLS handshake, time-to-first-byte, and body streaming as distinct phases.

- **CPU Scheduling**: When a callback shows 50ms of "sync time," was JavaScript actually executing, or was the thread descheduled? Perfetto scheduling events could distinguish CPU-bound work from contention.

- **Memory and GC Events**: V8 GC pauses are captured by Perfetto. A mysterious gap in async activity might correlate with a major GC cycle—currently invisible in the JSON trace.

- **KJ Event Loop Internals**: The workerd runtime emits Perfetto events for KJ promise resolution, I/O polling, and internal machinery. This could explain *why* a KJ-to-JS bridge took time to resolve.

- **Concurrent Request Context**: Perfetto traces span multiple requests. If request A is slow because request B triggered GC, that relationship would be visible.

*Analysis benefits:*

- **Gap Explanation**: The Gaps view currently classifies idle periods heuristically. With Perfetto data, we could show definitively: "This 200ms gap was: 150ms waiting for TCP data, 30ms in GC, 20ms idle."

- **Bottleneck Decomposition**: When a fetch is on the critical path, is it slow due to network latency, slow origin, or large response streaming? Perfetto I/O events could break this down.

- **Contention Detection**: If parallel operations are slower than expected, Perfetto scheduling data could reveal thread contention or event loop saturation.

- **Richer Waterfall**: Nested sub-operations within each async resource—expanding a fetch bar to reveal I/O phases, similar to browser DevTools network timing breakdown.

*Implementation considerations:*

The challenge is correlation—matching Perfetto events to async resources. This would require emitting `asyncId` as a Perfetto flow ID or annotation, parsing Perfetto's protobuf format (or using trace_processor), and handling clock alignment. The viewer could accept JSON-only (current), Perfetto-only (reduced causality analysis), or both (full fidelity).

*Verdict:* High potential value for diagnosing "why is this slow?"—the async trace shows *what* is slow, Perfetto could show *why* at the system level. Main cost is implementation complexity.

**IDE Integration**: VSCode extension showing traces inline with code, highlighting hot spots.

**Alerting Integration**: Trigger alerts when traces match problematic patterns (e.g., sequential fetches detected).

---

## Appendix: Keyboard Shortcuts Reference

**Navigation:**
- `1`-`6`: Switch views
- `?`: Help/guide
- `O`: Open file
- `P`: Paste JSON
- `Esc`: Close dialogs

**Analysis:**
- `C`: Toggle critical path
- `B`: Show bottlenecks
- `T`: Toggle pattern detection
- `F`: Filter to clicked resource
- `G`: Toggle stack grouping
- `E`: Toggle temporal edges
- `H`: Hide internal resources
- `S`: Toggle sibling grouping
- `A`: Toggle high contrast

**View-Specific:**
- Graph: `←`/`→` switch layouts
- Replay: `Space` play/pause, `R` reset, `←`/`→` step, `+`/`−` speed
- Replay: `Shift+L` loop, `Shift+O` ghost mode
- All: `Shift+R` reset view, `I` AI analysis prompt
