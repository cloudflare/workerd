# Experimental OpenTelemetry tail metadata

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
