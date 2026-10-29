# Experimental HTTP trace context

With `opentelemetry_api`, the standalone HTTP listener accepts valid W3C `traceparent`
and optional `tracestate` headers. A worker invocation joins the remote trace only when
native dispatch has not already supplied a user trace parent. Invalid parents are
ignored; invalid vendor state is discarded independently. Explicit sampling flags,
including an unsampled decision and reserved flag bits, are retained.

Global HTTP `fetch()` injects the built-in fetch span's identity and the invocation's
JavaScript-visible sampling decision. Without a recording fetch span, a valid active
JavaScript identity is passed through. An existing `traceparent`, even an invalid one,
is never overwritten; neither is existing `tracestate`. Injection edits only the native
outgoing headers, not the JavaScript Request.

Binding fetchers, actors, and JS RPC use native trigger propagation rather than automatic
HTTP injection. This conservative channel-based exclusion also covers bindings configured
to an external HTTP service; those can use explicit OTel propagation. A global outbound
configured to another worker still uses its native trigger as the authority, not HTTP
adoption. Vendor state is retained in memory across HTTP and local fetch dispatch, but
is not added to the internal RPC, hibernation, or tail-event wire schemas.

Enabling propagation sends user trace IDs and vendor state to origins, including third
parties and redirect destinations, just as standard OTel auto-instrumentation does. It
is not an authorization mechanism. Origin allowlists, suppression controls, and the
deployment policy for trusting incoming headers remain product decisions.

This user trace context is separate from an embedder's internal tracing and its own
trace-context export. The embedder remains responsible for translating platform trigger
context and preventing duplicate user-header injection. Disabling `opentelemetry_api`
leaves HTTP adoption and injection disabled and does not synthesize native sampling flags.
