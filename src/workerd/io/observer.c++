#include "observer.h"

#include "worker-interface.h"

#include <workerd/util/exception.h>
#include <workerd/util/use-perfetto-categories.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/map.h>
#include <kj/mutex.h>

namespace workerd {

kj::StringPtr getEventOutcomeName(EventOutcome outcome) {
  switch (outcome) {
    case EventOutcome::UNKNOWN:
      return "unknown"_kj;
    case EventOutcome::OK:
      return "ok"_kj;
    case EventOutcome::EXCEPTION:
      return "exception"_kj;
    case EventOutcome::EXCEEDED_CPU:
      return "exceededCpu"_kj;
    case EventOutcome::KILL_SWITCH:
      return "killSwitch"_kj;
    case EventOutcome::DAEMON_DOWN:
      return "daemonDown"_kj;
    case EventOutcome::SCRIPT_NOT_FOUND:
      return "scriptNotFound"_kj;
    case EventOutcome::CANCELED:
      return "canceled"_kj;
    case EventOutcome::EXCEEDED_MEMORY:
      return "exceededMemory"_kj;
    case EventOutcome::LOAD_SHED:
      return "loadShed"_kj;
    case EventOutcome::RESPONSE_STREAM_DISCONNECTED:
      return "responseStreamDisconnected"_kj;
    case EventOutcome::INTERNAL_ERROR:
      return "internalError"_kj;
    case EventOutcome::EXCEEDED_WALL_TIME:
      return "exceededWallTime"_kj;
    case EventOutcome::ABORTED:
      return "aborted"_kj;
  }
  return "unknown"_kj;
}

void traceWorkerEventOutcome(kj::StringPtr eventType, EventOutcome outcome) {
  if (!TRACE_EVENT_CATEGORY_ENABLED(WORKERD_TRACE_CATEGORY("event"))) {
    return;
  }

  TRACE_EVENT_INSTANT(WORKERD_TRACE_CATEGORY("event"), "Worker event outcome", "event_type",
      eventType.cStr(), "outcome", getEventOutcomeName(outcome).cStr());
}

namespace {
kj::Maybe<kj::Own<FeatureObserver>> featureObserver;

class FeatureObserverImpl final: public FeatureObserver {
 public:
  void use(Feature feature) const override {
    auto lock = counts.lockExclusive();
    lock->upsert(feature, 1, [](uint64_t& count, uint64_t value) { count += value; });
  }

  void collect(CollectCallback&& callback) const override {
    auto lock = counts.lockShared();
    for (auto& entry: *lock) {
      callback(entry.key, entry.value);
    }
  }

 private:
  kj::MutexGuarded<kj::HashMap<Feature, uint64_t>> counts;
};

}  // namespace

kj::Own<FeatureObserver> FeatureObserver::createDefault() {
  return kj::heap<FeatureObserverImpl>();
}

void FeatureObserver::init(kj::Own<FeatureObserver> instance) {
  KJ_ASSERT(featureObserver == kj::none);
  featureObserver = kj::mv(instance);
}

kj::Maybe<FeatureObserver&> FeatureObserver::get() {
  KJ_IF_SOME(impl, featureObserver) {
    return *impl;
  }
  return kj::none;
}

EventOutcome RequestObserver::outcomeFromException(const kj::Exception& e, FailureSource source) {
  if (e.getDetail(MEMORY_LIMIT_DETAIL_ID) != kj::none) {
    return EventOutcome::EXCEEDED_MEMORY;
  } else if (e.getDetail(CPU_LIMIT_DETAIL_ID) != kj::none) {
    return EventOutcome::EXCEEDED_CPU;
  } else if (e.getDetail(WALL_TIME_LIMIT_DETAIL_ID) != kj::none) {
    return EventOutcome::EXCEEDED_WALL_TIME;
  } else if (e.getDetail(SCRIPT_KILLED_DETAIL_ID) != kj::none) {
    return EventOutcome::KILL_SWITCH;
  } else if (source == RequestObserver::FailureSource::DEFERRED_PROXY &&
      e.getType() == kj::Exception::Type::DISCONNECTED) {
    return EventOutcome::RESPONSE_STREAM_DISCONNECTED;
  } else if (e.getType() == kj::Exception::Type::OVERLOADED) {
    // We use exception details to describe some overloaded exceptions more accurately, if no such
    // detail is present report internalError.
    return EventOutcome::INTERNAL_ERROR;
  } else {
    return EventOutcome::EXCEPTION;
  }
}

};  // namespace workerd
