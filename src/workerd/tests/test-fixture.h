#pragma once

#include <kj/function.h>

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg-test.h>

namespace workerd {

struct TestFixture {
  // TestFixture is responsible for creating workerd environment during tests.
  // All the infrastructure is started in the constructor. It is accessed through run() method.

  struct SetupParams {
    // waitScope of outer IO loop. New IO will be set up if missing.
    kj::Maybe<kj::WaitScope&> waitScope;
    kj::Maybe<CompatibilityFlags::Reader> featureFlags;
  };

  TestFixture(SetupParams params = { });

  struct V8Environment {
    v8::Isolate* isolate;

    v8::Local<v8::Value> compileAndRunScript(kj::StringPtr script) const;
    // Compile and run the script. Returns the result of last statement.

    v8::Local<v8::Object> compileAndInstantiateModule(
      kj::StringPtr name, kj::ArrayPtr<const char> src) const;
    // Compile and instantiate esm module. Returns module namespace object.
  };

  struct Environment : public V8Environment {
    IoContext& context;
    Worker::Lock& lock;
    jsg::Lock& js;
    CompatibilityFlags::Reader features;
  };

  template <typename T> struct RunReturnType { using Type = T; };
  template <typename T> struct RunReturnType<kj::Promise<T>> { using Type = T; };

  template<typename CallBack>
  auto runInIoContext(CallBack&& callback)
      -> typename RunReturnType<decltype(callback(kj::instance<const Environment&>()))>::Type {
    // Setup the incoming request and run given callback in worker's IO context.
    // callback should accept const Environment& parameter and return Promise<T>|void.
    // For void callbacks run waits for their completion, for promises waits for their resolution
    // and returns the result.

    auto request = createIncomingRequest();
    kj::WaitScope* waitScope;
    KJ_IF_MAYBE(ws, params.waitScope) {
      waitScope = ws;
    } else {
      waitScope = &KJ_REQUIRE_NONNULL(io).waitScope;
    }

    auto& context = request->getContext();
    return context.run([&](Worker::Lock& lock) {
      // auto features = workerBundle.getFeatureFlags();
      auto& js = jsg::Lock::from(lock.getIsolate());
      Environment env = {{.isolate=lock.getIsolate()}, context, lock, js};
      KJ_ASSERT(env.isolate == v8::Isolate::TryGetCurrent());
      return callback(env);
    }).wait(*waitScope);
  }

  void runInIoContext(
      kj::Function<kj::Promise<void>(const Environment&)>&& callback,
      kj::ArrayPtr<kj::StringPtr> errorsToIgnore);
  // Special void version of runInIoContext that ignores exceptions with given descriptions.

private:
  SetupParams params;
  kj::Maybe<kj::AsyncIoContext> io;
  capnp::MallocMessageBuilder configArena;
  capnp::MallocMessageBuilder workerBundleArena;
  kj::Own<kj::Timer> timer;
  kj::Own<TimerChannel> timerChannel;
  kj::Own<kj::EntropySource> entropySource;
  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  ThreadContext::HeaderIdBundle threadContextHeaderBundle;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::Own<IsolateLimitEnforcer> isolateLimitEnforcer;
  kj::Own<Worker::ApiIsolate> apiIsolate;
  kj::Own<Worker::Isolate> workerIsolate;
  kj::Own<Worker::Script> workerScript;
  kj::Own<Worker> worker;
  kj::Own<kj::TaskSet::ErrorHandler> errorHandler;
  kj::TaskSet waitUntilTasks;

  kj::Own<IoContext::IncomingRequest> createIncomingRequest();
};

}  // namespace workerd

