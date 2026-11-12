// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What native code sees of a DOMException in a context with the per-isolate bootstrap: the bytes
// it serializes to, what reading tags 7 and 8 creates, the kj::Exception it tunnels as, and how
// kj::str() and reportError() describe it, and the bytes the TypeScript implementation's
// QuotaExceededError serializes to. src/tests/dom-exception/ covers what JavaScript sees.

#include <workerd/jsg/dom-exception.h>
#include <workerd/jsg/script.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

void runWithBootstrap(kj::Function<void(jsg::Lock& js)> body) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  TestFixture fixture({.featureFlags = flags.asReader()});
  fixture.runInIoContext([&](const TestFixture::Environment& env) { body(env.js); });
}

jsg::JsValue eval(jsg::Lock& js, kj::StringPtr code) {
  return jsg::NonModuleScript::compile(js, code, "dom-exception-test.js"_kj).runAndReturn(js);
}

kj::Array<kj::byte> serialize(jsg::Lock& js, jsg::JsValue value) {
  jsg::Serializer serializer(js);
  serializer.write(js, value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

// `name`, `message` and `stack` as tag 8 writes them, after the V8 header (version 16).
kj::Array<kj::byte> tag8(kj::StringPtr name, kj::StringPtr message, kj::StringPtr stack) {
  kj::Vector<kj::byte> bytes;
  bytes.addAll(kj::arr<kj::byte>(0xff, 0x10, 0x5c, 8));
  for (const auto& field: {name, message, stack}) {
    KJ_ASSERT(field.size() < 0x80);
    bytes.add(field.size());
    bytes.addAll(field.asBytes());
  }
  return bytes.releaseAsArray();
}

// The properties a test checks of a deserialized DOMException, joined with commas.
kj::String describe(jsg::Lock& js, jsg::JsValue value) {
  auto fn = KJ_ASSERT_NONNULL(eval(js, R"js((e) => [
    e instanceof DOMException,
    Object.getPrototypeOf(e) === DOMException.prototype,
    e.name, e.message, e.code, e.stack,
  ].join())js"_kj)
                                  .tryCast<jsg::JsFunction>());
  return fn.call(js, js.undefined(), value).toString(js);
}

KJ_TEST("A DOMException serializes as tag 8") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = serialize(js, eval(js, R"js(
      const e = new DOMException('m', 'AbortError');
      e.stack = 'S';
      e
    )js"_kj));
    KJ_EXPECT(bytes == tag8("AbortError"_kj, "m"_kj, "S"_kj), bytes);
  });
}

KJ_TEST("A DOMException serializes its own name and message, not properties that shadow them") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = serialize(js, eval(js, R"js(
      class Sub extends DOMException {}
      const e = new Sub('m', 'AbortError');
      Object.defineProperty(e, 'name', { value: 'Other' });
      Object.defineProperty(e, 'message', { value: 'other' });
      e.extra = 1;
      e.stack = 'S';
      e
    )js"_kj));
    KJ_EXPECT(bytes == tag8("AbortError"_kj, "m"_kj, "S"_kj), bytes);
  });
}

KJ_TEST("A DOMException that native code created serializes as tag 8") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = serialize(js, eval(js, R"js(
      let e;
      try { structuredClone(() => {}); } catch (x) { e = x; }
      e.stack = 'S';
      e
    )js"_kj));
    KJ_EXPECT(
        bytes == tag8("DataCloneError"_kj, "() => {} could not be cloned."_kj, "S"_kj), bytes);
  });
}

KJ_TEST("Reading tag 8 creates a DOMException") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = tag8("NotFoundError"_kj, "m"_kj, "S"_kj);
    jsg::Deserializer deserializer(js, bytes);
    auto result = describe(js, deserializer.readValue(js));
    KJ_EXPECT(result == "true,true,NotFoundError,m,8,S"_kj, result);
  });
}

KJ_TEST("Reading tag 7 creates a DOMException") {
  runWithBootstrap([](jsg::Lock& js) {
    // The name, then an object holding the message and the stack.
    // clang-format off
    kj::byte bytes[] = {
      0xff, 0x0f, 0x5c, 7,
      13, 'N', 'o', 't', 'F', 'o', 'u', 'n', 'd', 'E', 'r', 'r', 'o', 'r',
      0x6f,
      0x22, 7, 'm', 'e', 's', 's', 'a', 'g', 'e', 0x22, 1, 'm',
      0x22, 5, 's', 't', 'a', 'c', 'k', 0x22, 1, 'S',
      0x7b, 2,
    };
    // clang-format on
    jsg::Deserializer deserializer(js, bytes);
    auto result = describe(js, deserializer.readValue(js));
    KJ_EXPECT(result == "true,true,NotFoundError,m,8,S"_kj, result);
  });
}

KJ_TEST("A thrown DOMException tunnels as jsg.DOMException(name)") {
  runWithBootstrap([](jsg::Lock& js) {
    auto exception = js.exceptionToKj(eval(js, "new DOMException('m', 'AbortError')"_kj));
    KJ_EXPECT(exception.getType() == kj::Exception::Type::FAILED);
    KJ_EXPECT(exception.getDescription() == "jsg.DOMException(AbortError): m"_kj,
        exception.getDescription());

    // A retryable one still tunnels as FAILED.
    auto aborted = js.exceptionToKj(eval(js, "AbortSignal.abort().reason"_kj));
    KJ_EXPECT(aborted.getType() == kj::Exception::Type::FAILED);
    KJ_EXPECT(
        aborted.getDescription() == "jsg.DOMException(AbortError): The operation was aborted"_kj,
        aborted.getDescription());

    // Its own name and message do not change it.
    auto shadowed = js.exceptionToKj(eval(js, R"js(
      const e = new DOMException('m', 'AbortError');
      Object.defineProperty(e, 'name', { value: 'Other' });
      e
    )js"_kj));
    KJ_EXPECT(shadowed.getDescription() == "jsg.DOMException(AbortError): m"_kj,
        shadowed.getDescription());
  });
}

KJ_TEST("A tunneled jsg.DOMException(name) becomes a DOMException") {
  runWithBootstrap([](jsg::Lock& js) {
    auto value =
        js.exceptionToJsValue(KJ_EXCEPTION(FAILED, "jsg.DOMException(NotFoundError): m"_kj));
    auto handle = jsg::JsValue(value.getHandle(js));
    auto fn = KJ_ASSERT_NONNULL(eval(js, R"js((e) => [
      e instanceof DOMException, e.name, e.message, e.code, typeof e.stack,
    ].join())js"_kj)
                                    .tryCast<jsg::JsFunction>());
    auto result = fn.call(js, js.undefined(), handle).toString(js);
    KJ_EXPECT(result == "true,NotFoundError,m,8,string"_kj, result);
  });
}

KJ_TEST("kj::str() describes a DOMException by its name and message") {
  runWithBootstrap([](jsg::Lock& js) {
    // The TypeScript DOMException's. The C++ one's name and message are lazy data properties,
    // which V8's description doesn't evaluate, so it is empty until JavaScript reads them.
    if (jsg::tryGetBootstrapDomExceptionPrototype(js) == kj::none) return;
    auto expectDescribed = [&](jsg::JsValue value, kj::StringPtr expected) {
      auto description = kj::str(value);
      KJ_EXPECT(description == expected, description);
    };
    expectDescribed(eval(js, "new DOMException('m', 'AbortError')"_kj), "AbortError: m"_kj);
    expectDescribed(eval(js, "new DOMException('', 'AbortError')"_kj), "AbortError"_kj);
    expectDescribed(eval(js, "new DOMException('m', '')"_kj), "m"_kj);
    expectDescribed(
        eval(js, "AbortSignal.abort().reason"_kj), "AbortError: The operation was aborted"_kj);
    expectDescribed(js.domException(kj::str("NotFoundError"), kj::str("m")), "NotFoundError: m"_kj);
  });
}

KJ_TEST("reportError() describes a DOMException by its name and message") {
  runWithBootstrap([](jsg::Lock& js) {
    kj::Vector<kj::String> reported;
    js.setErrorReporterCallback([&reported](jsg::Lock&, kj::String desc, const jsg::JsValue&,
                                    const jsg::JsMessage&) { reported.add(kj::mv(desc)); });
    js.reportError(eval(js, "new DOMException('m', 'AbortError')"_kj));
    js.reportError(eval(js, R"js(
      const e = new DOMException('m', 'NotFoundError');
      Object.defineProperty(e, 'name', { value: 'Other' });
      e
    )js"_kj));
    KJ_ASSERT(reported.size() == 2);
    KJ_EXPECT(reported[0] == "DOMException(AbortError): m"_kj, reported[0]);
    KJ_EXPECT(reported[1] == "DOMException(NotFoundError): m"_kj, reported[1]);
  });
}

// `s` as V8 writes a one-byte string.
kj::Array<kj::byte> v8String(kj::StringPtr s) {
  KJ_ASSERT(s.size() < 0x80);
  kj::Vector<kj::byte> bytes;
  bytes.addAll(kj::arr<kj::byte>(0x22, s.size()));
  bytes.addAll(s.asBytes());
  return bytes.releaseAsArray();
}

// `n`, a small integer, as V8 writes a Smi: zigzag-encoded.
kj::Array<kj::byte> v8Smi(int n) {
  uint32_t zigzag = n < 0 ? (static_cast<uint32_t>(-n) << 1) - 1 : static_cast<uint32_t>(n) << 1;
  KJ_ASSERT(zigzag < 0x80);
  return kj::arr<kj::byte>(0x49, zigzag);
}

kj::Array<kj::byte> v8Null() {
  return kj::arr<kj::byte>(0x30);
}

// A QuotaExceededError with stack 'S' as the TypeScript implementation writes it without enhanced
// error serialization, after the V8 header (version 16): an instance of a class implemented in
// JavaScript, with `messageForStack`, the value of its `message` property, and followed by its
// data, the object {message, quota, requested} with the given values.
kj::Array<kj::byte> quotaExceededError(kj::ArrayPtr<const kj::byte> message,
    kj::ArrayPtr<const kj::byte> quota,
    kj::ArrayPtr<const kj::byte> requested,
    kj::StringPtr messageForStack = "m"_kj) {
  kj::Vector<kj::byte> bytes;
  bytes.addAll(kj::arr<kj::byte>(0xff, 0x10, 0x5c, jsg::SERIALIZATION_TAG_JS_SERIALIZABLE));
  auto deserializeInfo = "dom-exception:QuotaExceededError"_kj;
  bytes.add(deserializeInfo.size());
  bytes.addAll(deserializeInfo.asBytes());
  // The flags: it has a message and a stack, which a reader keeps.
  bytes.add(7);
  bytes.addAll(v8String(messageForStack));
  bytes.addAll(v8String("S"_kj));
  bytes.add(0x6f);
  bytes.addAll(v8String("message"_kj));
  bytes.addAll(message);
  bytes.addAll(v8String("quota"_kj));
  bytes.addAll(quota);
  bytes.addAll(v8String("requested"_kj));
  bytes.addAll(requested);
  bytes.addAll(kj::arr<kj::byte>(0x7b, 3));
  return bytes.releaseAsArray();
}

// The name and message of the error that reading `bytes` throws.
kj::String readError(jsg::Lock& js, kj::ArrayPtr<const kj::byte> bytes) {
  JSG_TRY(js) {
    jsg::Deserializer deserializer(js, bytes);
    deserializer.readValue(js);
    return kj::str("nothing thrown");
  }
  JSG_CATCH(exception) {
    auto fn = KJ_ASSERT_NONNULL(
        eval(js, "(e) => `${e.name}: ${e.message}`"_kj).tryCast<jsg::JsFunction>());
    return fn.call(js, js.undefined(), jsg::JsValue(exception.getHandle(js))).toString(js);
  }
}

KJ_TEST("A QuotaExceededError serializes as a class implemented in JavaScript") {
  runWithBootstrap([](jsg::Lock& js) {
    if (jsg::tryGetBootstrapDomExceptionPrototype(js) == kj::none) return;
    auto bytes = serialize(js, eval(js, R"js(
      class Sub extends QuotaExceededError {}
      const e = new Sub('m', { quota: 1 });
      Object.defineProperty(e, 'message', { value: 'other' });
      e.extra = 1;
      e.stack = 'S';
      e
    )js"_kj));
    // The data holds the error's own message, and the message for the stack is the property's.
    auto expected = quotaExceededError(v8String("m"_kj), v8Smi(1), v8Null(), "other"_kj);
    KJ_EXPECT(bytes == expected, bytes, expected);
  });
}

KJ_TEST("Reading a QuotaExceededError creates one") {
  runWithBootstrap([](jsg::Lock& js) {
    if (jsg::tryGetBootstrapDomExceptionPrototype(js) == kj::none) return;
    auto bytes = quotaExceededError(v8String("m"_kj), v8Smi(1), v8Smi(2));
    jsg::Deserializer deserializer(js, bytes);
    auto fn = KJ_ASSERT_NONNULL(eval(js, R"js((e) => [
      Object.getPrototypeOf(e) === QuotaExceededError.prototype,
      Error.isError(e),
      e.name, e.message, e.code, e.quota, e.requested, e.stack,
    ].join())js"_kj)
                                    .tryCast<jsg::JsFunction>());
    auto result = fn.call(js, js.undefined(), deserializer.readValue(js)).toString(js);
    KJ_EXPECT(result == "true,true,QuotaExceededError,m,22,1,2,S"_kj, result);
  });
}

KJ_TEST("Reading a QuotaExceededError rejects data its constructor would") {
  runWithBootstrap([](jsg::Lock& js) {
    if (jsg::tryGetBootstrapDomExceptionPrototype(js) == kj::none) return;
    auto expectRejected = [&](kj::Array<kj::byte> bytes) {
      auto error = readError(js, bytes);
      KJ_EXPECT(
          error == "DataCloneError: Deserialization failed: invalid QuotaExceededError data."_kj,
          error);
    };
    // A double that isn't finite: Infinity.
    auto infinity = kj::arr<kj::byte>(0x4e, 0, 0, 0, 0, 0, 0, 0xf0, 0x7f);
    expectRejected(quotaExceededError(v8String("m"_kj), v8Smi(-1), v8Null()));
    expectRejected(quotaExceededError(v8String("m"_kj), v8Null(), v8Smi(-1)));
    expectRejected(quotaExceededError(v8String("m"_kj), v8Smi(2), v8Smi(1)));
    expectRejected(quotaExceededError(v8String("m"_kj), infinity, v8Null()));
    expectRejected(quotaExceededError(v8String("m"_kj), v8String("1"_kj), v8Null()));
    expectRejected(quotaExceededError(v8Smi(1), v8Null(), v8Null()));
  });
}

KJ_TEST("Without the TypeScript implementation, reading a QuotaExceededError fails") {
  runWithBootstrap([](jsg::Lock& js) {
    if (jsg::tryGetBootstrapDomExceptionPrototype(js) != kj::none) return;
    auto error = readError(js, quotaExceededError(v8String("m"_kj), v8Smi(1), v8Smi(2)));
    KJ_EXPECT(error ==
            "DataCloneError: Deserialization failed: unknown class implemented in JavaScript."_kj,
        error);
  });
}

}  // namespace
}  // namespace workerd::api
