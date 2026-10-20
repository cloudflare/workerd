// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests of how Worker::Script and Worker drive an isolate's code cache production window (see
// jsg::IsolateCodeCache). jsg/code-cache-test.c++ covers the window itself.

#include <workerd/io/worker-fs.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules-new.h>
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
    uint lookups = 0;
    // Lookups made inside the isolate's startup CPU limit, which the validator never pays.
    uint lookupsInStartupLimit = 0;
    uint inserts = 0;
  };
  kj::MutexGuarded<State> state;

  LookupResult lookup(const CodeCacheKey& key, jsg::MayElect mayElect) const override {
    auto lock = state.lockExclusive();
    ++lock->lookups;
    if (TestFixture::isInStartupLimitScope()) ++lock->lookupsInStartupLimit;
    KJ_IF_SOME(bytes, lock->entries.find(key)) {
      return {.hit = bytes.addRef()};
    }
    if (!mayElect || lock->claimed.contains(key)) return {};
    lock->claimed.insert(key);
    return {.producer = kj::heap<CountingProducer>(*this, key)};
  }

  kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<v8::ScriptCompiler::CachedData::CompatibilityCheckResult> reason,
      jsg::MayElect mayElect) const override {
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
constexpr kj::StringPtr MAIN_SCRIPT =
    "addEventListener('fetch', (event) => event.respondWith(new Response('hi')));"_kj;

struct CountingObserver final: public JsgIsolateObserver {
  mutable uint found = 0;
  mutable uint generated = 0;
  mutable uint bundleCompilationDepth = 0;

  kj::Own<void> onEsmCompilationStart(v8::Isolate*, kj::StringPtr, Option option) const override {
    if (option != Option::BUNDLE) return {};
    ++bundleCompilationDepth;
    return kj::heap(kj::defer([this]() { --bundleCompilationDepth; }));
  }

  void onCompileCacheFound(v8::Isolate*) const override {
    // Bootstrap scripts use the same observer; count only worker bundle hits here.
    if (bundleCompilationDepth > 0) ++found;
  }
  void onCompileCacheGenerated(v8::Isolate*) const override {
    ++generated;
  }
};

// Owns an isolate, taken from a TestFixture, whose later scripts use `store`.
struct IsolateWithStore {
  TestFixture fixture;
  kj::Own<const Worker::Isolate> isolate;
  kj::Own<CountingStore> store = kj::atomicRefcounted<CountingStore>();

  IsolateWithStore(TestFixture::SetupParams&& params = {.useRealTimers = false})
      : fixture(kj::mv(params)) {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      isolate = kj::atomicAddRef(env.lock.getWorker().getIsolate());
      jsg::IsolateBase::from(env.isolate).setCodeCacheStore(kj::atomicAddRef(*store));
    });
  }

  kj::Own<const Worker::Script> newScript(
      kj::Maybe<kj::Arc<jsg::modules::ModuleRegistry>> registry = kj::none) {
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
    return newScript(source, kj::mv(registry));
  }

  // A script in service-worker syntax.
  kj::Own<const Worker::Script> newClassicScript() {
    return newScript(Worker::Script::Source(Worker::Script::ScriptSource{
      .mainScript = MAIN_SCRIPT,
      .mainScriptName = "worker.js"_kj,
    }));
  }

  kj::Own<const Worker::Script> newScript(const Worker::Script::Source& source,
      kj::Maybe<kj::Arc<jsg::modules::ModuleRegistry>> registry = kj::none) {
    return isolate->newScript("code-cache-script"_kj, source, IsolateObserver::StartType::COLD,
        SpanParent(nullptr), newWorkerFileSystem(kj::heap<FsMap>(), getTmpDirectoryImpl()), false,
        kj::none, kj::none, kj::mv(registry));
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
  uint lookupCount() {
    return store->state.lockShared()->lookups;
  }
  uint lookupsInStartupLimit() {
    return store->state.lockShared()->lookupsInStartupLimit;
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

  // The module was looked up once, before the startup CPU limit applied.
  KJ_EXPECT(env.lookupCount() == 1);
  KJ_EXPECT(env.lookupsInStartupLimit() == 0);

  // The next script consumes the cache.
  worker = nullptr;
  script = nullptr;
  script = env.newScript();
  KJ_EXPECT(env.lookupCount() == 2);
  KJ_EXPECT(env.lookupsInStartupLimit() == 0);
  KJ_EXPECT(env.claimCount() == 0);
}

KJ_TEST("Identical bundle modules share a lookup outside the startup CPU limit") {
  IsolateWithStore env;
  Worker::Script::Source source(Worker::Script::ModulesSource{
    .mainModule = "main"_kj,
    .modules = kj::arr(
        Worker::Script::Module{
          .name = "main"_kj,
          .content = Worker::Script::EsModule{.body = MAIN_MODULE},
        },
        Worker::Script::Module{
          .name = "other"_kj,
          .content = Worker::Script::EsModule{.body = MAIN_MODULE},
        }),
    .isPython = false,
  });

  for (auto i: kj::zeroTo(2)) {
    auto script = env.newScript(source);
    auto worker = env.newWorker(*script);
    KJ_EXPECT(env.lookupCount() == i + 1);
    KJ_EXPECT(env.lookupsInStartupLimit() == 0);
    KJ_EXPECT(env.insertCount() == 1);
    KJ_EXPECT(env.claimCount() == 0);
  }
}

KJ_TEST("A Worker with the new registry consumes a lookup prepared outside the startup limit") {
  auto newRegistry = []() {
    const auto base = "file:///"_url;
    jsg::modules::ModuleRegistry::Builder builder(
        base, jsg::modules::ModuleRegistry::Builder::Options::CODE_CACHE_KEYS);
    jsg::modules::ModuleBundle::BundleBuilder bundleBuilder(base);
    bundleBuilder.addEsmModule("main"_kj, jsg::copyToArc(MAIN_MODULE),
        jsg::modules::Module::Flags::ESM, builder.newCodeCacheKey(MAIN_MODULE));
    return builder.add(bundleBuilder.finish()).finish();
  };
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setNewModuleRegistry(true);
  auto observer = kj::atomicRefcounted<CountingObserver>();
  IsolateWithStore env({
    .featureFlags = flags.asReader(),
    .mainModuleSource = MAIN_MODULE,
    .useRealTimers = false,
    .jsgIsolateObserver = kj::atomicAddRef(*observer),
    .newModuleRegistry = newRegistry(),
  });
  // The fixture ran its initial Worker before the store was installed.
  auto initialFound = observer->found;
  auto initialGenerated = observer->generated;

  for (auto i: kj::zeroTo(2)) {
    auto script = env.newScript(newRegistry());
    KJ_EXPECT(env.lookupCount() == i + 1);
    KJ_EXPECT(env.lookupsInStartupLimit() == 0);
    KJ_EXPECT(env.claimCount() == (i == 0 ? 1 : 0));
    auto worker = env.newWorker(*script);
    // First-import compilation happens in the Worker; it must reuse the prepared lookup.
    KJ_EXPECT(env.lookupCount() == i + 1);
    KJ_EXPECT(env.lookupsInStartupLimit() == 0);
    KJ_EXPECT(env.insertCount() == 1);
    KJ_EXPECT(env.claimCount() == 0);
    KJ_EXPECT(observer->generated == initialGenerated + 1);
    KJ_EXPECT(observer->found == initialFound + i);
  }
}

KJ_TEST("A Worker produces the cache of its service-worker script after running the top level") {
  IsolateWithStore env;

  auto script = env.newClassicScript();
  KJ_EXPECT(env.claimCount() == 1);
  KJ_EXPECT(env.insertCount() == 0);

  auto worker = env.newWorker(*script);
  KJ_EXPECT(env.claimCount() == 0);
  KJ_EXPECT(env.insertCount() == 1);
  KJ_EXPECT(env.lookupCount() == 1);
  KJ_EXPECT(env.lookupsInStartupLimit() == 0);
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
