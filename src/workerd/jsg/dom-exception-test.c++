// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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

KJ_TEST("DOMException's prototype is ErrorPrototype") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.expectEval(
      "Object.getPrototypeOf(DOMException.prototype) === Error.prototype", "boolean", "true");
}

KJ_TEST("DOMException has a stack property") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.expectEval("function throwError() { throw new DOMException('test error') }\n"
               "try { throwError() } catch (e) { e.stack }",
      "string",
      "Error: test error\n"
      "    at throwError (<anonymous>:1:31)\n"
      "    at <anonymous>:2:7");
}

KJ_TEST("DOMException has legacy code constants") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  // Test a subset of error codes that commonly appear in web APIs.
  e.expectEval("DOMException.INDEX_SIZE_ERR === 1", "boolean", "true");
  e.expectEval("DOMException.NOT_SUPPORTED_ERR === 9", "boolean", "true");
  e.expectEval("DOMException.SYNTAX_ERR === 12", "boolean", "true");
  e.expectEval("DOMException.INVALID_ACCESS_ERR === 15", "boolean", "true");
  e.expectEval("DOMException.TYPE_MISMATCH_ERR === 17", "boolean", "true");
  e.expectEval("DOMException.QUOTA_EXCEEDED_ERR === 22", "boolean", "true");
  e.expectEval("DOMException.DATA_CLONE_ERR === 25", "boolean", "true");
}

KJ_TEST("jsg::Lock domException") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto exception = js.domException(kj::str("foo"), kj::str("bar"));

      KJ_ASSERT(exception.has(js, "stack"_kj));
      KJ_ASSERT(exception.get(js, "name"_kj).toString(js) == "foo"_kj);
      KJ_ASSERT(exception.get(js, "message"_kj).toString(js) == "bar"_kj);
      KJ_ASSERT(exception.get(js, "code"_kj).toString(js) == "0"_kj);
      auto constructor =
          KJ_ASSERT_NONNULL(js.global().get(js, "DOMException"_kj).tryCast<JsObject>());
      KJ_ASSERT(exception.getPrototype(js) == constructor.get(js, "prototype"_kj));
    });
  });
}

KJ_TEST("DOMExceptions are recognized by their brand") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto fieldsOf = [&](kj::StringPtr code) -> kj::Maybe<kj::String> {
        auto script = jsg::check(v8::Script::Compile(js.v8Context(), js.str(code)));
        auto value = jsg::check(script->Run(js.v8Context()));
        return tryGetDOMExceptionFields(js.v8Isolate, value).map([&](DOMExceptionFields fields) {
          return kj::str(
              JsString(fields.name).toString(js), "/", JsString(fields.message).toString(js));
        });
      };

      KJ_EXPECT(KJ_ASSERT_NONNULL(fieldsOf("new DOMException('m', 'AbortError')"_kj)) ==
          "AbortError/m"_kj);
      KJ_EXPECT(KJ_ASSERT_NONNULL(fieldsOf("new (class extends DOMException {})('m')"_kj)) ==
          "Error/m"_kj);
      // The brand, not the JavaScript properties, which can be redefined.
      KJ_EXPECT(KJ_ASSERT_NONNULL(fieldsOf(
                    "const e = new DOMException('m', 'DataError');"
                    "Object.defineProperty(e, 'name', { value: 'X' }); e"_kj)) == "DataError/m"_kj);
      // Only an object's own brand counts.
      KJ_EXPECT(fieldsOf("Object.create(new DOMException('m'))"_kj) == kj::none);
      KJ_EXPECT(fieldsOf("({ name: 'AbortError', message: 'm' })"_kj) == kj::none);
      KJ_EXPECT(fieldsOf("new Error('m')"_kj) == kj::none);

      auto created = js.domException(kj::str("NotFoundError"), kj::str("gone"));
      auto fields = KJ_ASSERT_NONNULL(tryGetDOMExceptionFields(js.v8Isolate, created));
      KJ_EXPECT(JsString(fields.name).toString(js) == "NotFoundError"_kj);
      KJ_EXPECT(JsString(fields.message).toString(js) == "gone"_kj);
    });
  });
}

KJ_TEST("DOMExceptions are recognized while an exception is pending") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto created = js.domException(kj::str("AbortError"), kj::str("stopped"));
      v8::TryCatch tryCatch(js.v8Isolate);
      js.v8Isolate->ThrowError(js.str("pending"_kj));
      KJ_ASSERT(js.v8Isolate->HasPendingException());
      auto fields = KJ_ASSERT_NONNULL(tryGetDOMExceptionFields(js.v8Isolate, created));
      KJ_EXPECT(JsString(fields.name).toString(js) == "AbortError"_kj);
      KJ_EXPECT(JsString(fields.message).toString(js) == "stopped"_kj);
      KJ_EXPECT(js.v8Isolate->HasPendingException());
    });
  });
}

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
