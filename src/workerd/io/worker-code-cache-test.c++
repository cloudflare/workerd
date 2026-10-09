// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests of how Worker::Script and Worker drive an isolate's code cache production window (see
// jsg::IsolateCodeCache). jsg/code-cache-test.c++ covers the window itself.

#include <workerd/io/worker-fs.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/test-fixture.h>

#include <kj/map.h>
#include <kj/mutex.h>
#include <kj/test.h>

namespace workerd {
namespace {

using jsg::CodeCacheKey;
using jsg::CodeCacheStore;

// A CodeCacheStore that counts what it is asked to do.
class CountingStore final: public CodeCacheStore {
 public:
  struct State {
    kj::HashMap<CodeCacheKey, kj::Arc<Bytes>> entries;
    kj::HashSet<CodeCacheKey> claimed;
    uint inserts = 0;
  };
  kj::MutexGuarded<State> state;

  LookupResult lookup(const CodeCacheKey& key) const override {
    auto lock = state.lockExclusive();
    KJ_IF_SOME(bytes, lock->entries.find(key)) {
      return {.hit = bytes.addRef()};
    }
    if (lock->claimed.contains(key)) return {};
    lock->claimed.insert(key);
    return {.producer = kj::heap<CountingProducer>(*this, key)};
  }

  kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<v8::ScriptCompiler::CachedData::CompatibilityCheckResult> reason) const override {
    KJ_FAIL_ASSERT("unexpected reject", reason);
  }

 private:
  class CountingProducer final: public Producer {
   public:
    CountingProducer(const CountingStore& store, CodeCacheKey key): store(store), key(key) {}
    ~CountingProducer() noexcept(false) {
      store.state.lockExclusive()->claimed.eraseMatch(key);
    }

    void insert(Bytes bytes) override {
      auto lock = store.state.lockExclusive();
      ++lock->inserts;
      lock->entries.upsert(key, kj::arc<Bytes>(kj::mv(bytes)));
    }
    void generationFailed() override {
      KJ_FAIL_ASSERT("unexpected generation failure");
    }

   private:
    const CountingStore& store;
    CodeCacheKey key;
  };
};

constexpr kj::StringPtr MAIN_MODULE =
    "export default { fetch() { return new Response('hi'); } };"_kj;

// Owns an isolate, taken from a TestFixture, whose later scripts use `store`.
struct IsolateWithStore {
  TestFixture fixture;
  kj::Own<const Worker::Isolate> isolate;
  kj::Own<CountingStore> store = kj::atomicRefcounted<CountingStore>();

  IsolateWithStore() {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      isolate = kj::atomicAddRef(env.lock.getWorker().getIsolate());
      jsg::IsolateBase::from(env.isolate).setCodeCacheStore(kj::atomicAddRef(*store));
    });
  }

  kj::Own<const Worker::Script> newScript() {
    auto modules = kj::heapArray<Worker::Script::Module>(1);
    modules[0] = {
      .name = "main"_kj,
      .content = Worker::Script::EsModule{.body = MAIN_MODULE},
    };
    Worker::Script::Source source(Worker::Script::ModulesSource{
      .mainModule = "main"_kj,
      .modules = kj::mv(modules),
      .isPython = false,
    });
    return isolate->newScript("code-cache-script"_kj, source, IsolateObserver::StartType::COLD,
        SpanParent(nullptr), newWorkerFileSystem(kj::heap<FsMap>(), getTmpDirectoryImpl()));
  }

  kj::Own<const Worker> newWorker(const Worker::Script& script) {
    return kj::atomicRefcounted<Worker>(kj::atomicAddRef(script),
        kj::atomicRefcounted<WorkerObserver>(),
        [](jsg::Lock&, const Worker::Api&, v8::Local<v8::Object>, v8::Local<v8::Object>) {},
        IsolateObserver::StartType::COLD, SpanParent(nullptr),
        Worker::LockType(Worker::Lock::TakeSynchronously(kj::none)));
  }

  uint claimCount() {
    return store->state.lockShared()->claimed.size();
  }
  uint insertCount() {
    return store->state.lockShared()->inserts;
  }
};

KJ_TEST("A Worker produces the caches of its script's modules after running the top level") {
  IsolateWithStore env;

  auto script = env.newScript();
  // The script compiled the module and holds the election until a Worker runs it.
  KJ_EXPECT(env.claimCount() == 1);
  KJ_EXPECT(env.insertCount() == 0);

  auto worker = env.newWorker(*script);
  KJ_EXPECT(env.claimCount() == 0);
  KJ_EXPECT(env.insertCount() == 1);
}

KJ_TEST("A script that no Worker runs releases its elections when it is destroyed") {
  IsolateWithStore env;

  auto script = env.newScript();
  KJ_EXPECT(env.claimCount() == 1);

  script = nullptr;
  KJ_EXPECT(env.claimCount() == 0);
  KJ_EXPECT(env.insertCount() == 0);

  // The next script is elected again, and its Worker produces the cache.
  script = env.newScript();
  auto worker = env.newWorker(*script);
  KJ_EXPECT(env.insertCount() == 1);
}

}  // namespace
}  // namespace workerd
