# Experimental HTTP trace context

With `opentelemetry_api`, the standalone HTTP listener accepts valid W3C `traceparent`
and optional `tracestate` headers. A worker invocation joins the remote trace only when
native dispatch has not already supplied a user trace parent. Invalid parents are
ignored; invalid vendor state is discarded independently. Explicit sampling flags,
including an unsampled decision and reserved flag bits, are retained in native identities.
HTTP egress uses W3C Level 2 semantics: only the sampled and random bits (`flags & 0x03`)
are emitted. The same mask applies to JS-visible native contexts and shim injection;
native identities retain their original bits. The runtime does not set the random bit itself.

Global HTTP `fetch()` injects the built-in fetch span's identity and the invocation's
JavaScript-visible sampling decision. Without a recording fetch span, a valid active
JavaScript identity is passed through. An existing `traceparent` matching the invocation's
incoming trigger identity and flags is treated as forwarded context: `fetch(request)` and
`fetch(new Request(request))` replace it with the fetch identity. A different user-supplied
header, even an invalid one, wins. A `tracestate` equal to the adopted state is also treated
as forwarded: automatic rewrites replace it with the active context's state (including JS
updates), or drop it if the new context has none. This avoids attaching an old trace's state
to a different trace. A different explicit application state still wins.
Multiple vendor-state fields are comma-joined before comparison, just as
at HTTP adoption. If adoption discarded invalid or oversized state, there is no canonical
state to recognise as forwarded: the application-supplied fields remain unchanged, including
on a cross-trace rewrite. Injection edits only
the native outgoing headers, not the JavaScript Request. Redirect hops get their own fetch
identities, including cross-origin hops. OTel `suppressTracing()` disables native user spans
and automatic injection within that async scope; it does not remove explicit headers.

Without a tracer, an invocation with an adopted or native trigger retains a propagation-only
root visible to JS and egress, using the trigger's span ID rather than an unexported local
span. An invocation without either a trigger or a tracer does not invent a new identity,
including owner-less handlers such as hibernatable WebSocket callbacks. Clearing an explicit
context in such a handler cannot reveal an invented invocation parent. Explicit `00`
invocations retain propagation identities without
recording shim or native spans, even under a foreign explicitly sampled parent. Native
activation retains the foreign identity with the owning invocation's unsampled flags.

Binding fetchers, actors, and JS RPC use native trigger propagation rather than automatic
HTTP injection. This conservative channel-based exclusion also covers bindings configured
to an external HTTP service; those can use explicit OTel propagation. A global outbound
configured to another worker still uses its native trigger as the authority, not HTTP
adoption. Vendor state is retained in memory across HTTP and local fetch dispatch, but
is not added to the internal RPC, hibernation, or tail-event wire schemas.

Forwarded-header detection is local to one HTTP adoption boundary. In a chain such as
HTTP client → worker A → service binding or Durable Object B → origin, B's native trigger
is A's subrequest span, not the original client header. B's pass-through `fetch(request)`
therefore does not recognise that header as forwarded and leaves it unchanged. A fresh
fetch uses native propagation instead. This prototype does not rewrite headers on binding
or Durable Object stub egress; those channels remain excluded from automatic injection.

## Trust and replay

Any client of a standalone HTTP socket can choose the trace ID and sampling flags. `00`
can suppress user span recording; `01` can request it. Valid vendor state (up to 512 bytes)
is carried to origins, including third parties and cross-origin redirect destinations.
Empty vendor-state members are ignored; malformed non-empty members discard the state.
The JS extractor is intentionally more permissive: it drops malformed entries individually
and retains at most 32 valid members, while HTTP adoption rejects the whole state for an
invalid member or more than 32 non-empty members.
Neither trace IDs nor vendor state are authorization or account-boundary signals. Socket
adoption currently has no separate trust-boundary opt-in beyond `opentelemetry_api`.

Same-trace JS parents inherit the invocation's native sampling decision, not a cached JS
projection from a non-exporting caller. Span creation decides under the explicit parent,
even inside an unrelated unsampled surrounding context, so it cannot create a recording
carrier under a JS-only non-recording identity. A projected identity saved in module or object
memory and replayed under a different invocation trace is indistinguishable from a remote
parent with explicit flags. Applications must not treat such replay as proof of origin,
and deployments must choose their own incoming trust and retention policies.

## Single writer and embedder integration

This user trace context is separate from an embedder's internal tracing and its own
trace-context export. `IoChannelFactory::shouldInjectW3cTraceContext()` defaults to `false`
in the base class. Standalone workerd's `RustIoChannelFactory` and the test fixture explicitly
return `true`. An embedder that already writes W3C headers outside the isolate should leave
the capability off for all affected request channel factories when enabling the OTel flag.
That disables automatic insertion and forwarded-parent rewriting, while leaving
explicit application headers, native span creation, and native subrequest metadata intact.
The embedder's existing exporter remains the sole automatic writer and retains authority
over its own forwarding opt-outs, sampling checks, and account-boundary policies. JS can
still activate a foreign context with explicit sampled flags and thereby shape the native
identity supplied to that exporter, except that an explicitly unsampled owning invocation
keeps native work propagation-only. This capability is not a trust-policy boundary.
Conversely, JS may see projected `01` with a streaming sink while an exporter requiring an
explicit native sampled decision emits nothing. Standalone development (writer on) and an
embedded deployment (writer off) therefore need not have identical propagation behaviour.
No embedder is modified
by this prototype; adopting the capability is an integration prerequisite, not an automatic
production rollout. Coordinating explicit headers with that exporter is still its responsibility.

The legacy `BaseTracer::MakeUserRequestSpanFunc` and `setMakeUserRequestSpanFunc()` retain
their `(TraceId, Maybe<TraceFlags>)` source interface through an adapter. That adapter creates
invocation roots without vendor state and declines contexts with parent span IDs. Such parents
remain propagation-only carriers with their full identity rather than becoming recording
roots with missing parent IDs. Callers wishing to record these parents and retain state use
`setMakeUserRequestSpanContextFunc()` with a
`SpanContext` callback. The new channel-factory virtual still requires rebuilding embedders;
this is source compatibility, not binary ABI compatibility.

Disabling `opentelemetry_api`
leaves HTTP adoption and injection disabled and does not synthesize native sampling flags.

The stable `TracingSpanContext` type includes optional `traceState?: string` because it
describes the native Span API shared by both compatibility modes, not only the OTel adapter.
The additive optional field documents a value the flagged runtime can return; it is omitted
at runtime when absent or when the flag is off. Its presence in the type is not a promise
of vendor-state support in the existing RPC or tail schemas.

## Open questions

1. Who owns production W3C injection: the isolate or the embedder's existing exporter? The
   capability makes a single-writer choice possible; each deployment must explicitly choose.
2. Should standalone sockets adopt client context by default or require an `HttpOptions`
   opt-in? Should untrusted vendor state be dropped, and may client `00` suppress recording?
3. Should forwarded headers participate (replace the parent ID) or pass through? Does the
   platform ingress adopt the end user's context, and can fresh and pass-through fetches
   otherwise emit different traces?
4. Should a non-exporting invocation emit `00`, nothing, or `01`? Parent-based downstream
   samplers normally drop a trace continued with `00`.
5. Is it intentional that explicit upstream `00` suppresses built-in user spans with the flag
   on, while flag-off behavior can record them with flags zero?
6. What origin allowlists, cross-origin redirect policies, and per-fetch opt-outs are needed?
7. Should the final API use W3C Level 1 or Level 2 flag semantics? This prototype uses Level 2.
8. Should untraced flagged workers forward context from both adoption and native triggers?
   This prototype forwards both without recording and keeps the trigger's parent span ID;
   absent native flags project to `00`, even when the caller has a streaming sink.
9. Is vendor-state loss across processes, hibernation, and tail events acceptable, or should
   versioned schemas support it, including OTLP `trace_state` export?
10. Is adoption reachable through Wrangler/Miniflare? It runs only on the standalone listener;
    an intermediary entry worker may keep it from reaching the user worker.
