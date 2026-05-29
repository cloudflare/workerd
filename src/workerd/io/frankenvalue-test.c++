#include "frankenvalue.h"

#include <workerd/jsg/jsg-test.h>

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/test.h>

namespace workerd {
namespace {

jsg::V8System v8System;
class ContextGlobalObject: public jsg::Object, public jsg::ContextGlobal {};

enum class TestSerializationTag { TEST_SERIALIZABLE };

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
      jsg::Lock& js, TestSerializationTag tag, jsg::Deserializer& deserializer) {
    auto& handler = KJ_REQUIRE_NONNULL(deserializer.getExternalHandler());
    auto externalHandler = dynamic_cast<Frankenvalue::CapTableReader*>(&handler);
    KJ_REQUIRE(externalHandler != nullptr);

    auto& cap = KJ_REQUIRE_NONNULL(externalHandler->get(deserializer.readRawUint32()));
    auto typedCap = dynamic_cast<TestCapEntry*>(&cap);
    KJ_REQUIRE(typedCap != nullptr);

    return js.alloc<TestSerializableCap>(typedCap->value);
  }

  JSG_SERIALIZABLE(TestSerializationTag::TEST_SERIALIZABLE);

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

KJ_TEST("Frankenvalue fromCapnp rejects capTableSize uint32 overflow") {
  // Regression test for AUTOVULN-EW-EDGEWORKER-15: fromCapnpImpl() accumulated per-node
  // UInt32 capTableSize fields into a 32-bit uint capCount with no overflow check. An attacker
  // could craft capTableSize values that wrap around 2^32 so the final sum equals capTable.size()
  // while individual Property::capTableOffset/capTableSize values are arbitrary, leading to OOB
  // slice bounds in toJsImpl().
  //
  // Construction: root capTableSize=0x80000000, one property with capTableSize=0x80000001.
  // Walk: capCount=0 -> +=0x80000000 -> 0x80000000; record property offset=0x80000000;
  // recurse: capCount += 0x80000001 -> wraps to 0x00000001 (mod 2^32).
  // Final KJ_REQUIRE(capTable.size()==1 == capCount==1) would pass without the fix.

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Frankenvalue>();
  builder.setEmptyObject();
  builder.setCapTableSize(0x80000000u);

  auto props = builder.initProperties(1);
  props[0].setName("p");
  props[0].setEmptyObject();
  props[0].setCapTableSize(0x80000001u);

  // Provide exactly 1 real cap table entry — the wrapped sum would equal 1.
  kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> capTable;
  capTable.add(kj::heap<TestCapEntry>(42));

  // The fix must reject this before the overflow can produce bogus slice bounds.
  KJ_EXPECT_THROW_MESSAGE(
      "capTableSize exceeds", Frankenvalue::fromCapnp(builder.asReader(), kj::mv(capTable)));
}

KJ_TEST("Frankenvalue fromCapnp rejects capTableSize exceeding capTable") {
  // Simpler case: a single node claims more caps than actually exist, without overflow.
  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Frankenvalue>();
  builder.setEmptyObject();
  builder.setCapTableSize(100);  // Claims 100 caps but we only provide 1.

  kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> capTable;
  capTable.add(kj::heap<TestCapEntry>(42));

  KJ_EXPECT_THROW_MESSAGE(
      "capTableSize exceeds", Frankenvalue::fromCapnp(builder.asReader(), kj::mv(capTable)));
}

}  // namespace
}  // namespace workerd
