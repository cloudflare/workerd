# Async Trace Implementation Progress

This file tracks progress on implementing async tracing for Workers, enabling clinicjs/bubbleprof-style visualization of async activity.

## Goal

Enable visualization of async activity in workers by tracking:
- Async resource creation and destruction
- Causality chains (which async operation triggered which)
- Timing (when callbacks start/end, async delays)
- Stack traces at resource creation

## Architecture Overview

1. **AsyncTraceContext** (`src/workerd/io/async-trace.h/.c++`): Core tracking
   - Tracks async resources with unique IDs and trigger relationships
   - Captures deduplicated stack traces, timing at nanosecond precision
   - Supports annotations (e.g., URLs for fetch)
   - Emits Perfetto trace events and JSON output

2. **AsyncTracePromiseHook** (`src/workerd/io/async-trace-hooks.h/.c++`): V8 Promise integration
   - Uses V8's PromiseHook API for promise lifecycle tracking

3. **IoContext Integration** (`src/workerd/io/io-context.h/.c++`):
   - `asyncTrace` member per request, auto-enabled with Perfetto tracing

4. **API Instrumentation**: fetch, cache, timers, streams, KJ↔JS bridges

## Resource Types

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
  kTimer,             // setTimeout/setInterval/setImmediate
  kMicrotask,         // queueMicrotask
  kStreamRead/Write/PipeTo/PipeThrough,  // Stream operations
  kSocketConnect/StartTls/Close,         // Socket operations
  kWebSocket,         // WebSocket operations
  kCrypto,            // Crypto operations
  kAiInference,       // AI inference
  kOther              // Unclassified
};
```

## Implementation Status

### Core Infrastructure - Complete
- [x] AsyncTraceContext with resource tracking, stack traces, timing, annotations
- [x] V8 Promise hook integration
- [x] IoContext integration
- [x] JSON output format
- [x] Perfetto trace event emission

### Completed Instrumentation

**Basic APIs** (`src/workerd/api/`):
- [x] `fetch()` in `http.c++` - creates kFetch with URL/method annotations
- [x] `cache.match/put/delete` in `cache.c++` - creates kCacheGet/kCachePut
- [x] `setTimeout/setInterval` in `global-scope.c++` - creates kTimer with delay/type annotations
- [x] `setImmediate` in `global-scope.c++` - creates kTimer with type="setImmediate"
- [x] `queueMicrotask` in `global-scope.c++` - creates kMicrotask

**Stream Operations** (`src/workerd/api/streams/`):
- [x] `ReaderImpl::read()` → kStreamRead
- [x] `WritableStreamDefaultWriter::write()` → kStreamWrite
- [x] `ReadableStream::pipeTo()` → kStreamPipeTo
- [x] `ReadableStream::pipeThrough()` → kStreamPipeThrough

**Socket Operations** (`src/workerd/api/sockets.c++`):
- [x] `connect()` → kSocketConnect with address/secureTransport annotations
- [x] `Socket::startTls()` → kSocketStartTls with address annotation
- [x] `Socket::close()` → kSocketClose with address annotation

**KJ↔JS Bridges** (`src/workerd/io/io-context.h`):
- [x] `awaitIoImpl()` → kKjToJsBridge (KJ promise wrapped for JS)
- [x] `awaitJs()` → kJsToKjBridge (JS promise awaited in KJ)
- Both use `KJ_DEFER` for exception-safe callback exit

### TODO - Instrumentation Needed

The following APIs need instrumentation following the pattern in `http.c++`:

| API | File | Resource Types |
|-----|------|----------------|
| KV | `src/workerd/api/kv.c++` | kKvGet, kKvPut, kKvDelete, kKvList |
| Durable Objects | `src/workerd/api/actor-state.c++` | kDurableObjectGet/Put/Delete/List/Call |
| R2 | `src/workerd/api/r2*.c++` | kR2Get, kR2Put, kR2Delete, kR2List |
| D1 | `src/workerd/api/sql.c++` | kD1Query |
| Queues | `src/workerd/api/queue.c++` | kQueueSend |
| WebSocket | `src/workerd/api/web-socket.c++` | kWebSocket |
| Crypto | `src/workerd/api/crypto*.c++` | kCrypto |
| AI | `src/workerd/api/ai.c++` | kAiInference |

**Instrumentation pattern:**
```cpp
// Get trace context
auto& ioContext = IoContext::current();
if (auto* trace = ioContext.getAsyncTrace()) {
  auto asyncId = trace->createResource(AsyncTraceContext::ResourceType::kFetch,
                                        trace->getCurrentAsyncId());
  trace->addAnnotation(asyncId, "url", url.toString());
  // ... do work ...
  trace->enterCallback(asyncId);
  KJ_DEFER(trace->exitCallback(asyncId));
}
```

### Other TODO
- [ ] Unit tests for AsyncTraceContext
- [ ] Perfetto UI validation
- [ ] Consider exposing trace via response header or separate endpoint
- [ ] Production sampling strategy
- [ ] Integration with existing tracing infrastructure

## Notes for Resuming Instrumentation Work

1. **Verify build**: `bazel build //src/workerd/io:io`
2. **Test pattern**: Create a `.wd-test` exercising the API, verify JSON in logs
3. **Follow existing patterns**: See `http.c++` for fetch, `cache.c++` for cache ops
4. **For KJ promises**: Look at `awaitIo()` in `io-context.h`

## Key Files

| File | Purpose |
|------|---------|
| `src/workerd/io/async-trace.h/.c++` | Core AsyncTraceContext |
| `src/workerd/io/async-trace-hooks.h/.c++` | V8 Promise hook |
| `src/workerd/io/io-context.h/.c++` | IoContext integration |
| `src/workerd/api/http.c++` | fetch() instrumentation |
| `src/workerd/api/cache.c++` | Cache API instrumentation |
| `src/workerd/api/global-scope.c++` | Timer instrumentation |
| `src/workerd/api/streams/*.c++` | Stream instrumentation |

## JSON Output Format

```json
{
  "requestDurationNs": 123456789,
  "resources": [
    { "asyncId": 1, "triggerId": 0, "type": "root", "stackTraceId": 0,
      "createdAt": 0, "callbackStartedAt": 0, "callbackEndedAt": 123456789, "destroyedAt": 0 }
  ],
  "stackTraces": [{ "id": 0, "frames": ["functionName @ script.js:10:5"] }],
  "annotations": [{ "asyncId": 2, "key": "url", "value": "https://example.com" }]
}
```

## Capturing Traces

```bash
bazel-bin/src/workerd/server/workerd serve \
    --verbose --perfetto-trace=/tmp/trace.perfetto=workerd \
    samples/helloworld_esm/config.capnp 2>&1 &
curl http://localhost:8080/
pkill workerd  # Trace JSON output on shutdown
```

Trace JSON appears in stderr with prefix: `AsyncTrace completed; toJson() = {...}`

---

# Visualization Tool

Location: `tools/async-trace-viewer/index.html` (single-file HTML/CSS/JS application)

## Code Structure

The viewer is a single HTML file with embedded CSS and JavaScript. Key organization:

### State Variables (search for `// ... view state`)
Each view has its own state variables for hover, data caching, and render params:
```javascript
// Example pattern for view-specific state
let heatmapHoverCell = null;      // Current hover state
let heatmapData = null;           // Cached computed data for hit detection
let heatmapRenderParams = null;   // Render params for coordinate mapping
let heatmapBucketCount = 40;      // Control state
```

### Render Functions
Each view has a `render<ViewName>()` function (e.g., `renderHeatmap()`, `renderGaps()`). Pattern:
1. Get canvas and context
2. Account for controls height if view has controls div
3. Clear and draw
4. Store data/params for hover detection
5. Setup event handlers once (check `canvas._<view>HandlersSet`)

### Adding Controls to a View
1. Add control HTML inside the view's container div (before canvas)
2. Add CSS rule: `#<view>-view.active { display: flex; flex-direction: column; }`
3. Add state variables for control values
4. In render function, account for controls height: `container.clientHeight - controlsHeight`
5. In `setup<View>EventHandlers()`, add listeners for controls

### Event Handler Pattern
```javascript
function setup<View>EventHandlers(canvas) {
  // Control handlers
  document.getElementById('<view>-control').addEventListener('input/change', (e) => {
    stateVar = e.target.value;
    render<View>();
  });

  // Hover handler - update hover state, re-render, show/hide tooltip
  canvas.addEventListener('mousemove', (e) => { ... });
  canvas.addEventListener('mouseleave', () => { ... });

  // Click handler - populate sidebar details
  canvas.addEventListener('click', (e) => { ... });
}
```

### Tooltip Pattern
Each view with tooltips creates a tooltip element on first use:
```javascript
function show<View>Tooltip(e, data) {
  if (!<view>Tooltip) {
    <view>Tooltip = document.createElement('div');
    <view>Tooltip.style.cssText = `position: fixed; background: rgba(22, 33, 62, 0.95); ...`;
    document.body.appendChild(<view>Tooltip);
  }
  <view>Tooltip.innerHTML = html;
  <view>Tooltip.style.left = (e.clientX + 15) + 'px';
  <view>Tooltip.style.top = (e.clientY + 15) + 'px';
  <view>Tooltip.style.display = 'block';
}
```

### Keyboard Handling
Global keyboard handler at `document.addEventListener('keydown', ...)` around line 2334. View-specific keys check `currentView` first. Add new view-specific shortcuts there.

### Critical Path Integration
When adding critical path support to a view:
1. Check `showCriticalPath` global flag
2. Use `criticalPathSet.has(r.asyncId)` to check if resource is on critical path
3. Typically show orange color/border and 🔥 badge

## Testing Changes

```bash
cd tools/async-trace-viewer
python3 -m http.server 8888
# Open http://localhost:8888, use demo dropdown or load trace JSON
```

## Views

| Key | View | Description |
|-----|------|-------------|
| 1 | **Waterfall** | Timeline with concurrency graph, dependency arrows, hover highlighting |
| 2 | **Graph** | Bubble/Hierarchical/Force layouts (←/→ to switch), path highlighting |
| 3 | **Replay** | Animated playback with Grid/Bubble/Rings layouts, lifecycle badges, loop/ghost modes |
| 4 | **Parallelism** | Concurrent ops over time, efficiency metrics, ideal comparison |
| 5 | **Breakdown** | Treemap/Sunburst by type/trigger/stack, drill-down navigation |
| 6 | **Latency** | Histogram/CDF of async wait times, outlier detection |
| 7 | **Gaps** | Idle periods with classification, recommendations |
| 8 | **Heatmap** | Activity intensity grid, multiple color schemes |

## Analysis Features (🔬 dropdown)

| Key | Feature | Description |
|-----|---------|-------------|
| C | Critical Path | Highlights minimum-latency dependency chain |
| B | Bottlenecks | Top 5 resources consuming most time |
| T | Patterns | Detects 14 anti-patterns (sequential fetches, blocking, etc.) |
| F | Click Filter | Filter to clicked resource's ancestors/descendants |
| G | Stack Group | Group by creation stack trace |
| E | Temporal Edges | Shows timing-based causality |
| H | Hide Internal | Hides internal runtime machinery (bridges, empty stacks) |
| S | Group Siblings | Collapses sibling resources into compound nodes (options: Proximity 1-5ms, Cousins Y/N) |
| A | High Contrast | Accessibility mode |

## Understanding Sibling & Cousin Grouping

Async traces can contain many resources that are essentially "the same operation repeated". For example, reading a stream in chunks creates multiple similar promise chains. Sibling grouping collapses these repetitive patterns to reduce visual clutter.

### What are Siblings?

**Siblings** are resources that:
1. Were created from the **same code location** (same stack trace)
2. Were triggered by the **same parent** resource
3. Were created within a short **time window** (during parent's callback, or within the proximity threshold)

Example: A loop that creates 10 fetch requests will produce 10 sibling resources - all from the same line of code, all triggered by the same parent.

### What are Cousins?

**Cousins** are resources that:
1. Were created from the **same code location** (same stack trace)
2. Were triggered by **different parents that are themselves siblings**

Example: If you have 4 sibling stream-read operations, and each spawns 2 promise children from the same code location, those 8 promises are cousins of each other.

```
Parent #26 spawns siblings: #38, #39, #40, #41 (same stack trace)
  └─ #38 spawns: #44, #45
  └─ #39 spawns: #46, #47   ← These 8 promises are "cousins"
  └─ #40 spawns: #48, #50
  └─ #41 spawns: #49, #51
```

Without cousin grouping: 4 separate pairs (8 nodes shown as 4 groups)
With cousin grouping: 1 merged group (8 nodes collapsed into 1 compound node)

### Internal Resources

Resources classified as "internal" (runtime machinery like KJ↔JS bridges, or promises with no/internal-only stack traces) are grouped together if they share the same parent, regardless of their specific stack trace ID. This prevents internal implementation details from fragmenting otherwise cohesive groups.

### How to Interpret Compound Nodes

In the Graph view, when sibling grouping is enabled:
- **Compound nodes** appear with a dashed blue ring and a red count badge (e.g., "×8")
- **Click** a compound node to expand it and see individual members
- **Click** the minus badge on an expanded group to collapse it again
- Edges to/from the group connect to the "representative" (first member)

### Adjusting Grouping Behavior

Under the "Group Siblings" option in the Analysis dropdown:
- **Proximity (1-5ms)**: How close in time resources must be created to be considered siblings when the parent has no callback timing data
- **Cousins (Y/N)**: Whether to merge cousin groups into larger compound nodes

## Keyboard Shortcuts

**Navigation:** `1`-`8` switch views, `?` help, `O` load, `P` paste, `Esc` close

**Analysis:** `C` critical path, `B` bottlenecks, `T` patterns, `F` filter, `G` stack group, `E` temporal edges, `H` hide internal, `S` group siblings, `A` accessibility

**View-Specific:**
- Graph: `←`/`→` switch layouts
- Replay: `Space` play/pause, `R` reset, `←`/`→` step, `Shift+←`/`→` jump to events, `-`/`+` speed, `Shift+L` loop, `Shift+O` ghost
- Heatmap: `←`/`→` adjust bucket count
- All: `Shift+R` reset view, `I` AI analysis prompt

## Pattern Detection

14 patterns detected with configurable thresholds:

| Severity | Patterns |
|----------|----------|
| **High** | Sequential fetches, Event loop blocking (>50ms), Unbatched operations |
| **Medium** | Duplicate fetches, Deep chains (>10), Waterfall fetches, Long gaps, Promise/callback floods, Fetch concurrency (>6) |
| **Low** | Unresolved promises, Redundant timers, Cache misses, Hot callbacks |

## Sample Traces

**Real traces:** helloworld, async-context, durable-objects, nodejs-compat-fs/streams, tcp

**Anti-pattern examples:** sequential, duplicates, waterfall, unresolved, long-gaps, redundant-timers, cache-misses

---

# View Enhancement Tracker

## Implementation Details by View

### Waterfall
- **State:** Uses `highlightedChain` Set for hover highlighting
- **Features:** Concurrency graph (canvas above), dependency arrows (SVG overlay in tree mode), expandable stack traces (▶ button)

### Graph (combined Bubble/DAG)
- **State:** `dagLayoutMode` ('bubble'|'hierarchical'|'force'), `dagNodes`, `dagSimulationRunning`
- **Features:** 3 layouts with ←/→ switching, path highlighting on hover, drag to reposition nodes

### Replay
- **State:** `replayProgress`, `replayPlaying`, `replaySpeed`, `replayLoopMode`, `replayGhostMode`, `replayTrails`, `replayGhosts`, `replayLayoutMode`, `replayBubbleNodes`, `replayBubbleViewport`
- **Features:** Animated playback, 3-way layout toggle (Grid/Bubble/Rings), dynamic node sizing (grows during callback), lifecycle badges, ghost pulses for state changes, keyboard navigation
- **Grid Mode:** Resources in sequential grid, fixed node size, simple layout
- **Bubble Mode:** Hierarchical tree layout based on trigger relationships, position smoothing, auto-panning viewport, variable node size based on sync time
- **Rings Mode:** Time-based concentric rings (tree rings metaphor), resources placed on ring by creation time, angular position based on parent, sweeping time indicator

### Parallelism
- **State:** `parallelismHoverBucket`, `parallelismBucketData`, `parallelismRenderParams`
- **Features:** Stacked bars (sync solid, async faded), cyan ideal line, orange critical path line, efficiency sidebar

### Breakdown
- **State:** `breakdownGroupBy`, `breakdownVizMode`, `breakdownDrillPath`, `breakdownHoverItem`, `breakdownData`
- **Features:** Treemap/sunburst toggle, 4 grouping modes, drill-down with breadcrumbs, Shift+R to reset drill path
- **Gotcha:** Drill-down uses `canDrillDown = item.count > 1` and checks `wouldBeSameGroup` to prevent infinite recursion

### Latency
- **State:** `latencyMode` ('histogram'|'cdf'), `latencyShowOutliers`, `latencyBucketData`, `latencyHoverBucket`
- **Features:** Histogram/CDF toggle, outlier detection (>3σ with red border), percentile lines (p50/p90/p99)
- **Controls:** Mode dropdown, outliers checkbox

### Gaps
- **State:** `gapsHoverIndex`, `gapsShowClassification`, `gapsShowRecommendations`, `gapsThresholdPercent`, `gapsShowMinor`, `gapsData`
- **Features:** Gap classification by cause (fetch/timer/io/promise), color-coded gaps, optimization recommendations
- **Controls:** Threshold slider, show minor checkbox, classify checkbox, recommendations checkbox

### Heatmap
- **State:** `heatmapHoverCell`, `heatmapBucketCount`, `heatmapColorScheme`, `heatmapActivityMode`, `heatmapSortMode`, `heatmapData`, `heatmapRenderParams`
- **Features:** Hover/click interaction, 3 color schemes, 4 activity modes, 4 sort modes, critical path row highlighting
- **Controls:** Bucket slider (10-120, ←/→ keys), color dropdown, activity dropdown, sort dropdown

## Known Issues / Gotchas

1. **View switching CSS:** Views with controls need `#<view>-view.active { display: flex; flex-direction: column; }` to override the default `display: block`

2. **Breakdown drill-down recursion:** The trigger chain grouping traces to root ancestor; must use same logic in both grouping and `wouldBeSameGroup` check

3. **Canvas sizing:** For views with controls, must subtract controls height: `canvas.height = container.clientHeight - controlsHeight`

4. **Event handler setup:** Use `canvas._<view>HandlersSet` flag to avoid adding duplicate handlers on re-render

## Potential Future Enhancements

### General
- [ ] Cross-view linking (selections sync across views)
- [ ] Export/share functionality
- [ ] Comparison mode (diff two traces)
- [ ] Minimap/overview for navigation
- [ ] Search by asyncId, type, or stack frame

### Per-View
- **Latency:** Type filtering, zoom/range selection
- **Gaps:** Gap-to-resource linking, ideal timeline comparison
- **Heatmap:** Row filtering, time range zoom
- **Replay:** Timeline bookmarks, focus mode, export animation

---

# Session Notes

## Session (January 2025) - Visualization Enhancements

**Completed:**
- Latency view: hover/click, histogram/CDF toggle, outlier detection, critical path integration
- Gaps view: hover/click, gap classification with color-coding, threshold controls, optimization recommendations
- Heatmap view: hover/click, bucket slider with ←/→ keys, 3 color schemes, 4 activity modes, 4 sort modes, critical path rows

**Files modified:**
- `tools/async-trace-viewer/index.html` - all visualization changes
- `ASYNC_TRACE_PROGRESS.md` - consolidated from separate tracker file

## Latest Session (January 2025) - Socket/Microtask/Immediate Instrumentation

**Completed:**
- Added `kMicrotask` ResourceType to `async-trace.h`
- Instrumented `queueMicrotask()` in `global-scope.c++` - creates kMicrotask resource with callback tracking
- Instrumented `setImmediate()` in `global-scope.c++` - creates kTimer with type="setImmediate" annotation
- Added socket ResourceTypes: `kSocketConnect`, `kSocketStartTls`, `kSocketClose`
- Instrumented `connect()` in `sockets.c++` - creates kSocketConnect with address/secureTransport annotations
- Instrumented `Socket::startTls()` in `sockets.c++` - creates kSocketStartTls
- Instrumented `Socket::close()` in `sockets.c++` - creates kSocketClose

**Files modified:**
- `src/workerd/io/async-trace.h` - added kMicrotask, kSocketConnect, kSocketStartTls, kSocketClose
- `src/workerd/api/global-scope.c++` - instrumented queueMicrotask and setImmediate
- `src/workerd/api/sockets.c++` - instrumented connect, startTls, close

**Where we left off:**
- Visualization tool enhancements largely complete for all 8 views
- C++ instrumentation: fetch, cache, timers, streams, bridges, microtask, immediate, sockets complete
- Still could instrument: KV, DO, R2, D1, Queue, WebSocket, Crypto, AI APIs (if desired)

## Session (January 2025) - Bubble Replay Mode

**Completed:**
- Added bubble layout mode to Replay view alongside existing grid layout
- Layout toggle buttons (Grid/Bubble) in replay controls
- Dynamic node sizing: nodes grow as callbacks execute (proportional to sync time)
- Hierarchical tree layout based on trigger relationships (parents above children)
- Position smoothing: nodes animate smoothly when layout changes as new resources appear
- Auto-fit: viewport automatically scales to show all visible nodes
- All existing replay features work in both modes (ghost mode, trails, critical path, etc.)

**Implementation details:**
- `replayLayoutMode` state variable ('grid' or 'bubble')
- `replayBubbleNodes` Map stores current/target positions for smooth interpolation
- `computeReplayBubbleLayout()` computes hierarchical tree positions
- `updateReplayBubblePositions()` handles smooth position transitions
- Variable radius per node based on current sync time progress
- Click/hover handlers updated to use per-node radius

**Files modified:**
- `tools/async-trace-viewer/index.html` - bubble replay implementation

**Bubble Replay Features:**
- Nodes positioned hierarchically: root at top, children below
- Node size reflects callback execution time (grows during execution)
- Layout recomputes as new nodes appear, with smooth transitions
- Maintains tree structure visibility even with many resources
- Works with all existing replay controls (play/pause, speed, loop, ghost mode)
- Temporal edges rendered (dashed green lines) when enabled, appearing progressively as nodes become visible

## Session (January 2025) - Tree Rings Layout

**Completed:**
- Added "Rings" as third layout mode in Replay view (alongside Grid and Bubble)
- Time flows outward from center like tree rings
- Resources placed on concentric rings based on creation time
- Angular positioning based on parent-child relationships (children near parents)
- Node size reflects callback execution time (like bubble mode)
- Visual guides: concentric ring circles, radial lines, center point, sweeping time indicator

**Implementation details:**
- `replayLayoutMode` now supports 'grid', 'bubble', or 'rings'
- Ring assignment based on creation time divided into time buckets
- Angular position uses BFS from root, spreading children around parent angle
- Final angles blend parent-based position (30%) with even distribution (70%)
- Ring guide lines drawn behind nodes for visual context
- Sweeping time indicator line shows current replay position

**Rings Layout Features:**
- Center represents time 0 (trace start)
- Outer rings represent later time slices
- Resources appear on ring corresponding to their creation time
- Children positioned near their parent's angular position
- Node radius grows during callback execution
- 12 radial guide lines (like clock face)
- Animated time sweep line showing replay progress

## Session (January 2025) - Internal Event Classification & UI Improvements

**Completed:**

*Classification fixes:*
- Improved resource classification logic to properly identify internal events
- Added `isInternalFrame()` helper that parses stack frame format (`functionName @ module:line:col`)
- Internal module prefixes now detected: `node-internal:`, `node:`, `cloudflare-internal:`, `cloudflare:`, `workerd-internal:`, `workerd:`
- Resources with stacks containing only internal module frames now classified as "internal"
- Bridge types (`kj-to-js`, `js-to-kj`) now classified as "internal" rather than "typed" (API)

*Hide Internal toggle:*
- Replaced "All/User Only/Typed Only" filter combo with "Hide Internal" toggle in Analysis dropdown
- Keyboard shortcut: `H` (help changed to `?` only)
- Setting persisted to localStorage
- Affects Waterfall, Graph, and Heatmap views

*Waterfall controls relocation:*
- Moved Sort dropdown and Types dropdown from main header into Waterfall view control bar
- Consistent with other views (Heatmap, Gaps, Latency, Breakdown) that have view-specific controls
- Removed disabled-state logic for controls (no longer needed)
- Fixed Types dropdown alignment to prevent left-side clipping

**Files modified:**
- `tools/async-trace-viewer/index.html` - classification, Hide Internal, control bar changes

## Session (January 2025) - Group Siblings Feature

**Completed:**

*Sibling grouping algorithm:*
- Groups resources that share the same stack key + triggerId + temporal proximity
- Stack key is `stackTraceId` for user/typed resources, or `"internal"` for internal resources (empty stack or internal-only frames)
- Resources must be created during the trigger's callback execution window (or within proximity threshold of each other)
- Groups of 2+ resources get a "representative" (first resource in group)
- Optional "cousin grouping" merges groups whose triggers are themselves siblings

*Graph view compound nodes:*
- When "Group Siblings" enabled (Analysis dropdown or 'S' key), sibling groups collapse into compound nodes
- Compound nodes show larger radius with dashed blue ring
- Red count badge shows number of siblings in group (e.g., "3")
- Non-representative siblings hidden from graph, edges redirected to representative

*Click-to-expand/collapse:*
- Click compound node to expand - shows all siblings in their natural tree positions
- Expanded representative shows blue badge with minus sign ("−")
- Click minus badge to collapse back to compound node
- Layout properly handles parent redirection to keep tree connected

*Technical implementation:*
- `siblingGroups` Map stores group membership and representatives
- `expandedSiblingGroups` Set tracks which groups are currently expanded
- `_effectiveTriggerId` property handles edge redirection during layout
- All three layout modes (Force, Hierarchical, Bubble) updated for sibling support

**Files modified:**
- `tools/async-trace-viewer/index.html` - sibling grouping, compound nodes, expand/collapse

## Session (January 2025) - Analysis Dropdown UI Improvements

**Completed:**

*Configurable sibling proximity threshold:*
- Added `siblingProximityMs` setting (default 1ms, range 1-5ms)
- Slider in Analysis dropdown under "Group Siblings" option
- `computeSiblingGroups()` now uses configurable threshold instead of hardcoded 1ms
- Setting persisted to localStorage

*Reorganized Analysis dropdown:*
- Moved Pattern Thresholds from bottom of dropdown to directly under "Patterns" checkbox
- Sibling Proximity slider positioned directly under "Group Siblings" checkbox
- Consistent indentation (`padding-left: 24px`) for sub-options
- "Restore Defaults" button moved to bottom of dropdown

*Collapsible threshold sections:*
- Added expand/collapse toggles (▶/▼) on "Patterns" and "Group Siblings" lines
- Sections start collapsed by default
- `patternThresholdsExpanded` and `siblingOptionsExpanded` state variables
- Expanded state persisted to localStorage
- `updateExpandedSection(type)` helper function for UI updates

*Hotkey alignment fix:*
- Expand toggle uses `margin-left: auto` to push right
- Hotkey follows toggle with `margin-left: 0`
- Ensures consistent hotkey alignment across all dropdown items

**Files modified:**
- `tools/async-trace-viewer/index.html` - dropdown reorganization, collapsible sections
- `ASYNC_TRACE_PROGRESS.md` - session documentation

## Session (January 2025) - Internal Node & Cousin Grouping

**Completed:**

*Internal node grouping:*
- Resources classified as "internal" (empty stack trace OR only internal module frames) now use `"internal"` as their stack grouping key
- This allows internal runtime machinery to group together based on sharing the same trigger parent, regardless of their specific stackTraceId
- Leverages existing `_classification` property set during trace loading

*Cousin grouping:*
- New "Cousins" Y/N slider in sibling options (under "Group Siblings")
- When enabled, resources with the same stack key whose triggers are in the same sibling group get merged
- Example: In TCP trace, pairs like `(#44,#45)`, `(#46,#47)`, `(#48,#50)`, `(#49,#51)` merge into one group because their triggers (#38, #39, #40, #41) are siblings
- Reduces TCP trace from 38 sibling groups to 20 groups when enabled
- `cousinGrouping` state variable persisted to localStorage

*Algorithm changes in `computeSiblingGroups()`:*
- Phase 1: Compute initial sibling groups using `getStackKey(r)` which returns `"internal"` for internal resources
- Phase 2: If `cousinGrouping` enabled, re-key groups by `(stackKey, triggerSiblingGroupKey)` instead of `(stackKey, triggerId)`
- Merged cousin groups marked with `isCousin: true` property

*UI improvements:*
- Cousins slider styled consistently with Proximity slider (Y/N values)
- Added `white-space: nowrap` to "Group Siblings" label to prevent text wrapping
- Added `padding-right: 12px` to expanded option sections for better spacing
- Handler invalidates `dagNodes` and clears `expandedSiblingGroups` to force DAG re-render

*Tooltip enhancement:*
- Hovering over collapsed compound nodes now shows additional group information
- Displays group type ("Sibling group" or "Cousin group")
- Shows total member count (e.g., "8 resources")
- Lists type breakdown (e.g., "Types: js-promise: 6, stream-read: 2")
- Only shown for collapsed groups (not when expanded)

*Documentation:*
- Added "Sibling & Cousin Grouping" section to in-app Guide (❓ button)
- Explains what siblings and cousins are with visual example
- Documents compound node visual indicators and interaction
- Lists configurable options (Proximity, Cousins)
- Updated ASYNC_TRACE_PROGRESS.md with detailed session notes

**Files modified:**
- `tools/async-trace-viewer/index.html` - internal grouping, cousin grouping, tooltip, in-app guide

## Future Replay Animation Ideas

Brainstormed concepts for alternative replay visualizations:

### High Priority / Most Promising

1. **Pulse/Ripple Propagation** - Visualize "energy" flowing through the system. When a callback fires, send a visible pulse along edges to triggered children. Bottlenecks appear where pulses queue up.

2. **Flame Graph Animation** - Build a flame graph in real-time. Width = time consumed, stacking = async nesting. Watch flames grow as callbacks execute.

3. **Railway/Track Diagram** - Each resource gets a horizontal track. Execution = train moving. Trigger relationships = track switches. Clear parallelism vs serialization visualization.

4. **Pressure/Heat Map Flow** - Show pressure building in the graph. Executing nodes glow hot (red/orange), idle nodes cool (blue). Heat flows along edges, accumulates at bottlenecks.

5. **First-Person/Follow Mode** - Pick a resource and follow its lifecycle. Camera tracks it, dims unrelated activity. Great for debugging specific operations.

### Medium Priority

6. **Concurrency Lanes** - Swimming pool lanes representing concurrent capacity. Resources move through lanes during execution. Full lanes = visible queueing. (Attempted but removed - needs rethinking for better clarity)

7. **Particle System / Sparks** - Executing callbacks emit particles. Visual energy reflects activity. Particles flow along edges when triggering children.

8. **Delta/Diff Mode** - Show only changes, not cumulative state. New resources flash in, transitions pulse, completed fade out. Focuses attention on action.

9. **Causality Wave Front** - Time as a visible wave sweeping across visualization. Events cascade as wave passes through trigger chains.

10. **Resource Pool Visualization** - Group by type into visual pools. Show check-out (execute) and return (complete). Reveals contention patterns.

### Experimental

11. **Audio Sonification** - Map activity to sound. Types = instruments, depth = pitch, sync time = volume. Concurrent = chords, sequential = melody.

12. **3D Depth Mode** - Z-axis for time or trigger depth. Rotate for different perspectives on async execution.

## Data Science Visualization Ideas

Visualization techniques from other domains that could reveal async patterns:

### Tier 1: High Impact (Recommended)

**1. Phase Space Plot** ⭐⭐⭐
- **Domain:** Physics, dynamical systems
- **Concept:** X-axis = waiting time, Y-axis = execution time. Each resource is a point.
- **Value:** Instantly reveals outliers, clusters by behavior pattern, and whether problems are "waiting too long" vs "executing too long"
- **Problems solved:** Mystery latency, identifying slow operations, pattern discovery
- **Implementation:** Scatter plot, color by type, click to inspect, quadrant labels ("fast", "slow start", "slow execution", "slow everything")

**2. Sankey Diagram** ⭐⭐⭐
- **Domain:** Flow visualization, energy/material flow analysis
- **Concept:** Resources flow through states (created → waiting → executing → done/leaked). Width = count or cumulative duration.
- **Value:** Immediately shows where resources pile up, leak, or get stuck. Bottlenecks visible as wide flows into narrow states.
- **Problems solved:** Promise leaks, resource exhaustion, flow bottlenecks
- **Implementation:** States as columns, flows sized by count or duration, color by resource type

**3. Arc Diagram** ⭐⭐⭐
- **Domain:** Network science, genomics (sequence alignment)
- **Concept:** Linear timeline with arcs connecting parent→child relationships above the line. Arc height = time distance between creation events.
- **Value:** Shows dependency depth, cascade risk, long-lived dependencies. Long arcs spanning timeline = potential problems.
- **Problems solved:** Deep dependency chains, cascade failures, understanding trigger relationships
- **Implementation:** Time flows left-to-right, arcs connect parent to child, height proportional to time span

### Tier 2: Strong Value

**4. Piano Roll** ⭐⭐
- **Domain:** Music production (MIDI editors)
- **Concept:** Each resource type is a "note" row, horizontal bars show when active. Like a MIDI piano roll.
- **Value:** Intuitive parallelism visualization. Instantly reveals sequential patterns that should be parallel, gaps, bursts.
- **Problems solved:** Unexpected sequential execution, parallelism opportunities
- **Similar to:** Existing timeline but grouped by type rather than hierarchy

**5. Chord Diagram** ⭐⭐
- **Domain:** Network science, genomics (Circos)
- **Concept:** Circular layout with ribbons connecting resource types. Shows which types trigger which.
- **Value:** Understanding async "metabolism" - "fetch operations spawn 40% of promises, timers spawn 30%"
- **Problems solved:** Architectural understanding, type relationship patterns
- **Implementation:** Types around circle, ribbon width = spawn count between types

### Tier 3: Specialized Value

**6. Horizon Chart** ⭐
- **Domain:** Financial time series, monitoring
- **Concept:** Compact way to show multiple metrics (parallelism, memory, wait-queue-depth) simultaneously in minimal vertical space.
- **Value:** Dashboard-style monitoring, comparing multiple traces
- **Problems solved:** Multi-metric monitoring, trace comparison
- **Tradeoff:** Requires learning to read; less intuitive

**7. Candlestick/Box Plot Timeline** ⭐
- **Domain:** Financial analysis, statistics
- **Concept:** For each time bucket, show distribution of durations (min, quartiles, max, outliers).
- **Value:** Identifying "system gets slower over time" or "latency variance increases under load"
- **Problems solved:** Statistical patterns, performance degradation over time
- **Tradeoff:** More useful for aggregate analysis than single-trace debugging

### Other Considered Techniques

| Technique | Domain | Potential Use | Priority |
|-----------|--------|---------------|----------|
| Circos Plot | Genomics | Cross-type relationships in circular layout | Low (complex) |
| Phylogenetic Tree | Biology | Clustering similar execution patterns | Low (abstract) |
| Contour/Density Heatmap | GIS | Activity concentration in projected space | Medium |
| Trajectory Animation | Physics | Resources move through 2D state space | Medium |
| Spectrogram | Audio | Time vs type with intensity = activity | Medium |
| Flow Map | Cartography | Arrow-based spawn frequency visualization | Low |
| Entropy Visualization | Information theory | Predictability of async patterns | Low (abstract)

### Implementation Priority

If implementing incrementally:
1. **Phase Space Plot** - Simple, immediately actionable, answers "which operations are slow and why"
2. **Sankey Diagram** - Flow understanding, spotting leaks and pileups
3. **Arc Diagram** - Causal chain visualization, cascade risk assessment
