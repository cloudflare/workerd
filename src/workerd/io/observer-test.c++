#include "observer.h"
#include "worker-interface.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("EventOutcome names match the schema") {
  EventOutcome outcomes[] = {EventOutcome::UNKNOWN, EventOutcome::OK, EventOutcome::EXCEPTION,
    EventOutcome::EXCEEDED_CPU, EventOutcome::KILL_SWITCH, EventOutcome::DAEMON_DOWN,
    EventOutcome::SCRIPT_NOT_FOUND, EventOutcome::CANCELED, EventOutcome::EXCEEDED_MEMORY,
    EventOutcome::LOAD_SHED, EventOutcome::RESPONSE_STREAM_DISCONNECTED,
    EventOutcome::INTERNAL_ERROR, EventOutcome::EXCEEDED_WALL_TIME, EventOutcome::ABORTED};

  for (auto outcome: outcomes) {
    KJ_EXPECT(getEventOutcomeName(outcome) == kj::str(outcome));
  }
  KJ_EXPECT(getEventOutcomeName(static_cast<EventOutcome>(255)) == "unknown");
}

KJ_TEST("FeatureObserver") {
  FeatureObserver::init(FeatureObserver::createDefault());

  auto& observer = KJ_ASSERT_NONNULL(FeatureObserver::get());

  observer.use(FeatureObserver::Feature::TEST);
  observer.use(FeatureObserver::Feature::TEST);
  observer.use(FeatureObserver::Feature::TEST);

  uint64_t count = 0;
  observer.collect([&](FeatureObserver::Feature feature, const uint64_t value) {
    KJ_ASSERT(feature == FeatureObserver::Feature::TEST);
    count = value;
  });
  KJ_ASSERT(count == 3);
}

}  // namespace
}  // namespace workerd
