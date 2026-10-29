# Experimental OpenTelemetry tail metadata

## Detached roots (multi-trace invocations)

The adapter creates detached roots for `root: true` or a parent context without a span
(`trace.deleteSpan`, `ROOT_CONTEXT`). A foreign remote parent without `root: true` remains an
ordinary child; an invalid all-zero parent does not mint a trace. Native/public tracing APIs
never detach on their own, including runtime spans under bare ROOT_CONTEXT. Native descendants
of an active adapter root follow that root's trace, as do logs, marks, HTTP propagation and
service-binding/RPC triggers. Invocation-scoped events retain onset identity.

The adapter's internal empty context is an invocation fallback, not an explicit request to
detach. Actor constructors, constructor-started concurrency blocks, hibernatable WebSocket
handlers and restore callbacks install request trace scopes; ordinary spans there attach to
the event's invocation just as in fetch handlers.

Roots inherit the owning invocation's native sampling and flags, not the active remote context's.
Unsampled roots have fresh non-recording identities with JS flags 0 and emit no spans. Native
carriers retain the invocation's flags, including reserved bits and absence, even when a root
identity is rewrapped after activation or closure in the same invocation.
Untraced roots remain INVALID and suppress native spans and propagation while the current frame
is also untraced. Reusing an INVALID root in a traced request does not suppress that request.
Recording roots generate native identities once, with empty state
and isRemote false. The runtime derives `spanOpen.root: true` only when a parentless open has a
registered trace different from onset. Only detached-aware factories enable new trace creation.
Legacy factories use attached roots, so JS identity, logs, egress and tail stay in the invocation's
trace. Missing registration at an opted-in factory is still a backend defect: attachment of the
tail alone cannot repair inconsistent JS identities or propagation.
All later events retain their span's trace. Group by invocationId across traces, not traceId.

Production must deploy root-aware converters before producers and add a temporary autogate in
addition to the experimental flag (the PoC intentionally has no autogate). Consumers must accept
only marked parentless opens, enforce the identity and lifetime contract in the schema/types,
and close all spans at outcome.
No embedder changes are included here. Embedders must adopt the detached-aware span factory,
register every placeholder's trace/flags (including foreign-parent spans), forward metadata and
`SpanOpenData.trace @5` through every process transport, and register it before opening the span.
No-parent activation reuses the invocation's existing carrier for every factory type. Keep shared
predictable counters across distinct parent carriers and forward metadata-aware opens. Consumers
must also use the triggering event's trace for issue attribution. Invocation navigation, billing, root quotas,
platform links and parentless RPC method enrichment remain product decisions.

Minted IDs are self-identifying: a random 64-bit nonce followed by SipHash-2-4's 64-bit tag of
the nonce's big-endian bytes. BoringSSL supplies SipHash; each tracer lazily draws its 128-bit
key from its invocation entropy source (or the runtime's cryptographic source). A zero nonce
is replaced with 1 so the W3C trace ID is always non-zero and 32 hex digits. Membership recomputes
the tag, even after closure, without retaining IDs or allocating per root. Only the key is
stored: constant memory, no eviction and no external-memory accounting. The former growing
registry could exhaust the isolate's memory budget during long-lived requests; this removes
that root-count-dependent isolate-kill failure mode, including for unsampled roots.
A foreign ID has a 2^-64 false-match probability under an unknown random key. A false match
only applies the invocation's native flags or rejects that trace as a WebSocket trigger; this
is not an authentication boundary. Nonce collisions have ordinary 64-bit birthday probability.
Predictable test mode uses a fixed SipHash key and the existing process-wide deterministic
trace-ID counter for nonces, consumes no entropy, and remains distinct across invocations.

The `opentelemetry_api` adapter emits optional kind, instrumentation scope and start time on
`spanOpen`, separate `spanEvent` and `spanLinks` marks, optional exception time, and optional end
time on `spanClose`. Native spans and flag-off output keep their existing schema. Runtime
children do not inherit adapter metadata. Only adapter-created spans accept explicit end times;
the invocation's lifetime remains platform-controlled.

Application times are lossless `[Unix seconds, nanoseconds]` tuples in `[0, 2^63 ns)`. Invalid
times fall back to observation time. Fractional/non-normalized HrTime is normalized; numeric
inputs use the OpenTelemetry SDK time-origin comparison. An end before the effective start is
clamped to the start. Outer timestamps and sequence numbers always describe observation, not
application time.

Scope name and version are capped at 256 UTF-8 bytes, schema URL at 2,048, and event names at 64,
without splitting UTF-8 characters. Each span retains at most 128 events and 128 links, with at
most 128 attributes per event/link. Creation links and each `addLinks` call form a single mark.
Streaming retains the first events rather than evicting already-emitted events as the buffered
SDK does. Attributes, exceptions, events and links still share the 64 KiB span budget; separate
budgets and dropped counts on close are deferred. Invalid link identities are still dropped,
including links with attributes: retaining invalid identities requires a separate schema decision.

## Embedder integration

Override `SpanObserver::setOpenMetadata`, `setEndTimestamp`, `onEvent`, `onLinks` and
`onExceptionWithTimestamp`. `UserSpanObserver` gates all marks on acceptance, forwarding them
to `SpanSubmitter::submitUserSpanOpenWithMetadata`, `submitSpanCloseWithTimestamp`,
`submitSpanEvent`, `submitSpanLinks` and `submitSpanExceptionWithTimestamp`. Submitters must use
the creator's weak tracer (`runIfAlive`), never the current request's tracer. Invocation marks
likewise use the stored invocation context and tracer. A span reused across actor requests
continues writing to its creator's stream and cannot keep a dead tracer alive.

With the flag on, logs, diagnostics and exceptions reported while the frame is entered follow
the frame owner's tracer.
Long-lived work started by a Durable Object constructor (timers, listeners or background loops)
therefore loses its logs and spans once the constructing request's tracer expires; an unsampled
constructing request cannot record spans in the first place. There is no fallback to the newest
request: that would put frame-owned logs and spans in different streams, or misattribute them to
an unrelated overlapping request. Uncaught timer/callback task failures are an exception: the
frame has unwound before task-failure reporting, so those exceptions still go to the newest
request's tracer and invocation, possibly a different stream than the callback's logs. This
limitation is unchanged with the flag off. Start request-specific work in that request's scope
instead.

A root retained in Durable Object memory is not a cross-request recording session. Once its
creator's tracer expires, later children, marks and closure can be dropped; no later stream
receives that root's close. A hibernatable WebSocket captures a real active accept-time identity,
unless its trace was minted as a detached root by the invocation's tracer. In that case, or
without a real active identity, it uses the invocation identity when one exists. Later
message/close/error invocations inherit neither sampling flags nor vendor state. Accepting
inside a detached root does not pull later events into that root's trace.
There is no dedicated per-invocation root-count quota. Recording spans share the existing export
budgets; self-identifying minted IDs use constant per-tracer memory regardless of root count.

Forward corresponding `BaseTracer` methods and all metadata/time fields over each transport,
including process-sandbox boundaries. The default hooks intentionally preserve old embedders:
unsupported marks/metadata are ignored, timed exceptions become ordinary exceptions, and
metadata-aware open/close fall back to ordinary open/close. Enabling the flag requires overriding
the entire forwarding path, including predictable-mode observation timestamps.

## Converter and rollout

Ship unknown-union-safe readers at least a release ahead of producers and update converters
before enabling the flag. Converters must accept both new mark types, require a span ID on all
marks, and look up the span before awaiting in concurrent handlers. Kind is set at open (unknown
values use existing name rules); user spans default to INTERNAL. Export scope via
`instrumentationScope`, with schema URL on `ScopeSpans`; empty version may equal missing. Scope
is user-controlled and is not an origin/authentication signal.

Pass explicit tuples through as HrTime without duplicate-timestamp adjustment. Wrap serialized
link tracestate in a TraceState object before OTLP export; validate ID lengths and resolve links
only within the account. Cap events per span/invocation and expose dropped counts. Clamp export
end to `max(explicitEnd ?? observed, start)` and reject implausible times. Partition, index and
expire using observation time only. Explicit exception time supplies the event and log timestamp,
while the tail timestamp supplies observedTimestamp; `recordException` does not imply unhandled.

The hand-written `types/defines/trace.d.ts` is concatenated unconditionally into every generated
type entrypoint. Experimental-only filtering would require generator support rather than a
runtime compat check, so the additions remain in stable snapshots for now. Consumers should
allow unknown event kinds during rollout rather than rely on exhaustive tail-union switches.
