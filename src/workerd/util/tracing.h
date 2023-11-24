#pragma once

#include <kj/memory.h>
#include <kj/string.h>
#define PERFETTO_ENABLE_LEGACY_TRACE_EVENTS 1
#include "perfetto/tracing.h"

PERFETTO_DEFINE_CATEGORIES_IN_NAMESPACE(
    workerd,
    perfetto::Category("workerd"));
PERFETTO_USE_CATEGORIES_FROM_NAMESPACE(workerd);

namespace workerd {

// The PerfettoSession initializes a Perfetto tracing session, initializing the
// Perfetto subsystem on first initialization and writing events to the given
// file path.
class PerfettoSession {
public:
  PerfettoSession(kj::StringPtr path, kj::StringPtr categories);
  PerfettoSession(PerfettoSession&&) = default;
  PerfettoSession& operator=(PerfettoSession&&) = default;
  KJ_DISALLOW_COPY(PerfettoSession);
  ~PerfettoSession() noexcept(false);

  void flush();

private:
  struct Impl;
  kj::Own<Impl> impl;

  friend constexpr bool _kj_internal_isPolymorphic(PerfettoSession ::Impl*);
};

KJ_DECLARE_NON_POLYMORPHIC(PerfettoSession::Impl);
}  // workerd
