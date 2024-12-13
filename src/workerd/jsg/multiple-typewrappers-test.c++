#include <workerd/io/compatibility-date.h>
#include <workerd/io/observer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::jsg::test {
jsg::V8System v8System;

struct TestApi1: public jsg::Object {
  TestApi1() = default;
  TestApi1(jsg::Lock&, const jsg::Url&) {}
  int test1(jsg::Lock& js) {
    return 1;
  }

  int test2(jsg::Lock& js) {
    return 2;
  }
  static jsg::Ref<TestApi1> constructor() {
    return jsg::alloc<TestApi1>();
  }

  JSG_RESOURCE_TYPE(TestApi1, workerd::CompatibilityFlags::Reader flags) {
    if (flags.getPythonWorkers()) {
      JSG_METHOD(test2);
    } else {
      JSG_METHOD(test1);
    }
  }
};
struct TestApi2: public jsg::Object {
  TestApi2() = default;
  TestApi2(jsg::Lock&, const jsg::Url&) {}
  int test1(jsg::Lock& js) {
    return 1;
  }

  int test2(jsg::Lock& js) {
    return 2;
  }
  static jsg::Ref<TestApi2> constructor() {
    return jsg::alloc<TestApi2>();
  }

  JSG_RESOURCE_TYPE(TestApi2, workerd::CompatibilityFlags::Reader flags) {
    if (flags.getPythonWorkers()) {
      JSG_METHOD(test2);
    } else {
      JSG_METHOD(test1);
    }
  }
};

struct BaseTestContext: public jsg::Object, public jsg::ContextGlobal {
  int test1(jsg::Lock& js) {
    return 1;
  }

  int test2(jsg::Lock& js) {
    return 2;
  }
  JSG_RESOURCE_TYPE(BaseTestContext, workerd::CompatibilityFlags::Reader flags) {
    if (flags.getPythonWorkers()) {
      JSG_METHOD(test2);
    } else {
      JSG_METHOD(test1);
    }
    JSG_NESTED_TYPE(TestApi1);
  }
};

struct TestContext: public BaseTestContext {
  int test3(jsg::Lock& js) {
    return 3;
  }

  int test4(jsg::Lock& js) {
    return 4;
  }
  JSG_RESOURCE_TYPE(TestContext, workerd::CompatibilityFlags::Reader flags) {
    JSG_INHERIT(BaseTestContext);
    if (flags.getPythonWorkers()) {
      JSG_METHOD(test4);
    } else {
      JSG_METHOD(test3);
    }
    JSG_NESTED_TYPE(TestApi2);
  }
};

JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext, BaseTestContext, TestApi1, TestApi2);

class Configuration {
 public:
  Configuration(workerd::CompatibilityFlags::Reader& flags): flags(flags) {}
  operator const workerd::CompatibilityFlags::Reader() const {
    return flags;
  }

 private:
  workerd::CompatibilityFlags::Reader& flags;
};

void expectEval(
    jsg::Lock& js, kj::StringPtr code, kj::StringPtr expectedType, kj::StringPtr expectedValue) {
  // Create a string containing the JavaScript source code.
  v8::Local<v8::String> source = jsg::v8Str(js.v8Isolate, code);

  // Compile the source code.
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(js.v8Context(), source).ToLocal(&script)) {
    KJ_FAIL_EXPECT("code didn't parse", code);
    return;
  }

  v8::TryCatch catcher(js.v8Isolate);

  // Run the script to get the result.
  v8::Local<v8::Value> result;
  if (script->Run(js.v8Context()).ToLocal(&result)) {
    v8::String::Utf8Value type(js.v8Isolate, result->TypeOf(js.v8Isolate));
    v8::String::Utf8Value value(js.v8Isolate, result);

    KJ_EXPECT(*type == expectedType, *type, expectedType);
    KJ_EXPECT(*value == expectedValue, *value, expectedValue);
  } else if (catcher.HasCaught()) {
    v8::String::Utf8Value message(js.v8Isolate, catcher.Exception());

    KJ_EXPECT(expectedType == "throws", expectedType, catcher.Exception());
    KJ_EXPECT(*message == expectedValue, *message, expectedValue);
  } else {
    KJ_FAIL_EXPECT("returned empty handle but didn't throw exception?");
  }
}

KJ_TEST("Create a context with a configuration then create a default context with another") {
  capnp::MallocMessageBuilder flagsArena;
  auto flags = flagsArena.initRoot<::workerd::CompatibilityFlags>();
  auto flagsReader = flags.asReader();
  TestIsolate isolate(v8System, Configuration(flagsReader), kj::heap<IsolateObserver>(), {}, false);
  isolate.runInLockScope([&](TestIsolate::Lock& lock) {
    jsg::JsContext<TestContext> context =
        lock.newContextWithConfiguration<TestContext>(Configuration(flagsReader), {});
    v8::Local<v8::Context> ctx = context.getHandle(lock);
    KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
    v8::Context::Scope scope(ctx);

    expectEval(lock, "test1()", "number", "1");
    expectEval(lock, "test2()", "throws", "ReferenceError: test2 is not defined");
    expectEval(lock, "test3()", "number", "3");
    expectEval(lock, "test4()", "throws", "ReferenceError: test4 is not defined");
    expectEval(lock, "new TestApi1().test1()", "number", "1");
    expectEval(lock, "new TestApi1().test2()", "throws",
        "TypeError: (intermediate value).test2 is not a function");
    expectEval(lock, "new TestApi2().test1()", "number", "1");
    expectEval(lock, "new TestApi2().test2()", "throws",
        "TypeError: (intermediate value).test2 is not a function");
  });
  flags.setPythonWorkers(true);
  isolate.instantiateDefaultWrapper(Configuration(flagsReader));
  isolate.runInLockScope([&](TestIsolate::Lock& lock) {
    jsg::JsContext<TestContext> context = lock.newContext<TestContext>();
    v8::Local<v8::Context> ctx = context.getHandle(lock);
    KJ_ASSERT(!ctx.IsEmpty(), "unable to enter invalid v8::Context");
    v8::Context::Scope scope(ctx);

    expectEval(lock, "test1()", "throws", "ReferenceError: test1 is not defined");
    expectEval(lock, "test2()", "number", "2");
    expectEval(lock, "test3()", "throws", "ReferenceError: test3 is not defined");
    expectEval(lock, "test4()", "number", "4");
    expectEval(lock, "new TestApi1().test1()", "throws",
        "TypeError: (intermediate value).test1 is not a function");
    expectEval(lock, "new TestApi1().test2()", "number", "2");
    expectEval(lock, "new TestApi2().test1()", "throws",
        "TypeError: (intermediate value).test1 is not a function");
    expectEval(lock, "new TestApi2().test2()", "number", "2");
  });
}

}  // namespace workerd::jsg::test
