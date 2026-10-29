# Experimental HTTP trace context

With `opentelemetry_api`, the standalone HTTP listener accepts valid W3C `traceparent`
and optional `tracestate` headers. A worker invocation joins the remote trace only when
native dispatch has not already supplied a user trace parent. Invalid parents are
ignored; invalid vendor state is discarded independently. Explicit sampling flags,
including an unsampled decision and reserved flag bits, are retained in native identities.
HTTP egress uses W3C Level 2 semantics: only the sampled and random bits (`flags & 0x03`)
are emitted. The runtime does not set the random bit itself.

Global HTTP `fetch()` injects the built-in fetch span's identity and the invocation's
JavaScript-visible sampling decision. Without a recording fetch span, a valid active
JavaScript identity is passed through. An existing `traceparent` matching the invocation's
incoming trigger identity and flags is treated as forwarded context: `fetch(request)` and
`fetch(new Request(request))` replace it with the fetch identity. A different user-supplied
header, even an invalid one, wins. Existing `tracestate` is preserved. Injection edits only
the native outgoing headers, not the JavaScript Request. Redirect hops get their own fetch
identities, including cross-origin hops. OTel `suppressTracing()` disables native user spans
and automatic injection within that async scope; it does not remove explicit headers.

Without a tracer, an invocation with an adopted or native trigger retains a propagation-only
root visible to JS and egress. An invocation without either a trigger or a tracer does not
invent a new identity. Explicit `00` invocations retain propagation identities without
recording user or built-in user spans.

Binding fetchers, actors, and JS RPC use native trigger propagation rather than automatic
HTTP injection. This conservative channel-based exclusion also covers bindings configured
to an external HTTP service; those can use explicit OTel propagation. A global outbound
configured to another worker still uses its native trigger as the authority, not HTTP
adoption. Vendor state is retained in memory across HTTP and local fetch dispatch, but
is not added to the internal RPC, hibernation, or tail-event wire schemas.

## Trust and replay

Any client of a standalone HTTP socket can choose the trace ID and sampling flags. `00`
can suppress user span recording; `01` can request it. Valid vendor state (up to 512 bytes)
is carried to origins, including third parties and cross-origin redirect destinations.
Empty vendor-state members are ignored; malformed non-empty members discard the state.
Neither trace IDs nor vendor state are authorization or account-boundary signals. Socket
adoption currently has no separate trust-boundary opt-in beyond `opentelemetry_api`.

Same-trace JS parents inherit the invocation's native sampling decision, not a cached JS
projection from a non-exporting caller. A projected identity saved in module or object
memory and replayed under a different invocation trace is indistinguishable from a remote
parent with explicit flags. Applications must not treat such replay as proof of origin,
and deployments must choose their own incoming trust and retention policies.

## Single writer and embedder integration

This user trace context is separate from an embedder's internal tracing and its own
trace-context export. `IoChannelFactory::shouldInjectW3cTraceContext()` defaults to `true`
for standalone workerd. An embedder that already writes W3C headers outside the isolate
must override it to `false` for all affected request channel factories before enabling the
OTel flag. That disables automatic insertion and forwarded-parent rewriting, while leaving
explicit application headers, native span creation, and native subrequest metadata intact.
The embedder's existing exporter remains the sole automatic writer and retains authority
over forwarding opt-outs, sampling, and account-boundary policies. No embedder is modified
by this prototype; adopting the capability is an integration prerequisite, not an automatic
production rollout. Coordinating explicit headers with that exporter is still its responsibility.

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
   This prototype forwards both without recording.
9. Is vendor-state loss across processes, hibernation, and tail events acceptable, or should
   versioned schemas support it, including OTLP `trace_state` export?
10. Is adoption reachable through Wrangler/Miniflare? It runs only on the standalone listener;
    an intermediary entry worker may keep it from reaching the user worker.
