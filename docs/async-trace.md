# Async tracing

Async tracing records the asynchronous activity of each request a worker handles: every timer,
I/O wait, binding call and (optionally) promise, when each one ran JavaScript, and what caused
what. Use it to find out what a slow request spent its time waiting for, which `fetch()` or KV
call resumed the handler at a given point, where a timer, subrequest or stream read was started,
and which caller delivered a subrequest to a service binding.

The model follows Node.js's [`async_hooks`](https://nodejs.org/api/async_hooks.html): activity
is a set of _resources_ with lifecycle events and causal edges between them. Async tracing is a
diagnostics tool for people running workerd, mainly in local development. Workers can't see or
use it. It is separate from Workers' span tracing (`trace.h`, tail workers, user spans), but builds
on its spans; see [Compared with span tracing](#compared-with-span-tracing).

## Quick start

All of this is configured on the command line of `workerd serve` or `workerd test`.

Write a trace file (newline-delimited JSON, one event per line):

```sh
workerd serve config.capnp --async-trace=/tmp/trace.ndjson
```

Also record where each resource was created, up to 8 JavaScript stack frames deep:

```sh
workerd serve config.capnp --async-trace=/tmp/trace.ndjson --async-trace-stacks=8
```

Also trace every JavaScript promise. This adds a lot of events and slows the worker down:

```sh
workerd serve config.capnp --async-trace=/tmp/trace.ndjson --async-trace-promises
```

View the same activity on a timeline in [Perfetto](https://ui.perfetto.dev) by recording the
`workerd.async` category:

```sh
workerd serve config.capnp --perfetto-trace=/tmp/trace.pftrace=workerd,workerd.async
```

With an inspector attached (`--inspector-addr`), DevTools async stack traces continue across
timers, I/O and binding calls, not only across promises. No extra flag is needed. This covers
requests and Durable Object contexts that start while DevTools is connected (see
[Inspector](#inspector)).

The options can be combined. `--async-trace-stacks` also adds creation stacks to the Perfetto
events, and `--async-trace-promises` adds promises to every output.

| Option                         | Effect                                                                                            |
| ------------------------------ | ------------------------------------------------------------------------------------------------- |
| `--async-trace=<path>`         | Writes the NDJSON trace to `<path>` (overwritten).                                                |
| `--async-trace-stacks=<n>`     | Records up to `<n>` (1-64) frames of the JavaScript stack that created each resource.             |
| `--async-trace-promises`       | Traces JavaScript promises through V8's promise hook.                                             |
| `--perfetto-trace=<path>=<categories>` | With `workerd.async` among the categories, writes the async activity to the Perfetto trace. |
| `--inspector-addr=<addr>`      | While DevTools is connected, reports resources to the inspector as async tasks, for async stacks. |

Trace files contain request data. Binding operations carry their span tags verbatim, including
URLs, HTTP methods, status codes and storage keys, and creation stacks name source files and
functions. Handle trace files as you would logs.

### Cost

When none of the options above is given, async tracing is off, and each instrumentation point
costs one null check. When it is on, every context spends a little time recording resources and
turns. The two optional tiers cost more:

- `--async-trace-stacks` walks the JavaScript stack each time a resource is created.
- `--async-trace-promises` installs V8's promise hook, which slows down every promise operation in
  the isolate for its whole life. workerd never installs it in a multi-tenant process.

Timings from a traced run are not representative of an untraced one.

## Concepts

### Contexts

A trace is organized by _context_. There is one context per `IoContext`, which means one per
request for a stateless worker, and one per Durable Object instance (shared by the requests it
handles concurrently).
Each context has an ID (`ctx`) that is unique within the process. Its events form a well-formed
sequence that starts with `ctx` and ends with `ctx_end`.

### Resources

A _resource_ is anything that can later cause JavaScript to run, or that the trace should account
for. Each one has an ID (`id`), unique within its isolate. `0` means "none". Every resource has a
_kind_, which describes the mechanism, and a _name_, which says what it is:

| Kind         | What it is                                                                 | Names                                                                                                                                                                            |
| ------------ | -------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `request`    | The root of an incoming event.                                             | The event type: `fetch`, `connect`, `scheduled`, `alarm`, `queue`, `jsrpc`, `test`, `trace`, `tail_stream`, `hibernatable_websocket`, `restore`, `restore_rpc_stub`, `udp_connect` |
| `operation`  | A binding call or other named I/O.                                         | The span name (`fetch`, `kv_get`, `durable_object_storage_get`, `queue_send`, `websocket_open`, ...), or a named I/O wait (`stream_read`, `stream_write`, `stream_pipe`, `stream_close`, `socket_connect`, `socket_start_tls`, `socket_disconnect`, `datagram_receive`, `datagram_send`) |
| `timer`      | A timer.                                                                   | `setTimeout`, `setInterval`, `setImmediate`, `scheduler.wait`                                                                                                                    |
| `microtask`  | A `queueMicrotask()` callback.                                             | `queueMicrotask`                                                                                                                                                                 |
| `kj_to_js`   | Any other point where JavaScript waits on I/O (an `awaitIo()` bridge).     | `awaitIo`                                                                                                                                                                        |
| `js_to_kj`   | A point where the runtime waits on a JavaScript promise (`awaitJs()`).     | `awaitJs`                                                                                                                                                                        |
| `js_promise` | A JavaScript promise. Only with `--async-trace-promises`.                  | `Promise`                                                                                                                                                                        |
| `other`      | Anything else.                                                             |                                                                                                                                                                                  |

A resource has these events:

- `init` when it is created.
- `before` and `after` around each callback attributed to it. There can be any number of these
  pairs, since an interval fires many times and a request's handler runs in several turns. They
  nest, so a microtask runs inside the callback of whatever queued it.
- `settle` when the work behind it finishes, with outcome `ok`, `error` or `canceled`. A timer
  settles when it fires, an I/O wait when its result is ready, and a request when it is done. The
  callback that consumes the result usually runs just after.
- `destroy` if it is dropped without settling, such as a cleared timer.
- `annotate` for each key/value detail, such as a span tag. An operation's annotations include
  the request URL, method and response status.

### Turns and causes

A _turn_ is one entry into JavaScript from the event loop. workerd takes the isolate lock, runs
some JavaScript, drains the microtask queue, and releases the lock. Because the microtask queue is
empty when a turn starts, everything that runs during the turn, including every promise
continuation, follows from the event that started it. That event is the turn's _cause_: the timer
that fired, the I/O that completed, or else the request itself.

Attributing work to turns provides causal edges without hooking every promise. Each `turn` event
names its `cause`, and the cause's `before` and `after` bracket the turn.

### Triggers and execution

Every `init` carries two edges. `trigger` is what caused the resource to be created, by default
the cause of the current turn. `exec` is the resource whose callback was running when it was
created (Node's `executionAsyncId`); it differs from the trigger inside nested callbacks such as
microtasks.

Following `trigger` edges from any resource back to a `request` gives the chain of events that
led to it.

### Operations and adoption

Binding calls (`fetch`, KV, Durable Object storage, R2, ...) are made within a trace span. Each
span created for a traced context also creates an `operation` resource, named after the span and
annotated with the span's tags. The JavaScript side of the call then waits for the result through
an `awaitIo()` bridge. That bridge _adopts_ the operation instead of creating a generic `awaitIo`
resource, so when the result arrives, the turn's cause is `kv_get` or `fetch` rather than
`awaitIo`.

An `awaitIo()` bridge adopts the most recently created operation of the current turn that is
still pending. Operations that have settled, or that were explicitly detached, don't
count. If more than one was eligible, the trace counts the bridge in `ambiguousBindings`.

I/O that has no span, such as reads from KJ-backed streams or socket connects, is named at the
`awaitIo()` call instead (see [Instrumenting runtime code](#instrumenting-runtime-code)).

### Parents

An operation created within another operation's span names it as its `parent`. This edge
describes structure, not causation, and is separate from `trigger`. For example, a `jsRpcCall`
belongs to its RPC session's operation, while its trigger is the cause of the turn that made the
call. The parent may have settled already.

### Links between contexts

Resource IDs are per isolate, so a trigger can't point into another context. Instead, when a
request is delivered synchronously from another context's turn (a service binding or a `fetch()`
to the worker's own `SELF` binding), the trace records a `link` from the new request to the
latest operation the caller created in that turn (typically its `fetch`), or to the caller's
running resource if there is none. Requests delivered asynchronously, or from outside the
process, have no link.

### Creation stacks

With `--async-trace-stacks=<n>`, each resource records the JavaScript stack that created it.
Stacks are deduplicated per isolate. A `stack` event defines each one before its first use, and
`init` refers to it by ID. Resources created while no JavaScript is running have no stack.
Promises never record one, because there are too many. An isolate keeps every distinct stack for
its lifetime, up to 10,000 stacks or about 16 MiB; once it reaches either limit, resources created
at new sites have no stack.

### Promises

With `--async-trace-promises`, every promise created during a traced turn becomes a `js_promise`
resource. Its trigger is the promise it derives from (through `.then()` or `await`) when that
promise belongs to the same context, and otherwise the turn's cause. Its reactions run as
`before`/`after` callbacks, and it settles when it resolves or rejects. Promises are forgotten
when they settle and never get `destroy`.

With promises traced, the trace shows the chain from one `await` to the next inside a turn.
Without them, it shows only what started each turn.

## Compared with span tracing

Span tracing records which operations a request performed and how long each took. Async tracing
records how the request's JavaScript ran over time, and which event caused each part of it to run.

### Purpose and audience

Spans (`trace.h`, `SpanBuilder`, `TraceContext`, user spans) are a product observability feature.
They run in production, including in multi-tenant processes, and reach customers through tail
workers and the tracing export path. Span names and tags are part of what customers depend on.

Async tracing is for whoever runs workerd. Workers can't see it, nothing reaches customers, and
it is off unless configured. Its format is versioned for tools, not offered as a product API.

### What each one models

Spans form a tree of intervals. Each binding call gets a start, an end and tags, nested under the
request or under another span through span-context propagation. From spans you learn that a
request made a `kv_get` that took 40 ms, inside some outer span.

Async tracing forms a causal graph of resources and turns. It records when JavaScript ran
(`before`/`after`), which completion started each turn, what created each resource (`trigger`,
`exec`), and how long each turn waited for the isolate lock. From it you learn that the `kv_get`
completed, that its completion caused a turn that waited 1 ms for the lock and ran for 2 ms, and
that the turn started a `setTimeout` whose firing caused the next turn. Spans can't say what
resumed the handler or what scheduled a callback.

### Coverage

Spans exist only where runtime code creates them, mostly binding calls and subrequests. Async
tracing also covers timers, `queueMicrotask`, every `awaitIo()` and `awaitJs()` bridge, stream
and socket I/O, the turns themselves, and optionally every promise. It can also record the
JavaScript stack that created each resource.

### Causality

A span's parent means "this happened within that operation", and comes from whichever span is
current when the call is made. An async trace `trigger` means "this was caused by that event".
Async tracing keeps the structural relationship too, as the separate `parent` field.

### Timing

In production, user-facing spans use the runtime's Spectre-safe clock, which doesn't advance while
JavaScript runs, so they can't show how CPU time and waiting divide up within a request. Async
tracing uses a real monotonic clock with nanosecond resolution, which is acceptable for a local
tool.

### How they connect

Async tracing builds on spans rather than replacing them:

- Every `TraceContext` created for a traced context also creates an `operation` resource, named
  after the span, with the span's tags as annotations. This happens even when no tracer observes
  the span, which is the usual case in local workerd. `IoContext::makeUserTraceSpan()` records the
  operation with its own context's tracker. A child span made with
  `TraceContextParent::newChild()`, which cannot reach the context, uses the tracker of the
  innermost turn on the thread.
- The `awaitIo()` bridge that waits for the call adopts that operation, so the turn it resumes is
  attributed to `kv_get` rather than to a generic bridge.
- Span parents provide `parent` edges.

None of this changes the spans themselves: their names, tags and lifetimes are the same with or
without async tracing.

### Outputs and cost

Spans go to the tracer, and from there to tail workers and trace export. They are designed to be
on in production. Async tracing goes to the NDJSON file, Perfetto and the inspector. When it is
off, it costs a null check per instrumentation point, and its expensive tiers (creation stacks and
the promise hook) are opt-in. The promise hook is never installed in a multi-tenant process.

### Which to use

Use spans to see which calls a request made and how long each took, in production and visible
to customers. Use async tracing to see why a request was slow, what it was waiting on, and what
made each piece of JavaScript run, while debugging locally.

## Outputs

### The NDJSON trace (`--async-trace`)

The file holds one JSON object per line, and the field `e` names the event. Times (`at`, `start`,
`end`, ...) are integer nanoseconds since the process's trace epoch. The header's `epochUnixMs`
gives the wall-clock time of that epoch.

The first line is the header. Its `v` is the format's major version (currently 1), which changes
only for breaking changes. Consumers must ignore event types and fields they don't know.

Lines from different contexts are interleaved, but each line is whole. Within one context, lines
appear in time order.

| `e`        | Fields                                                                                   | Meaning                                                                                                                                                                           |
| ---------- | ---------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `header`   | `v`, `producer`, `version`, `epochUnixMs`, `pid`                                          | First line.                                                                                                                                                                       |
| `ctx`      | `ctx`, `iso`, `worker`, `actor`, `at`                                                     | A context began. `iso` identifies the isolate (resource IDs are unique per isolate); `actor` is the Durable Object ID, or `null`.                                                 |
| `stack`    | `iso`, `id`, `frames`                                                                     | Defines creation stack `id` of isolate `iso`, before its first use. Each frame has `fn`, `script`, `scriptId`, `line`, `col` (1-based), innermost first.                         |
| `init`     | `ctx`, `id`, `kind`, `name`, `trigger`, `exec`, `at`, optional `stack`, optional `parent` | A resource was created.                                                                                                                                                           |
| `annotate` | `ctx`, `id`, `k`, `v`                                                                     | A detail of a resource (e.g. a span tag).                                                                                                                                         |
| `before`   | `ctx`, `id`, `at`                                                                         | A callback attributed to `id` started.                                                                                                                                            |
| `after`    | `ctx`, `id`, `at`                                                                         | That callback ended.                                                                                                                                                              |
| `settle`   | `ctx`, `id`, `outcome`, `at`                                                              | The resource's work finished: `ok`, `error` or `canceled`.                                                                                                                        |
| `destroy`  | `ctx`, `id`, `at`                                                                         | The resource was dropped without settling.                                                                                                                                        |
| `turn`     | `ctx`, `cause`, `start`, optional `locked`, `end`                                         | A turn ended. `start` is when it was requested, `locked` when the isolate lock was held and JavaScript could run, `end` when it finished. `locked - start` is time spent waiting for the lock. |
| `link`     | `ctx`, `id`, `fromIso`, `fromCtx`, `fromId`                                               | Request `id` was delivered synchronously by resource `fromId` of context `fromCtx` (isolate `fromIso`).                                                                           |
| `ctx_end`  | `ctx`, `at`, `created`, `dropped`, `unknown`, `unbalanced`, `ambiguousBindings`, `unusedOperationNames`, `foreignThread` | The context ended, with its statistics (see [Completeness](#completeness)).                                                                                              |
| `exit`     | `at`, `open`                                                                              | Last line when workerd exits: the contexts in `open` never ended and have no `ctx_end`. Absent with `KJ_CLEAN_SHUTDOWN`, where every context ends normally. |

Sinks buffer events and write them to the file at the end of each outermost turn and when a
context ends. If workerd is killed by a signal, recent events may be missing, and there is no
`exit` line.

#### A worked example

This is a test that POSTs to its own `SELF` binding (some annotations omitted):

```text
{"e":"header","v":1,"producer":"workerd","version":"2026-10-08","epochUnixMs":1791512794842,"pid":815451}
{"e":"ctx","ctx":1,"iso":1,"worker":"g5","actor":null,"at":7694070}
{"e":"init","ctx":1,"id":1,"kind":"request","name":"test","trigger":0,"exec":0,"at":7770983}
{"e":"before","ctx":1,"id":1,"at":8544251}
{"e":"init","ctx":1,"id":2,"kind":"operation","name":"fetch","trigger":1,"exec":1,"at":8550574}
{"e":"annotate","ctx":1,"id":2,"k":"http.request.method","v":"POST"}
{"e":"annotate","ctx":1,"id":2,"k":"url.full","v":"http://x/"}
{"e":"after","ctx":1,"id":1,"at":8856546}
{"e":"turn","ctx":1,"cause":1,"start":7842917,"locked":7866725,"end":8856546}
{"e":"ctx","ctx":2,"iso":1,"worker":"g5","actor":null,"at":8591191}
{"e":"init","ctx":2,"id":3,"kind":"request","name":"fetch","trigger":0,"exec":0,"at":8697775}
{"e":"link","ctx":2,"id":3,"fromIso":1,"fromCtx":1,"fromId":2}
...
{"e":"settle","ctx":1,"id":2,"outcome":"ok","at":9529676}
{"e":"before","ctx":1,"id":2,"at":9555610}
{"e":"annotate","ctx":1,"id":2,"k":"http.response.status_code","v":"200"}
{"e":"init","ctx":1,"id":8,"kind":"kj_to_js","name":"awaitIo","trigger":2,"exec":2,"at":9642580}
{"e":"after","ctx":1,"id":2,"at":9655105}
{"e":"turn","ctx":1,"cause":2,"start":9547246,"locked":9553354,"end":9655105}
...
{"e":"ctx_end","ctx":1,"at":9936166,"created":5,"dropped":0,"unknown":0,"unbalanced":0,"ambiguousBindings":0,"unusedOperationNames":0,"foreignThread":0}
{"e":"exit","at":9984846,"open":[]}
```

In this trace:

1. The test handler (`request` 1) runs its first turn. During it, it calls `env.SELF.fetch()`,
   which creates `operation` 2, named `fetch`, triggered by the request and annotated with the
   method and URL.
2. The fetch is delivered synchronously to a new context (2) as `request` 3. The `link` records
   that it came from operation 2 of context 1.
3. When the response arrives, operation 2 settles, and the next turn of context 1 has `cause` 2,
   meaning the `fetch` resumed the handler. Reading the response body creates `awaitIo` 8, triggered by
   the fetch.
4. Both contexts end with all-zero statistics, and the file ends with `exit`.

#### Processing a trace

This script prints, for each turn, the context, the resource that caused it, and how long it
ran:

```python
import json, sys

events = [json.loads(line) for line in open(sys.argv[1])]
iso = {e["ctx"]: e["iso"] for e in events if e["e"] == "ctx"}
# Resource IDs are unique per isolate, not per process.
names = {(iso[e["ctx"]], e["id"]): e["name"] for e in events if e["e"] == "init"}
for e in events:
    if e["e"] == "turn":
        cause = names.get((iso[e["ctx"]], e["cause"]), "?")
        print(f"ctx {e['ctx']}: turn caused by {cause}, {(e['end'] - e['start']) / 1e6:.3f} ms")
```

Key resources by `(iso, id)`, not by `id` alone, because different isolates reuse the same IDs.

### Perfetto (`workerd.async`)

When the Perfetto session records the `workerd.async` category, each context appears as a track
named `<worker> ctx <n>` (with the actor ID for Durable Objects). Each turn is a `turn` slice on
that track, with a nested `lock` slice for the time spent acquiring the isolate lock.

Each resource has its own track under the context, named after the resource, with one slice from
`init` to `settle` (or `destroy`). The slice's args are `kind`, `id`, `trigger`, `exec`,
`parent`, `stack` (with `--async-trace-stacks`) and, at the end, `outcome`. Callbacks are `run`
slices on the resource's track, and annotations are instant events.

Flow arrows go from a resource's creation (a `create` instant on the creator's track) to its
slice, from its settlement to the callback that consumed it, and from a cross-context link
(`deliver`) to the request it delivered (`delivered`).

Only Perfetto sessions that are already recording when an isolate is created see that isolate's
contexts. `--perfetto-trace` starts at startup, so it sees all of them.

### Inspector

While a DevTools session is connected, each resource is reported to V8 as an async task. Scheduling the
task captures the current stack, and each callback runs as the task. DevTools then shows async stack
traces that continue across `setTimeout`, `queueMicrotask`, I/O and binding calls. V8 does this
only while a DevTools session has asked for async stacks (`Debugger.setAsyncCallStackDepth`).
`js_to_kj` resources are not reported, because they never run a callback. Neither are promises,
which V8 tracks itself, nor resources created while the isolate is not locked, such as requests.
The callbacks of a resource that was not reported do not run as tasks, so they don't hide the async
stack of a V8 task they run inside.

Whether a context reports to the inspector is decided when the context is created, so tracking
costs nothing while no DevTools session is connected. A context created before DevTools connects
is not reported: a Durable Object that was already running shows only promise async stacks (which
V8 tracks itself) until its context is replaced. A context created while DevTools was connected
keeps reporting after it disconnects. Only DevTools sessions count; a `node:inspector` session
does not.

## Completeness

The `ctx_end` statistics count events that the tracker could not attribute correctly. When they
are all zero, the context's trace is complete and consistent.

| Statistic           | Counts                                                                                                 |
| ------------------- | ------------------------------------------------------------------------------------------------------ |
| `created`           | Resources created (informational).                                                                     |
| `dropped`           | Resources not recorded because the context reached its cap of live resources (100,000).                |
| `unknown`           | Events naming a resource the tracker doesn't know.                                                     |
| `unbalanced`        | `after`s that didn't match the innermost `before`, and callbacks still open when a turn ended.          |
| `ambiguousBindings` | `awaitIo()` bridges that adopted an operation while more than one was eligible.                         |
| `unusedOperationNames` | `IoContext::AwaitIoOperation` scopes that ended normally without any bridge taking their name.      |
| `foreignThread`     | Calls from a thread other than the context's. These are dropped.                                        |

A nonzero value other than `created` usually means an instrumentation bug in the runtime, and is
worth reporting.

## Limitations

- Async tracing covers JavaScript activity and the I/O it waits for. It does not show what
  happens inside the runtime's own KJ code between those points.
- Without `--async-trace-promises`, causality is resolved per turn. Everything in a turn is
  attributed to the turn's cause, not to the specific `await` that ran it.
- Cross-context links exist only for synchronous in-process delivery.
- `parent` edges are recorded only where the runtime passes span parents explicitly. Some call
  sites pass them only when spans are observed by a tracer, so they are often absent in local
  workerd.
- Standard (JavaScript-backed) streams do no I/O of their own, so they have no `stream_*`
  operations. Their activity shows up as promises with `--async-trace-promises`.

## Architecture

The state and the NDJSON writer are implemented in Rust (`src/rust/async-trace/`). The C++ side is
a thin facade over them (`src/workerd/io/async-trace.{h,c++}`), together with instrumentation and
sinks that need KJ or V8.

```text
IsolateObserver ──getAsyncTraceConfig()──▶ Worker::Isolate ── AsyncTraceIsolate (IDs, stacks)
                ──addAsyncTraceSinks()──▶ IoContext ── AsyncTracker ──▶ sinks
                                                          │              ├─ NDJSON (Rust)
   instrumentation: turns, awaitIo/awaitJs, timers,       │              ├─ Perfetto
   microtasks, spans, requests, promise hook ─────────────┘              └─ Inspector
```

When a `Worker::Isolate` is created, `IsolateObserver::getAsyncTraceConfig()`
decides whether the isolate supports async tracing, and with which options
(`AsyncTraceConfig::stackDepth`, `promises`). A Perfetto session recording `workerd.async`, or an
inspector, also enables it. The isolate then owns an `AsyncTraceIsolate`, which allocates resource
IDs and deduplicates stacks.

Each `IoContext` asks `IsolateObserver::addAsyncTraceSinks()` for sinks, and adds the Perfetto
and Inspector sinks when those apply (the Inspector sink only while a DevTools session is
connected). If there is at least one sink, it creates an
`AsyncTracker`; otherwise `IoContext::tryGetAsyncTracker()` returns none. In workerd's server,
`--async-trace` makes the observer add the NDJSON sink. An embedder can implement the observer to
enable tracing for selected isolates and deliver events through its own `AsyncTraceListener`.

An `AsyncTracker` belongs to one context and is used only from that context's thread. It hands
out `AsyncResource` handles, tracks turns, callback scopes and pending operations, and passes
events to its sinks. When the context is destroyed, the tracker reports `ctx_end` and drops the
sinks. Handles that outlive it do nothing.

The NDJSON sink is implemented in Rust and serializes events without a C++ call per event. Each
context buffers its own lines, and one process-wide `AsyncTraceWriter` appends them to the file.
C++ sinks implement `AsyncTraceListener` (`src/rust/async-trace/listener.h`); the Perfetto and
Inspector sinks are in `async-trace-perfetto.c++` and `async-trace-inspector.c++`.

`async-trace-stacks.c++` captures creation stacks through V8, and `async-trace-promises.c++`
implements the promise hook. The hook is installed only at isolate creation, only when
configured, and never in multi-tenant processes.

## Instrumenting runtime code

Binding calls made through trace spans and waited on with `awaitIo()` are traced without extra
code. The cases below each need a line or two. All of the calls described here do nothing when the
context isn't traced.

### I/O without a span

Name the bridge by wrapping exactly one `awaitIo()` call in
`IoContext::AwaitIoOperation`. The first bridge created in the scope becomes an `operation` with
that name, and no other bridge adopts it:

```c++
IoContext::AwaitIoOperation traceOperation(ioContext, "stream_read"_kj);
return ioContext.awaitIo(js, kj::mv(promise));
```

If the scope ends normally without any bridge taking the name, the trace counts it in
`unusedOperationNames`.

### A span that outlives the call

A `TraceContext` whose span is stored somewhere (in a client,
a task, or a promise awaited some other way) instead of being attached to the promise passed to
the next `awaitIo()` must call `traceContext.detachAsync()`. Otherwise an unrelated `awaitIo()`
later in the turn could adopt its operation. `getSubrequestNoChecks()` does this for spans parked
on subrequest clients. If the caller's next `awaitIo()` awaits that very subrequest, pass
`SpanAwaitedNext::YES` to `getHttpClient()` or `getSubrequestChannel()` to keep the operation
adoptable, as queue sends and WebSocket opens do.

### Nested spans

Create child spans from the parent's `TraceContext::getSpanParents()`, so that
the child operation records its `parent`.

### A new event type

Call `IncomingRequest::delivered("<event type>"_kj)` so that the `request`
resource is named after the event. Without an argument, it is named after the calling function.

### A new kind of callback source

Create a resource with
`tracker.create(AsyncKind::..., name)` from `IoContext::tryGetAsyncTracker()` when the callback is
scheduled, keep the `AsyncResource` with the callback, and when it runs, either call
`resource.enterAsTurnCause()` (if it starts a turn) or open an `AsyncTracker::CallbackScope` (if
it runs nested inside one). Call `settle()` when the underlying work finishes. Destroying the
handle without settling reports `destroy`. Timers (`io-context.c++`) and `queueMicrotask`
(`global-scope.c++`) are examples.

## Tests

| Test                                                            | Covers                                                                                    |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| `//src/rust/async-trace:async-trace_test`                       | Tracker semantics, adoption rules, links, NDJSON format.                                  |
| `//src/workerd/io:async-trace-test@`                            | The C++ facade.                                                                           |
| `//src/workerd/io:async-trace-io-test@`                         | Instrumentation in `IoContext`: turns, timers, bridges, spans, adoption, named I/O.       |
| `//src/workerd/server/tests/async-trace:async-trace-test`       | End to end: runs `scenario.js` with `--async-trace` and checks the trace with `check.js`. |
| `//src/workerd/server/tests/async-trace:async-trace-perfetto-test` | The Perfetto output.                                                                   |
| `//src/workerd/server/tests/inspector:inspector-test`           | DevTools async stacks across timers and microtasks.                                       |

The assertions in `check.js` spell out what a trace of the scenario guarantees.
