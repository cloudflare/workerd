// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An isolate group shares one read-only heap among its isolates, set up from the snapshot of
// whichever isolate joined first, and drops it when its last isolate is disposed. Booting from a
// custom snapshot therefore needs a group with no other live isolates whenever new groups can't
// be created. This test lives in its own binary because the Evaluator used elsewhere keeps an
// isolate alive for the whole process.

#include "dom-exception.h"
#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct DOMExceptionContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(DOMExceptionContext) {
    JSG_NESTED_TYPE(DOMException);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DOMExceptionIsolate, DOMExceptionContext);

KJ_TEST("A DOMException keeps its brand through a context snapshot") {
  kj::Own<SnapshotArtifact> artifact;
  {
    DOMExceptionIsolate zygote(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>(),
        defaultExternalStringAllocator(), {}, true,
        SnapshotConfig(MutableSnapshot{.artifact = kj::atomicRefcounted<SnapshotArtifact>()}));
    zygote.runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
      v8::Global<v8::Context> contextHandle;
      lock.withinHandleScope([&] {
        auto context = lock.template newContext<DOMExceptionContext>();
        auto handle = context.getHandle(lock.v8Isolate);
        IsolateBase::from(lock.v8Isolate).setSnapshotDefaultContext(handle);
        JSG_WITHIN_CONTEXT_SCOPE(lock, handle, [&](jsg::Lock& js) {
          js.global().set(
              js, "saved"_kj, js.domException(kj::str("NotFoundError"), kj::str("gone")));
        });
        contextHandle = context.extractContextGlobalForSnapshot();
      });
      auto& base = IsolateBase::from(lock.v8Isolate);
      base.createSnapshotBlob(kj::mv(contextHandle), {});
      artifact = base.extractSnapshotArtifact();
    });
  }

  DOMExceptionIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>(),
      defaultExternalStringAllocator(), {}, true,
      SnapshotConfig(FinalizedSnapshot{.artifact = artifact->addRef()}));
  isolate.runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto saved = js.global().get(js, "saved"_kj);
      auto fields = KJ_ASSERT_NONNULL(tryGetDOMExceptionFields(js.v8Isolate, saved));
      KJ_EXPECT(JsString(fields.name).toString(js) == "NotFoundError"_kj);
      KJ_EXPECT(JsString(fields.message).toString(js) == "gone"_kj);
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
