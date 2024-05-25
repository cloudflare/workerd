#include "observer.h"
#include "worker-interface.h"
#include <kj/common.h>
#include <kj/map.h>
#include <kj/mutex.h>

namespace workerd {

namespace {
kj::Maybe<kj::Own<FeatureObserver>> featureObserver;

class FeatureObserverImpl final: public FeatureObserver {
public:
  void use(Feature feature) const override {
    auto lock = counts.lockExclusive();
    lock->upsert(feature, 1, [](uint64_t& count, uint64_t value) {
      count += value;
    });
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
};
