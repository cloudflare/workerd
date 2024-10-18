// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-common.h"

#include <kj/list.h>

namespace workerd {

// The Streaming Trace model is designed around the idea of spans. A span is a logical
// grouping of events. Spans can be nested and have outcomes.
// All events always occur within the context of a span.
//
// The streaming trace itself is considered the root span, whose span ID is always 0. The
// root span will always start with an Onset event that communicates basic metadata about
// the worker being traced (for instance, script ID, script version, etc) and the triggering
// event. The streaming trace always ends with an Outcome event that communicates the final
// disposition of the traced worker.
//
// Stage spans can have any number of child spans (and those spans can have child spans of
// their own).
//
// Every span always ends with a Span event that identifies the outcome of that span (which
// can be unknown, ok, canceled, or exception).
//
// Setting the outcome of a span will implicitly close all child spans with the same outcome
// if those are not already closed. If a span is dropped without setting the outcome, and the
// streaming trace is still alive, the span will be implicitly canceled.
//
// Currently the StreamingTrace implementation is not thread-safe. It is expected that
// the StreamingTrace and all Spans are used by a single-thread.

// ======================================================================================
// StreamEvent

// All events on the streaming trace are StreamEvents. A StreamEvent is essentialy
// just an envelope for the actual event data.
struct StreamEvent final {
  // The ID of the streaming trace session. This is used to correlate all events
  // occurring within the same trace session.
  kj::String id;

  struct Span {
    kj::String id;
    kj::String parent;
  };
  // The span in which this event has occurred.
  Span span;

  kj::Date timestampNs;

  // All events in the stream are sequentially ordered, regardless of what span
  // they are in. This allows the exact sequence of events to be easily reconstructed
  // on the receiving end.
  uint32_t sequence;

  using Event = kj::OneOf<trace::Onset,
      trace::Dropped,
      trace::SpanClose,
      trace::LogV2,
      trace::Exception,
      trace::DiagnosticChannelEvent,
      trace::Metrics,
      trace::Subrequest>;
  Event event;

  explicit StreamEvent(
      kj::String id, Span&& span, kj::Date timestampNs, uint32_t sequence, Event&& event);
  StreamEvent(rpc::Trace::StreamEvent::Reader reader);

  void copyTo(rpc::Trace::StreamEvent::Builder builder) const;
  StreamEvent clone() const;
};

// ======================================================================================
// StreamingTrace

class StreamingTrace final {
public:
  // A StreamingTrace Id provides the unique identifier for a streaming tail session.
  // It is used as a correlation key for all events in a single tail stream.
  // There need to be some reasonable guarantees of uniqueness at a fairly
  // large scale but does not necessarily need to be globally unique. Tail
  // workers that are receiving and aggregating tails from multiple workers
  // and metals across many colos need to have some reasonable assurance
  // that there are unlikely to see collisions. The requirements for generating
  // reasonably unique IDs in workerd will be different than generating the
  // same in a production environment so we abstract the details and allow
  // different implementations to be used.
  //
  // Applications should generally treat Ids as opaque strings. Every StreamEvent
  // within a single tail stream will share the same Id
  class IdFactory {
  public:
    virtual kj::String newTraceId() = 0;
    virtual kj::String newSpanId() = 0;

    // An IdFactory implementation that generates Ids that are simply
    // random UUIDs. This should generally only be used in local development
    // or standalone uses of workerd.
    static kj::Own<IdFactory> newUuidIdFactory();
  };

  // The delegate is the piece that actually handles the output of the stream events
  using Delegate = kj::Function<void(StreamEvent&&)>;

  // The timeProvider is a function that returns the current time. This is used
  // to abstract exactly how the trace gets current time.
  struct TimeProvider {
    virtual kj::Date getNow() const = 0;
  };

  static kj::Own<StreamingTrace> create(IdFactory& idFactory,
      trace::Onset&& onset,
      Delegate delegate,
      const TimeProvider& timeProvider);

  // The constructor is public only to support use of kj::heap. It is not intended
  // to be used directly. Use the static create(...) method instead.
  explicit StreamingTrace(kj::String id,
      trace::Onset&& onset,
      Delegate delegate,
      const TimeProvider& timeProvider,
      IdFactory& idFactory);
  ~StreamingTrace() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(StreamingTrace);

  // Spans represent logical groupings of events within a tail stream. For instance,
  // a span might represent a single stage in a pipeline, or nested subgroupings of
  // events within a stage.
  //
  // Calling setOutcome(...) on the span will cause the span to be explicitly
  // closed with a Span event emitted to the tail stream indicating the outcome.
  // If the span is dropped without setting the outcome, and the StreamingTrace
  // is still active, then a Span event indicating that the span was canceled
  // will be emitted. If the StreamingTrace is not active, then dropping the
  // span becomes a non-op and the consumer of the stream will need to infer
  // the outcome from the absence of a Span event.
  //
  // Unrelated spans are permitted to overlap in time but dropping or setting
  // the outcome of a parent span will implicitly close all active child spans.
  //
  // Setting the outcome on the StreamingTrace's root span will implicitly close
  // all active child spans and prevent any new spans from being opened.
  class Span final {
  public:
    KJ_DISALLOW_COPY_AND_MOVE(Span);
    virtual ~Span() noexcept(false);

    void addLog(trace::LogV2&& log);
    void addException(trace::Exception&& exception);
    void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event);
    void addMetrics(trace::Metrics&& metrics);
    void addSubrequest(trace::Subrequest&& subrequest);
    kj::Maybe<kj::Own<Span>> newChildSpan();

    // Setting the outcome of the span explicitly closes the span, after which
    // no further events can be emitted in this span (any other calls will
    // be silently ignored)
    void setOutcome(EventOutcome outcome, kj::Maybe<trace::FetchResponseInfo> info = kj::none);

  private:
    struct Impl;

    // Keep the link and spans before the impl so that they are created and destroyed
    // in the correct order.
    kj::ListLink<Span> link;
    kj::List<Span, &Span::link> spans;
    kj::Maybe<kj::Own<Impl>> impl;

    friend class StageSpan;
    friend class StreamingTrace;

  public:
    // Public only to support use of kj::heap. Not intended to be called directly.
    Span(kj::List<Span, &Span::link>& spans,
        StreamingTrace& trace,
        kj::String id,
        kj::StringPtr parent);
  };

  // Notify the streaming trace that events in the sequence range (start:end) have been dropped.
  void addDropped(uint32_t start, uint32_t end);

  // Opens the root span associated with this streaming trace.
  // This can only be called once.
  kj::Own<Span> openRootSpan(trace::EventInfo&& eventInfo);

  kj::Maybe<kj::StringPtr> getId() const;

private:
  struct Impl;
  kj::Maybe<kj::Own<Impl>> impl;
  kj::List<Span, &Span::link> spans;

  uint32_t getNextSequence();
  void addStreamEvent(StreamEvent&& event);

  friend class Span;
  friend class StageSpan;
  friend struct Span::Impl;
};

}  // namespace workerd
