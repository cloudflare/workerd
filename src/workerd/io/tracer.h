// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>

#include <kj/refcount.h>

namespace workerd {
namespace tracing {
class TailStreamWriter;
}  // namespace tracing

class WorkerTracer;

// An abstract class that defines shares functionality for tracers that have different
// characteristics. This interface is used to submit both buffered and streaming tail events.
// TODO(streaming-tail): When further consolidating the tail worker implementations, the interface
// of the add* methods below should make more sense: The invocation span context below is currently
// only being used in the streaming model, when we have switched the buffered model to streaming
// there will be plenty of cleanup potential.
class BaseTracer: public kj::Refcounted {
 public:
  virtual ~BaseTracer() noexcept(false) {};

  // Adds log line to trace.  For Spectre, timestamp should only be as accurate as JS Date.now().
  virtual void addLog(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      LogLevel logLevel,
      kj::String message) = 0;
  // Add a span.
  virtual void addSpan(tracing::CompleteSpan&& span) = 0;

  virtual void addException(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) = 0;

  virtual void addDiagnosticChannelEvent(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String channel,
      kj::Array<kj::byte> message) = 0;

  // Adds info about the event that triggered the trace.  Must not be called more than once.
  virtual void setEventInfo(
      IoContext::IncomingRequest& incomingRequest, tracing::EventInfo&& info) = 0;

  // Sets the return event for Streaming Tail Worker, including fetchResponseInfo (HTTP status code)
  // if available. Must not be called more than once, and fetchResponseInfo should only be set for
  // fetch events. For buffered tail worker, there is no distinct return event so we only add
  // fetchResponseInfo to the trace if present.
  virtual void setReturn(kj::Maybe<kj::Date> time = kj::none,
      kj::Maybe<tracing::FetchResponseInfo> fetchResponseInfo = kj::none) = 0;

  // Reports the outcome event of the worker invocation. For Streaming Tail Worker, this will be the
  // final event, causing the stream to terminate.
  virtual void setOutcome(EventOutcome outcome,
      kj::Duration cpuTime,
      kj::Duration wallTime,
      kj::Maybe<uint64_t> responseBodySize = kj::none,
      kj::Maybe<uint64_t> requestBodySize = kj::none) = 0;

  virtual void setBodySizes(
      kj::Maybe<uint64_t> responseBodySize, kj::Maybe<uint64_t> requestBodySize) = 0;

  // Report time as seen from the incoming Request when the request is complete, since it will not
  // be available afterwards.
  virtual void recordTimestamp(kj::Date timestamp) = 0;

  SpanParent makeUserRequestSpan();

  using MakeUserRequestSpanFunc = kj::Function<SpanParent()>;

  // Allow setting the user request span after the tracer has been created so its observer can
  // reference the tracer. This can only be set once.
  void setMakeUserRequestSpanFunc(MakeUserRequestSpanFunc func);

  virtual void setJsRpcInfo(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      const kj::ConstString& methodName) = 0;

  // Mark this tracer as intentionally unused (e.g., for duplicate alarm requests).
  // When set, the destructor will not log a warning about missing Onset event.
  void markUnused() {
    markedUnused = true;
  }

 protected:
  // Retrieves the current timestamp. If the IoContext is no longer available, we assume that the
  // worker must have wrapped up and reported its outcome event, we report completeTime in that case
  // acordingly.
  kj::Date getTime();

  // helper method for addSpan() implementations
  void adjustSpanTime(tracing::CompleteSpan& span);

  // Function to create the root span for the new tracing format.
  kj::Maybe<MakeUserRequestSpanFunc> makeUserRequestSpanFunc;

  // Time to be reported for the outcome event time. This will be set before the outcome is
  // dispatched.
  kj::Date completeTime = kj::UNIX_EPOCH;

  // Weak reference to the IoContext, used to report span end time if available.
  kj::Maybe<kj::Own<IoContext::WeakRef>> weakIoContext;

  // When true, the destructor will not log a warning about missing Onset event.
  // Set via markUnused() when a tracer is intentionally not used (e.g., duplicate alarm requests).
  bool markedUnused = false;
};

// Records a worker stage's trace information into a Trace object.  When all references to the
// Tracer are released, its Trace is considered complete and ready for submission.
class WorkerTracer final: public BaseTracer {
 public:
  explicit WorkerTracer(kj::Maybe<kj::Rc<kj::Refcounted>> parentPipeline,
      kj::Own<Trace> trace,
      PipelineLogLevel pipelineLogLevel,
      kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter);
  virtual ~WorkerTracer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(WorkerTracer);

  // Returns a promise that fulfills when trace is complete. Only one such promise can
  // exist at a time. Used in workerd, where we don't have to worry about pipelines.
  kj::Promise<kj::Own<Trace>> onComplete();

  void addLog(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      LogLevel logLevel,
      kj::String message) override;
  void addSpan(tracing::CompleteSpan&& span) override;
  void addException(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override;
  void addDiagnosticChannelEvent(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String channel,
      kj::Array<kj::byte> message) override;
  // Set event info (equivalent to Onset event under streaming). We use the incomingRequest here
  // since the IoContext may not have the IncomingRequest linked to it yet (depending on if
  // delivered() has been set), so it might not be possible to acquire the required timestamp and
  // span context from it.
  void setEventInfo(
      IoContext::IncomingRequest& incomingRequest, tracing::EventInfo&& info) override;
  // Variant for when we don't have a proper IoContext but instead provide context and timestamp
  // directly, used internally for RPC-based tracing.
  void setEventInfoInternal(
      const tracing::InvocationSpanContext& context, kj::Date timestamp, tracing::EventInfo&& info);

  void setOutcome(EventOutcome outcome,
      kj::Duration cpuTime,
      kj::Duration wallTime,
      kj::Maybe<uint64_t> responseBodySize = kj::none,
      kj::Maybe<uint64_t> requestBodySize = kj::none) override;
  void setBodySizes(
      kj::Maybe<uint64_t> responseBodySize, kj::Maybe<uint64_t> requestBodySize) override;
  virtual void recordTimestamp(kj::Date timestamp) override;

  // Set a worker-level tag/attribute to be provided in the onset event.
  void setWorkerAttribute(kj::ConstString key, Span::TagValue value);

  void setReturn(kj::Maybe<kj::Date> time = kj::none,
      kj::Maybe<tracing::FetchResponseInfo> fetchResponseInfo = kj::none) override;

  void setJsRpcInfo(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      const kj::ConstString& methodName) override;

 private:
  PipelineLogLevel pipelineLogLevel;
  kj::Own<Trace> trace;
  // span attributes to be added to the onset event.
  kj::Vector<tracing::Attribute> attributes;

  // TODO(streaming-tail): Top-level invocation span context, used to add a placeholder span context
  // for trace events. This should no longer be needed after merging the existing span ID and
  // InvocationSpanContext interfaces.
  kj::Maybe<tracing::InvocationSpanContext> topLevelInvocationSpanContext;

  // own an instance of the pipeline to make sure it doesn't get destroyed
  // before we're finished tracing. kj::Refcounted serves as a fill-in here since the pipeline
  // tracer is not needed otherwise.
  kj::Maybe<kj::Rc<kj::Refcounted>> parentPipeline;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Own<Trace>>>> completeFulfiller;

  kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter;
};

class SpanSubmitter: public kj::Refcounted {
 public:
  virtual void submitSpan(tracing::SpanId context, tracing::SpanId spanId, const Span& span) = 0;
  virtual tracing::SpanId makeSpanId() = 0;
};

// The user tracing observer
class UserSpanObserver final: public SpanObserver {
 public:
  // constructor for top-level observer
  UserSpanObserver(kj::Own<SpanSubmitter> submitter)
      : submitter(kj::mv(submitter)),
        spanId(tracing::SpanId::nullId),
        parentSpanId(tracing::SpanId::nullId) {}
  // constructor for subsequent observers attached to a span
  UserSpanObserver(kj::Own<SpanSubmitter> submitter, tracing::SpanId parentSpanId)
      : submitter(kj::mv(submitter)),
        spanId(this->submitter->makeSpanId()),
        parentSpanId(parentSpanId) {}
  KJ_DISALLOW_COPY(UserSpanObserver);

  kj::Own<SpanObserver> newChild() override;
  void report(const Span& span) override;
  kj::Date getTime() override;

 private:
  kj::Own<SpanSubmitter> submitter;
  tracing::SpanId spanId;
  tracing::SpanId parentSpanId;
};

}  // namespace workerd
