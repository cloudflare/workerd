#include "ffi.h"

#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <v8.h>

#include <kj/common.h>

using namespace kj_rs;

namespace workerd {

struct TestContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext);

namespace rust::jsg_test {

// Lazy initialization of V8System to avoid static initialization order issues
static ::workerd::jsg::V8System& getV8System() {
  static ::workerd::jsg::V8System v8System;
  return v8System;
}

TestHarness::TestHarness()
    : isolate(kj::heap<TestIsolate>(getV8System(), kj::heap<::workerd::jsg::IsolateObserver>())) {}

kj::Own<TestHarness> create_test_harness() {
  return kj::heap<TestHarness>();
}

void TestHarness::run_in_context(::rust::Fn<void(Isolate*)> callback) const {
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    auto context = lock.newContext<TestContext>();
    v8::Context::Scope contextScope(context.getHandle(isolate->getIsolate()));

    auto realm = ::workerd::rust::jsg::realm_create(isolate->getIsolate());
    ::workerd::jsg::setAlignedPointerInEmbedderData(context.getHandle(isolate->getIsolate()),
        ::workerd::jsg::ContextPointerSlot::RUST_REALM, realm);

    callback(isolate->getIsolate());

    ::workerd::rust::jsg::realm_dispose(realm);
  });
}

}  // namespace rust::jsg_test

}  // namespace workerd
