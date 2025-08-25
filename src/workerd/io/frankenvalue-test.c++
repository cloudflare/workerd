#include "frankenvalue.h"

#include <workerd/jsg/jsg-test.h>

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/test.h>

namespace workerd {
namespace {

jsg::V8System v8System;
class ContextGlobalObject: public jsg::Object, public jsg::ContextGlobal {};

enum class TestSerilaizationTag { TEST_SERIALIZABLE };

struct TestCapEntry final: public Frankenvalue::CapTableEntry {
  int value;

  TestCapEntry(int value): value(value) {}
  kj::Own<CapTableEntry> clone() override {
    return kj::heap<TestCapEntry>(value);
  }
};

class TestSerializableCap: public jsg::Object {
 public:
  TestSerializableCap(int value): value(value) {}

  int toJSON() {
    return value;
  }

  JSG_RESOURCE_TYPE(TestSerializableCap) {
    JSG_METHOD(toJSON);
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
    auto& handler = KJ_ASSERT_NONNULL(serializer.getExternalHandler());
    auto externalHandler = dynamic_cast<Frankenvalue::CapTableBuilder*>(&handler);
    KJ_ASSERT(externalHandler != nullptr);
    serializer.writeRawUint32(externalHandler->add(kj::heap<TestCapEntry>(value)));
  }

  static jsg::Ref<TestSerializableCap> deserialize(
      jsg::Lock& js, TestSerilaizationTag tag, jsg::Deserializer& deserializer) {
    auto& handler = KJ_REQUIRE_NONNULL(deserializer.getExternalHandler());
    auto externalHandler = dynamic_cast<Frankenvalue::CapTableReader*>(&handler);
    KJ_REQUIRE(externalHandler != nullptr);

    auto& cap = KJ_REQUIRE_NONNULL(externalHandler->get(deserializer.readRawUint32()));
    auto typedCap = dynamic_cast<TestCapEntry*>(&cap);
    KJ_REQUIRE(typedCap != nullptr);

    return js.alloc<TestSerializableCap>(typedCap->value);
  }

  JSG_SERIALIZABLE(TestSerilaizationTag::TEST_SERIALIZABLE);

 private:
  int value;
};

struct TestContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext, TestSerializableCap);

KJ_TEST("Frankenvalue") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);

  // Silence compiler warning.
  (void)TestSerializableCap::jsgSerializeOneway;
  (void)TestSerializableCap::jsgSerializeOldTags;

  e.run([&](auto& lock) {
    jsg::Lock& js = lock;
    auto makeSerializableCap = [&](int i) {
      return jsg::JsValue(lock.wrap(js.v8Context(), js.alloc<TestSerializableCap>(i)));
    };

    // Create a value based on JSON.
    Frankenvalue value = Frankenvalue::fromJson(kj::str(R"({"baz": 321, "qux": "xyz"})"_kj));

    // prop1 is empty.
    value.setProperty(kj::str("prop1"), {});

    // prop2 is a V8-serialized value.
    value.setProperty(kj::str("prop2"), [&]() {
      auto obj = js.obj();
      obj.set(js, "foo", js.num(123));
      obj.set(js, "bar", js.str("abc"_kj));
      obj.set(js, "baz", makeSerializableCap(234));
      obj.set(js, "qux", makeSerializableCap(345));

      auto result = Frankenvalue::fromJs(js, obj);

      result.setProperty(kj::str("nested"), [&]() {
        auto nested = js.obj();
        nested.set(js, "corge", js.num(222));
        nested.set(js, "grault",
            jsg::JsValue(lock.wrap(js.v8Context(), js.alloc<TestSerializableCap>(333))));
        return Frankenvalue::fromJs(js, nested);
      }());
      result.setProperty(kj::str("nested2"), [&]() {
        auto nested = js.obj();
        nested.set(js, "garply", js.num(444));
        nested.set(js, "waldo",
            jsg::JsValue(lock.wrap(js.v8Context(), js.alloc<TestSerializableCap>(555))));
        return Frankenvalue::fromJs(js, nested);
      }());

      return result;
    }());

    KJ_ASSERT(value.getCapTable().size() == 4);
    KJ_EXPECT(kj::downcast<TestCapEntry>(*value.getCapTable()[0]).value == 234);
    KJ_EXPECT(kj::downcast<TestCapEntry>(*value.getCapTable()[1]).value == 345);
    KJ_EXPECT(kj::downcast<TestCapEntry>(*value.getCapTable()[2]).value == 333);
    KJ_EXPECT(kj::downcast<TestCapEntry>(*value.getCapTable()[3]).value == 555);

    // Round trip through capnp.
    {
      capnp::MallocMessageBuilder message;
      auto builder = message.initRoot<rpc::Frankenvalue>();
      value.toCapnp(builder);

      kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> newCapTable;
      for (auto& entry: value.getCapTable()) {
        newCapTable.add(entry->clone());
      }

      value = Frankenvalue::fromCapnp(builder.asReader(), kj::mv(newCapTable));
    }

    // Use clone().
    value = value.clone();

    // Back to JS, then JSON, then check that nothing was lost.
    KJ_EXPECT(js.serializeJson(value.toJs(js)) ==
        R"({"baz":321,"qux":"xyz","prop1":{},"prop2":{"foo":123,"bar":"abc","baz":234,"qux":345)"_kj
        R"(,"nested":{"corge":222,"grault":333},"nested2":{"garply":444,"waldo":555}}})"_kj);
  });
}

}  // namespace
}  // namespace workerd
