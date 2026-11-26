// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What native code sees of a DOMException in a context with the per-isolate bootstrap: the bytes
// it serializes to, what reading tags 7, 8 and 24 creates, the kj::Exception it tunnels as, and
// how kj::str() and reportError() describe it, and the bytes V8's QuotaExceededError serializes to
// and what reading them creates. src/tests/dom-exception/ covers what JavaScript sees.

#include <workerd/jsg/script.h>
#include <workerd/jsg/ser.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/message.h>
#include <kj/test.h>

#include <bit>

namespace workerd::api {
namespace {

void runWithBootstrap(kj::Function<void(jsg::Lock& js)> body,
    jsg::EnhancedErrorSerialization enhancedErrorSerialization =
        jsg::EnhancedErrorSerialization::NO) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setEnhancedErrorSerialization(enhancedErrorSerialization.toBool());
  TestFixture fixture({.featureFlags = flags.asReader()});
  fixture.runInIoContext([&](const TestFixture::Environment& env) { body(env.js); });
}

bool usingV8DomException() {
  return util::Autogate::isEnabled(util::AutogateKey::V8_DOM_EXCEPTION);
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

// Serialized data as a Serializer writes it: the V8 header (version 16) and a host object's tag,
// then what each call adds.
class Bytes {
 public:
  explicit Bytes(uint32_t tag) {
    bytes.addAll(kj::arr<kj::byte>(0xff, 0x10, 0x5c));
    varint(tag);
  }

  // `n` as V8 writes a varint, as writeRawUint32() and writeRawUint64() do.
  Bytes& varint(uint64_t n) {
    do {
      bytes.add((n & 0x7f) | (n >= 0x80 ? 0x80 : 0));
      n >>= 7;
    } while (n != 0);
    return *this;
  }

  // `text` as writeLengthDelimited() writes it.
  Bytes& lengthDelimited(kj::StringPtr text) {
    varint(text.size());
    bytes.addAll(text.asBytes());
    return *this;
  }

  // `text`, which is Latin-1, as V8 writes a string value.
  Bytes& string(kj::StringPtr text) {
    bytes.add(0x22);
    return lengthDelimited(text);
  }

  // Bytes of V8's format that the test spells out.
  Bytes& raw(std::initializer_list<kj::byte> raw) {
    bytes.addAll(raw);
    return *this;
  }

  kj::Array<kj::byte> array() const {
    return kj::heapArray(bytes.asPtr());
  }

 private:
  kj::Vector<kj::byte> bytes;
};

// V8's values that tests write: undefined, null, the int32 1, and a reference to the first object
// read, which is the DOMException a test reads.
constexpr kj::byte UNDEFINED = 0x5f;
constexpr kj::byte NULL_VALUE = 0x30;
constexpr std::initializer_list<kj::byte> ONE = {0x49, 2};
constexpr std::initializer_list<kj::byte> FIRST_OBJECT = {0x5e, 0};

// A DOMException as tag 24 writes it, as far as its name `name` and its message 'm'.
Bytes tag24(kj::StringPtr name) {
  Bytes bytes(jsg::SERIALIZATION_TAG_DOM_EXCEPTION_V3);
  bytes.lengthDelimited(name).lengthDelimited("m"_kj);
  return bytes;
}

// A QuotaExceededError as tag 25 writes it, with the message 'm' and the stack 'S', `flags`, the
// bits of each amount the flags say it has, and no other properties.
kj::Array<kj::byte> tag25(uint32_t flags, std::initializer_list<double> amounts) {
  Bytes bytes(jsg::SERIALIZATION_TAG_QUOTA_EXCEEDED_ERROR);
  bytes.lengthDelimited("QuotaExceededError"_kj).lengthDelimited("m"_kj).varint(1).string("S"_kj);
  bytes.varint(flags);
  for (auto amount: amounts) bytes.varint(std::bit_cast<uint64_t>(amount));
  return bytes.varint(0).array();
}

constexpr uint32_t HAS_QUOTA = jsg::QUOTA_EXCEEDED_ERROR_HAS_QUOTA;
constexpr uint32_t HAS_REQUESTED = jsg::QUOTA_EXCEEDED_ERROR_HAS_REQUESTED;

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

KJ_TEST("The C++ DOMException serializes as tag 8") {
  runWithBootstrap([](jsg::Lock& js) {
    if (usingV8DomException()) return;
    // Its own name and message, not properties that shadow them, and no other own properties.
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

KJ_TEST("V8's DOMException serializes as tag 24") {
  runWithBootstrap([](jsg::Lock& js) {
    if (!usingV8DomException()) return;
    // Its own name and message, not properties that shadow them, and, without enhanced error
    // serialization, no other own properties.
    auto bytes = serialize(js, eval(js, R"js(
      class Sub extends DOMException {}
      const e = new Sub('m', 'AbortError');
      Object.defineProperty(e, 'name', { value: 'Other' });
      Object.defineProperty(e, 'message', { value: 'other' });
      e.extra = 1;
      e.stack = 'S';
      e
    )js"_kj));
    auto expected = tag24("AbortError"_kj).varint(1).string("S"_kj).varint(0).array();
    KJ_EXPECT(bytes == expected, bytes, expected);
  });
}

KJ_TEST("With enhanced error serialization, V8's DOMException serializes its own properties") {
  runWithBootstrap([](jsg::Lock& js) {
    if (!usingV8DomException()) return;
    // Those keyed by a string other than `stack`, as for other errors, those that shadow its
    // attributes included.
    auto bytes = serialize(js, eval(js, R"js(
      const e = new DOMException('m', 'AbortError');
      e.stack = 'S';
      Object.defineProperty(e, 'name', { value: 'Other' });
      e.extra = 1;
      e[Symbol('s')] = 1;
      e[0] = 1;
      e
    )js"_kj));
    auto expected = tag24("AbortError"_kj)
                        .varint(1)
                        .string("S"_kj)
                        .varint(2)
                        .string("name"_kj)
                        .string("Other"_kj)
                        .string("extra"_kj)
                        .raw(ONE)
                        .array();
    KJ_EXPECT(bytes == expected, bytes, expected);
  }, jsg::EnhancedErrorSerialization::YES);
}

KJ_TEST("V8's DOMException serializes its stack as whatever value it holds") {
  runWithBootstrap([](jsg::Lock& js) {
    if (!usingV8DomException()) return;
    // `change` changes the stack of `e`.
    auto expectStack = [&](kj::StringPtr change, Bytes& expected) {
      auto code = kj::str(
          "(() => { const e = new DOMException('m', 'AbortError'); ", change, "; return e; })()");
      auto bytes = serialize(js, eval(js, code));
      auto array = expected.varint(0).array();
      KJ_EXPECT(bytes == array, change, bytes, array);
    };
    expectStack("delete e.stack"_kj, tag24("AbortError"_kj).varint(0));
    expectStack("e.stack = undefined"_kj, tag24("AbortError"_kj).varint(1).raw({UNDEFINED}));
    expectStack("e.stack = null"_kj, tag24("AbortError"_kj).varint(1).raw({NULL_VALUE}));
    expectStack("e.stack = { a: 1 }"_kj,
        tag24("AbortError"_kj).varint(1).raw({0x6f}).string("a"_kj).raw(ONE).raw({0x7b, 1}));
  });
}

KJ_TEST("A DOMException that native code created serializes as the context's does") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = serialize(js, eval(js, R"js(
      let e;
      try { structuredClone(() => {}); } catch (x) { e = x; }
      e.stack = 'S';
      e
    )js"_kj));
    auto message = "() => {} could not be cloned."_kj;
    kj::Array<kj::byte> expected;
    if (usingV8DomException()) {
      expected = Bytes(jsg::SERIALIZATION_TAG_DOM_EXCEPTION_V3)
                     .lengthDelimited("DataCloneError"_kj)
                     .lengthDelimited(message)
                     .varint(1)
                     .string("S"_kj)
                     .varint(0)
                     .array();
    } else {
      expected = tag8("DataCloneError"_kj, message, "S"_kj);
    }
    KJ_EXPECT(bytes == expected, bytes, expected);
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
    // V8's DOMException's. The C++ one's name and message are lazy data properties, which V8's
    // description doesn't evaluate, so it is empty until JavaScript reads them.
    if (!usingV8DomException()) return;
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

// What reading `bytes` creates, with `preserveStackInErrors`, described by `describe`, a function
// of it.
kj::String readAndDescribe(jsg::Lock& js,
    kj::ArrayPtr<const kj::byte> bytes,
    kj::StringPtr describe,
    bool preserveStackInErrors = true) {
  jsg::Deserializer deserializer(js, bytes, kj::none, kj::none,
      jsg::Deserializer::Options{.preserveStackInErrors = preserveStackInErrors});
  auto value = deserializer.readValue(js);
  auto fn = KJ_ASSERT_NONNULL(eval(js, describe).tryCast<jsg::JsFunction>());
  return fn.call(js, js.undefined(), value).toString(js);
}

KJ_TEST("Reading tag 24 creates the context's DOMException") {
  runWithBootstrap([](jsg::Lock& js) {
    auto bytes = tag24("NotFoundError"_kj)
                     .varint(1)
                     .string("S"_kj)
                     .varint(1)
                     .string("extra"_kj)
                     .raw(ONE)
                     .array();
    // V8's gets the other own properties. The C++ class drops them.
    auto result = readAndDescribe(js, bytes, R"js((e) => [
      Object.getPrototypeOf(e) === DOMException.prototype, Error.isError(e),
      e.name, e.message, e.code, e.stack, e.extra,
    ].join())js"_kj);
    auto expected = usingV8DomException() ? "true,true,NotFoundError,m,8,S,1"_kj
                                          : "true,false,NotFoundError,m,8,S,"_kj;
    KJ_EXPECT(result == expected, result);
  });
}

KJ_TEST("Reading tag 24 keeps its stack, whatever its value, with preserveStackInErrors") {
  runWithBootstrap([](jsg::Lock& js) {
    auto describeStack = R"js((e) => [
      Object.hasOwn(e, 'stack'), e.stack === null ? 'null' : typeof e.stack, e.stack?.a,
    ].join())js"_kj;
    auto stackOf = [&](Bytes& bytes) {
      return readAndDescribe(js, bytes.varint(0).array(), describeStack);
    };
    auto result = stackOf(tag24("NotFoundError"_kj).varint(1).raw({NULL_VALUE}));
    KJ_EXPECT(result == "true,null,"_kj, result);
    result = stackOf(
        tag24("NotFoundError"_kj).varint(1).raw({0x6f}).string("a"_kj).raw(ONE).raw({0x7b, 1}));
    KJ_EXPECT(result == "true,object,1"_kj, result);
    // Without a stack, V8's has none, and the C++ class's, which it always has, is undefined.
    result = stackOf(tag24("NotFoundError"_kj).varint(0));
    KJ_EXPECT(
        result == (usingV8DomException() ? "false,undefined,"_kj : "true,undefined,"_kj), result);
  });
}

KJ_TEST("Without preserveStackInErrors, reading tag 24 or 25 uses the local stack") {
  runWithBootstrap([](jsg::Lock& js) {
    auto describeStack = "(e) => typeof e.stack === 'string' && e.stack.split('\\n')[0]"_kj;
    auto stackOf = [&](kj::ArrayPtr<const kj::byte> bytes) {
      return readAndDescribe(js, bytes, describeStack, false);
    };
    for (auto& bytes: {tag24("NotFoundError"_kj).varint(1).string("S"_kj).varint(0).array(),
           tag24("NotFoundError"_kj).varint(1).raw({NULL_VALUE}).varint(0).array(),
           tag24("NotFoundError"_kj).varint(0).varint(0).array()}) {
      auto result = stackOf(bytes);
      KJ_EXPECT(result == "NotFoundError: m"_kj, result);
    }
    if (usingV8DomException()) {
      auto result = stackOf(tag25(0, {}));
      KJ_EXPECT(result == "QuotaExceededError: m"_kj, result);
    }
  });
}

KJ_TEST("Reading tag 24 rejects malformed data") {
  runWithBootstrap([](jsg::Lock& js) {
    auto expectRejected = [&](kj::Array<kj::byte> bytes, kj::StringPtr expected) {
      auto error = readError(js, bytes);
      KJ_EXPECT(error == expected, error);
    };
    expectRejected(tag24("NotFoundError"_kj).varint(2).array(),
        "DataCloneError: Deserialization failed: unsupported DOMException."_kj);
    expectRejected(tag24("NotFoundError"_kj).varint(0).varint(1).raw(ONE).raw(ONE).array(),
        "DataCloneError: Deserialization failed: DOMException property name is not a string."_kj);
    expectRejected(
        tag24("NotFoundError"_kj).varint(0).varint(1).string("stack"_kj).raw(ONE).array(),
        "DataCloneError: Deserialization failed: DOMException has `stack` among its other properties."_kj);
  });
}

// V8 registers a host object only once it has been read, so nothing in it can refer to it.
KJ_TEST("A DOMException whose stack or other properties refer back to it can't be read") {
  runWithBootstrap([](jsg::Lock& js) {
    auto expectRejected = [&](kj::Array<kj::byte> bytes) {
      auto error = readError(js, bytes);
      KJ_EXPECT(error == "Error: Unable to deserialize cloned data."_kj, error);
    };
    expectRejected(tag24("NotFoundError"_kj).varint(1).raw(FIRST_OBJECT).varint(0).array());
    expectRejected(tag24("NotFoundError"_kj)
                       .varint(0)
                       .varint(1)
                       .string("a"_kj)
                       .raw({0x6f})
                       .string("b"_kj)
                       .raw(FIRST_OBJECT)
                       .raw({0x7b, 1})
                       .array());
  });
}

KJ_TEST("A QuotaExceededError serializes as tag 25") {
  runWithBootstrap([](jsg::Lock& js) {
    if (!usingV8DomException()) return;
    // Its own message, not a property that shadows it, and, without enhanced error serialization,
    // no other own properties.
    auto bytes = serialize(js, eval(js, R"js(
      class Sub extends QuotaExceededError {}
      const e = new Sub('m', { quota: 1 });
      Object.defineProperty(e, 'message', { value: 'other' });
      e.extra = 1;
      e.stack = 'S';
      e
    )js"_kj));
    auto expected = tag25(HAS_QUOTA, {1});
    KJ_EXPECT(bytes == expected, bytes, expected);

    bytes = serialize(js, eval(js, R"js(
      const f = new QuotaExceededError('m', { quota: 1, requested: 2.5 });
      f.stack = 'S';
      f
    )js"_kj));
    expected = tag25(HAS_QUOTA | HAS_REQUESTED, {1, 2.5});
    KJ_EXPECT(bytes == expected, bytes, expected);
  });
}

KJ_TEST("Reading tag 25 creates a QuotaExceededError") {
  runWithBootstrap([](jsg::Lock& js) {
    if (!usingV8DomException()) return;
    auto describe = R"js((e) => [
      Object.getPrototypeOf(e) === QuotaExceededError.prototype,
      Error.isError(e),
      e.name, e.message, e.code, e.quota, e.requested, e.stack,
    ].join())js"_kj;
    auto result = readAndDescribe(js, tag25(HAS_QUOTA | HAS_REQUESTED, {1, 2}), describe);
    KJ_EXPECT(result == "true,true,QuotaExceededError,m,22,1,2,S"_kj, result);
    result = readAndDescribe(js, tag25(0, {}), describe);
    KJ_EXPECT(result == "true,true,QuotaExceededError,m,22,,,S"_kj, result);
  });
}

KJ_TEST("Reading tag 25 rejects malformed data and data the constructor would") {
  runWithBootstrap([](jsg::Lock& js) {
    if (!usingV8DomException()) return;
    auto expectRejected = [&](kj::Array<kj::byte> bytes, kj::StringPtr expected) {
      auto error = readError(js, bytes);
      KJ_EXPECT(error == expected, error);
    };
    auto invalid = "DataCloneError: Deserialization failed: invalid QuotaExceededError amounts."_kj;
    expectRejected(tag25(HAS_QUOTA, {-1}), invalid);
    expectRejected(tag25(HAS_REQUESTED, {-1}), invalid);
    expectRejected(tag25(HAS_QUOTA | HAS_REQUESTED, {2, 1}), invalid);
    expectRejected(tag25(HAS_QUOTA, {kj::inf()}), invalid);
    expectRejected(tag25(HAS_REQUESTED, {kj::nan()}), invalid);
    expectRejected(
        tag25(4, {}), "DataCloneError: Deserialization failed: unsupported QuotaExceededError."_kj);
    expectRejected(Bytes(jsg::SERIALIZATION_TAG_QUOTA_EXCEEDED_ERROR)
                       .lengthDelimited("NotFoundError"_kj)
                       .lengthDelimited("m"_kj)
                       .varint(0)
                       .varint(0)
                       .varint(0)
                       .array(),
        "DataCloneError: Deserialization failed: invalid QuotaExceededError name."_kj);
  });
}

KJ_TEST("Without V8's DOMException, reading tag 25 fails") {
  runWithBootstrap([](jsg::Lock& js) {
    if (usingV8DomException()) return;
    auto error = readError(js, tag25(HAS_QUOTA | HAS_REQUESTED, {1, 2}));
    KJ_EXPECT(
        error == "DataCloneError: Deserialization failed: QuotaExceededError is not supported."_kj,
        error);
  });
}

KJ_TEST("A DOMException given its stack captures none") {
  runWithBootstrap([](jsg::Lock& js) {
    // v8::Exception::GetStackTrace() returns the detailed stack trace, which V8 captures with the
    // stack only while this is on.
    js.v8Isolate->SetCaptureStackTraceForUncaughtExceptions(true);
    KJ_DEFER(js.v8Isolate->SetCaptureStackTraceForUncaughtExceptions(false));
    auto captured = [](jsg::JsValue value) {
      return !v8::Exception::GetStackTrace(value).IsEmpty();
    };
    auto read = [&](kj::Array<kj::byte> bytes) {
      jsg::Deserializer deserializer(js, bytes);
      return deserializer.readValue(js);
    };
    KJ_EXPECT(captured(js.domException(kj::str("AbortError"), kj::str("m"))));
    KJ_EXPECT(!captured(js.domException(kj::str("AbortError"), kj::str("m"), kj::str("S"))));
    KJ_EXPECT(!captured(read(tag8("NotFoundError"_kj, "m"_kj, "S"_kj))));
    KJ_EXPECT(
        !captured(read(tag24("NotFoundError"_kj).varint(1).raw({NULL_VALUE}).varint(0).array())));
    if (usingV8DomException()) {
      KJ_EXPECT(!captured(read(tag25(HAS_QUOTA, {1}))));
    }
  });
}

}  // namespace
}  // namespace workerd::api
