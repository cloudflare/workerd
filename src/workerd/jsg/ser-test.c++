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
    FOO = 0,
    BAR = 1,
    BAZ = 2,
    QUX = 3,
    BOX = 4,
    // RawJsSerializable writes this tag, which the Deserializer reads itself.
    JS_SERIALIZABLE = SERIALIZATION_TAG_JS_SERIALIZABLE,
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

  // Box holds any value, which it serializes with a nested write().
  struct Box: public jsg::Object {
    JsRef<JsValue> val;
    Box(JsRef<JsValue> val): val(kj::mv(val)) {}

    static jsg::Ref<Box> constructor(jsg::Lock& js, JsRef<JsValue> val) {
      return js.alloc<Box>(kj::mv(val));
    }

    JsValue getVal(Lock& js) {
      return val.getHandle(js);
    }

    JSG_RESOURCE_TYPE(Box) {
      JSG_READONLY_PROTOTYPE_PROPERTY(val, getVal);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.write(js, val.getHandle(js));
    }
    static jsg::Ref<Box> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      return js.alloc<Box>(JsRef<JsValue>(js, deserializer.readValue(js)));
    }
    JSG_SERIALIZABLE(SerializationTag::BOX);

   private:
    void visitForGc(GcVisitor& visitor) {
      visitor.visit(val);
    }
  };

  // Writes the record of an instance of a class implemented in JavaScript as it is given, as a
  // peer could: its deserializeInfo, its flags and, if given, its message and its stack. The
  // instance's data is written separately (roundTripWithData()).
  struct RawJsSerializable: public jsg::Object {
    kj::String deserializeInfo;
    uint32_t flags;
    kj::Maybe<JsRef<JsValue>> stack;
    kj::Maybe<JsRef<JsValue>> message;
    RawJsSerializable(kj::String deserializeInfo,
        uint32_t flags,
        kj::Maybe<JsRef<JsValue>> stack,
        kj::Maybe<JsRef<JsValue>> message)
        : deserializeInfo(kj::mv(deserializeInfo)),
          flags(flags),
          stack(kj::mv(stack)),
          message(kj::mv(message)) {}

    static jsg::Ref<RawJsSerializable> constructor(jsg::Lock& js,
        kj::String deserializeInfo,
        uint32_t flags,
        jsg::Optional<JsRef<JsValue>> stack,
        jsg::Optional<JsRef<JsValue>> message) {
      return js.alloc<RawJsSerializable>(
          kj::mv(deserializeInfo), flags, kj::mv(stack), kj::mv(message));
    }

    JSG_RESOURCE_TYPE(RawJsSerializable) {}

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeLengthDelimited(deserializeInfo);
      serializer.writeRawUint32(flags);
      KJ_IF_SOME(m, message) {
        serializer.write(js, m.getHandle(js));
      }
      KJ_IF_SOME(s, stack) {
        serializer.write(js, s.getHandle(js));
      }
    }
    static jsg::Ref<RawJsSerializable> deserialize(
        Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_UNREACHABLE;
    }
    JSG_SERIALIZABLE(SerializationTag::JS_SERIALIZABLE);

   private:
    void visitForGc(GcVisitor& visitor) {
      visitor.visit(stack, message);
    }
  };

  // Stands in for the per-isolate bootstrap's modules: one module, "test", whose exports
  // setModule() sets.
  struct TestModules final: public BootstrapModules {
    kj::Maybe<JsRef<JsValue>> exports;

    kj::Maybe<JsValue> tryGetLoadedExports(Lock& js, kj::StringPtr specifier) override {
      if (specifier != "test"_kj) return kj::none;
      KJ_IF_SOME(e, exports) {
        return e.getHandle(js);
      }
      return kj::none;
    }
  };
  TestModules modules;

  void setModule(Lock& js, JsValue exports) {
    modules.exports = JsRef<JsValue>(js, exports);
    setAlignedPointerInEmbedderData(js.v8Context(), ContextPointerSlot::BOOTSTRAP_STATE,
        static_cast<BootstrapModules*>(&modules));
  }

  void clearModules(Lock& js) {
    setAlignedPointerInEmbedderData(js.v8Context(), ContextPointerSlot::BOOTSTRAP_STATE, nullptr);
  }

  // The key of the V8 private property `name`, as the bootstrap's utils.getPrivateSymbol()
  // returns it.
  JsValue getPrivateSymbol(Lock& js, kj::String name) {
    v8::Local<v8::String> slot = js.strIntern("key"_kj);
    auto holder = v8::ObjectTemplate::New(js.v8Isolate);
    holder->Set(slot, v8::Private::ForApi(js.v8Isolate, js.str(name)));
    return JsValue(check(check(holder->NewInstance(js.v8Context()))->Get(js.v8Context(), slot)));
  }

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

  // Like roundTrip(), but with class instances not serializable, as for RPC.
  JsValue roundTripStrict(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js, {.treatClassInstancesAsPlainObjects = false});
      ser.write(js, in);
      ser.release();
    });
    Deserializer deser(js, content);
    return deser.readValue(js);
  }

  // Writes two values with one Serializer and reads them back with one Deserializer.
  JsValue roundTripTwo(Lock& js, JsValue a, JsValue b) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, a);
      ser.write(js, b);
      ser.release();
    });
    Deserializer deser(js, content);
    auto first = deser.readValue(js);
    auto second = deser.readValue(js);
    return js.arr(first, second);
  }

  // Writes `raw`, a RawJsSerializable, followed by `data` as its instance's data, and reads them
  // back, with preserveStackInErrors unless it is false.
  JsValue roundTripWithData(
      Lock& js, JsValue raw, JsValue data, jsg::Optional<bool> preserveStackInErrors) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, raw);
      ser.write(js, data);
      ser.release();
    });
    Deserializer deser(js, content,
        Deserializer::Options{.preserveStackInErrors = preserveStackInErrors.orDefault(true)});
    return deser.readValue(js);
  }

  // Writes `in`, cuts the last `drop` bytes off, and reads what is left, as a peer could send it.
  JsValue roundTripTruncated(Lock& js, JsValue in, uint32_t drop) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });
    KJ_REQUIRE(drop <= content.data.size());
    Deserializer deser(js, content.data.first(content.data.size() - drop));
    return deser.readValue(js);
  }

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(modules.exports);
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

  JSG_RESOURCE_TYPE(SerTestContext) {
    JSG_NESTED_TYPE(Foo);
    JSG_NESTED_TYPE(Bar);
    JSG_NESTED_TYPE(Baz);
    JSG_NESTED_TYPE(Qux);
    JSG_NESTED_TYPE(Box);
    JSG_NESTED_TYPE(RawJsSerializable);
    JSG_METHOD(setModule);
    JSG_METHOD(clearModules);
    JSG_METHOD(getPrivateSymbol);
    JSG_METHOD(roundTrip);
    JSG_METHOD(roundTripStrict);
    JSG_METHOD(roundTripTwo);
    JSG_METHOD(roundTripWithData);
    JSG_METHOD(roundTripTruncated);
    JSG_METHOD(foreignRoundTrip);
    JSG_METHOD(foreignMatchesSerializer);
    JSG_METHOD(foreignToSerializer);
    JSG_METHOD(serializerToForeign);
    JSG_METHOD(structuredCloneWithTransfer);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolate,
    SerTestContext,
    SerTestContext::Foo,
    SerTestContext::Bar,
    SerTestContext::Baz,
    SerTestContext::Qux,
    SerTestContext::Box,
    SerTestContext::RawJsSerializable);

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

// Classes implemented in JavaScript, as the per-isolate bootstrap's serialization.ts sets them up,
// exported by the module "test". `constructed` counts constructor calls, and `log` records the
// clone and deserialize calls.
constexpr kj::StringPtr JS_CLASSES = R"(
  const kMode = getPrivateSymbol('workerd:serializeMode');
  const kCloneable = getPrivateSymbol('workerd:cloneable');
  const kX = getPrivateSymbol('test:x');
  let constructed = 0;
  const log = [];

  // A class's descriptor, as makeCloneable() makes it.
  function descriptor(deserializeInfo, clone, deserialize) {
    return Object.freeze({ __proto__: null, deserializeInfo, clone, deserialize });
  }
  function makeCloneable(ctor, deserializeInfo, clone, deserialize) {
    return ctor.prototype[kCloneable] = descriptor(deserializeInfo, clone, deserialize);
  }

  class Point {
    constructor(x) { constructed++; this[kMode] = pointCloneable; this[kX] = x; }
    get x() { return this[kX]; }
  }
  const pointCloneable = makeCloneable(Point, 'test:Point', (p) => {
    log.push(`clone ${p[kX]}`);
    return p[kX];
  }, (p, data) => {
    log.push(`deserialize ${data}`);
    // Detects that the data went through deserialize().
    p[kX] = data + 1;
  });

  // Holds any value as its data, which may be the instance itself.
  class Holder {
    constructor(value) { this[kMode] = holderCloneable; this[kX] = value; }
    get value() { return this[kX]; }
  }
  const holderCloneable = makeCloneable(Holder, 'test:Holder', (h) => ({ value: h[kX] }),
      (h, data) => { h[kX] = data.value; });

  class TestError extends Error {
    constructor(message) { super(message); this[kMode] = testErrorCloneable; }
  }
  TestError.prototype.name = 'TestError';
  const testErrorCloneable = makeCloneable(TestError, 'test:TestError', (e) => e.message,
      (e, data) => {
    Object.defineProperty(e, 'message', { value: data, writable: true, configurable: true });
  });

  // Accepts only a number as its data.
  class Strict {
    constructor(x) { this[kMode] = strictCloneable; this[kX] = x; }
    get x() { return this[kX]; }
  }
  const strictCloneable = makeCloneable(Strict, 'test:Strict', (s) => s[kX], (s, data) => {
    if (typeof data !== 'number') throw new TypeError('Strict data is not a number');
    s[kX] = data;
  });

  class Opaque { constructor() { this[kMode] = 0; } }

  // Cloneable, but written as `deserializeInfo` with `data`, so as anything a peer could send.
  class Forged {
    constructor(deserializeInfo, data = 1) {
      this[kMode] = descriptor(deserializeInfo, () => data, () => {});
    }
  }

  class Throws { constructor() { this[kMode] = throwsCloneable; } }
  const throwsCloneable = makeCloneable(Throws, 'test:Throws', () => 1, () => {
    throw new Error('throw from deserialize');
  });

  class NoDeserialize {}
  class NotADescriptor {}
  NotADescriptor.prototype[kCloneable] = 1;
  class NoDeserializeFunction {}
  NoDeserializeFunction.prototype[kCloneable] = descriptor('test:NoDeserializeFunction', () => 1);

  setModule({ Point, Holder, TestError, Strict, Throws, NoDeserialize, NotADescriptor,
      NoDeserializeFunction, notAFunction: 1 });
)"_kj;

KJ_TEST("serialization of classes implemented in JavaScript") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);
  auto expectEval = [&](kj::StringPtr code, kj::StringPtr type, kj::StringPtr value) {
    e.expectEval(kj::str(JS_CLASSES, code), type, value);
  };

  // An instance is made from the class's prototype without calling the constructor, and
  // initialized from its data.
  expectEval("const p = roundTrip(new Point(1));\n"
             "`${p instanceof Point} ${Object.getPrototypeOf(p) === Point.prototype} ${p.x} "
             "${constructed} ${log}`",
      "string", "true true 2 1 clone 1,deserialize 1");

  // The copy is cloneable too.
  expectEval("roundTrip(roundTrip(new Point(1))).x", "number", "3");

  // A subclass's instance is serialized as one of the class whose descriptor marked it.
  expectEval("class Sub extends Point {}\n"
             "const p = roundTrip(new Sub(1));\n"
             "`${Object.getPrototypeOf(p) === Point.prototype} ${p.x}`",
      "string", "true 2");

  // An instance that appears twice is written once, and read back as one object.
  expectEval("const p = new Point(1);\n"
             "const [a, b] = roundTrip([p, p]);\n"
             "`${a === b} ${a.x} ${log}`",
      "string", "true 2 clone 1,deserialize 1");

  // Its data may refer to the instance itself, and to other instances.
  expectEval("const h = new Holder(null);\n"
             "h[kX] = h;\n"
             "const copy = roundTrip(h);\n"
             "copy.value === copy",
      "boolean", "true");
  expectEval("const r = roundTrip(new Holder([new Point(1), new Holder(new Point(5))]));\n"
             "`${r.value[0].x} ${r.value[1].value.x}`",
      "string", "2 6");
  expectEval("const p = new Point(1);\n"
             "const r = roundTrip({ p, h: new Holder(p) });\n"
             "r.h.value === r.p",
      "boolean", "true");

  // The deserialize methods run once everything has been read, in the order the instances were
  // read.
  expectEval("roundTrip([new Point(1), new Holder(new Point(2)), new Point(3)]);\n"
             "log.join()",
      "string", "clone 1,clone 3,clone 2,deserialize 1,deserialize 3,deserialize 2");

  // A class inheriting from Error makes native errors, with no own message until initialized.
  expectEval("const err = roundTrip(new TestError('boom'));\n"
             "`${err instanceof TestError} ${Object.prototype.toString.call(err)} ${err.message}`",
      "string", "true [object Error] boom");

  // Its stack is written as a native error's is, and restored as an own data property that, like
  // the stack it replaces, is not enumerable.
  expectEval("const e = new TestError('boom');\n"
             "e.stack = 'sender stack';\n"
             "const copy = roundTrip(e);\n"
             "const { value, enumerable } = Object.getOwnPropertyDescriptor(copy, 'stack');\n"
             "`${value} ${enumerable} ${Object.keys(copy).includes('stack')}`",
      "string", "sender stack false false");
  // Without one, it keeps the stack captured when it was made, whose first line, as a native
  // error's does, gives its name and the writer's message. It is not enumerable either.
  expectEval("const e = new TestError('boom');\n"
             "delete e.stack;\n"
             "const copy = roundTrip(e);\n"
             "const { enumerable } = Object.getOwnPropertyDescriptor(copy, 'stack');\n"
             "`${copy.stack.split('\\n')[0]} ${enumerable}`",
      "string", "TestError: boom false");
  // A stack that is not a string is not written, so one that refers back to the error, which a
  // reader could not resolve, does not make the error unreadable.
  expectEval("const e = new TestError('boom');\n"
             "e.stack = { e };\n"
             "roundTrip(e).stack.split('\\n')[0]",
      "string", "TestError: boom");

  // The record's flags say whether a stack follows (1), whether the writer was not using enhanced
  // error serialization (2), and whether a message follows (4). Without preserveStackInErrors, the
  // reader keeps the stack only with flag 2, whatever its own setting (off, here); otherwise the
  // stack it captured begins with the record's message, not the one deserialize() sets. A stack
  // or message of a class that doesn't inherit from Error is ignored, a stack or message that
  // isn't a string is rejected, and so are flags this version doesn't know.
  expectEval("const e = roundTripWithData(\n"
             "    new RawJsSerializable('test:TestError', 1, 'forged stack'), 'm');\n"
             "`${e.stack} ${e.message}`",
      "string", "forged stack m");
  expectEval(
      "const e = roundTripWithData(\n"
      "    new RawJsSerializable('test:TestError', 5, 'forged stack', 'header'), 'm', false);\n"
      "`${e.stack.split('\\n')[0]} ${e.stack.includes('forged')} ${e.message}`",
      "string", "TestError: header false m");
  expectEval("const e = roundTripWithData(\n"
             "    new RawJsSerializable('test:TestError', 1, 'forged stack'), 'm', false);\n"
             "`${e.stack.split('\\n')[0]} ${e.message}`",
      "string", "TestError m");
  expectEval("const e = roundTripWithData(\n"
             "    new RawJsSerializable('test:TestError', 3, 'forged stack'), 'm', false);\n"
             "`${e.stack} ${e.message}`",
      "string", "forged stack m");
  expectEval("const p = roundTripWithData(\n"
             "    new RawJsSerializable('test:Point', 5, 'stack', 'message'), 1);\n"
             "`${Object.hasOwn(p, 'stack')} ${Object.hasOwn(p, 'message')} ${p.x}`",
      "string", "false false 2");
  expectEval("try {\n"
             "  roundTripWithData(new RawJsSerializable('test:TestError', 4, undefined, 1), 'm');\n"
             "} catch (e) {\n"
             "  `${e.name}: ${e.message}`\n"
             "}",
      "string",
      "DataCloneError: Deserialization failed: message of an object implemented in JavaScript is "
      "not a string.");
  expectEval("try {\n"
             "  roundTripWithData(new RawJsSerializable('test:TestError', 1, {}), 'm');\n"
             "} catch (e) {\n"
             "  `${e.name}: ${e.message}`\n"
             "}",
      "string",
      "DataCloneError: Deserialization failed: stack of an object implemented in JavaScript is "
      "not a string.");
  expectEval("try { roundTripWithData(new RawJsSerializable('test:Point', 8), 1); } catch (e) {\n"
             "  `${e.name}: ${e.message}`\n"
             "}",
      "string",
      "DataCloneError: Deserialization failed: unsupported object implemented in "
      "JavaScript.");

  // A record that ends too soon is a DataCloneError wherever it ends: in the flags, in the
  // deserializeInfo, in its length, or in the tag before them. After the header and V8's host
  // object tag, the record is its tag, the length 10, 'test:Point' and the flags, a byte each but
  // the name. Cut off before V8's tag, it is V8's own error for malformed data.
  for (auto drop: {1, 2, 12, 13}) {
    expectEval(kj::str("try {\n"
                       "  roundTripTruncated(new RawJsSerializable('test:Point', 0), ",
                   drop,
                   ");\n"
                   "} catch (e) {\n"
                   "  `${e.name}: ${e.message}`\n"
                   "}"),
        "string", "DataCloneError: Deserialization failed: the data is malformed.");
  }
  expectEval("roundTripTruncated(new RawJsSerializable('test:Point', 0), 14)", "throws",
      "Error: Unable to deserialize cloned data.");

  // Instances serialize the same with class instances otherwise not serializable, as for RPC.
  expectEval("roundTripStrict(new Point(1)).x", "number", "2");

  // Two top-level values each have their instances' data after them.
  expectEval("const p = new Point(1);\n"
             "const [a, b] = roundTripTwo(new Holder(p), p);\n"
             "`${a.value.x} ${b.x} ${a.value === b} ${log}`",
      "string", "2 2 true clone 1,deserialize 1");

  // An instance nested in an API object's own serialization is read as part of the top-level
  // value.
  expectEval("const p = new Point(1);\n"
             "const [a, b] = roundTrip([p, new Box(new Holder(p))]);\n"
             "`${a.x} ${b.val.value === a}`",
      "string", "2 true");

  // The class that marked an instance decides how it is written, whatever its prototype is now.
  expectEval("const p = new Point(1);\n"
             "Object.setPrototypeOf(p, Holder.prototype);\n"
             "const r = roundTrip(p);\n"
             "`${Object.getPrototypeOf(r) === Point.prototype} ${r.x} ${log}`",
      "string", "true 2 clone 1,deserialize 1");

  // An instance whose mark is not a descriptor is not serializable.
  expectEval("try { roundTrip(new Opaque()); } catch (e) { `${e.name}: ${e.message}` }", "string",
      "DataCloneError: Could not serialize object of type \"Opaque\". This type does not support "
      "serialization.");
  expectEval("const p = new Point(1);\n"
             "p[kMode] = 'yes';\n"
             "try { roundTrip(p); } catch (e) { e.name }",
      "string", "DataCloneError");
  // Nor is one whose descriptor lacks a clone function or a deserializeInfo string, or isn't
  // frozen, so that the serializer reads each descriptor once.
  // Nor is a descriptor that is a workerd API object, whose interceptors could run code.
  for (auto mark: {"{}"_kj, "descriptor('test:Point')"_kj, "descriptor(1, () => 1)"_kj,
         "{ __proto__: null, deserializeInfo: 'test:Point', clone: () => 1 }"_kj,
         "Object.freeze(Object.assign(new Foo(1), { deserializeInfo: 'test:Point', clone: () => 1 "
         "}))"_kj}) {
    expectEval(kj::str("const n = new NoDeserialize();\n"
                       "n[kMode] = ",
                   mark,
                   ";\n"
                   "try { roundTrip(n); } catch (e) { e.name }"),
        "string", "DataCloneError");
  }

  // Data naming anything but a loaded class with a descriptor with a deserialize function is
  // rejected.
  for (auto deserializeInfo: {"test:NoDeserialize"_kj, "test:NotADescriptor"_kj,
         "test:NoDeserializeFunction"_kj, "test:notAFunction"_kj, "test:Missing"_kj,
         "test:toString"_kj, "other:Point"_kj, "test"_kj, ""_kj}) {
    expectEval(kj::str("try { roundTrip(new Forged('", deserializeInfo,
                   "')); } catch (e) { `${e.name}: ${e.message}` }"),
        "string",
        "DataCloneError: Deserialization failed: unknown class implemented in JavaScript.");
  }

  // Nor is a class found through an accessor, which the deserializer can't call: an accessor
  // export, however it was defined, or a descriptor's accessor deserialize.
  constexpr kj::StringPtr ACCESSOR_CLASS = R"(
    let getterCalls = 0;
    class Lazy {}
    Lazy.prototype[kCloneable] = descriptor('test:Lazy', () => 1, () => {});
    class LazyDeserialize {}
    LazyDeserialize.prototype[kCloneable] = Object.freeze({ __proto__: null,
        deserializeInfo: 'test:LazyDeserialize', clone: () => 1,
        get deserialize() { getterCalls++; return () => {}; } });
  )"_kj;
  for (auto exports: {"{ get Lazy() { getterCalls++; return Lazy; } }"_kj,
         "Object.defineProperty({}, 'Lazy', { get() { getterCalls++; return Lazy; } })"_kj}) {
    expectEval(kj::str(ACCESSOR_CLASS, "setModule(", exports,
                   ");\n"
                   "try { roundTrip(new Forged('test:Lazy')); } catch (e) {\n"
                   "  `${e.name}: ${e.message} ${getterCalls}`\n"
                   "}"),
        "string",
        "DataCloneError: Deserialization failed: unknown class implemented in JavaScript. 0");
  }
  expectEval(kj::str(ACCESSOR_CLASS,
                 "setModule({ LazyDeserialize });\n"
                 "try { roundTrip(new Forged('test:LazyDeserialize')); } catch (e) {\n"
                 "  `${e.name}: ${e.message} ${getterCalls}`\n"
                 "}"),
      "string",
      "DataCloneError: Deserialization failed: unknown class implemented in JavaScript. 0");

  // Nor is one found through a workerd API object, as the module's exports or as the class's
  // descriptor.
  expectEval("setModule(Object.assign(new Foo(1), { Point }));\n"
             "try { roundTrip(new Point(1)); } catch (e) { `${e.name}: ${e.message}` }",
      "string", "DataCloneError: Deserialization failed: unknown class implemented in JavaScript.");
  expectEval("class Wrapped {}\n"
             "Wrapped.prototype[kCloneable] = Object.assign(new Foo(1), { deserialize() {} });\n"
             "setModule({ Wrapped });\n"
             "try { roundTrip(new Forged('test:Wrapped')); } catch (e) {\n"
             "  `${e.name}: ${e.message}`\n"
             "}",
      "string", "DataCloneError: Deserialization failed: unknown class implemented in JavaScript.");

  // What a deserialize function throws reaches the caller, such as its rejection of data that
  // no instance of its class would have written.
  expectEval("roundTrip(new Throws())", "throws", "Error: throw from deserialize");
  expectEval("roundTrip(new Forged('test:Strict', 'nope'))", "throws",
      "TypeError: Strict data is not a number");

  // A Proxy is not an instance, whatever it wraps.
  expectEval("try { roundTrip(new Proxy(new Point(1), {})); } catch (e) { e.name }", "string",
      "DataCloneError");

  // A mark on an object whose prototype is Object.prototype or null is ignored.
  expectEval("const o = { a: 1 };\n"
             "o[kMode] = 0;\n"
             "const n = Object.create(null);\n"
             "n[kMode] = 0;\n"
             "`${roundTrip(o).a} ${Object.keys(roundTrip(n)).length}`",
      "string", "1 0");

  // A plain class instance is still serialized as a plain object.
  expectEval("class Plain { constructor() { this.a = 1; } }\n"
             "JSON.stringify(roundTrip(new Plain()))",
      "string", "{\"a\":1}");

  // A deserializer outside C++ JSG cannot finish instances.
  expectEval("try { serializerToForeign(new Point(1)); } catch (e) { `${e.name}: ${e.message}` }",
      "string",
      "DataCloneError: This deserializer does not support objects implemented in JavaScript.");
}

KJ_TEST("deserialization of arbitrary classes implemented in JavaScript") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // Whatever class and data a peer names, reading it returns a value or throws; it never crashes.
  // The peer is a Forged instance naming any class, or a random string, with any data. The
  // pseudo-random sequence is fixed, so every run reads the same data.
  e.expectEval(kj::str(JS_CLASSES, R"(
    let seed = 1;
    function random(n) {
      seed = (seed * 48271) % 2147483647;
      return seed % n;
    }
    const names = ['test:Point', 'test:Holder', 'test:TestError', 'test:Strict', 'test:Throws',
        'test:NoDeserialize', 'test:NotADescriptor', 'test:NoDeserializeFunction',
        'test:notAFunction', 'test:Missing', 'test:toString', 'other:Point', 'test', '', ':',
        'test:', 'test:Point:', '\0test:Point'];
    function randomName() {
      if (random(4) > 0) return names[random(names.length)];
      let name = '';
      for (let n = random(12); n > 0; n--) name += 'test:Point\0\u00e9\u{1f600}'[random(13)];
      return name;
    }
    const values = [
      () => undefined, () => null, () => random(100), () => -0.5, () => 'x', () => ({}),
      () => [], () => ({ value: new Point(1) }), () => new Point(2), () => new Holder(null),
      () => new Strict(3), () => new TestError('e'), () => new Error('e'), () => new Uint8Array(2),
      () => new Map([[1, new Point(4)]]), () => new Forged(randomName(), random(10)),
      () => new Forged(randomName(), new Forged(randomName())),
    ];
    let read = 0;
    let threw = 0;
    for (let i = 0; i < 3000; i++) {
      const forged = new Forged(randomName(), values[random(values.length)]());
      try {
        roundTrip([forged, forged]);
        read++;
      } catch (e) {
        threw++;
      }
    }
    `${read > 0} ${threw > 0}`
  )"),
      "string", "true true");
}

KJ_TEST("serialization of classes implemented in JavaScript without bootstrap modules") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // Without the modules to find classes in, nothing can be deserialized.
  e.expectEval(kj::str(JS_CLASSES,
                   "clearModules();\n"
                   "try { roundTrip(new Point(1)); } catch (e) { `${e.name}: ${e.message}` }"),
      "string", "DataCloneError: Deserialization failed: unknown class implemented in JavaScript.");
}

KJ_TEST("a SharedArrayBuffer id that names no buffer is malformed data") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);
  e.run([&](jsg::Lock& js) {
    auto released = ({
      Serializer ser(js);
      ser.write(js, JsValue(v8::SharedArrayBuffer::New(js.v8Isolate, 8)));
      ser.release();
    });
    KJ_ASSERT(released.sharedArrayBuffers.size() == 1);

    // With its buffer, the data is read.
    {
      Deserializer deser(js, released);
      KJ_EXPECT(deser.readValue(js).isSharedArrayBuffer());
    }

    // Without it, the id names no buffer.
    Deserializer deser(
        js, released.data, kj::none, kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>());
    JSG_TRY(js) {
      deser.readValue(js);
      KJ_FAIL_EXPECT("an id that names no buffer was accepted");
    }
    JSG_CATCH(exception) {
      KJ_EXPECT(kj::str(exception.getHandle(js)) == "Error: Unable to deserialize cloned data.");
    }
  });
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
}  // namespace
}  // namespace workerd::jsg::test
