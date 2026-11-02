// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

// A context template inherits the context templates of its JSG superclasses, including ones that
// can't be a context's global object themselves.
struct SplitContextBase: public Object {
  JSG_RESOURCE_TYPE(SplitContextBase) {
    JSG_NESTED_TYPE(NumberBox);
  }
};
struct SplitContext: public SplitContextBase, public ContextGlobal {
  JSG_RESOURCE_TYPE(SplitContext) {
    JSG_INHERIT(SplitContextBase);
    JSG_NESTED_TYPE(BoxBox);
  }
};
JSG_DECLARE_SPLIT_ISOLATE_TYPE(SplitIsolate, 2, SplitContext, SplitContextBase, NumberBox, BoxBox);

// An application defines each shard in a translation unit of its own. Here, shard 1 is instead
// compiled with the isolate type's constructor, which can see the definition of
// registerTemplateShard() that this provides.
JSG_DEFINE_ISOLATE_TEMPLATE_SHARD(SplitIsolate, 0);

KJ_TEST("split isolate types build templates in every shard") {
  Evaluator<SplitContext, SplitIsolate> e(v8System);
  e.expectEval("new NumberBox(123).value", "number", "123");
  e.expectEval("new BoxBox(new NumberBox(123), 321).inner.value", "number", "444");
  e.expectEval("var n = new NumberBox(123);\n"
               "n.incrementBy(321);\n"
               "n.getValue()",
      "number", "444");
  e.expectEval("new NumberBox(123) instanceof NumberBox", "boolean", "true");
  e.expectEval("new NumberBox(123) instanceof BoxBox", "boolean", "false");
}

}  // namespace
}  // namespace workerd::jsg::test
