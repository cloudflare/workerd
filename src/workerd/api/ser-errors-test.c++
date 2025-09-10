#include <workerd/io/features.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

KJ_TEST("Stacks not preserved in untrusted deserialization") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setEnhancedErrorSerialization(true);
  flags.setWorkerdExperimental(true);
  auto t = TestFixture({.featureFlags = flags.asReader()});

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto obj = KJ_ASSERT_NONNULL(env.js.typeError(""_kj).tryCast<jsg::JsObject>());
    obj.set(env.js, "foo"_kj, env.js.str("bar"_kj));
    obj.set(env.js, "stack"_kj, env.js.str("test stack"_kj));
    auto stack = obj.get(env.js, "stack"_kj);

    KJ_ASSERT(FeatureFlags::get(env.js).getEnhancedErrorSerialization());

    jsg::Serializer ser(env.js);
    ser.write(env.js, obj);
    auto content = ser.release();
    {
      // Untrusted... stack must not be preserved.
      jsg::Deserializer deser(env.js, content.data, kj::none, kj::none,
          jsg::Deserializer::Options{.preserveStackInErrors = false});
      auto val = KJ_ASSERT_NONNULL(deser.readValue(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = val.get(env.js, "stack"_kj);
      KJ_ASSERT(!checkedStack.strictEquals(stack));
    }
    {
      // Trusted ... stack must be preserved.
      jsg::Deserializer deser(env.js, content.data, kj::none, kj::none,
          jsg::Deserializer::Options{.preserveStackInErrors = true});
      auto val = KJ_ASSERT_NONNULL(deser.readValue(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = val.get(env.js, "stack"_kj);
      KJ_ASSERT(checkedStack.strictEquals(stack));
    }
    {
      // When using structuredClone, stacks are preserved by default.
      auto obj = KJ_ASSERT_NONNULL(env.js.typeError(""_kj).tryCast<jsg::JsObject>());
      obj.set(env.js, "name"_kj, env.js.str("CustomError"_kj));
      obj.set(env.js, "stack"_kj, env.js.str("test stack"_kj));
      obj.set(env.js, "foo"_kj, env.js.str("bar"_kj));

      auto other = KJ_ASSERT_NONNULL(jsg::structuredClone(env.js, obj).tryCast<jsg::JsObject>());
      auto checkedStack = other.get(env.js, "stack"_kj);
      KJ_ASSERT(checkedStack.strictEquals(obj.get(env.js, "stack"_kj)));
      KJ_ASSERT(other.get(env.js, "foo"_kj).strictEquals(obj.get(env.js, "foo"_kj)));
      KJ_ASSERT(other.get(env.js, "name"_kj).strictEquals(obj.get(env.js, "name"_kj)));
    }
  });
}

KJ_TEST("Stacks preserved by default when using regular deserialization") {
  auto t = TestFixture();

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto obj = KJ_ASSERT_NONNULL(env.js.typeError(""_kj).tryCast<jsg::JsObject>());
    obj.set(env.js, "foo"_kj, env.js.str("bar"_kj));
    obj.set(env.js, "stack"_kj, env.js.str("test stack"_kj));
    auto stack = obj.get(env.js, "stack"_kj);

    KJ_ASSERT(!FeatureFlags::get(env.js).getEnhancedErrorSerialization());

    jsg::Serializer ser(env.js);
    ser.write(env.js, obj);
    auto content = ser.release();
    {
      // By default, stacks are preserved.
      jsg::Deserializer deser(env.js, content.data);
      auto val = KJ_ASSERT_NONNULL(deser.readValue(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = val.get(env.js, "stack"_kj);
      KJ_ASSERT(checkedStack.strictEquals(stack));
    }
    {
      // Trusted ... stack must be preserved.
      jsg::Deserializer deser(env.js, content.data, kj::none, kj::none,
          // The option is ignored since the compat flag is off
          jsg::Deserializer::Options{.preserveStackInErrors = false});
      auto val = KJ_ASSERT_NONNULL(deser.readValue(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = val.get(env.js, "stack"_kj);
      KJ_ASSERT(checkedStack.strictEquals(stack));
    }
  });
}

KJ_TEST("Tunneled exceptions do not preserve stack by default") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setEnhancedErrorSerialization(true);
  flags.setWorkerdExperimental(true);
  auto t = TestFixture({.featureFlags = flags.asReader()});

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto obj = KJ_ASSERT_NONNULL(env.js.typeError("abc"_kj).tryCast<jsg::JsObject>());
    obj.set(env.js, "name"_kj, env.js.str("CustomError"_kj));
    obj.set(env.js, "foo"_kj, env.js.str("bar"_kj));
    obj.set(env.js, "stack"_kj, env.js.str("test stack"_kj));
    auto stack = obj.get(env.js, "stack"_kj);

    KJ_ASSERT(FeatureFlags::get(env.js).getEnhancedErrorSerialization());

    {
      // Untrusted... stack must not be preserved.
      kj::Exception ex = env.js.exceptionToKj(obj);
      auto val = env.js.exceptionToJsValue(kj::mv(ex));
      auto obj = KJ_ASSERT_NONNULL(val.getHandle(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = obj.get(env.js, "stack"_kj);
      KJ_ASSERT(!checkedStack.strictEquals(stack));
      KJ_ASSERT(obj.get(env.js, "name"_kj).strictEquals(env.js.str("CustomError"_kj)));
      KJ_ASSERT(obj.get(env.js, "message"_kj).strictEquals(env.js.str("abc"_kj)));
      KJ_ASSERT(obj.get(env.js, "foo"_kj).strictEquals(env.js.str("bar"_kj)));
    }
    {
      // Trusted... stack must be preserved.
      kj::Exception ex = env.js.exceptionToKj(obj);
      auto val = env.js.exceptionToJsValue(kj::mv(ex), {.trusted = true});
      auto obj = KJ_ASSERT_NONNULL(val.getHandle(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = obj.get(env.js, "stack"_kj);
      KJ_ASSERT(checkedStack.strictEquals(stack));
      KJ_ASSERT(obj.get(env.js, "name"_kj).strictEquals(env.js.str("CustomError"_kj)));
      KJ_ASSERT(obj.get(env.js, "message"_kj).strictEquals(env.js.str("abc"_kj)));
      KJ_ASSERT(obj.get(env.js, "foo"_kj).strictEquals(env.js.str("bar"_kj)));
    }
    {
      // Ignore detail means we reconstruct the error without the serialized detail
      kj::Exception e = env.js.exceptionToKj(obj);
      auto val = env.js.exceptionToJsValue(kj::mv(e), {.ignoreDetail = true});
      auto obj = KJ_ASSERT_NONNULL(val.getHandle(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = obj.get(env.js, "stack"_kj);
      KJ_ASSERT(!checkedStack.strictEquals(stack));
      KJ_ASSERT(obj.get(env.js, "name"_kj).strictEquals(env.js.str("Error"_kj)));
      KJ_ASSERT(obj.get(env.js, "message"_kj).strictEquals(env.js.str("CustomError: abc"_kj)));
      KJ_ASSERT(obj.get(env.js, "foo"_kj).strictEquals(env.js.undefined()));
    }
  });
}

KJ_TEST("Tunneled exceptions do not preserve stack by default") {
  auto t = TestFixture();

  t.runInIoContext([](const workerd::TestFixture::Environment& env) {
    auto obj = KJ_ASSERT_NONNULL(env.js.typeError("abc"_kj).tryCast<jsg::JsObject>());
    obj.set(env.js, "name"_kj, env.js.str("CustomError"_kj));
    obj.set(env.js, "foo"_kj, env.js.str("bar"_kj));
    obj.set(env.js, "stack"_kj, env.js.str("test stack"_kj));
    auto stack = obj.get(env.js, "stack"_kj);

    KJ_ASSERT(!FeatureFlags::get(env.js).getEnhancedErrorSerialization());

    {
      kj::Exception ex = env.js.exceptionToKj(obj);
      auto val = env.js.exceptionToJsValue(kj::mv(ex));
      auto obj = KJ_ASSERT_NONNULL(val.getHandle(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = obj.get(env.js, "stack"_kj);
      KJ_ASSERT(!checkedStack.strictEquals(stack));
      KJ_ASSERT(obj.get(env.js, "name"_kj).strictEquals(env.js.str("Error"_kj)));
      KJ_ASSERT(obj.get(env.js, "message"_kj).strictEquals(env.js.str("CustomError: abc"_kj)));
      KJ_ASSERT(obj.get(env.js, "foo"_kj).strictEquals(env.js.undefined()));
    }
    {
      kj::Exception ex = env.js.exceptionToKj(obj);
      auto val = env.js.exceptionToJsValue(kj::mv(ex), {.ignoreDetail = true});
      auto obj = KJ_ASSERT_NONNULL(val.getHandle(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = obj.get(env.js, "stack"_kj);
      KJ_ASSERT(!checkedStack.strictEquals(stack));
      KJ_ASSERT(obj.get(env.js, "name"_kj).strictEquals(env.js.str("Error"_kj)));
      KJ_ASSERT(obj.get(env.js, "message"_kj).strictEquals(env.js.str("CustomError: abc"_kj)));
      KJ_ASSERT(obj.get(env.js, "foo"_kj).strictEquals(env.js.undefined()));
    }
    {
      kj::Exception ex = env.js.exceptionToKj(obj);
      auto val = env.js.exceptionToJsValue(kj::mv(ex), {.trusted = true});
      auto obj = KJ_ASSERT_NONNULL(val.getHandle(env.js).tryCast<jsg::JsObject>());
      auto checkedStack = obj.get(env.js, "stack"_kj);
      KJ_ASSERT(checkedStack.strictEquals(stack));
      KJ_ASSERT(obj.get(env.js, "name"_kj).strictEquals(env.js.str("Error"_kj)));
      KJ_ASSERT(obj.get(env.js, "message"_kj).strictEquals(env.js.str("abc"_kj)));
      KJ_ASSERT(obj.get(env.js, "foo"_kj).strictEquals(env.js.undefined()));
    }
  });
}

}  // namespace
}  // namespace workerd::api
