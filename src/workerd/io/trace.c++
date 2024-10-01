// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>
#include <workerd/util/thread-scopes.h>

#include <kj/debug.h>
#include <kj/time.h>

#include <cstdlib>

namespace workerd {

SpanBuilder& SpanBuilder::operator=(SpanBuilder&& other) {
  end();
  observer = kj::mv(other.observer);
  span = kj::mv(other.span);
  return *this;
}

SpanBuilder::~SpanBuilder() noexcept(false) {
  end();
}

void SpanBuilder::end() {
  KJ_IF_SOME(o, observer) {
    KJ_IF_SOME(s, span) {
      s.endTime = kj::systemPreciseCalendarClock().now();
      o->report(s);
      span = kj::none;
    }
  }
}

void SpanBuilder::setOperationName(kj::ConstString operationName) {
  KJ_IF_SOME(s, span) {
    s.operationName = kj::mv(operationName);
  }
}

void SpanBuilder::setTag(kj::ConstString key, TagValue value) {
  KJ_IF_SOME(s, span) {
    auto keyPtr = key.asPtr();
    s.tags.upsert(
        kj::mv(key), kj::mv(value), [keyPtr](TagValue& existingValue, TagValue&& newValue) {
      // This is a programming error, but not a serious one. We could alternatively just emit
      // duplicate tags and leave the Jaeger UI in charge of warning about them.
      [[maybe_unused]] static auto logged = [keyPtr]() {
        KJ_LOG(WARNING, "overwriting previous tag", keyPtr);
        return true;
      }();
      existingValue = kj::mv(newValue);
    });
  }
}

void SpanBuilder::addLog(kj::Date timestamp, kj::ConstString key, TagValue value) {
  KJ_IF_SOME(s, span) {
    if (s.logs.size() >= Span::MAX_LOGS) {
      ++s.droppedLogs;
    } else {
      s.logs.add(Span::Log{.timestamp = timestamp,
        .tag = {
          .key = kj::mv(key),
          .value = kj::mv(value),
        }});
    }
  }
}

PipelineTracer::~PipelineTracer() noexcept(false) {
  KJ_IF_SOME(p, parentTracer) {
    for (auto& t: traces) {
      p->traces.add(kj::addRef(*t));
    }
  }
  KJ_IF_SOME(f, completeFulfiller) {
    f.get()->fulfill(traces.releaseAsArray());
  }
}

kj::Promise<kj::Array<kj::Own<Trace>>> PipelineTracer::onComplete() {
  KJ_REQUIRE(completeFulfiller == kj::none, "onComplete() can only be called once");

  auto paf = kj::newPromiseAndFulfiller<kj::Array<kj::Own<Trace>>>();
  completeFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

kj::Own<WorkerTracer> PipelineTracer::makeWorkerTracer(PipelineLogLevel pipelineLogLevel,
    ExecutionModel executionModel,
    kj::Maybe<kj::String> scriptId,
    kj::Maybe<kj::String> stableId,
    kj::Maybe<kj::String> scriptName,
    kj::Maybe<kj::Own<ScriptVersion::Reader>> scriptVersion,
    kj::Maybe<kj::String> dispatchNamespace,
    kj::Array<kj::String> scriptTags,
    kj::Maybe<kj::String> entrypoint) {
  auto trace = kj::refcounted<Trace>(trace::Onset(kj::none,  // TODO(now): Pass the account id
      kj::mv(stableId), kj::mv(scriptName), kj::mv(scriptVersion), kj::mv(dispatchNamespace),
      kj::mv(scriptId), kj::mv(scriptTags), kj::mv(entrypoint)), executionModel);
  traces.add(kj::addRef(*trace));
  return kj::refcounted<WorkerTracer>(kj::addRef(*this), kj::mv(trace), pipelineLogLevel);
}

void PipelineTracer::addTrace(rpc::Trace::Reader reader) {
  traces.add(kj::refcounted<Trace>(reader));
}

WorkerTracer::WorkerTracer(
    kj::Own<PipelineTracer> parentPipeline, kj::Own<Trace> trace, PipelineLogLevel pipelineLogLevel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::mv(trace)),
      parentPipeline(kj::mv(parentPipeline)),
      self(kj::refcounted<WeakRef<WorkerTracer>>(kj::Badge<WorkerTracer>{}, *this)) {}
WorkerTracer::WorkerTracer(PipelineLogLevel pipelineLogLevel, ExecutionModel executionModel)
    : pipelineLogLevel(pipelineLogLevel),
      trace(kj::refcounted<Trace>(trace::OnsetInfo {
        .executionModel = executionModel,
      })),
      self(kj::refcounted<WeakRef<WorkerTracer>>(kj::Badge<WorkerTracer>{}, *this)) {}

void WorkerTracer::log(kj::Date timestamp, LogLevel logLevel, kj::String message, bool isSpan) {
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  trace->addLog(trace::Log(timestamp, logLevel, kj::mv(message)), isSpan);
}

void WorkerTracer::addSpan(const trace::Span& span, kj::String spanContext) {
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for exceptions vs.
  //   logs.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  trace->addSpan(kj::mv(span), kj::mv(spanContext));
}

void WorkerTracer::addException(
    kj::Date timestamp, kj::String name, kj::String message, kj::Maybe<kj::String> stack) {
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for exceptions vs.
  //   logs.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  trace->addException(trace::Exception(timestamp, kj::mv(name), kj::mv(message), kj::mv(stack)));
}

void WorkerTracer::addDiagnosticChannelEvent(
    kj::Date timestamp, kj::String channel, kj::Array<kj::byte> message) {
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  trace->addDiagnosticChannelEvent(
      trace::DiagnosticChannelEvent(timestamp, kj::mv(channel), kj::mv(message)));
}

void WorkerTracer::setEventInfo(kj::Date timestamp, trace::EventInfo&& info) {
  // TODO(someday): For now, we're using logLevel == none as a hint to avoid doing anything
  //   expensive while tracing.  We may eventually want separate configuration for event info vs.
  //   logs.
  // TODO(perf): Find a way to allow caller to avoid the cost of generation if the info struct
  //   won't be used?
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }

  trace->setEventInfo(timestamp, kj::mv(info));
}

void WorkerTracer::setOutcomeInfo(trace::Outcome&& info) {
  trace->setOutcome(kj::mv(info));
}

void WorkerTracer::setFetchResponseInfo(trace::FetchResponseInfo&& info) {
  // Match the behavior of setEventInfo(). Any resolution of the TODO comments
  // in setEventInfo() that are related to this check while probably also affect
  // this function.
  if (pipelineLogLevel == PipelineLogLevel::NONE) {
    return;
  }
  trace->setFetchResponseInfo(kj::mv(info));
}

void WorkerTracer::extractTrace(rpc::Trace::Builder builder) {
  trace->copyTo(builder);
}

void WorkerTracer::setTrace(rpc::Trace::Reader reader) {
  trace->mergeFrom(reader, pipelineLogLevel);
}

ScopedDurationTagger::ScopedDurationTagger(
    SpanBuilder& span, kj::ConstString key, const kj::MonotonicClock& timer)
    : span(span),
      key(kj::mv(key)),
      timer(timer),
      startTime(timer.now()) {}

ScopedDurationTagger::~ScopedDurationTagger() noexcept(false) {
  auto duration = timer.now() - startTime;
  if (isPredictableModeForTest()) {
    duration = 0 * kj::NANOSECONDS;
  }
  span.setTag(kj::mv(key), duration / kj::NANOSECONDS);
}

}  // namespace workerd
