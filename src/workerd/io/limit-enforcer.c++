#include "limit-enforcer.h"
#include "actor-cache.h"
#include <kj/memory.h>

namespace workerd {

namespace {
// IsolateLimitEnforcer that enforces no limits.
class NullIsolateLimitEnforcer final: public IsolateLimitEnforcer {
public:
  v8::Isolate::CreateParams getCreateParams() override { return {}; }
  void customizeIsolate(v8::Isolate* isolate) override {}
  ActorCacheSharedLruOptions getActorCacheLruOptions() override {
    // TODO(someday): Make this configurable?
    return {
      .softLimit = 16 * (1ull << 20), // 16 MiB
      .hardLimit = 128 * (1ull << 20), // 128 MiB
      .staleTimeout = 30 * kj::SECONDS,
      .dirtyListByteLimit = 8 * (1ull << 20), // 8 MiB
      .maxKeysPerRpc = 128,

      // For now, we use `neverFlush` to implement in-memory-only actors.
      // See WorkerService::getActor().
      .neverFlush = true
    };
  }
  kj::Own<void> enterStartupJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
    return {};
  }
  kj::Own<void> enterDynamicImportJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
    return {};
  }
  kj::Own<void> enterLoggingJs(
      jsg::Lock& lock, kj::Maybe<kj::Exception>& error) const override {
    return {};
  }
  kj::Own<void> enterInspectorJs(
      jsg::Lock& loc, kj::Maybe<kj::Exception>& error) const override {
    return {};
  }
  void completedRequest(kj::StringPtr id) const override {}
  bool exitJs(jsg::Lock& lock) const override { return false; }
  void reportMetrics(IsolateObserver& isolateMetrics) const override {}
};

}

kj::Own<IsolateLimitEnforcer> newNullIsolateLimitEnforcer() {
  return kj::heap<NullIsolateLimitEnforcer>();
}

};
