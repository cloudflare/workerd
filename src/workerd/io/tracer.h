// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/trace.h>

#include <kj/refcount.h>

namespace workerd {
namespace tracing {

// A utility class that receives tracing events and generates/reports TailEvents.
class TailStreamWriter final: public kj::Refcounted {
 public:
  // If the Reporter returns false, then the writer should transition into a
  // closed state.
  using Reporter = kj::Function<bool(TailEvent&&)>;

  TailStreamWriter(Reporter reporter);
  KJ_DISALLOW_COPY_AND_MOVE(TailStreamWriter);

  // TODO(streaming-tail): Provide proper time stamps in all cases, getting rid of UNIX_EPOCH here.
  void report(const InvocationSpanContext& context,
      TailEvent::Event&& event,
      kj::Date time = kj::UNIX_EPOCH);

  inline bool isClosed() const {
    return state == kj::none;
  }

 private:
  struct State {
    Reporter reporter;
    uint32_t sequence = 0;
    State(Reporter reporter): reporter(kj::mv(reporter)) {}
  };
  kj::Maybe<State> state;
  bool onsetSeen = false;
  bool outcomeSeen = false;
};
}  // namespace tracing

class WorkerTracer;

// A tracer which records traces for a set of stages. All traces for a pipeline's stages and
// possible subpipeline stages are recorded here, where they can be used to call a pipeline's
// trace worker.
class PipelineTracer: public kj::Refcounted, public kj::EnableAddRefToThis<PipelineTracer> {
 public:
  // Creates a pipeline tracer (with a possible parent).
  explicit PipelineTracer() = default;
  virtual ~PipelineTracer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(PipelineTracer);

  // Returns a promise that fulfills when traces are complete.  Only one such promise can
  // exist at a time.
  kj::Promise<kj::Array<kj::Own<Trace>>> onComplete();

  // Makes a tracer for a worker stage.
  kj::Own<WorkerTracer> makeWorkerTracer(PipelineLogLevel pipelineLogLevel,
      ExecutionModel executionModel,
      kj::Maybe<kj::String> scriptId,
      kj::Maybe<kj::String> stableId,
      kj::Maybe<kj::String> scriptName,
      kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
      kj::Maybe<kj::String> dispatchNamespace,
      kj::Array<kj::String> scriptTags,
      kj::Maybe<kj::String> entrypoint,
      kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter);

  // Adds a trace from the contents of `reader` this is used in sharded workers to send traces back
  // to the host where tracing was initiated.
  void addTrace(rpc::Trace::Reader reader);

  // When collecting traces from multiple stages in a pipeline, this is called by the
  // tracer for a subordinate stage to add its collected traces to the parent pipeline.
  void addTracesFromChild(kj::ArrayPtr<kj::Own<Trace>> traces);

  void addTailStreamWriter(kj::Own<tracing::TailStreamWriter>&& writer);

 private:
  kj::Vector<kj::Own<Trace>> traces;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Array<kj::Own<Trace>>>>> completeFulfiller;
  kj::Vector<kj::Own<tracing::TailStreamWriter>> tailStreamWriters;

  friend class WorkerTracer;
};

// An abstract class that defines shares functionality for tracers that have different
// characteristics. This interface is used to submit both legacy and streaming tail events.
// TODO(streaming-tail): When further consolidating the tail worker implementations, the interface
// of the add* methods below should make more sense: The invocation span context below is currently
// only being used in the streaming model, when we have switched the legacy model to streaming
// there will be plenty of cleanup potential.
class BaseTracer: public kj::Refcounted {
 public:
  // Adds log line to trace.  For Spectre, timestamp should only be as accurate as JS Date.now().
  virtual void addLog(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      LogLevel logLevel,
      kj::String message) = 0;
  // Add a span. There can be at most MAX_USER_SPANS spans in a trace.
  virtual void addSpan(CompleteSpan&& span) = 0;

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
  virtual void setEventInfo(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      tracing::EventInfo&& info) = 0;

  // Adds info about the response. Must not be called more than once, and only
  // after passing a FetchEventInfo to setEventInfo().
  virtual void setFetchResponseInfo(tracing::FetchResponseInfo&& info) = 0;

  // Reports the outcome event of the worker invocation. For Streaming Tail Worker, this will be the
  // final event, causing the stream to terminate.
  virtual void setOutcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime) = 0;

  virtual SpanParent getUserRequestSpan() = 0;
};

// Records a worker stage's trace information into a Trace object.  When all references to the
// Tracer are released, its Trace is considered complete and ready for submission.
class WorkerTracer: public BaseTracer {
 public:
  explicit WorkerTracer(kj::Rc<PipelineTracer> parentPipeline,
      kj::Own<Trace> trace,
      PipelineLogLevel pipelineLogLevel,
      kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter);
  explicit WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel);
  virtual ~WorkerTracer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(WorkerTracer);

  void addLog(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      LogLevel logLevel,
      kj::String message) override;
  void addSpan(CompleteSpan&& span) override;
  void addException(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String name,
      kj::String message,
      kj::Maybe<kj::String> stack) override;
  void addDiagnosticChannelEvent(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      kj::String channel,
      kj::Array<kj::byte> message) override;
  void setEventInfo(const tracing::InvocationSpanContext& context,
      kj::Date timestamp,
      tracing::EventInfo&& info) override;
  void setFetchResponseInfo(tracing::FetchResponseInfo&& info) override;
  void setOutcome(EventOutcome outcome, kj::Duration cpuTime, kj::Duration wallTime) override;
  SpanParent getUserRequestSpan() override;
  // Allow setting the user request span after the tracer has been created so its observer can
  // reference the tracer. This can only be set once.
  void setUserRequestSpan(SpanBuilder&& span);

 private:
  PipelineLogLevel pipelineLogLevel;
  kj::Own<Trace> trace;
  // The root span for the new tracing format.
  SpanBuilder userRequestSpan;

  // TODO(streaming-tail): Top-level invocation span context, used to add a placeholder span context
  // for trace events. This should no longer be needed after merging the existing span ID and
  // InvocationSpanContext interfaces.
  kj::Maybe<tracing::InvocationSpanContext> topLevelInvocationSpanContext;

  // own an instance of the pipeline to make sure it doesn't get destroyed
  // before we're finished tracing
  kj::Maybe<kj::Rc<PipelineTracer>> parentPipeline;

  kj::Maybe<kj::Own<tracing::TailStreamWriter>> maybeTailStreamWriter;
};
}  // namespace workerd
