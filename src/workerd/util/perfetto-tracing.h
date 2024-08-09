#pragma once

#if defined(WORKERD_USE_PERFETTO)
#include <kj/memory.h>
#define PERFETTO_ENABLE_LEGACY_TRACE_EVENTS 1
// Only include a smaller header here to keep include size under control. This approach is
// recommended in the full perfetto header (perfetto/tracing.h).
#include "perfetto/tracing/track_event.h"

PERFETTO_DEFINE_CATEGORIES_IN_NAMESPACE(workerd::traces, perfetto::Category("workerd"));

namespace kj {
class StringPtr;
class String;
}  // namespace kj

namespace workerd {

// The PerfettoSession initializes a Perfetto tracing session, initializing the
// Perfetto subsystem on first initialization and writing events to the given
// file path.
class PerfettoSession {
public:
  explicit PerfettoSession(kj::StringPtr path, kj::StringPtr categories);

  // Create a PerfettoSession on an existing fd (the constructor will handle
  // wrapping the fd in kj::AutocloseFd)
  explicit PerfettoSession(int fd, kj::StringPtr categories);
  PerfettoSession(PerfettoSession&&) = default;
  PerfettoSession& operator=(PerfettoSession&&) = default;
  KJ_DISALLOW_COPY(PerfettoSession);
  ~PerfettoSession() noexcept(false);

  void flush();

  // Receives a comma-separated list of trace categories and returns an array.
  static kj::Array<kj::ArrayPtr<const char>> parseCategories(kj::StringPtr categories);

  // Called by embedders at most once to register the workerd track events.
  static void registerWorkerdTracks();

private:
  struct Impl;
  kj::Own<Impl> impl;

  friend constexpr bool _kj_internal_isPolymorphic(PerfettoSession::Impl*);
};

#define PERFETTO_FLOW_FROM_POINTER(ptr) perfetto::Flow::FromPointer(ptr)
#define PERFETTO_TERMINATING_FLOW_FROM_POINTER(ptr) perfetto::TerminatingFlow::FromPointer(ptr)
#define PERFETTO_TRACK_FROM_POINTER(ptr) perfetto::Track::FromPointer(ptr)

KJ_DECLARE_NON_POLYMORPHIC(PerfettoSession::Impl);
}  // namespace workerd

#else  // defined(WORKERD_USE_PERFETTO)
struct PerfettoNoop {};
// We define non-op versions of the instrumentation macros here so that we can
// still instrument and build when perfetto is not enabled.
#define TRACE_EVENT(...)
#define TRACE_EVENT_BEGIN(...)
#define TRACE_EVENT_END(...)
#define TRACE_EVENT_INSTANT(...)
#define TRACE_COUNTER(...)
#define TRACE_EVENT_CATEGORY_ENABLED(...) false
#define PERFETTO_FLOW_FROM_POINTER(ptr)                                                            \
  PerfettoNoop {}
#define PERFETTO_TERMINATING_FLOW_FROM_POINTER(ptr)                                                \
  PerfettoNoop {}
#define PERFETTO_TRACK_FROM_POINTER(ptr)                                                           \
  PerfettoNoop {}
#endif  // defined(WORKERD_USE_PERFETTO)
