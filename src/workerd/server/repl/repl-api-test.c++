// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-api.h"

#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::server {
namespace {

// Holds a KJ async object, as the REPL's hosts do (repl-server.h, repl-environment.c++), and says
// when it is destroyed.
class AsyncState {
 public:
  explicit AsyncState(bool& destroyed): destroyed(destroyed) {}
  ~AsyncState() noexcept(false) {
    destroyed = true;
  }
  KJ_DISALLOW_COPY_AND_MOVE(AsyncState);

 private:
  bool& destroyed;
  kj::ForkedPromise<void> closing = kj::Promise<void>(kj::READY_NOW).fork();
};

class TestServerHost final: public ReplServerHost, public kj::Refcounted {
 public:
  explicit TestServerHost(bool& destroyed): state(destroyed) {}

  kj::Own<ReplServerHost> addRef() override {
    return kj::addRef(*this);
  }
  kj::StringPtr getPath() const override {
    return "test.capnp"_kj;
  }
  bool isClosed() const override {
    return true;
  }
  bool isWatching() const override {
    return false;
  }
  kj::ArrayPtr<const kj::String> getWarnings() const override {
    return nullptr;
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> service(
      kj::StringPtr, kj::Maybe<kj::StringPtr>, kj::Maybe<Frankenvalue>) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> socket(kj::StringPtr) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> durableObject(const ReplActorAddress&) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<void> reload() override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<void> close() override {
    return kj::READY_NOW;
  }

 private:
  AsyncState state;
};

class TestEnvironmentHost final: public ReplEnvironmentHost, public kj::Refcounted {
 public:
  explicit TestEnvironmentHost(bool& destroyed): state(destroyed) {}

  kj::Own<ReplEnvironmentHost> addRef() override {
    return kj::addRef(*this);
  }
  kj::StringPtr getCompatibilityDate() const override {
    return "2025-01-01"_kj;
  }
  kj::ArrayPtr<const kj::String> getCompatibilityFlags() const override {
    return nullptr;
  }
  bool isClosed() const override {
    return true;
  }
  kj::Promise<kj::String> evaluate(kj::String) override {
    KJ_UNIMPLEMENTED("not used");
  }
  kj::Promise<void> close() override {
    return kj::READY_NOW;
  }

 private:
  AsyncState state;
};

// Drops `handle`, its last reference, where JavaScript objects are destroyed: under
// DISALLOW_KJ_IO_DESTRUCTORS_SCOPE, which aborts if a KJ async object is destroyed.
template <typename T>
void dropAsJavaScriptDoes(jsg::Ref<T> handle) {
  DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;
  auto dropped = kj::mv(handle);
}

KJ_TEST("dropping a server's last handle destroys no KJ I/O objects") {
  TestFixture fixture;
  bool destroyed = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // Never wrapped, so that this is its only reference.
    dropAsJavaScriptDoes(env.js.alloc<ReplServer>(kj::refcounted<TestServerHost>(destroyed)));
    // The IoContext destroyed the host, as the handle went.
    KJ_EXPECT(destroyed);
  });
}

KJ_TEST("dropping an environment's last handle destroys no KJ I/O objects") {
  TestFixture fixture;
  bool destroyed = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    dropAsJavaScriptDoes(
        env.js.alloc<ReplEnvironmentHandle>(kj::refcounted<TestEnvironmentHost>(destroyed)));
    KJ_EXPECT(destroyed);
  });
}

}  // namespace
}  // namespace workerd::server
