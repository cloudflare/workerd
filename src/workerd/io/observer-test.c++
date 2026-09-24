#include "observer.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("IsolateObserver creation forwards to the no-argument callback") {
  class Observer final: public IsolateObserver {
   public:
    void created() override {
      ++calls;
    }

    uint calls = 0;
  } observer;

  observer.createdWithUuid("isolate-uuid"_kj);
  KJ_ASSERT(observer.calls == 1);
}

KJ_TEST("IsolateObserver creation dispatches to the UUID-aware callback") {
  class Observer final: public IsolateObserver {
   public:
    void created() override {
      ++legacyCalls;
    }

    void createdWithUuid(kj::StringPtr uuid) override {
      ++calls;
      isolateUuid = kj::str(uuid);
    }

    uint legacyCalls = 0;
    uint calls = 0;
    kj::String isolateUuid;
  } observer;

  IsolateObserver& base = observer;
  base.createdWithUuid("isolate-uuid"_kj);
  KJ_ASSERT(observer.calls == 1);
  KJ_ASSERT(observer.legacyCalls == 0);
  KJ_ASSERT(observer.isolateUuid == "isolate-uuid"_kj);
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
