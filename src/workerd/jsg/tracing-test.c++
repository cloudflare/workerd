// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System({"--expose-gc"_kj});

class NumberBoxHolder: public Object {
  // An object that holds a NumberBox and implements GC visitation correctly.
  //
  // This differs from BoxBox (in jsg-test.h) in that this just holds the exact object you give
  // it, whereas BoxBox likes to create new objects.

public:
  explicit NumberBoxHolder(Ref<NumberBox> inner): inner(kj::mv(inner)) {}

  Ref<NumberBox> inner;

  static Ref<NumberBoxHolder> constructor(Ref<NumberBox> inner) {
    return jsg::alloc<NumberBoxHolder>(kj::mv(inner));
  }

  Ref<NumberBox> getInner() { return inner.addRef(); }

  JSG_RESOURCE_TYPE(NumberBoxHolder) {
    JSG_READONLY_INSTANCE_PROPERTY(inner, getInner);
  }

private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

struct TraceTestContext: public Object {
  kj::Maybe<jsg::Ref<NumberBox>> strongRef;
  // A strong reference to a NumberBox which may be get and set.

  jsg::Ref<NumberBox> getStrongRef() {
    return KJ_REQUIRE_NONNULL(strongRef).addRef();
  }

  void setStrongRef(jsg::Ref<NumberBox> ref) {
    strongRef = kj::mv(ref);
  }

  JSG_RESOURCE_TYPE(TraceTestContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(NumberBoxHolder);
    JSG_INSTANCE_PROPERTY(strongRef, getStrongRef, setStrongRef);
  }
};

JSG_DECLARE_ISOLATE_TYPE(TraceTestIsolate, TraceTestContext, NumberBox,
                          NumberBoxHolder);

KJ_TEST("TracedReference usage does not lead to crashes") {
  Evaluator<TraceTestContext, TraceTestIsolate> e(v8System);

  e.expectEval(
      // Create an object holding another object.
      "let holder = new NumberBoxHolder(new NumberBox(123));\n"

      // Do a GC pass to make sure traced wrappers are allocated.
      "gc();\n"

      // Now put the NumberBox into the context's strongRef, and remove the holder. So now
      // the object is only reachable via a strong ref.
      "strongRef = holder.inner;\n"
      "holder = null;\n"

      // Invoke GC. Since the NumberBox is not reachable via tracing from any other object, its
      // tracedWrapper will not be marked and will therefore become invalid.
      "gc();\n"

      // Now create a new holder which holds the NumberBox.
      "holder = new NumberBoxHolder(strongRef);\n"

      // Invoke GC. The new holder will be traced, finding the NumberBox. It had better not crash
      // on the tracedWrapper having been collected!
      "gc();\n"

      // Verify the value is still there...
      "holder.inner.value", "number", "123");
}

}  // namespace
}  // namespace workerd::jsg::test
