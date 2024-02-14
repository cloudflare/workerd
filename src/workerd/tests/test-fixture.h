// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/function.h>

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/api/memory-cache.h>

namespace workerd {

// TestFixture is responsible for creating workerd environment during tests.
// All the infrastructure is started in the constructor. It is accessed through run() method.
struct TestFixture {
  struct SetupParams {
    // waitScope of outer IO loop. New IO will be set up if missing.
    kj::Maybe<kj::WaitScope&> waitScope;
    kj::Maybe<CompatibilityFlags::Reader> featureFlags;
    kj::Maybe<kj::StringPtr> mainModuleSource;
    // If set, make a stub of an Actor with the given id.
    kj::Maybe<Worker::Actor::Id> actorId;
  };

  TestFixture(SetupParams&& params = { });

  struct V8Environment {
    v8::Isolate* isolate;
  };

  struct Environment : public V8Environment {
    IoContext& context;
    Worker::Lock& lock;
    jsg::Lock& js;
    CompatibilityFlags::Reader features;
  };

  template <typename T> struct RunReturnType { using Type = T; };
  template <typename T> struct RunReturnType<kj::Promise<T>> { using Type = T; };

  // Setup the incoming request and run given callback in worker's IO context.
  // callback should accept const Environment& parameter and return Promise<T>|void.
  // For void callbacks run waits for their completion, for promises waits for their resolution
  // and returns the result.
  template<typename CallBack>
  auto runInIoContext(CallBack&& callback)
      -> typename RunReturnType<decltype(callback(kj::instance<const Environment&>()))>::Type {
    auto request = createIncomingRequest();
    kj::WaitScope* waitScope;
    KJ_IF_SOME(ws, this->waitScope) {
      waitScope = &ws;
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

  // Special void version of runInIoContext that ignores exceptions with given descriptions.
  void runInIoContext(
      kj::Function<kj::Promise<void>(const Environment&)>&& callback,
      kj::ArrayPtr<kj::StringPtr> errorsToIgnore);

  struct Response {
    uint statusCode;
    kj::String body;
  };

  // Performs HTTP request on the default module handler, and waits for full response.
  Response runRequest(kj::HttpMethod method, kj::StringPtr url, kj::StringPtr body);

private:
  kj::Maybe<kj::WaitScope&> waitScope;
  capnp::MallocMessageBuilder configArena;
  workerd::server::config::Worker::Reader config;
  kj::Maybe<kj::AsyncIoContext> io;
  capnp::MallocMessageBuilder workerBundleArena;
  kj::Own<kj::Timer> timer;
  kj::Own<TimerChannel> timerChannel;
  kj::Own<kj::EntropySource> entropySource;
  kj::Maybe<kj::Own<Worker::Actor>> actor;
  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  ThreadContext::HeaderIdBundle threadContextHeaderBundle;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::Own<IsolateLimitEnforcer> isolateLimitEnforcer;
  kj::Own<Worker::ValidationErrorReporter> errorReporter;
  api::MemoryCacheMap memoryCacheMap;
  kj::Own<Worker::Api> api;
  kj::Own<Worker::Isolate> workerIsolate;
  kj::Own<Worker::Script> workerScript;
  kj::Own<Worker> worker;
  kj::Own<kj::TaskSet::ErrorHandler> errorHandler;
  kj::TaskSet waitUntilTasks;
  kj::Own<kj::HttpHeaderTable> headerTable;

  kj::Own<IoContext::IncomingRequest> createIncomingRequest();
};

}  // namespace workerd
