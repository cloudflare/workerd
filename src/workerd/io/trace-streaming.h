// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "trace-common.h"

#include <kj/list.h>

namespace workerd {

// The Streaming Trace model is designed around the idea of spans. A span is a logical
// grouping of events. Spans can be nested, they have outcomes, and they can be transactional.
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
// If a span is transactional, then a final span outcome of canceled or exception should
// indicate that all events with the span should be considered invalid and be discarded.
// For instance, an open output gate should be traced as a transactional span. If the
// output gate fails then all events occuring while the gate was open become invalid.
//
// Example session:
//
//   Span 0 - Onset Event (fetch)
//   Span 1 - Log (in outgate gate)
//   Span 1 - Span Event (exception, output gate failed)
//   Span 0 - Log
//   Span 0 - Outcome Event (ok)
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
    uint32_t id = 0;
    uint32_t parent = 0;
    bool transactional = false;
  };
  // The span in which this event has occurred.
  Span span;

  kj::Date timestampNs;

  // All events in the stream are sequentially ordered, regardless of what span
  // they are in. This allows the exact sequence of events to be easily reconstructed
  // on the receiving end.
  uint32_t sequence;

  using Info = trace::EventInfo;
  using Detail = trace::EventDetail;
  using Event = kj::OneOf<trace::Onset, trace::Outcome, trace::Dropped, trace::SpanClose, Detail>;
  Event event;

  explicit StreamEvent(
      kj::String id, Span span, kj::Date timestampNs, uint32_t sequence, Event event);
  StreamEvent(rpc::Trace::StreamEvent::Reader reader);

  void copyTo(rpc::Trace::StreamEvent::Builder builder) const;
  StreamEvent clone() const;
};

// ======================================================================================
// StreamingTrace

class SpanBase: public trace::TraceBase {
public:
  virtual void addLog(trace::LogV2&& log) = 0;

  enum class Options {
    NONE,
    // If a span is transactional, an unsuccessful outcome may indicate that
    // all events within the span should be discarded.
    TRANSACTIONAL,
  };
};

class StreamingTrace final: public SpanBase {
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
    class Id {
    public:
      virtual kj::String toString() const = 0;
      virtual bool equals(const Id& other) const = 0;
      virtual kj::Own<Id> clone() const = 0;
    };

    virtual kj::Own<Id> newId() = 0;

    // An IdFactory implementation that generates Ids that are simply
    // random UUIDs. This should generally only be used in local development
    // or standalone uses of workerd.
    static kj::Own<IdFactory> newUuidIdFactory();
    static kj::Own<const IdFactory::Id> newIdFromString(kj::StringPtr str);
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
  explicit StreamingTrace(kj::Own<const IdFactory::Id> id,
      trace::Onset&& onset,
      Delegate delegate,
      const TimeProvider& timeProvider);
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
  // Setting the outcome on the StreamingTrace will implicitly close all active
  // spans.
  // spans and prevent any new spans from being opened.
  class Span final: public SpanBase {
  public:
    KJ_DISALLOW_COPY_AND_MOVE(Span);
    virtual ~Span() noexcept(false);

    void addLog(trace::LogV2&& log) override;
    void addException(trace::Exception&& exception) override;
    void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) override;
    void addMark(kj::StringPtr mark) override;
    void addMetrics(trace::Metrics&& metrics) override;
    void addSubrequest(trace::Subrequest&& subrequest) override;
    void addSubrequestOutcome(trace::SubrequestOutcome&& outcome) override;
    void addCustom(trace::Tags&& tags) override;
    kj::Maybe<kj::Own<Span>> newChildSpan(
        kj::Date timestamp, trace::Tags tags = nullptr, Options options = Options::NONE);

    // Setting the outcome of the span explicitly closes the span, after which
    // no further events can be emitted.
    void setOutcome(trace::SpanClose::Outcome outcome, trace::Tags tags = nullptr);

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
        uint32_t parentSpan = 0,
        trace::Tags tags = nullptr,
        Options options = Options::NONE);
  };

  // Set the EventInfo for the Onset event. If the Onset already has event
  // info, then this will assert.
  void setEventInfo(trace::EventInfo&& eventInfo);

  // Explicitly close the tail stream with the given outcome. All open stage
  // spans will be implicitly closed with the same outcome. If the outcome
  // has already been set, this will assert.
  void setOutcome(trace::Outcome&& outcome);

  // Notify the streaming trace that events in the sequence range (start:end) have been dropped.
  void addDropped(uint32_t start, uint32_t end);

  kj::Maybe<const IdFactory::Id&> getId() const;

  void addLog(trace::LogV2&& log) override;
  void addException(trace::Exception&& exception) override;
  void addDiagnosticChannelEvent(trace::DiagnosticChannelEvent&& event) override;
  void addMark(kj::StringPtr mark) override;
  void addMetrics(trace::Metrics&& metrics) override;
  void addSubrequest(trace::Subrequest&& subrequest) override;
  void addSubrequestOutcome(trace::SubrequestOutcome&& outcome) override;
  void addCustom(trace::Tags&& tags) override;
  kj::Maybe<kj::Own<Span>> newChildSpan(
      kj::Date timestamp, trace::Tags tags = nullptr, Options options = Options::NONE);

private:
  struct Impl;
  kj::Maybe<kj::Own<Impl>> impl;
  kj::List<Span, &Span::link> spans;

  uint32_t getNextSpanId();
  uint32_t getNextSequence();
  void addStreamEvent(StreamEvent&& event);

  friend class Span;
  friend class StageSpan;
  friend struct Span::Impl;
};

constexpr static StreamingTrace::SpanBase::Options operator|(
    const StreamingTrace::SpanBase::Options& a, const StreamingTrace::SpanBase::Options& b) {
  return static_cast<StreamingTrace::SpanBase::Options>(static_cast<int>(a) | static_cast<int>(b));
}

constexpr static StreamingTrace::SpanBase::Options operator&(
    const StreamingTrace::SpanBase::Options& a, const StreamingTrace::SpanBase::Options& b) {
  return static_cast<StreamingTrace::SpanBase::Options>(static_cast<int>(a) & static_cast<int>(b));
}

}  // namespace workerd
