#include "frankenvalue.h"

#include <workerd/jsg/jsg-test.h>

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/test.h>

namespace workerd {
namespace {

jsg::V8System v8System;
class ContextGlobalObject: public jsg::Object, public jsg::ContextGlobal {};

struct TestContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext);

KJ_TEST("Frankenvalue") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);

  e.run([&](jsg::Lock& js) {
    // Create a value based on JSON.
    Frankenvalue value = Frankenvalue::fromJson(kj::str(R"({"baz": 321, "qux": "xyz"})"_kj));

    // prop1 is empty.
    value.setProperty(kj::str("prop1"), {});

    // prop2 is a V8-serialized value.
    value.setProperty(kj::str("prop2"), ({
      auto obj = js.obj();
      obj.set(js, "foo", js.num(123));
      obj.set(js, "bar", js.str("abc"_kj));
      Frankenvalue::fromJs(js, obj);
    }));

    // Round trip through capnp.
    {
      capnp::MallocMessageBuilder message;
      auto builder = message.initRoot<rpc::Frankenvalue>();
      value.toCapnp(builder);
      value = Frankenvalue::fromCapnp(builder.asReader());
    }

    // Use clone().
    value = value.clone();

    // Back to JS, then JSON, then check that nothing was lost.
    KJ_EXPECT(js.serializeJson(value.toJs(js)) ==
        R"({"baz":321,"qux":"xyz","prop1":{},"prop2":{"foo":123,"bar":"abc"}})"_kj);
  });
}

}  // namespace
}  // namespace workerd
