// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct SelfStruct {
  SelfRef self;
  int i;

  JSG_STRUCT(self, i);
};

struct StructContext: public Object, public ContextGlobal {
  kj::String readTestStruct(TestStruct s) {
    return kj::str(s.str, ", ", s.num, ", ", s.box->value);
  }
  TestStruct makeTestStruct(jsg::Lock& js, kj::String str, double num, NumberBox& box) {
    return {kj::mv(str), num, js.alloc<NumberBox>(box.value)};
  }
  V8Ref<v8::Object> readSelfStruct(Lock& js, SelfStruct s) {
    KJ_ASSERT(s.i == 123);
    return kj::mv(s.self);
  }
  SelfStruct makeSelfStruct(Lock& js) {
    return {.self = {js.v8Isolate, v8::Object::New(js.v8Isolate)}, .i = 456};
  }

  JSG_RESOURCE_TYPE(StructContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(readTestStruct);
    JSG_METHOD(makeTestStruct);
    JSG_METHOD(readSelfStruct);
    JSG_METHOD(makeSelfStruct);
  }
};
JSG_DECLARE_ISOLATE_TYPE(StructIsolate, StructContext, NumberBox, TestStruct, SelfStruct);

KJ_TEST("structs") {
  Evaluator<StructContext, StructIsolate> e(v8System);
  e.expectEval(
      "readTestStruct({str: 'foo', num: 123, box: new NumberBox(456)})", "string", "foo, 123, 456");
  e.expectEval("var s = makeTestStruct('foo', 123, new NumberBox(456));\n"
               "[s.str, s.num, s.box.value].join(', ')",
      "string", "foo, 123, 456");

  e.expectEval("readTestStruct({str: 'foo', num: 123, box: 'wrong'})", "throws",
      "TypeError: Incorrect type for the 'box' field on 'TestStruct': the provided "
      "value is not of type 'NumberBox'.");

  e.expectEval(
      "JSON.stringify(readSelfStruct({i: 123, x: 'foo'}))", "string", "{\"i\":123,\"x\":\"foo\"}");
  e.expectEval("JSON.stringify(makeSelfStruct())", "string", "{\"i\":456}");
}

}  // namespace
}  // namespace workerd::jsg::test
