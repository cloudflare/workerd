#include "ffi.h"

#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <v8.h>

#include <kj/common.h>

using namespace kj_rs;

namespace {
// Enable predictable mode so RequestGarbageCollectionForTesting actually triggers GC.
// Without this, V8 may defer or skip the requested collection.
bool initPredictableMode = (workerd::setPredictableModeForTest(), true);
}  // namespace

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

TestHarness::TestHarness(::workerd::jsg::V8StackScope&)
    : isolate(kj::heap<TestIsolate>(getV8System(), kj::heap<::workerd::jsg::IsolateObserver>())),
      locker(isolate->getIsolate()),
      isolateScope(isolate->getIsolate()),
      realm(::workerd::rust::jsg::realm_create(isolate->getIsolate())) {
  isolate->getIsolate()->SetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM, &*realm);
}

kj::Own<TestHarness> create_test_harness() {
  return ::workerd::jsg::runInV8Stack(
      [](::workerd::jsg::V8StackScope& stackScope) { return kj::heap<TestHarness>(stackScope); });
}

void TestHarness::run_in_context(::rust::Fn<void(Isolate*)> callback) const {
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    auto context = lock.newContext<TestContext>();
    v8::Context::Scope contextScope(context.getHandle(isolate->getIsolate()));
    callback(isolate->getIsolate());
  });
}

}  // namespace rust::jsg_test

}  // namespace workerd
