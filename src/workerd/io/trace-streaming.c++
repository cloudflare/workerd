// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-streaming.h"

#include <workerd/util/uuid.h>

namespace workerd {

// ======================================================================================
// TailIDs
namespace {
// The UuidId implementation is really intended only for testing and local development.
// In production, it likely makes more sense to use a RayID or something that can be
// better correlated to other diagnostic and tracing mechanisms, and that can be better
// guaranteed to be sufficiently unique across the entire production environment.
class UuidIdFactory final: public StreamingTrace::IdFactory {
public:
  kj::String newTraceId() override {
    return randomUUID(kj::none);
  }
  kj::String newSpanId() override {
    return randomUUID(kj::none);
  }
};

UuidIdFactory uuidIdFactory;
}  // namespace

kj::Own<StreamingTrace::IdFactory> StreamingTrace::IdFactory::newUuidIdFactory() {
  return kj::Own<UuidIdFactory>(&uuidIdFactory, kj::NullDisposer::instance);
}

// ======================================================================================
// StreamingTrace

struct StreamingTrace::Impl {
  kj::String id;
  trace::Onset onsetInfo;
  StreamingTrace::Delegate delegate;
  const StreamingTrace::TimeProvider& timeProvider;
  uint32_t spanCounter = 0;
  uint32_t sequenceCounter = 0;
  StreamingTrace::IdFactory& idFactory;

  Impl(kj::String id,
      trace::Onset&& onset,
      StreamingTrace::Delegate delegate,
      const TimeProvider& timeProvider,
      StreamingTrace::IdFactory& idFactory)
      : id(kj::mv(id)),
        onsetInfo(kj::mv(onset)),
        delegate(kj::mv(delegate)),
        timeProvider(timeProvider),
        idFactory(idFactory) {}
};

kj::Own<StreamingTrace> StreamingTrace::create(IdFactory& idFactory,
    trace::Onset&& onset,
    Delegate delegate,
    const TimeProvider& timeProvider) {
  return kj::heap<StreamingTrace>(
      idFactory.newTraceId(), kj::mv(onset), kj::mv(delegate), timeProvider, idFactory);
}

StreamingTrace::StreamingTrace(kj::String id,
    trace::Onset&& onset,
    Delegate delegate,
    const TimeProvider& timeProvider,
    IdFactory& idFactory)
    : impl(kj::heap<StreamingTrace::Impl>(
          kj::mv(id), kj::mv(onset), kj::mv(delegate), timeProvider, idFactory)) {}

StreamingTrace::~StreamingTrace() noexcept(false) {
  for (auto& span: spans) {
    // If the streaming tracing is dropped without having an outcome explicitly
    // specified, the outcome is explicitly set to unknown.
    span.setOutcome(EventOutcome::UNKNOWN);
  }
  // Stage spans should be closed by calling setOutcome above
  KJ_ASSERT(spans.empty(), "all stage spans must be closed before the trace is destroyed");
}

kj::Own<StreamingTrace::Span> StreamingTrace::openRootSpan(trace::EventInfo&& eventInfo) {
  auto& i = *KJ_ASSERT_NONNULL(impl, "the streaming trace is closed");
  KJ_ASSERT(i.onsetInfo.info == kj::none, "the root span can only be opened once");
  i.onsetInfo.info = kj::mv(eventInfo);
  auto spanId = i.idFactory.newSpanId();
  StreamEvent event(kj::str(i.id), {.id = kj::str(spanId), .parent = kj::str(spanId)},
      i.timeProvider.getNow(), getNextSequence(), i.onsetInfo.clone());
  addStreamEvent(kj::mv(event));
  return kj::heap<StreamingTrace::Span>(spans, *this, kj::str(spanId), spanId);
}

uint32_t StreamingTrace::getNextSequence() {
  auto& i = KJ_ASSERT_NONNULL(impl, "the streaming trace is closed");
  return i->sequenceCounter++;
}

void StreamingTrace::addStreamEvent(StreamEvent&& event) {
  KJ_IF_SOME(i, impl) {
    i->delegate(kj::mv(event));
  }
}

kj::Maybe<kj::StringPtr> StreamingTrace::getId() const {
  KJ_IF_SOME(i, impl) {
    return i->id.asPtr();
  }
  return kj::none;
}

// ======================================================================================

struct StreamingTrace::Span::Impl {
  StreamingTrace::Span& self;
  kj::List<Span, &Span::link>& spans;
  StreamingTrace& trace;
  kj::String id;
  kj::String parentSpan;
  bool eventInfoSet = false;
  Impl(StreamingTrace::Span& self,
      kj::List<Span, &Span::link>& spans,
      StreamingTrace& trace,
      kj::String id,
      kj::StringPtr parentSpan)
      : self(self),
        spans(spans),
        trace(trace),
        id(kj::mv(id)),
        parentSpan(kj::str(parentSpan)) {
    spans.add(self);
  }
  KJ_DISALLOW_COPY_AND_MOVE(Impl);
  ~Impl() {
    KJ_ASSERT(self.link.isLinked());
    spans.remove(self);
  }

  StreamEvent makeStreamEvent(auto payload) const {
    auto tailId = KJ_ASSERT_NONNULL(trace.getId(), "the streaming trace is closed");
    auto& traceImpl = KJ_ASSERT_NONNULL(trace.impl);
    return StreamEvent(kj::str(tailId),
        StreamEvent::Span{
          .id = kj::str(id),
          .parent = kj::str(parentSpan),
        },
        traceImpl->timeProvider.getNow(), trace.getNextSequence(), kj::mv(payload));
  }
};

StreamingTrace::Span::Span(kj::List<Span, &Span::link>& spans,
    StreamingTrace& trace,
    kj::String id,
    kj::StringPtr parentSpan)
    : impl(kj::heap<Impl>(*this, spans, trace, kj::mv(id), parentSpan)) {
  KJ_DASSERT(this, link.isLinked());
}

void StreamingTrace::Span::setOutcome(
    EventOutcome outcome, kj::Maybe<trace::FetchResponseInfo> maybeInfo) {
  KJ_IF_SOME(i, impl) {  // Then close out the stream by destroying the impl
    for (auto& span: spans) {
      span.setOutcome(outcome);
    }
    KJ_ASSERT(spans.empty(), "all child spans must be closed before the parent span is closed");
    i->trace.addStreamEvent(i->makeStreamEvent(trace::SpanClose(outcome, kj::mv(maybeInfo))));
    impl = kj::none;
  }
}

StreamingTrace::Span::~Span() noexcept(false) {
  setOutcome(EventOutcome::UNKNOWN);
}

void StreamingTrace::Span::addLog(trace::LogV2&& log) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(log)));
  }
}

void StreamingTrace::Span::addException(trace::Exception&& exception) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(exception)));
  }
}

void StreamingTrace::Span::addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(event)));
  }
}

void StreamingTrace::Span::addMetrics(trace::Metrics&& metrics) {
  KJ_IF_SOME(i, impl) {
    i->trace.addStreamEvent(i->makeStreamEvent(kj::mv(metrics)));
  }
}

kj::Maybe<kj::Own<StreamingTrace::Span>> StreamingTrace::Span::addSubrequest(
    trace::Subrequest&& subrequest) {
  KJ_IF_SOME(i, impl) {
    auto span = kj::heap<Span>(
        spans, i->trace, KJ_ASSERT_NONNULL(i->trace.impl)->idFactory.newSpanId(), i->id);
    i->trace.addStreamEvent(KJ_ASSERT_NONNULL(span->impl)->makeStreamEvent(kj::mv(subrequest)));
    return kj::mv(span);
  }
  return kj::none;
}

kj::Maybe<kj::Own<StreamingTrace::Span>> StreamingTrace::Span::newChildSpan() {
  KJ_IF_SOME(i, impl) {
    return kj::heap<Span>(
        spans, i->trace, KJ_ASSERT_NONNULL(i->trace.impl)->idFactory.newSpanId(), i->id);
  }
  return kj::none;
}

// ======================================================================================
// StreamEvent

namespace {
StreamEvent::Span getSpan(const rpc::Trace::StreamEvent::Reader& reader) {
  auto span = reader.getSpan();
  return StreamEvent::Span{
    .id = kj::str(span.getId()),
    .parent = kj::str(span.getParent()),
  };
}

StreamEvent::Event getEvent(const rpc::Trace::StreamEvent::Reader& reader) {
  auto event = reader.getEvent();
  switch (event.which()) {
    case rpc::Trace::StreamEvent::Event::Which::ONSET: {
      return trace::Onset(event.getOnset());
    }
    case rpc::Trace::StreamEvent::Event::Which::SPAN_CLOSE: {
      return trace::SpanClose(event.getSpanClose());
    }
    case rpc::Trace::StreamEvent::Event::Which::LOG: {
      return trace::LogV2(event.getLog());
    }
    case rpc::Trace::StreamEvent::Event::Which::EXCEPTION: {
      return trace::Exception(event.getException());
    }
    case rpc::Trace::StreamEvent::Event::Which::DIAGNOSTIC_CHANNEL: {
      return trace::DiagnosticChannelEvent(event.getDiagnosticChannel());
    }
    case rpc::Trace::StreamEvent::Event::Which::METRICS: {
      auto metrics = event.getMetrics();
      kj::Vector<trace::Metric> vec(metrics.size());
      for (size_t i = 0; i < metrics.size(); i++) {
        trace::Metric metric(metrics[i]);
        vec.add(kj::mv(metric));
      }
      return vec.releaseAsArray();
    }
    case rpc::Trace::StreamEvent::Event::Which::SUBREQUEST: {
      return trace::Subrequest(event.getSubrequest());
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

StreamEvent::StreamEvent(
    kj::String id, Span&& span, kj::Date timestampNs, uint32_t sequence, Event&& event)
    : id(kj::mv(id)),
      span(kj::mv(span)),
      timestampNs(timestampNs),
      sequence(sequence),
      event(kj::mv(event)) {}

StreamEvent::StreamEvent(rpc::Trace::StreamEvent::Reader reader)
    : id(kj::str(reader.getId())),
      span(getSpan(reader)),
      timestampNs(reader.getTimestampNs() * kj::MILLISECONDS + kj::UNIX_EPOCH),
      sequence(reader.getSequence()),
      event(getEvent(reader)) {}

void StreamEvent::copyTo(rpc::Trace::StreamEvent::Builder builder) const {
  builder.setId(id);
  auto spanBuilder = builder.initSpan();
  spanBuilder.setId(span.id);
  spanBuilder.setParent(span.parent);
  builder.setTimestampNs((timestampNs - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  builder.setSequence(sequence);

  auto eventBuilder = builder.initEvent();
  KJ_SWITCH_ONEOF(event) {
    KJ_CASE_ONEOF(onset, trace::Onset) {
      onset.copyTo(eventBuilder.getOnset());
    }
    KJ_CASE_ONEOF(span, trace::SpanClose) {
      span.copyTo(eventBuilder.getSpanClose());
    }
    KJ_CASE_ONEOF(log, trace::LogV2) {
      log.copyTo(eventBuilder.getLog());
    }
    KJ_CASE_ONEOF(exception, trace::Exception) {
      exception.copyTo(eventBuilder.getException());
    }
    KJ_CASE_ONEOF(diagnosticChannelEvent, trace::DiagnosticChannelEvent) {
      diagnosticChannelEvent.copyTo(eventBuilder.getDiagnosticChannel());
    }
    KJ_CASE_ONEOF(metrics, trace::Metrics) {
      auto metricsBuilder = eventBuilder.initMetrics(metrics.size());
      for (size_t i = 0; i < metrics.size(); i++) {
        metrics[i].copyTo(metricsBuilder[i]);
      }
    }
    KJ_CASE_ONEOF(subrequest, trace::Subrequest) {
      subrequest.copyTo(eventBuilder.getSubrequest());
    }
  }
}

StreamEvent StreamEvent::clone() const {
  Span maybeNewSpan{
    .id = kj::str(span.id),
    .parent = kj::str(span.parent),
  };

  Event newEvent = ([&]() -> Event {
    KJ_SWITCH_ONEOF(event) {
      KJ_CASE_ONEOF(onset, trace::Onset) {
        return onset.clone();
      }
      KJ_CASE_ONEOF(span, trace::SpanClose) {
        return span.clone();
      }
      KJ_CASE_ONEOF(log, trace::LogV2) {
        return log.clone();
      }
      KJ_CASE_ONEOF(exception, trace::Exception) {
        return exception.clone();
      }
      KJ_CASE_ONEOF(diagnosticChannelEvent, trace::DiagnosticChannelEvent) {
        return diagnosticChannelEvent.clone();
      }
      KJ_CASE_ONEOF(metric, trace::Metrics) {
        kj::Vector<trace::Metric> newMetrics(metric.size());
        for (auto& m: metric) {
          newMetrics.add(m.clone());
        }
        return newMetrics.releaseAsArray();
      }
      KJ_CASE_ONEOF(subrequest, trace::Subrequest) {
        return subrequest.clone();
      }
    }
    KJ_UNREACHABLE;
  })();

  return StreamEvent(kj::str(id), kj::mv(maybeNewSpan), timestampNs, sequence, kj::mv(newEvent));
}

}  // namespace workerd
