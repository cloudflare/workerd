// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "features.h"

#include <workerd/jsg/setup.h>
#include <workerd/jsg/util.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd {
namespace {

jsg::V8System v8System;

struct TestContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext);

using FlagSetter = void (CompatibilityFlags::Builder::*)(bool);
constexpr FlagSetter FLAG_SETTERS[] = {
  &CompatibilityFlags::Builder::setCaptureThrowsAsRejections,
  &CompatibilityFlags::Builder::setSetToStringTag,
  &CompatibilityFlags::Builder::setShouldSetImmutablePrototype,
  &CompatibilityFlags::Builder::setPythonWorkers,
  &CompatibilityFlags::Builder::setSpecCompliantPropertyAttributes,
  &CompatibilityFlags::Builder::setNodeJsCompat,
  &CompatibilityFlags::Builder::setNodeJsCompatV2,
  &CompatibilityFlags::Builder::setEnableNodeJsProcessV2,
  &CompatibilityFlags::Builder::setRequireReturnsDefaultExport,
  &CompatibilityFlags::Builder::setThrowOnUnrecognizedImportAssertion,
  &CompatibilityFlags::Builder::setNoTopLevelAwaitInRequire,
  &CompatibilityFlags::Builder::setEnhancedErrorSerialization,
  &CompatibilityFlags::Builder::setFastJsgStruct,
  &CompatibilityFlags::Builder::setWasmEsmIntegrationBuiltins,
};

void checkSettings(CompatibilityFlags::Reader flags) {
  TestIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](TestIsolate::Lock& js) {
    configureJsgIsolate(js, flags);
    auto& base = jsg::IsolateBase::from(js.v8Isolate);
    KJ_EXPECT(
        jsg::getCaptureThrowsAsRejections(js.v8Isolate) == flags.getCaptureThrowsAsRejections());
    KJ_EXPECT(base.shouldSetToStringTag() == flags.getSetToStringTag());
    KJ_EXPECT(base.shouldSetImmutablePrototype() ==
        (flags.getShouldSetImmutablePrototype() || flags.getPythonWorkers()));
    KJ_EXPECT(base.shouldUseSpecCompliantPropertyAttributes() ==
        flags.getSpecCompliantPropertyAttributes());
    KJ_EXPECT(base.isNodeJsCompatEnabled() == flags.getNodeJsCompatV2());
    KJ_EXPECT(base.isNodeJsProcessV2Enabled() == flags.getEnableNodeJsProcessV2());
    KJ_EXPECT(
        base.isRequireReturnsDefaultExportEnabled() == flags.getRequireReturnsDefaultExport());
    KJ_EXPECT(js.getThrowOnUnrecognizedImportAssertion() ==
        flags.getThrowOnUnrecognizedImportAssertion());
    KJ_EXPECT(base.isTopLevelAwaitEnabled() == !flags.getNoTopLevelAwaitInRequire());
    KJ_EXPECT(js.isUsingEnhancedErrorSerialization() == flags.getEnhancedErrorSerialization());
    KJ_EXPECT(js.isUsingFastJsgStruct() == flags.getFastJsgStruct());
    KJ_EXPECT(js.isWasmEsmIntegrationBuiltinsEnabled() == flags.getWasmEsmIntegrationBuiltins());
  });
}

KJ_TEST("JSG isolate settings with all compatibility flags false") {
  capnp::MallocMessageBuilder message;
  checkSettings(message.getRoot<CompatibilityFlags>());
}

KJ_TEST("JSG isolate settings follow individually selected compatibility flags") {
  for (auto setFlag: FLAG_SETTERS) {
    capnp::MallocMessageBuilder message;
    auto flags = message.initRoot<CompatibilityFlags>();
    (flags.*setFlag)(true);
    checkSettings(flags.asReader());
  }
}

KJ_TEST("JSG isolate settings with all relevant compatibility flags true") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  for (auto setFlag: FLAG_SETTERS) {
    (flags.*setFlag)(true);
  }
  checkSettings(flags.asReader());
}

}  // namespace
}  // namespace workerd
