#include "isolate-limit-enforcer.h"
#include <workerd/io/actor-cache.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/setup.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <fcntl.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <kj/windows-sanity.h>
#endif

namespace workerd {
namespace {

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
  kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& lock, size_t iterations) const override {
    // No limit on the number of iterations in workerd
    return kj::none;
  }
};

class LimitedArrayBufferAllocator final: public v8::ArrayBuffer::Allocator {
public:
  LimitedArrayBufferAllocator(size_t limit): limit(limit) {}
  ~LimitedArrayBufferAllocator() {}

  void* Allocate(size_t length) override {
    if (length > limit) return nullptr;
    return calloc(length, 1);
  }
  void* AllocateUninitialized(size_t length) override {
    if (length > limit) return nullptr;
    return malloc(length);
  }
  void Free(void* data, size_t length) override {
    free(data);
  }

private:
  size_t limit;
};

class ConfiguredIsolateLimitEnforcer final: public IsolateLimitEnforcer {
public:
  ConfiguredIsolateLimitEnforcer(server::config::Worker::Limits::Reader limits)
      : softHeapLimitMb(limits.getHeapSoftLimitMb()),
        heapHardLimitMb(limits.getHeapHardLimitMb()),
        heapSnapshotNearHeapLimit(limits.getHeapSnapshotNearHeapLimit()),
        heapLimitMultiplier(limits.getHeapLimitMultiplier()),
        heapLimitExceedsMax(limits.getHeapLimitExceedsMax()),
        heapInitialYoungGenSizeMb(limits.getHeapInitialYoungGenSizeMb()) {}

  v8::Isolate::CreateParams getCreateParams() override {
    v8::Isolate::CreateParams params;
    uint64_t softLimit = softHeapLimitMb * 1024 * 1024;
    if (softLimit > 0) {
      params.constraints.set_max_young_generation_size_in_bytes(
          kj::min(softLimit, heapInitialYoungGenSizeMb * 1024 * 1024));
      params.constraints.set_max_old_generation_size_in_bytes(softLimit);
      params.array_buffer_allocator_shared =
          std::make_shared<LimitedArrayBufferAllocator>(softLimit);
    }
    return params;
  }

  static size_t nearHeapLimit(void* data, size_t currentHeapLimit, size_t initialHeapLimit) {
    auto& self = *static_cast<ConfiguredIsolateLimitEnforcer*>(data);
    size_t newLimit = currentHeapLimit * self.heapLimitMultiplier;
    // We can hit this again when taking the heapsnapshot... just increase the limit
    // and continue, even if it exceeds the configured hard limit
    if (self.inNearLimitCallback) return newLimit;
    self.inNearLimitCallback = true;
    KJ_DEFER(self.inNearLimitCallback = false);

    if ((self.exceededCounter >= self.heapLimitExceedsMax) ||
        (self.heapHardLimitMb > self.softHeapLimitMb &&
         newLimit > self.heapHardLimitMb * 1024 * 1024)) {
      self.maybeGenerateHeapshot();
      ([&]() noexcept(true) {
        // We are intentionally crashing the process here.
        kj::throwFatalException(KJ_EXCEPTION(FAILED,
            "Exceeded the configured hard heap limit.",
            currentHeapLimit,
            self.exceededCounter,
            self.heapHardLimitMb));
      })();
    } else {
      KJ_LOG(WARNING, "Exceeded the configured soft heap limit. Setting new limit",
             currentHeapLimit,
             newLimit,
             self.exceededCounter++);
      self.maybeGenerateHeapshot();
    }
    return newLimit;
  }

  void customizeIsolate(v8::Isolate* isolate) override {
    KJ_REQUIRE(v8Isolate == nullptr, "one IsolateLimitEnforcer can only be used by one isolate");
    v8Isolate = isolate;

    isolate->AddNearHeapLimitCallback(&nearHeapLimit, this);
  }

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
  kj::Maybe<size_t> checkPbkdfIterations(jsg::Lock& lock, size_t iterations) const override {
    // No limit on the number of iterations in workerd
    return kj::none;
  }

  void maybeGenerateHeapshot() {
    if (heapSnapshotCounter >= heapSnapshotNearHeapLimit || v8Isolate == nullptr) return;

    static jsg::HeapSnapshotActivity activity([](auto, auto) {
      return true;
    });
    static jsg::HeapSnapshotDeleter deleter;

    auto snapshot = kj::Own<const v8::HeapSnapshot>(
        v8Isolate->GetHeapProfiler()->TakeHeapSnapshot(&activity, nullptr, true, true),
        deleter);

    jsg::IsolateBase& base = jsg::IsolateBase::from(v8Isolate);
    kj::String filename = kj::str("heapshot-", base.getUuid(), "-",
                                  heapSnapshotCounter++, ".heapsnapshot");

    KJ_LOG(WARNING, kj::str("Generating heap snapshot: ", filename));

#ifdef _WIN32
    HANDLE handle;
    const auto path = kj::Path::parse(filename).forWin32Api(true);
    KJ_WIN32_HANDLE_ERRORS(handle = CreateFileW(
        path.begin(), FILE_GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) {
      case ERROR_ALREADY_EXISTS:
      case ERROR_FILE_EXISTS:
        KJ_LOG(WARNING, "Heap snapshot destination file already exists. Skipping");
        return;
      default:
        KJ_FAIL_WIN32("CreateFileW", error);
    }
    kj::AutoCloseHandle autoHandle(handle);
    kj::HandleOutputStream out(autoHandle.get());
#else
    auto fd = open(filename.cStr(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    KJ_REQUIRE(fd >= 0, "Unable to open heap snapshot file for writing");
    kj::AutoCloseFd autoFd(fd);
    kj::FdOutputStream out(autoFd.get());
#endif

    jsg::HeapSnapshotWriter writer([&](kj::Maybe<kj::ArrayPtr<char>> maybeChunk) {
      KJ_IF_SOME(chunk, maybeChunk) {
        out.write(chunk.begin(), chunk.size());
      } else {
        out.write(nullptr, 0);
      }
      return true;
    });

    snapshot->Serialize(&writer);
  }

private:
  uint64_t softHeapLimitMb;
  uint64_t heapHardLimitMb;
  uint32_t heapSnapshotNearHeapLimit;
  uint32_t heapLimitMultiplier;
  uint32_t heapLimitExceedsMax;
  uint32_t heapInitialYoungGenSizeMb;
  v8::Isolate* v8Isolate = nullptr;

  // Indicates the number of times we've hit the soft limit.
  // Once this reaches heapLimitExceedsMax, we'll terminate the isolate.
  uint32_t exceededCounter = 0;

  // The number of heapsnapshots we've generated.
  uint32_t heapSnapshotCounter = 0;
  bool inNearLimitCallback = false;
};

}  // namespace

kj::Own<IsolateLimitEnforcer> newNullIsolateLimitEnforcer() {
  return kj::heap<NullIsolateLimitEnforcer>();
}

kj::Own<workerd::IsolateLimitEnforcer> newConfiguredIsolateLimitEnforcer(
    server::config::Worker::Limits::Reader configuredLimits) {
  return kj::heap<ConfiguredIsolateLimitEnforcer>(configuredLimits);
}

}  // namespace workerd
