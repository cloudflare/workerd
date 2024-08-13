#include "observer.h"
#include "worker-interface.h"

#include <kj/test.h>

namespace workerd {
namespace {

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
