// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "ser.h"

#include <v8-value-serializer-version.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public jsg::Object, public ContextGlobal {};

kj::Array<kj::byte> lastSerializedData;

struct SerTestContext: public ContextGlobalObject {
  enum class SerializationTag {
    FOO,
    BAR,
    BAZ,
    QUX,
  };

  struct Foo: public jsg::Object {
    uint i;
    Foo(uint i): i(i) {}

    static jsg::Ref<Foo> constructor(jsg::Lock& js, uint i) {
      return js.alloc<Foo>(i);
    }

    int getI() {
      return i;
    }

    JSG_RESOURCE_TYPE(Foo) {
      JSG_READONLY_PROTOTYPE_PROPERTY(i, getI);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeRawUint32(i);
    }
    static jsg::Ref<Foo> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_ASSERT(tag == SerializationTag::FOO);

      // Intentionally deserialize differently so we can detect it.
      return js.alloc<Foo>(deserializer.readRawUint32() + 2);
    }
    JSG_SERIALIZABLE(SerializationTag::FOO);
  };

  struct Bar: public jsg::Object {
    kj::String text;
    Bar(kj::String text): text(kj::mv(text)) {}

    static jsg::Ref<Bar> constructor(jsg::Lock& js, kj::String text) {
      return js.alloc<Bar>(kj::mv(text));
    }

    kj::String getText() {
      return kj::str(text);
    }

    JSG_RESOURCE_TYPE(Bar) {
      JSG_READONLY_PROTOTYPE_PROPERTY(text, getText);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeRawUint64(text.size());
      serializer.writeRawBytes(text.asBytes());
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_ASSERT(tag == SerializationTag::BAR);

      size_t size = deserializer.readRawUint64();
      auto bytes = deserializer.readRawBytes(size);
      // Intentionally deserialize differently so we can detect it.
      return js.alloc<Bar>(kj::str(bytes.asChars(), '!'));
    }
    JSG_SERIALIZABLE(SerializationTag::BAR);
  };

  struct Baz: public jsg::Object {
    bool serializeThrows;
    Baz(bool serializeThrows): serializeThrows(serializeThrows) {}
    static jsg::Ref<Baz> constructor(jsg::Lock& js, bool serializeThrows) {
      return js.alloc<Baz>(serializeThrows);
    }

    JSG_RESOURCE_TYPE(Baz) {}

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      JSG_REQUIRE(!serializeThrows, Error, "throw from serialize()");
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      JSG_FAIL_REQUIRE(Error, "throw from deserialize()");
    }
    JSG_SERIALIZABLE(SerializationTag::BAZ);
  };

  // Qux is like Bar but serializes its string by converting it to a JS value first.
  struct Qux: public jsg::Object {
    kj::String text;
    Qux(kj::String text): text(kj::mv(text)) {}

    static jsg::Ref<Qux> constructor(jsg::Lock& js, kj::String text) {
      return js.alloc<Qux>(kj::mv(text));
    }

    kj::String getText() {
      return kj::str(text);
    }

    JSG_RESOURCE_TYPE(Qux) {
      JSG_READONLY_PROTOTYPE_PROPERTY(text, getText);
    }

    void serialize(
        jsg::Lock& js, jsg::Serializer& serializer, const TypeHandler<kj::String>& stringHandler) {
      // V2 prefers to serialize the string as a JS value.
      serializer.write(js, JsValue(stringHandler.wrap(js, kj::str(text, '?'))));
    }
    static jsg::Ref<Qux> deserialize(Lock& js,
        SerializationTag tag,
        Deserializer& deserializer,
        const TypeHandler<kj::String>& stringHandler) {
      KJ_ASSERT(tag == SerializationTag::QUX);

      return js.alloc<Qux>(
          KJ_ASSERT_NONNULL(stringHandler.tryUnwrap(js, deserializer.readValue(js))));
    }
    JSG_SERIALIZABLE(SerializationTag::QUX);
  };

  JsValue roundTrip(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });

    auto result = ({
      Deserializer deser(js, content);
      deser.readValue(js);
    });

    // Save the last serialization off to the side.
    lastSerializedData = kj::mv(content.data);

    return result;
  }

  // A serializer outside C++ JSG: its own delegates drive V8's serializer, and API objects go
  // through writeApiObject() and readApiObject().
  struct ForeignSerializer final: public v8::ValueSerializer::Delegate {
    explicit ForeignSerializer(v8::Isolate* isolate): isolate(isolate), ser(isolate, this) {
      ser.WriteHeader();
    }
    void ThrowDataCloneError(v8::Local<v8::String> message) override {
      v8::Local<v8::Value> exception;
      if (newDataCloneError(isolate, message).ToLocal(&exception)) {
        isolate->ThrowException(exception);
      }
    }
    v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate, v8::Local<v8::Object> object) override {
      return writeApiObject(isolate, ser, object);
    }
    v8::Isolate* isolate;
    v8::ValueSerializer ser;
  };

  struct ForeignDeserializer final: public v8::ValueDeserializer::Delegate {
    ForeignDeserializer(v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> data)
        : deser(isolate, data.begin(), data.size(), this) {}
    v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override {
      uint32_t tag;
      KJ_ASSERT(deser.ReadUint32(&tag));
      return readApiObject(isolate, deser, tag);
    }
    v8::ValueDeserializer deser;
  };

  kj::Array<kj::byte> foreignSerialize(Lock& js, JsValue in) {
    ForeignSerializer ser(js.v8Isolate);
    check(ser.ser.WriteValue(js.v8Context(), in));
    auto [data, size] = ser.ser.Release();
    return kj::Array<kj::byte>(data, size, SERIALIZED_BUFFER_DISPOSER);
  }

  JsValue foreignDeserialize(Lock& js, kj::ArrayPtr<const kj::byte> data) {
    ForeignDeserializer deser(js.v8Isolate, data);
    check(deser.deser.ReadHeader(js.v8Context()));
    return JsValue(check(deser.deser.ReadValue(js.v8Context())));
  }

  // Round-trips `in` through the foreign serializer and deserializer.
  JsValue foreignRoundTrip(Lock& js, JsValue in) {
    auto data = foreignSerialize(js, in);
    return foreignDeserialize(js, data);
  }

  // Whether the foreign serializer writes `in` exactly as Serializer does.
  bool foreignMatchesSerializer(Lock& js, JsValue in) {
    auto foreign = foreignSerialize(js, in);
    Serializer ser(js);
    ser.write(js, in);
    return foreign == ser.release().data;
  }

  // Reads with Deserializer what the foreign serializer wrote, and the other way round.
  JsValue foreignToSerializer(Lock& js, JsValue in) {
    auto data = foreignSerialize(js, in);
    Deserializer deser(js, data);
    return deser.readValue(js);
  }
  JsValue serializerToForeign(Lock& js, JsValue in) {
    Serializer ser(js);
    ser.write(js, in);
    auto released = ser.release();
    return foreignDeserialize(js, released.data);
  }

  // Mirror of the global `structuredClone(value, { transfer })`, for testing transfer handling.
  JsValue structuredCloneWithTransfer(
      Lock& js, JsValue value, jsg::Optional<kj::Array<JsValue>> transfer) {
    kj::Maybe<kj::Array<JsValue>> maybeTransfer;
    KJ_IF_SOME(t, transfer) {
      maybeTransfer = kj::mv(t);
    }
    return jsg::structuredClone(js, value, kj::mv(maybeTransfer));
  }

  // The bytes Serializer writes for `value`.
  kj::Array<kj::byte> serializeBytes(Lock& js, JsValue value) {
    Serializer ser(js);
    ser.write(js, value);
    return ser.release().data;
  }

  // Reads `data` with Deserializer, given an empty list of SharedArrayBuffers.
  JsValue deserializeWithoutSharedArrayBuffers(Lock& js, kj::Array<kj::byte> data) {
    Deserializer deser(js, data, kj::none, kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>());
    return deser.readValue(js);
  }

  // Reads `data` with Deserializer.
  JsValue deserializeBytes(Lock& js, kj::Array<kj::byte> data) {
    Deserializer deser(js, data);
    return deser.readValue(js);
  }

  // How many buffers Serializer transfers when it writes `value` with `transfer`.
  uint32_t transferredBufferCount(Lock& js, JsValue value, kj::Array<JsValue> transfer) {
    Serializer ser(js);
    for (auto& item: transfer) {
      ser.transfer(js, item);
    }
    ser.write(js, value);
    return ser.release().transferredArrayBuffers.size();
  }

  JSG_RESOURCE_TYPE(SerTestContext) {
    JSG_NESTED_TYPE(Foo);
    JSG_NESTED_TYPE(Bar);
    JSG_NESTED_TYPE(Baz);
    JSG_NESTED_TYPE(Qux);
    JSG_METHOD(roundTrip);
    JSG_METHOD(foreignRoundTrip);
    JSG_METHOD(foreignMatchesSerializer);
    JSG_METHOD(foreignToSerializer);
    JSG_METHOD(serializerToForeign);
    JSG_METHOD(structuredCloneWithTransfer);
    JSG_METHOD(transferredBufferCount);
    JSG_METHOD(deserializeBytes);
    JSG_METHOD(serializeBytes);
    JSG_METHOD(deserializeWithoutSharedArrayBuffers);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolate,
    SerTestContext,
    SerTestContext::Foo,
    SerTestContext::Bar,
    SerTestContext::Baz,
    SerTestContext::Qux);

KJ_TEST("deserialization header diagnostics expose metadata without payload contents") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);
  auto expectFailure = [&](kj::ArrayPtr<const kj::byte> data, kj::StringPtr header, uint version) {
    e.run([&](jsg::Lock& js) {
      auto failure = kj::runCatchingExceptions([&]() {
        Deserializer deser(js, data, kj::none, kj::none,
            Deserializer::Options{.version = 15, .diagnosticContext = "RPC arguments"_kj});
      });
      auto& exception = KJ_ASSERT_NONNULL(failure);
      KJ_EXPECT(exception.getType() == kj::Exception::Type::FAILED);
      KJ_EXPECT(exception.getDescription() ==
          kj::str("worker_do_not_log; message = Unable to deserialize cloned data due to invalid "
                  "or unsupported version. [context=RPC arguments; bytes=",
              data.size(), "; header=", header, "; wireVersion=", version, "; maxWireVersion=",
              v8::CurrentValueSerializerFormatVersion(), "; v8=", v8::V8::GetVersion(), "]"));
      auto jsError = js.exceptionToJs(kj::mv(exception));
      KJ_EXPECT(kj::str(jsError.getHandle(js)).startsWith("Error: internal error"));
    });
  };

  expectFailure({}, "empty", 0);
  expectFailure("private payload"_kj.asBytes(), "missing", 0);
  const kj::byte incompleteHeader[] = {0xff, 0x80};
  expectFailure(kj::arrayPtr(incompleteHeader, 1), "present", 0);
  expectFailure(incompleteHeader, "present", 0);
  const kj::byte legacyHeader[] = {0xff, 12};
  expectFailure(legacyHeader, "present", 12);
  // A complete buffer with an unsupported multi-byte version, followed by private contents.
  const kj::byte futureHeader[] = {0xff, 0xff, 0x01, 's', 'e', 'c', 'r', 'e', 't'};
  expectFailure(futureHeader, "present", 255);
}

KJ_TEST("deserialization diagnostics preserve successful reads and other errors") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);
  e.run([&](jsg::Lock& js) {
    for (auto version: {13u, 15u, v8::CurrentValueSerializerFormatVersion()}) {
      Serializer ser(js, {.version = version});
      ser.write(js, js.str("private payload"_kj));
      auto data = ser.release();
      Deserializer deser(js, data, Deserializer::Options{.diagnosticContext = "RPC result"_kj});
      KJ_EXPECT(deser.getVersion() == version);
      KJ_EXPECT(kj::str(deser.readValue(js)) == "private payload");
    }

    Serializer headerlessSer(js, {.version = 15, .omitHeader = true});
    headerlessSer.write(js, js.str("private payload"_kj));
    auto headerlessData = headerlessSer.release();
    Deserializer headerlessDeser(js, headerlessData,
        Deserializer::Options{
          .version = 15, .readHeader = false, .diagnosticContext = "RPC result"_kj});
    KJ_EXPECT(kj::str(headerlessDeser.readValue(js)) == "private payload");

    // Call sites without a diagnostic context still report header metadata.
    KJ_EXPECT_THROW_MESSAGE("[context=unknown; bytes=15; header=missing; wireVersion=0;",
        Deserializer(js, "private payload"_kj.asBytes()));

    // A valid header followed by an invalid value fails in ReadValue(), not ReadHeader().
    const kj::byte invalidValue[] = {0xff, 15, 0xff};
    Deserializer deser(js, invalidValue, kj::none, kj::none,
        Deserializer::Options{.diagnosticContext = "RPC result"_kj});
    JSG_TRY(js) {
      deser.readValue(js);
      KJ_FAIL_EXPECT("invalid value was accepted");
    }
    JSG_CATCH(exception) {
      KJ_EXPECT(kj::str(exception.getHandle(js)) == "Error: Unable to deserialize cloned data.");
    }
  });
}

// Define a whole second JSG isolate type that contains "updated" code where Bar no longer wraps
// a string, it wraps an arbitrary value.
struct SerTestContextV2: public ContextGlobalObject {
  enum class SerializationTag { FOO, BAR_OLD, BAZ, QUX, BAR_V2 };

  struct Bar: public jsg::Object {
    JsRef<JsValue> val;
    Bar(JsRef<JsValue> val): val(kj::mv(val)) {}

    static jsg::Ref<Bar> constructor(jsg::Lock& js, JsRef<JsValue> val) {
      return js.alloc<Bar>(kj::mv(val));
    }

    JsRef<JsValue> getVal(Lock& js) {
      return val.addRef(js);
    }

    JSG_RESOURCE_TYPE(Bar) {
      JSG_READONLY_PROTOTYPE_PROPERTY(val, getVal);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      // V2 just writes a value!
      serializer.write(js, JsValue(val.getHandle(js)));
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      if (tag == SerializationTag::BAR_OLD) {
        // Oh, it's an old value.
        size_t size = deserializer.readRawUint64();
        auto bytes = deserializer.readRawBytes(size);

        return js.alloc<Bar>(JsRef<JsValue>(js, js.str(kj::str("old:", bytes.asChars()))));
      } else {
        KJ_ASSERT(tag == SerializationTag::BAR_V2);

        return js.alloc<Bar>(JsRef<JsValue>(js, deserializer.readValue(js)));
      }
    }
    JSG_SERIALIZABLE(SerializationTag::BAR_V2, SerializationTag::BAR_OLD);
  };

  JsValue roundTrip(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });

    auto result = ({
      Deserializer deser(js, content);
      deser.readValue(js);
    });

    // Save the last serialization off to the side.
    lastSerializedData = kj::mv(content.data);

    return result;
  }

  JsValue deserializeLast(Lock& js) {
    Deserializer deser(js, lastSerializedData);
    return deser.readValue(js);
  }

  JSG_RESOURCE_TYPE(SerTestContextV2) {
    JSG_NESTED_TYPE(Bar);
    JSG_METHOD(roundTrip);
    JSG_METHOD(deserializeLast);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolateV2, SerTestContextV2, SerTestContextV2::Bar);

KJ_TEST("serialization") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // Test serializing built-in values.
  e.expectEval("roundTrip(123)", "number", "123");
  e.expectEval("JSON.stringify(roundTrip({foo: 123}))", "string", "{\"foo\":123}");

  // Test serializing host objects.
  e.expectEval("roundTrip(new Foo(123)).i", "number", "125");
  e.expectEval("roundTrip(new Qux(\"hello\")).text", "string", "hello?");
  e.expectEval("roundTrip(new Bar(\"hello\")).text", "string", "hello!");

  // Test throwing from serialize/deserialize
  e.expectEval("roundTrip(new Baz(true)).text", "throws", "Error: throw from serialize()");
  e.expectEval("roundTrip(new Baz(false)).text", "throws", "Error: throw from deserialize()");

  // Let's set up the "new version" of the code.
  Evaluator<SerTestContextV2, SerTestIsolateV2> e2(v8System);

  // This will deserialize the last-serialized bytes from above, where we serialized Bar("hello").
  // However, it is using a "new version" of the code where Bar's serialization has changed, but
  // the old version is still accepted.
  e2.expectEval("deserializeLast().val", "string", "old:hello");

  // Also try round-tripping the new version. It now accepts arbitrary values, not just strings.
  e2.expectEval("roundTrip(new Bar(123)).val", "number", "123");

  // Note that cycles through host objects are correctly serialized!
  //
  // V8 BUG ALERT: The below works if we use `obj` as the root of serialization, but NOT if we
  //   use `bar` as the root. The reason is a flaw in the design of V8's callbacks for parsing
  //   host objects. V8 makes a single callback to the embedder which fully reads the object and
  //   returns a handle. However, this means that V8 cannot put the object into the backreference
  //   table until this callback returns. If, while parsing the object, we encounter a
  //   backreference to the object itself (a cycle), the deserializer will find the backreference
  //   is not in the table and therefore raises an error. This is not a problem for native objects
  //   because V8 allocates the object first, then immediately adds it to the backreference table,
  //   and only then parses its content -- and this is why everything works fine if we start with
  //   a native object as the root, as in this test. The API for host objects needs to be extended
  //   somehow to allow the object to be inserted into the table before parsing its content.
  e2.expectEval("let obj = {i: 321};\n"
                "let bar = new Bar(obj);\n"
                "obj.bar = bar;\n"
                "roundTrip(obj).bar.val.bar.val.bar.val.i",
      "number", "321");
}

KJ_TEST("serialization of API objects by a serializer outside C++ JSG") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  e.expectEval("foreignRoundTrip(new Foo(123)).i", "number", "125");
  e.expectEval("foreignRoundTrip(new Bar(\"hello\")).text", "string", "hello!");
  e.expectEval("foreignRoundTrip(new Qux(\"hello\")).text", "string", "hello?");
  e.expectEval(
      "foreignRoundTrip({ foo: new Foo(1), qux: [new Qux(\"a\")] }).qux[0].text", "string", "a?");

  // Values an API object writes go back through the foreign serializer's delegate.
  e.expectEval("let obj = {i: 321};\n"
               "obj.qux = new Qux(\"x\");\n"
               "foreignRoundTrip(obj).qux.text",
      "string", "x?");

  // The bytes and the format are those of Serializer and Deserializer.
  e.expectEval("foreignMatchesSerializer({ foo: new Foo(7), bar: new Bar(\"b\"), n: [1, 'x'] })",
      "boolean", "true");
  e.expectEval("foreignToSerializer(new Bar(\"to\")).text", "string", "to!");
  e.expectEval("serializerToForeign(new Bar(\"from\")).text", "string", "from!");

  // What a type's serializer or deserializer throws reaches the caller.
  e.expectEval("foreignRoundTrip(new Baz(true))", "throws", "Error: throw from serialize()");
  e.expectEval("foreignRoundTrip(new Baz(false))", "throws", "Error: throw from deserialize()");

  // An object that is not a serializable API object, and a value V8 can't clone, throw a
  // DataCloneError, as Serializer does.
  e.expectEval("try { foreignRoundTrip(() => 1); } catch (e) { `${e.name}: ${e.message}` }",
      "string", "DataCloneError: () => 1 could not be cloned.");
  e.expectEval(
      "try { foreignRoundTrip(roundTrip); } catch (e) { e.name }", "string", "DataCloneError");
}

KJ_TEST("recursive structuredClone with transfer") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // A getter invoked during serialization performs a nested structuredClone that transfers the
  // SAME ArrayBuffer the outer clone is also transferring. Because detaching is deferred until the
  // serializer's release(), the inner clone runs to completion (including its own release(), which
  // detaches the buffer) while the outer clone is still inside write(). The outer release() then
  // attempts to detach the already-detached buffer; this must not crash. Each structuredClone uses
  // its own serializer, so the nested call does not disturb the outer's transfer list.
  //
  // Expected: buf is detached (byteLength 0) and the nested clone observed the real data (42).
  e.expectEval("const buf = new ArrayBuffer(8);\n"
               "new Uint8Array(buf)[0] = 42;\n"
               "const obj = { get nested() {\n"
               "  return structuredCloneWithTransfer(new Uint8Array(buf), [buf]);\n"
               "} };\n"
               "const outer = structuredCloneWithTransfer(obj, [buf]);\n"
               "`${buf.byteLength},${outer.nested[0]}`",
      "string", "0,42");
}

KJ_TEST("malformed native errors throw") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // A header, V8's tag for a host object, and the native error tag.
  constexpr auto prefix = "const prefix = [0xff, 0x0f, 0x5c, 0x0a];\n"_kj;

  // Cut short before its type: an internal error.
  {
    KJ_EXPECT_LOG(ERROR, "deserialization failure, possible corruption");
    e.expectEval(kj::str(prefix,
                     "try { deserializeBytes(new Uint8Array(prefix)); } catch (e) {\n"
                     "  e.message.startsWith('internal error; reference = ')\n"
                     "}"),
        "boolean", "true");
  }

  // A message that is not a string: a DataCloneError.
  e.expectEval(kj::str(prefix,
                   "try { deserializeBytes(new Uint8Array([...prefix, 0x01, 0x5f])); }\n"
                   "catch (e) { `${e.name}: ${e.message}` }"),
      "string", "DataCloneError: Deserialization failed: error message is not a string");
}

KJ_TEST("transfer transfers each buffer once") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // A view and its buffer, and two views of one buffer, each transfer the buffer once.
  e.expectEval("const b = new ArrayBuffer(8);\n"
               "const u = new Uint8Array(b); u[0] = 7;\n"
               "transferredBufferCount({ u, b }, [u, b])",
      "number", "1");
  e.expectEval("const b = new ArrayBuffer(8);\n"
               "transferredBufferCount([], [new Uint8Array(b), new Uint8Array(b, 4)])",
      "number", "1");
  e.expectEval(
      "transferredBufferCount([], [new ArrayBuffer(1), new ArrayBuffer(1)])", "number", "2");

  // The clone keeps the view on the buffer.
  e.expectEval("const b = new ArrayBuffer(8);\n"
               "const u = new Uint8Array(b, 4); u[0] = 7;\n"
               "const out = structuredCloneWithTransfer({ u, b }, [u, b]);\n"
               "`${b.byteLength},${out.u[0]},${out.u.buffer === out.b}`",
      "string", "0,7,true");
}

KJ_TEST("a SharedArrayBuffer beyond the given list is corrupt data") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);
  KJ_EXPECT_LOG(ERROR, "deserialization failure, possible corruption");
  e.expectEval("const data = serializeBytes(new SharedArrayBuffer(1));\n"
               "try { deserializeWithoutSharedArrayBuffers(data); } catch (e) {\n"
               "  e.message.startsWith('internal error; reference = ')\n"
               "}",
      "boolean", "true");
}

}  // namespace
}  // namespace workerd::jsg::test
