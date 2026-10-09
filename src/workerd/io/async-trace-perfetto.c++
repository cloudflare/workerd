// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace-perfetto.h"

#include <workerd/util/use-perfetto-categories.h>

#include <kj/map.h>
#include <kj/vector.h>

#include <string>

namespace workerd {

#ifdef WORKERD_USE_PERFETTO

namespace {

// Track and flow IDs are hashes, so that IDs from different isolates and contexts, and from other
// producers in the same trace, don't collide (unlike small integers).
uint64_t mix(uint64_t a, uint64_t b, uint64_t salt) {
  // splitmix64 finalizer over a combination of the inputs.
  uint64_t x = a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull) ^ salt;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

constexpr uint64_t CONTEXT_TRACK = 0x6374785f74726b31ull;
constexpr uint64_t RESOURCE_TRACK = 0x7265735f74726b31ull;
constexpr uint64_t CREATE_FLOW = 0x6372655f666c6f31ull;
constexpr uint64_t SETTLE_FLOW = 0x73746c5f666c6f31ull;
constexpr uint64_t LINK_FLOW = 0x6c6e6b5f666c6f31ull;

const char* kindName(AsyncKind kind) {
  switch (kind) {
    case AsyncKind::REQUEST:
      return "request";
    case AsyncKind::KJ_TO_JS:
      return "kj_to_js";
    case AsyncKind::JS_TO_KJ:
      return "js_to_kj";
    case AsyncKind::TIMER:
      return "timer";
    case AsyncKind::MICROTASK:
      return "microtask";
    case AsyncKind::OPERATION:
      return "operation";
    case AsyncKind::JS_PROMISE:
      return "js_promise";
    case AsyncKind::OTHER:
      return "other";
  }
  return "other";
}

const char* outcomeName(AsyncOutcome outcome) {
  switch (outcome) {
    case AsyncOutcome::OK:
      return "ok";
    case AsyncOutcome::ERROR:
      return "error";
    case AsyncOutcome::CANCELED:
      return "canceled";
  }
  return "error";
}

std::string toStd(kj::ArrayPtr<const char> text) {
  return std::string(text.begin(), text.size());
}

class PerfettoSink final: public AsyncTraceListener {
 public:
  PerfettoSink(kj::StringPtr worker, kj::Maybe<kj::StringPtr> actor)
      : label(actor.map([&](kj::StringPtr id) { return kj::str(worker, " (", id, ")"); })
                  .orDefault([&]() { return kj::str(worker); })) {}

  ~PerfettoSink() noexcept(false) {
    // Normally erased by onContextEnd(); this covers a tracker destroyed without closing.
    if (contextOpen) traces::TrackEvent::EraseTrackDescriptor(contextTrack());
  }

  // The tracker calls this first, when the sink is added.
  void onContextBegin(uint64_t ctx, uint64_t isolateId) override {
    isolate = isolateId;
    contextId = mix(ctx, isolateId, CONTEXT_TRACK);
    auto track = contextTrack();
    auto desc = track.Serialize();
    desc.set_name(kj::str(label, " ctx ", ctx).cStr());
    traces::TrackEvent::SetTrackDescriptor(track, desc);
    contextOpen = true;
  }

  void onInit(uint64_t ctx, const AsyncInitEvent& event) override {
    auto track = resourceTrack(event.id);
    auto desc = track.Serialize();
    desc.set_name(toStd(event.name));
    traces::TrackEvent::SetTrackDescriptor(track, desc);

    auto args = [&](perfetto::EventContext ctx) {
      ctx.AddDebugAnnotation("kind", kindName(event.kind));
      ctx.AddDebugAnnotation("id", event.id);
      ctx.AddDebugAnnotation("trigger", event.trigger);
      ctx.AddDebugAnnotation("exec", event.execution);
      if (event.stack != 0) {
        KJ_IF_SOME(text, stacks.find(event.stack)) {
          ctx.AddDebugAnnotation("stack", std::string(text.cStr(), text.size()));
        }
      }
    };
    if (event.execution != 0) {
      auto flow = mix(isolate, event.id, CREATE_FLOW);
      TRACE_EVENT_INSTANT("workerd.async", "create", resourceTrack(event.execution),
          perfetto::Flow::ProcessScoped(flow));
      TRACE_EVENT_BEGIN("workerd.async",
          perfetto::DynamicString(event.name.begin(), event.name.size()), track,
          perfetto::TerminatingFlow::ProcessScoped(flow), args);
    } else {
      TRACE_EVENT_BEGIN("workerd.async",
          perfetto::DynamicString(event.name.begin(), event.name.size()), track, args);
    }
  }

  void onLink(
      uint64_t ctx, uint64_t id, uint64_t fromIsolate, uint64_t fromCtx, uint64_t fromId) override {
    // Track IDs are derived from IDs alone, so the other context's tracks can be named here.
    perfetto::Track fromContext(mix(fromCtx, fromIsolate, CONTEXT_TRACK));
    perfetto::Track from(mix(fromIsolate, fromId, RESOURCE_TRACK), fromContext);
    auto flow = mix(mix(fromIsolate, fromId, LINK_FLOW), mix(isolate, id, LINK_FLOW), LINK_FLOW);
    TRACE_EVENT_INSTANT("workerd.async", "deliver", from, perfetto::Flow::ProcessScoped(flow));
    TRACE_EVENT_INSTANT("workerd.async", "delivered", resourceTrack(id),
        perfetto::TerminatingFlow::ProcessScoped(flow), "from_ctx", fromCtx, "from_id", fromId);
  }

  void onStack(
      uint64_t isolateId, uint32_t id, kj::ArrayPtr<const AsyncStackFrame> frames) override {
    // One line per frame, innermost first, as in a JavaScript stack trace. Bounded by the number
    // of distinct creation sites the context uses.
    kj::Vector<kj::String> lines(frames.size());
    for (auto& frame: frames) {
      auto function = frame.function.size() == 0 ? "<anonymous>"_kj.asArray() : frame.function;
      lines.add(kj::str(function, " (", frame.script, ":", frame.line, ":", frame.column, ")"));
    }
    stacks.upsert(id, kj::strArray(lines, "\n"));
  }

  void onSettle(uint64_t ctx, AsyncId id, AsyncOutcome outcome, uint64_t atNs) override {
    auto track = resourceTrack(id);
    TRACE_EVENT_END("workerd.async", track, "outcome", outcomeName(outcome),
        perfetto::Flow::ProcessScoped(mix(isolate, id, SETTLE_FLOW)));
    traces::TrackEvent::EraseTrackDescriptor(track);
  }

  void onDestroy(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    auto track = resourceTrack(id);
    TRACE_EVENT_END("workerd.async", track, "outcome", "destroyed");
    traces::TrackEvent::EraseTrackDescriptor(track);
  }

  void onBefore(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    // Every callback terminates the settle flow; only the first after settling has one to end.
    TRACE_EVENT_BEGIN("workerd.async", "run", resourceTrack(id),
        perfetto::TerminatingFlow::ProcessScoped(mix(isolate, id, SETTLE_FLOW)));
  }

  void onAfter(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    TRACE_EVENT_END("workerd.async", resourceTrack(id));
  }

  void onAnnotate(uint64_t ctx,
      AsyncId id,
      kj::ArrayPtr<const char> key,
      kj::ArrayPtr<const char> value) override {
    TRACE_EVENT_INSTANT("workerd.async", perfetto::DynamicString(key.begin(), key.size()),
        resourceTrack(id), "value", toStd(value));
  }

  void onTurn(uint64_t ctx, const AsyncTurn& turn) override {
    {
      auto track = contextTrack();
      // Turns are reported when they end, with times on the tracker's clock. Map them onto the
      // trace clock through the end, which is now.
      int64_t offset = static_cast<int64_t>(traces::TrackEvent::GetTraceTimeNs()) -
          static_cast<int64_t>(turn.endNs);
      auto at = [offset](uint64_t ns) {
        return static_cast<uint64_t>(static_cast<int64_t>(ns) + offset);
      };
      TRACE_EVENT_BEGIN("workerd.async", "turn", track, at(turn.startNs), "cause", turn.cause);
      KJ_IF_SOME(locked, turn.lockedNs) {
        TRACE_EVENT_BEGIN("workerd.async", "lock", track, at(turn.startNs));
        TRACE_EVENT_END("workerd.async", track, at(locked));
      }
      TRACE_EVENT_END("workerd.async", track, at(turn.endNs));
    }
  }

  void onContextEnd(uint64_t ctx, uint64_t atNs, const AsyncContextStats& stats) override {
    TRACE_EVENT_INSTANT("workerd.async", "ctx_end", contextTrack(), "created", stats.created,
        "dropped", stats.dropped, "unknown", stats.unknown, "unbalanced", stats.unbalanced,
        "ambiguousBindings", stats.ambiguousBindings, "unusedOperationNames",
        stats.unusedOperationNames, "foreignThread", stats.foreignThread);
    traces::TrackEvent::EraseTrackDescriptor(contextTrack());
    contextOpen = false;
  }

 private:
  kj::String label;
  uint64_t isolate = 0;
  // Set by onContextBegin() and kept, so that resource tracks keep the same uuids (a child track's
  // uuid depends on its parent's).
  uint64_t contextId = 0;
  bool contextOpen = false;
  // Formatted creation stacks by ID, as reported by onStack().
  kj::HashMap<uint32_t, kj::String> stacks;

  perfetto::Track contextTrack() const {
    return perfetto::Track(contextId);
  }

  perfetto::Track resourceTrack(AsyncId id) const {
    return perfetto::Track(mix(isolate, id, RESOURCE_TRACK), contextTrack());
  }
};

}  // namespace

bool isAsyncTracePerfettoEnabled() {
  return TRACE_EVENT_CATEGORY_ENABLED("workerd.async");
}

kj::Own<AsyncTraceListener> newAsyncTracePerfettoSink(
    kj::StringPtr worker, kj::Maybe<kj::StringPtr> actor) {
  return kj::heap<PerfettoSink>(worker, actor);
}

#else  // defined(WORKERD_USE_PERFETTO)

bool isAsyncTracePerfettoEnabled() {
  return false;
}

kj::Own<AsyncTraceListener> newAsyncTracePerfettoSink(
    kj::StringPtr worker, kj::Maybe<kj::StringPtr> actor) {
  KJ_UNIMPLEMENTED("perfetto tracing is not supported by this build");
}

#endif  // defined(WORKERD_USE_PERFETTO)

}  // namespace workerd
