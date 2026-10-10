// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Export of user spans to an OTLP/HTTP collector, independent of any worker tracer.
// startExportedUserSpan() opens the span whose descendants are exported together, typically an
// invocation's (see BaseTracer::setStartInvocationSpanFunc()). OtlpSpanSubmitter is the SpanSubmitter that
// turns those spans into encoded ExportTraceServiceRequests; how spans map onto OTLP and how they
// are batched is decided in Rust (src/rust/otlp). An OtlpSpanExporter says where the requests go,
// and postOtlpTraces() is the POST an exporter makes.

#include <workerd/io/script-version.capnp.h>
#include <workerd/io/tracer.h>
#include <workerd/rust/otlp/bridge.rs.h>
#include <workerd/util/strong-bool.h>

#include <kj/compat/http.h>

namespace workerd {

// Whether the query is cut from `url.full` and `url.query` left out.
WD_STRONG_BOOL(RedactQueryString);

// Where an OtlpSpanSubmitter's spans go, and which spans it takes. Refcounted because the
// submitters of one invocation share it and may outlive the tracer.
class OtlpSpanExporter: public kj::Refcounted {
 public:
  // `request` is an encoded ExportTraceServiceRequest holding `spanCount` spans. Failures are the
  // exporter's to handle: this is called while spans close, including from the submitter's
  // destructor.
  virtual void exportSpans(kj::Array<const kj::byte> request, uint spanCount) = 0;

  // Spans the submitter refused because too many were open, or the id was already open.
  virtual void spansDropped(uint count) {}

  // Whether a span opened by the runtime, or by user code, is exported under this name. Neither
  // is asked about the invocation's root span, which is always exported.
  virtual bool exportsRuntimeSpan(const kj::ConstString& operationName) {
    return true;
  }
  virtual bool exportsUserSpan(const kj::ConstString& operationName) {
    return true;
  }
};

// POSTs one request to `url`, a collector's `/v1/traces`, as `application/x-protobuf` along with
// `headers`. Throws unless the collector answers 2xx. `client` and `url` must outlive the promise.
kj::Promise<void> postOtlpTraces(kj::HttpClient& client,
    kj::StringPtr url,
    kj::HttpHeaders headers,
    kj::Array<const kj::byte> request);

// What exported spans belong to: `service.name`, followed in the OTLP resource by the rest under
// the names the streaming tail worker's OTel conversion gives them. The entrypoint and Durable
// Object id are also set on every span.
struct OtlpServiceInfo {
  kj::StringPtr serviceName;
  kj::Maybe<kj::StringPtr> scriptName;
  kj::Maybe<ScriptVersion::Reader> scriptVersion;
  kj::Maybe<kj::StringPtr> dispatchNamespace;
  kj::ArrayPtr<const kj::String> scriptTags = nullptr;
  kj::Maybe<kj::StringPtr> entrypoint;
  kj::Maybe<kj::StringPtr> durableObjectId;
  kj::Maybe<const tracing::TracePreview&> preview;
  kj::Maybe<kj::StringPtr> colo;
  kj::Maybe<kj::StringPtr> region;
};

// The OTLP resource attributes that follow `service.name`, and the tags set on every span.
kj::Array<Span::Tag> makeOtlpResource(const OtlpServiceInfo& service);
kj::Array<Span::Tag> makeOtlpIdentityTags(const OtlpServiceInfo& service);

// The user tracing submitter that exports one span tree through an OtlpSpanExporter. It serves
// the whole tree, because UserSpanObserver::newChild() shares the submitter. A span is held from
// its open to its close; closed spans are batched and exported when the root span closes or the
// batch grows large. Spans that close after the root (waitUntil, Durable Object handoff) are
// exported as they close, because the submitter's destruction, which exports whatever is left,
// may wait for a garbage collection. It holds no tracer or request state, so stale SpanParents
// held in async context storage are safe.
//
// At close each span gets URL attributes derived from `url.full`, the default tags, and
// `cloudflare.invocation.sequence.number`, counted in open order so the root is 1. `fetch` spans
// to a binding's internal host are not exported. Spans still open when the submitter is destroyed
// are closed then and flagged with `cloudflare.warning.type` of `span_not_ended`.
class OtlpSpanSubmitter final: public SpanSubmitter {
 public:
  struct Options {
    // The OTLP resource: `service.name` followed by `resource`.
    kj::StringPtr serviceName;
    kj::ArrayPtr<const Span::Tag> resource = nullptr;
    // Applied to every span at close without overriding the span's own tags.
    kj::ArrayPtr<const Span::Tag> defaultTags = nullptr;
    RedactQueryString redactQueryString = RedactQueryString::NO;
    // In predictable mode span ids count up from 1 with this as their top byte, so that tests can
    // tell apart the spans of submitters that share a trace.
    uint8_t predictableSpanSpace = 0;
  };

  // `context`'s trace id and flags are shared by every span. The root span is `rootSpanId`, or
  // takes the first span id if that is null; see getRootSpanId(). `options` is only read during
  // construction.
  OtlpSpanSubmitter(kj::EntropySource& entropySource,
      kj::Own<OtlpSpanExporter> exporter,
      const tracing::SpanContext& context,
      Options options,
      tracing::SpanId rootSpanId = tracing::SpanId::nullId);
  ~OtlpSpanSubmitter() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(OtlpSpanSubmitter);

  bool submitSpanOpen(tracing::SpanId spanId,
      tracing::SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override;
  bool submitUserSpanOpen(tracing::SpanId spanId,
      tracing::SpanId parentSpanId,
      kj::ConstString operationName,
      kj::Date startTime) override;
  // `startTime` is unused: the span keeps the one it was opened with.
  void submitSpanClose(
      tracing::SpanId spanId, kj::Date startTime, kj::Date endTime, Span::TagMap&& tags) override;
  void submitSpanUpdate(tracing::SpanId spanId, tracing::SpanUpdate&& update) override;
  void submitSpanException(tracing::SpanId spanId,
      kj::Date timestamp,
      kj::Maybe<tracing::Exception::Code> code,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override;

  tracing::SpanId makeSpanId() override;
  tracing::SpanId getRootSpanId() const {
    return rootSpanId;
  }

 private:
  bool open(tracing::SpanId spanId,
      tracing::SpanId parentSpanId,
      kj::StringPtr operationName,
      kj::Date startTime);
  void exportBatch(rust::otlp::Batch batch);

  kj::EntropySource& entropySource;
  uint64_t predictableSpanId;
  kj::Own<OtlpSpanExporter> exporter;
  tracing::SpanId rootSpanId;
  ::rust::Box<rust::otlp::SpanBuffer> buffer;
};

// Opens a span named `operationName` whose spans, its own and its descendants', are exported
// through `exporter` by an OtlpSpanSubmitter with `options`. It continues `context`'s trace with
// its sampling decision, under `context`'s span if it has one. `spanId` is the span's own when it
// already has one, as an invocation's does (see BaseTracer::setStartInvocationSpanFunc()).
// `entropySource` must outlive every one of those spans.
SpanBuilder startExportedUserSpan(kj::EntropySource& entropySource,
    kj::Own<OtlpSpanExporter> exporter,
    const tracing::SpanContext& context,
    const OtlpSpanSubmitter::Options& options,
    kj::ConstString operationName,
    tracing::SpanId spanId = tracing::SpanId::nullId);

}  // namespace workerd
