// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "string.h"
namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal { };

struct BoolContext: public ContextGlobalObject {
  kj::String takeBool(bool b) {
    return kj::str(b);
  }
  JSG_RESOURCE_TYPE(BoolContext) {
    JSG_METHOD(takeBool);
  }
};
JSG_DECLARE_ISOLATE_TYPE(BoolIsolate, BoolContext);

KJ_TEST("bool") {
  Evaluator<BoolContext, BoolIsolate> e(v8System);
  e.expectEval("takeBool(false)", "string", "false");
  e.expectEval("takeBool(true)", "string", "true");
  e.expectEval("takeBool(123)", "string", "true");
  e.expectEval("takeBool({})", "string", "true");
  e.expectEval("takeBool('')", "string", "false");
  e.expectEval("takeBool('false')", "string", "true");
  e.expectEval("takeBool(null)", "string", "false");
  e.expectEval("takeBool(undefined)", "string", "false");
  e.expectEval("takeBool()",
               "throws", "TypeError: Failed to execute 'takeBool' on 'BoolContext': parameter 1 is "
                         "not of type 'boolean'.");
}

// ========================================================================================

struct OptionalContext: public ContextGlobalObject {
  struct TestOptionalFields {
    Optional<kj::String> optional;
    LenientOptional<kj::String> lenient;
    kj::Maybe<kj::String> nullable;

    JSG_STRUCT(optional, lenient, nullable);
  };

  struct TestAllOptionalFields {
    Optional<kj::String> optString;
    Optional<double> optDouble;

    JSG_STRUCT(optString, optDouble);
  };

  double takeOptional(Optional<Ref<NumberBox>> num) {
    return kj::mv(num).orDefault(jsg::alloc<NumberBox>(321))->value;
  }
  double takeMaybe(kj::Maybe<Ref<NumberBox>> num) {
    return kj::mv(num).orDefault(jsg::alloc<NumberBox>(321))->value;
  }
  double takeLenientOptional(LenientOptional<Ref<NumberBox>> num) {
    return kj::mv(num).orDefault(jsg::alloc<NumberBox>(321))->value;
  }
  kj::String takeOptionalMaybe(Optional<kj::Maybe<kj::String>> arg) {
    return kj::mv(arg).orDefault(kj::str("(absent)")).orDefault(kj::str("(null)"));
  }
  Optional<Ref<NumberBox>> returnOptional(double value) {
    if (value == 321) return kj::none; else return jsg::alloc<NumberBox>(value);
  }
  kj::Maybe<Ref<NumberBox>> returnMaybe(double value) {
    if (value == 321) return kj::none; else return jsg::alloc<NumberBox>(value);
  }

  kj::String readTestOptionalFields(TestOptionalFields s) {
    return kj::str(kj::mv(s.optional).orDefault(kj::str("(absent)")), ", ",
                   kj::mv(s.lenient).orDefault(kj::str("(absent)")), ", ",
                   kj::mv(s.nullable).orDefault(kj::str("(absent)")));
  }
  TestOptionalFields makeTestOptionalFields(Optional<kj::String> optional,
                                            LenientOptional<kj::String> lenient,
                                            kj::Maybe<kj::String> nullable) {
    return { kj::mv(optional), kj::mv(lenient), kj::mv(nullable) };
  }

  kj::String readTestAllOptionalFields(TestAllOptionalFields s) {
    return kj::str(kj::mv(s.optString).orDefault(kj::str("(absent)")), ", ",
                   kj::mv(s.optDouble).orDefault(321));
  }

  JSG_RESOURCE_TYPE(OptionalContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(takeOptional);
    JSG_METHOD(takeMaybe);
    JSG_METHOD(takeLenientOptional);
    JSG_METHOD(takeOptionalMaybe);
    JSG_METHOD(returnOptional);
    JSG_METHOD(returnMaybe);
    JSG_METHOD(readTestOptionalFields);
    JSG_METHOD(makeTestOptionalFields);
    JSG_METHOD(readTestAllOptionalFields);
  }
};
JSG_DECLARE_ISOLATE_TYPE(OptionalIsolate, OptionalContext, OptionalContext::TestOptionalFields,
    OptionalContext::TestAllOptionalFields, NumberBox);

KJ_TEST("optionals and maybes") {
  Evaluator<OptionalContext, OptionalIsolate> e(v8System);
  e.expectEval("takeOptional(new NumberBox(123))", "number", "123");
  e.expectEval("takeOptional()", "number", "321");
  e.expectEval("takeOptional(undefined)", "number", "321");
  e.expectEval("returnOptional(123).value", "number", "123");
  e.expectEval("returnOptional(321)", "undefined", "undefined");

  e.expectEval("takeMaybe(new NumberBox(123))", "number", "123");
  e.expectEval("takeMaybe(null)", "number", "321");
  e.expectEval("takeMaybe(undefined)", "number", "321");
  e.expectEval("returnMaybe(123).value", "number", "123");
  e.expectEval("returnMaybe(321)", "object", "null");

  e.expectEval("takeMaybe()",
      "throws", "TypeError: Failed to execute 'takeMaybe' on 'OptionalContext': parameter 1 is not "
                "of type 'NumberBox'.");
  e.expectEval("takeOptional(null)",
      "throws", "TypeError: Failed to execute 'takeOptional' on 'OptionalContext': parameter 1 is not "
                "of type 'NumberBox'.");

  e.expectEval("takeLenientOptional(new NumberBox(123))", "number", "123");
  e.expectEval("takeLenientOptional()", "number", "321");
  e.expectEval("takeLenientOptional(undefined)", "number", "321");
  e.expectEval("takeLenientOptional(null)", "number", "321");
  e.expectEval("takeLenientOptional((foo) => {})", "number", "321");

  e.expectEval("takeOptionalMaybe()", "string", "(absent)");
  e.expectEval("takeOptionalMaybe(null)", "string", "(null)");
  e.expectEval("takeOptionalMaybe(undefined)", "string", "(absent)");
  e.expectEval("takeOptionalMaybe('a string')", "string", "a string");

  e.expectEval("readTestOptionalFields({nullable: null})", "string", "(absent), (absent), (absent)");
  e.expectEval("readTestOptionalFields({optional: 'foo', lenient: 'bar', nullable: null})",
             "string", "foo, bar, (absent)");
  e.expectEval("readTestOptionalFields({optional: 'foo', lenient: 'bar', nullable: 'baz'})",
             "string", "foo, bar, baz");

#define ENUMERATE_OBJECT \
      "var items = [];\n" \
      "for (var key in object) {\n" \
      "  items.push(key + ': ' + object[key]);\n" \
      "}\n" \
      "items.join(', ')"

  e.expectEval(
      "var object = makeTestOptionalFields(undefined, undefined, null);\n"
      ENUMERATE_OBJECT,
      "string", "nullable: null");
  e.expectEval(
      "var object = makeTestOptionalFields('foo', 'bar', null);\n"
      ENUMERATE_OBJECT,
      "string", "optional: foo, lenient: bar, nullable: null");
  e.expectEval(
      "var object = makeTestOptionalFields('foo', 'bar', 'baz');\n"
      ENUMERATE_OBJECT,
      "string", "optional: foo, lenient: bar, nullable: baz");
  e.expectEval(
      "var object = makeTestOptionalFields(undefined, undefined, 'bar');\n"
      ENUMERATE_OBJECT,
      "string", "nullable: bar");
#undef ENUMERATE_OBJECT

  e.expectEval("readTestAllOptionalFields({})", "string", "(absent), 321");
  e.expectEval("readTestAllOptionalFields(null)", "string", "(absent), 321");
  e.expectEval("readTestAllOptionalFields(undefined)", "string", "(absent), 321");
  e.expectEval("readTestAllOptionalFields()",
      "throws", "TypeError: Failed to execute 'readTestAllOptionalFields' on 'OptionalContext': "
                "parameter 1 is not of type 'TestAllOptionalFields'.");
}

// ========================================================================================
struct MaybeContext: public ContextGlobalObject {

  void test(kj::Maybe<kj::OneOf<NonCoercible<kj::String>>> arg) {}

  JSG_RESOURCE_TYPE(MaybeContext) {
    JSG_METHOD(test);
  }
};
JSG_DECLARE_ISOLATE_TYPE(MaybeIsolate, MaybeContext);

KJ_TEST("maybes - don't substitute null") {

  static const auto config = JsgConfig {
    .noSubstituteNull = true,
  };

  struct MaybeConfig {
    operator const JsgConfig&() const { return config; }
  };

  // This version uses the MaybeConfig above that sets noSubstituteNull = true.
  Evaluator<MaybeContext, MaybeIsolate, MaybeConfig> e(v8System);
  e.expectEval("test({})", "throws",
               "TypeError: Failed to execute 'test' on 'MaybeContext': parameter 1 is not "
               "of type 'string'.");

  // This version uses the default JsgConfig with the noSubstituteNull = false.
  Evaluator<MaybeContext, MaybeIsolate, JsgConfig> e2(v8System);
  e2.expectEval("test({})", "undefined", "undefined");
}

// ========================================================================================

struct OneOfContext: public ContextGlobalObject {
  kj::String takeOneOf(kj::OneOf<double, kj::String, Ref<NumberBox>> value) {
    if (value.is<double>()) {
      return kj::str("double: ", value.get<double>());
    } else if (value.is<kj::String>()) {
      return kj::str("kj::String: ", value.get<kj::String>());
    } else if (value.is<Ref<NumberBox>>()) {
      return kj::str("NumberBox: ", value.get<Ref<NumberBox>>()->value);
    } else {
      return kj::str("none of the above -- can't get here");
    }
  }
  kj::OneOf<double, kj::String, Ref<NumberBox>> returnOneOf(
      kj::Maybe<double> num, kj::Maybe<kj::String> str, kj::Maybe<Ref<NumberBox>> box) {
    kj::OneOf<double, kj::String, Ref<NumberBox>> result;
    KJ_IF_SOME(n, num) {
      result.init<double>(n);
    } else KJ_IF_SOME(s, str) {
      result.init<kj::String>(kj::mv(s));
    } else KJ_IF_SOME(b, box) {
      result.init<Ref<NumberBox>>(b.addRef());
    }
    return result;
  }

  using StringOrBool = kj::OneOf<kj::String, bool>;
  using NumberOrBool = kj::OneOf<double, bool>;
  using StringOrNumber = kj::OneOf<kj::String, double>;

  kj::String takeStringOrBool(StringOrBool value) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(s, kj::String) {
        return kj::str("kj::String: ", s);
      }
      KJ_CASE_ONEOF(b, bool) {
        return kj::str("bool: ", b);
      }
    }
    KJ_UNREACHABLE;
  }
  kj::String takeNumberOrBool(NumberOrBool value) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(d, double) {
        return kj::str("double: ", d);
      }
      KJ_CASE_ONEOF(b, bool) {
        return kj::str("bool: ", b);
      }
    }
    KJ_UNREACHABLE;
  }
  kj::String takeStringOrNumber(StringOrNumber value) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(s, kj::String) {
        return kj::str("kj::String: ", s);
      }
      KJ_CASE_ONEOF(d, double) {
        return kj::str("double: ", d);
      }
    }
    KJ_UNREACHABLE;
  }

  using NestedOneOf = kj::OneOf<double, StringOrBool>;

  kj::String takeNestedOneOf(NestedOneOf value) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(d, double) {
        return kj::str("double: ", d);
      }
      KJ_CASE_ONEOF(oof, kj::OneOf<kj::String, bool>) {
        KJ_SWITCH_ONEOF(oof) {
          KJ_CASE_ONEOF(s, kj::String) {
            return kj::str("kj::String: ", s);
          }
          KJ_CASE_ONEOF(b, bool) {
            return kj::str("bool: ", b);
          }
        }
      }
    }
    KJ_UNREACHABLE;
  }

  JSG_RESOURCE_TYPE(OneOfContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(takeOneOf);
    JSG_METHOD(returnOneOf);
    JSG_METHOD(takeStringOrBool);
    JSG_METHOD(takeNumberOrBool);
    JSG_METHOD(takeStringOrNumber);
    JSG_METHOD(takeNestedOneOf);
  }
};
JSG_DECLARE_ISOLATE_TYPE(OneOfIsolate, OneOfContext, NumberBox);

KJ_TEST("OneOf") {
  Evaluator<OneOfContext, OneOfIsolate> e(v8System);
  e.expectEval("takeOneOf(123)", "string", "double: 123");
  e.expectEval("takeOneOf('foo')", "string", "kj::String: foo");
  e.expectEval("takeOneOf(new NumberBox(321))", "string", "NumberBox: 321");
  e.expectEval("takeOneOf(undefined)", "string", "kj::String: undefined");

  e.expectEval("returnOneOf(123, null, null)", "number", "123");
  e.expectEval("returnOneOf(null, 'foo', null)", "string", "foo");
  e.expectEval("returnOneOf(null, null, new NumberBox(321)).value", "number", "321");
  e.expectEval("returnOneOf(null, null, null)", "undefined", "undefined");

  e.expectEval("takeStringOrBool(123)",     "string", "kj::String: 123");
  e.expectEval("takeStringOrBool('123')",   "string", "kj::String: 123");
  e.expectEval("takeStringOrBool(true)",    "string", "bool: true");

  e.expectEval("takeNumberOrBool(123)",     "string", "double: 123");
  e.expectEval("takeNumberOrBool('123')",   "string", "double: 123");
  e.expectEval("takeNumberOrBool(true)",    "string", "bool: true");

  e.expectEval("takeStringOrNumber(123)",   "string", "double: 123");
  e.expectEval("takeStringOrNumber('123')", "string", "kj::String: 123");
  e.expectEval("takeStringOrNumber(true)",  "string", "kj::String: true");

  e.expectEval("takeNestedOneOf(123)",       "string", "double: 123");
  e.expectEval("takeNestedOneOf('123')",     "string", "kj::String: 123");
  e.expectEval("takeNestedOneOf(true)",      "string", "bool: true");
  e.expectEval("takeNestedOneOf(undefined)", "string", "kj::String: undefined");
  e.expectEval("takeNestedOneOf(null)",      "string", "kj::String: null");
  e.expectEval("takeNestedOneOf({})",        "string", "kj::String: [object Object]");
}

// ========================================================================================

struct DictContext: public ContextGlobalObject {
  kj::String takeDict(Dict<Ref<NumberBox>> dict) {
    return kj::strArray(
        KJ_MAP(f, dict.fields) { return kj::str(f.name, ": ", f.value->value); }, ", ");
  }
  kj::String takeDictOfFunctions(Lock& js, Dict<Function<int()>> dict) {
    return kj::strArray(
        KJ_MAP(f, dict.fields) { return kj::str(f.name, ": ", f.value(js)); }, ", ");
  }
  Dict<Ref<NumberBox>> returnDict() {
    auto builder = kj::heapArrayBuilder<Dict<Ref<NumberBox>>::Field>(3);
    builder.add(Dict<Ref<NumberBox>>::Field {kj::str("foo"), jsg::alloc<NumberBox>(123)});
    builder.add(Dict<Ref<NumberBox>>::Field {kj::str("bar"), jsg::alloc<NumberBox>(456)});
    builder.add(Dict<Ref<NumberBox>>::Field {kj::str("baz"), jsg::alloc<NumberBox>(789)});
    return { builder.finish() };
  }

  JSG_RESOURCE_TYPE(DictContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(takeDict);
    JSG_METHOD(takeDictOfFunctions);
    JSG_METHOD(returnDict);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DictIsolate, DictContext, NumberBox);

KJ_TEST("dicts") {
  Evaluator<DictContext, DictIsolate> e(v8System);
  e.expectEval(
      "takeDict({foo: new NumberBox(123), bar: new NumberBox(456), baz: new NumberBox(789)})",
      "string", "foo: 123, bar: 456, baz: 789");
  e.expectEval(
      "var dict = returnDict();\n"
      "[dict.foo.value, dict.bar.value, dict.baz.value].join(', ')",
      "string", "123, 456, 789");

  e.expectEval(
      "takeDict({foo: new NumberBox(123), bar: 456, baz: new NumberBox(789)})",
      "throws",
      "TypeError: Incorrect type for map entry 'bar': the provided value is not of type "
      "'NumberBox'.");

  e.expectEval(
      "takeDictOfFunctions({\n"
      "  foo() { return this.bar() + 123; },\n"
      "  bar() { return 456; }\n"
      "})",
      "string", "foo: 579, bar: 456");
}

// ========================================================================================

struct IntContext: public ContextGlobalObject {
  kj::String takeInt(int i) {
    return kj::str("int: ", i);
  }
  int returnInt() {
    return 123;
  }
  JSG_RESOURCE_TYPE(IntContext) {
    JSG_METHOD(takeInt);
    JSG_METHOD(returnInt);
  }
};
JSG_DECLARE_ISOLATE_TYPE(IntIsolate, IntContext);

KJ_TEST("integers") {
  Evaluator<IntContext, IntIsolate> e(v8System);
  e.expectEval("takeInt(123)", "string", "int: 123");
  e.expectEval("returnInt()", "number", "123");

  e.expectEval("takeInt(1)", "string", "int: 1");
  e.expectEval("takeInt(-1)", "string", "int: -1");
  e.expectEval("takeInt(123.5)", "string", "int: 123");
  e.expectEval("takeInt(null)", "string", "int: 0");
  e.expectEval("takeInt(undefined)", "string", "int: 0");
  e.expectEval("takeInt(Number.NaN)", "string", "int: 0");
  e.expectEval("takeInt(Number.POSITIVE_INFINITY)", "string", "int: 0");
  e.expectEval("takeInt(Number.NEGATIVE_INFINITY)", "string", "int: 0");
  e.expectEval("takeInt({})", "string", "int: 0");

  e.expectEval("takeInt(2147483647)", "string", "int: 2147483647");
  e.expectEval("takeInt(-2147483648)", "string", "int: -2147483648");

  e.expectEval("takeInt(2147483648)", "throws",
               "TypeError: Value out of range. Must be between "
               "-2147483648 and 2147483647 (inclusive).");
  e.expectEval("takeInt(-2147483649)", "throws",
               "TypeError: Value out of range. Must be between "
               "-2147483648 and 2147483647 (inclusive).");
  e.expectEval("takeInt(Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be between "
               "-2147483648 and 2147483647 (inclusive).");
  e.expectEval("takeInt(-Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be between "
               "-2147483648 and 2147483647 (inclusive).");
}

// ========================================================================================
struct Uint32Context: public ContextGlobalObject {
  kj::String takeUint32(uint32_t i) {
    return kj::str("uint32_t: ", i);
  }
  uint32_t returnUint32() {
    return 123;
  }
  uint32_t takeOneOfUint32(kj::OneOf<kj::String, uint32_t> i) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(str, kj::String) {
        KJ_FAIL_ASSERT("Should not have been interpreted as a string.");
      }
      KJ_CASE_ONEOF(num, uint32_t) {
        return num;
      }
    }
    KJ_UNREACHABLE;
  }

  JSG_RESOURCE_TYPE(Uint32Context) {
    JSG_METHOD(takeUint32);
    JSG_METHOD(takeOneOfUint32);
    JSG_METHOD(returnUint32);
  }
};
JSG_DECLARE_ISOLATE_TYPE(Uint32Isolate, Uint32Context);
KJ_TEST("unsigned integers") {
  Evaluator<Uint32Context, Uint32Isolate> e(v8System);
  e.expectEval("takeUint32(123)", "string", "uint32_t: 123");
  e.expectEval("returnUint32()", "number", "123");

  e.expectEval("takeUint32(1)", "string", "uint32_t: 1");
  e.expectEval("takeUint32(123.5)", "string", "uint32_t: 123");
  e.expectEval("takeUint32(null)", "string", "uint32_t: 0");

  e.expectEval("takeOneOfUint32(1)", "number", "1");

  e.expectEval("takeUint32(-1)",
               "throws",
               "TypeError: The value cannot be converted because it is negative and this "
               "API expects a positive number.");
  e.expectEval("takeUint32({})",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint32(undefined)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint32(Number.NaN)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint32(Number.POSITIVE_INFINITY)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint32(Number.NEGATIVE_INFINITY)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");

  e.expectEval("takeUint32(4294967295)", "string", "uint32_t: 4294967295");

  e.expectEval("takeUint32(4294967296)", "throws",
               "TypeError: Value out of range. Must be less than or equal to 4294967295.");
  e.expectEval("takeUint32(Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be less than or equal to 4294967295.");
}

// ========================================================================================
struct Uint64Context: public ContextGlobalObject {
  kj::String takeUint64(uint64_t i) {
    return kj::str("uint64_t: ", i);
  }
  uint64_t returnUint64() {
    return 123;
  }
  kj::String takeInt64(int64_t i) {
    return kj::str("int64_t: ", i);
  }
  uint64_t takeOneOfUint64(kj::OneOf<kj::String, uint64_t> i) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(str, kj::String) {
        KJ_FAIL_ASSERT("Should not have been interpreted as a string.");
      }
      KJ_CASE_ONEOF(num, uint64_t) {
        return num;
      }
    }
    KJ_UNREACHABLE;
  }
  int64_t takeOneOfInt64(kj::OneOf<kj::String, int64_t> i) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(str, kj::String) {
        KJ_FAIL_ASSERT("Should not have been interpreted as a string.");
      }
      KJ_CASE_ONEOF(num, int64_t) {
        return num;
      }
    }
    KJ_UNREACHABLE;
  }
  int64_t returnInt64() {
    return 123;
  }
  JSG_RESOURCE_TYPE(Uint64Context) {
    JSG_METHOD(takeUint64);
    JSG_METHOD(takeOneOfUint64);
    JSG_METHOD(takeOneOfInt64);
    JSG_METHOD(returnUint64);
    JSG_METHOD(takeInt64);
    JSG_METHOD(returnInt64);
  }
};
JSG_DECLARE_ISOLATE_TYPE(Uint64Isolate, Uint64Context);
KJ_TEST("bigints") {
  Evaluator<Uint64Context, Uint64Isolate> e(v8System);
  e.expectEval("takeUint64(123)", "string", "uint64_t: 123");
  e.expectEval("takeUint64(123n)", "string", "uint64_t: 123");
  e.expectEval("takeUint64(1n)", "string", "uint64_t: 1");
  e.expectEval("takeUint64(1)", "string", "uint64_t: 1");
  e.expectEval("takeUint64(123.5)", "string", "uint64_t: 123");
  e.expectEval("takeUint64(null)", "string", "uint64_t: 0");
  e.expectEval("takeUint64(BigInt(1))", "string", "uint64_t: 1");

  e.expectEval("takeOneOfUint64(1)", "bigint", "1");
  e.expectEval("takeOneOfUint64(1n)", "bigint", "1");

  e.expectEval("takeOneOfInt64(1)", "bigint", "1");
  e.expectEval("takeOneOfInt64(1n)", "bigint", "1");

  e.expectEval("takeInt64(123)", "string", "int64_t: 123");
  e.expectEval("takeInt64(123n)", "string", "int64_t: 123");

  e.expectEval("takeInt64(1n)", "string", "int64_t: 1");
  e.expectEval("takeInt64(-1n)", "string", "int64_t: -1");
  e.expectEval("takeInt64(1)", "string", "int64_t: 1");
  e.expectEval("takeInt64(-1)", "string", "int64_t: -1");
  e.expectEval("takeInt64(123.5)", "string", "int64_t: 123");
  e.expectEval("takeInt64(null)", "string", "int64_t: 0");
  e.expectEval("takeInt64('1')", "string", "int64_t: 1");
  e.expectEval("takeInt64(BigInt(-1))", "string", "int64_t: -1");

  e.expectEval("returnUint64()", "bigint", "123");
  e.expectEval("returnInt64()", "bigint", "123");

  e.expectEval("takeUint64(-1)",
               "throws",
               "TypeError: The value cannot be converted because it is negative and this "
               "API expects a positive bigint.");
  e.expectEval("takeUint64(-1n)",
               "throws",
               "TypeError: The value cannot be converted because it is either negative and "
               "this API expects a positive bigint, or the value would be truncated.");

  e.expectEval("takeUint64(undefined)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeInt64(undefined)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeInt64('hello')",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeInt64({})",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeInt64(Number.NaN)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeInt64(Number.POSITIVE_INFINITY)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeInt64(Number.NEGATIVE_INFINITY)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");

  e.expectEval("takeUint64('hello')",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint64({})",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint64(Number.NaN)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint64(Number.POSITIVE_INFINITY)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");
  e.expectEval("takeUint64(Number.NEGATIVE_INFINITY)",
               "throws",
               "TypeError: The value cannot be converted because it is not an integer.");

  e.expectEval("takeUint64(18446744073709551615n)", "string", "uint64_t: 18446744073709551615");

  e.expectEval("takeUint64(18446744073709551616n)", "throws",
               "TypeError: The value cannot be converted because it is either negative "
               "and this API expects a positive bigint, or the value would be truncated.");

  e.expectEval("takeInt64(9223372036854775807n)", "string", "int64_t: 9223372036854775807");
  e.expectEval("takeInt64(9223372036854775808n)", "throws",
               "TypeError: The value cannot be converted because it would be truncated.");
}

// ========================================================================================

struct Int8Context: public ContextGlobalObject {
  kj::String takeInt8(int8_t i) {
    return kj::str("int8_t: ", i);
  }
  kj::String takeUint8(uint8_t i) {
    return kj::str("uint8_t: ", i);
  }
  int8_t returnInt8() {
    return 123;
  }
  uint8_t returnUint8() {
    return 123;
  }
  JSG_RESOURCE_TYPE(Int8Context) {
    JSG_METHOD(takeInt8);
    JSG_METHOD(takeUint8);
    JSG_METHOD(returnInt8);
    JSG_METHOD(returnUint8);
  }
};
JSG_DECLARE_ISOLATE_TYPE(Int8Isolate, Int8Context);

KJ_TEST("int8 integers") {
  Evaluator<Int8Context, Int8Isolate> e(v8System);
  e.expectEval("takeInt8(123)", "string", "int8_t: 123");
  e.expectEval("takeUint8(123)", "string", "uint8_t: 123");
  e.expectEval("returnInt8()", "number", "123");
  e.expectEval("returnUint8()", "number", "123");

  e.expectEval("takeInt8(1)", "string", "int8_t: 1");
  e.expectEval("takeInt8(-1)", "string", "int8_t: -1");
  e.expectEval("takeInt8(123.5)", "string", "int8_t: 123");

  e.expectEval("takeInt8(127)", "string", "int8_t: 127");
  e.expectEval("takeInt8(-128)", "string", "int8_t: -128");
  e.expectEval("takeUint8(255)", "string", "uint8_t: 255");

  e.expectEval("takeUint8(-1)", "throws",
               "TypeError: The value cannot be converted because it is negative and this "
               "API expects a positive number.");
  e.expectEval("takeInt8(32768)", "throws",
               "TypeError: Value out of range. Must be between "
               "-128 and 127 (inclusive).");
  e.expectEval("takeInt8(-32769)", "throws",
               "TypeError: Value out of range. Must be between "
               "-128 and 127 (inclusive).");
  e.expectEval("takeInt8(Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be between "
               "-128 and 127 (inclusive).");
  e.expectEval("takeInt8(-Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be between "
               "-128 and 127 (inclusive).");
}

// ========================================================================================

struct Int16Context: public ContextGlobalObject {
  kj::String takeInt16(int16_t i) {
    return kj::str("int16_t: ", i);
  }
  kj::String takeUint16(uint16_t i) {
    return kj::str("uint16_t: ", i);
  }
  int16_t returnInt16() {
    return 123;
  }
  uint16_t returnUint16() {
    return 123;
  }
  JSG_RESOURCE_TYPE(Int16Context) {
    JSG_METHOD(takeInt16);
    JSG_METHOD(takeUint16);
    JSG_METHOD(returnInt16);
    JSG_METHOD(returnUint16);
  }
};
JSG_DECLARE_ISOLATE_TYPE(Int16Isolate, Int16Context);

KJ_TEST("int16 integers") {
  Evaluator<Int16Context, Int16Isolate> e(v8System);
  e.expectEval("takeInt16(123)", "string", "int16_t: 123");
  e.expectEval("takeUint16(123)", "string", "uint16_t: 123");
  e.expectEval("returnInt16()", "number", "123");
  e.expectEval("returnUint16()", "number", "123");

  e.expectEval("takeInt16(1)", "string", "int16_t: 1");
  e.expectEval("takeInt16(-1)", "string", "int16_t: -1");
  e.expectEval("takeInt16(123.5)", "string", "int16_t: 123");

  e.expectEval("takeInt16(32767)", "string", "int16_t: 32767");
  e.expectEval("takeInt16(-32768)", "string", "int16_t: -32768");
  e.expectEval("takeUint16(65535)", "string", "uint16_t: 65535");

  e.expectEval("takeUint16(-1)", "throws",
               "TypeError: The value cannot be converted because it is negative and this "
               "API expects a positive number.");
  e.expectEval("takeInt16(32768)", "throws",
               "TypeError: Value out of range. Must be between "
               "-32768 and 32767 (inclusive).");
  e.expectEval("takeInt16(-32769)", "throws",
               "TypeError: Value out of range. Must be between "
               "-32768 and 32767 (inclusive).");
  e.expectEval("takeInt16(Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be between "
               "-32768 and 32767 (inclusive).");
  e.expectEval("takeInt16(-Number.MAX_SAFE_INTEGER)", "throws",
               "TypeError: Value out of range. Must be between "
               "-32768 and 32767 (inclusive).");
}

// ========================================================================================

struct DoubleContext: public ContextGlobalObject {
  kj::String takeDouble(double d) {
    return kj::str("double: ", d);
  }
  double returnDouble() {
    return 123.5;
  }

  JSG_RESOURCE_TYPE(DoubleContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(takeDouble);
    JSG_METHOD(returnDouble);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DoubleIsolate, DoubleContext, NumberBox);

KJ_TEST("floating points") {
  Evaluator<DoubleContext, DoubleIsolate> e(v8System);
  e.expectEval("takeDouble(123)", "string", "double: 123");
  e.expectEval("takeDouble(123.5)", "string", "double: 123.5");
  e.expectEval("takeDouble('123')", "string", "double: 123");
  e.expectEval("takeDouble(' \\r\\n123')", "string", "double: 123");
  e.expectEval("takeDouble('0x7b')", "string", "double: 123");
  e.expectEval("takeDouble(true)", "string", "double: 1");
  e.expectEval("takeDouble(Number.MAX_SAFE_INTEGER)", "string", "double: 9007199254740991");
  e.expectEval("takeDouble({ valueOf: function() { return 456.7; } })", "string", "double: 456.7");
  e.expectEval("returnDouble()", "number", "123.5");

  e.expectEval("takeDouble([Symbol.iterator])",
      "throws", "TypeError: Cannot convert a Symbol value to a string");
  e.expectEval("takeDouble('123asdf')", "string", "double: nan");
  e.expectEval("takeDouble('asdf123')", "string", "double: nan");
  e.expectEval("takeDouble(null)", "string", "double: 0");
  e.expectEval("takeDouble(undefined)", "string", "double: nan");
  e.expectEval("takeDouble(Number.NaN)", "string", "double: nan");
  e.expectEval("takeDouble(Number.POSITIVE_INFINITY)", "string", "double: inf");
  e.expectEval("takeDouble(Number.NEGATIVE_INFINITY)", "string", "double: -inf");
  e.expectEval("takeDouble({})", "string", "double: nan");
  e.expectEval("takeDouble(new NumberBox(321))", "string", "double: nan");
}

// ========================================================================================

struct StringContext: public ContextGlobalObject {
  kj::String takeString(kj::String s) {
    return kj::mv(s);
  }
  JSG_RESOURCE_TYPE(StringContext) {
    JSG_METHOD(takeString);
  }
};
JSG_DECLARE_ISOLATE_TYPE(StringIsolate, StringContext);

KJ_TEST("kj::Strings") {
  Evaluator<StringContext, StringIsolate> e(v8System);
  e.expectEval("takeString(false)", "string", "false");
  e.expectEval("takeString(true)", "string", "true");
  e.expectEval("takeString(123)", "string", "123");
  e.expectEval("takeString(Number.NaN)", "string", "NaN");
  e.expectEval("takeString(Number.POSITIVE_INFINITY)", "string", "Infinity");
  e.expectEval("takeString(null)", "string", "null");
  e.expectEval("takeString(undefined)", "string", "undefined");
  e.expectEval("takeString('an actual string')", "string", "an actual string");
  e.expectEval("takeString({ toString: function() { return 'toString()ed'; } })",
               "string", "toString()ed");
}

// ========================================================================================

struct ByteStringContext: public ContextGlobalObject {
  ByteString takeByteString(ByteString s) {
    return kj::mv(s);
  }
  JSG_RESOURCE_TYPE(ByteStringContext) {
    JSG_METHOD(takeByteString);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ByteStringIsolate, ByteStringContext);

KJ_TEST("ByteStrings") {
  Evaluator<ByteStringContext, ByteStringIsolate> e(v8System);
  e.expectEval("takeByteString('foo\\0bar') === 'foo\\0bar'", "boolean", "true");
  // ﬃ is 0xEF 0xAC 0x83 in UTF-8.
  e.expectEval("takeByteString('\\xEF\\xAC\\x83') === '\\xEF\\xAC\\x83'", "boolean", "true");

  // TODO(cleanup): ByteString should become HeaderString somewhere in the api directory.
}

// ========================================================================================

struct RawContext: public ContextGlobalObject {
  struct TwoValues {
    Value $foo;
    Value $bar;
    JSG_STRUCT($foo, $bar);
  };
  TwoValues twoValues(Value foo, Value bar) {
    return { kj::mv(foo), kj::mv(bar) };
  }
  JSG_RESOURCE_TYPE(RawContext) {
    JSG_METHOD(twoValues);
  }
};
JSG_DECLARE_ISOLATE_TYPE(RawIsolate, RawContext, RawContext::TwoValues);

KJ_TEST("Raw Values") {
  Evaluator<RawContext, RawIsolate> e(v8System);
  e.expectEval(
      "JSON.stringify(twoValues({baz: 123}, 'abcd'))",

      "string", "{\"foo\":{\"baz\":123},\"bar\":\"abcd\"}"
  );
}

// ========================================================================================

struct DateContext: public ContextGlobalObject {
  kj::Date takeDate(kj::Date date) {
    return date;
  }
  JSG_RESOURCE_TYPE(DateContext) {
    JSG_METHOD(takeDate);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DateIsolate, DateContext);

KJ_TEST("Date Values") {
  Evaluator<DateContext, DateIsolate> e(v8System);
  e.expectEval("takeDate(new Date('2022-01-22T00:54:57.893Z')).toUTCString()",
               "string",
               "Sat, 22 Jan 2022 00:54:57 GMT");
  e.expectEval("takeDate(12345).valueOf()",
               "number",
               "12345");
  e.expectEval("takeDate(8640000000000000).valueOf()",
               "throws",
               "TypeError: This API doesn't support dates after 2189."),
  e.expectEval("takeDate(-8640000000000000).valueOf()",
               "throws",
               "TypeError: This API doesn't support dates before 1687."),
  e.expectEval("takeDate(1/0)",
               "throws",
               "TypeError: The value cannot be converted because it is not a valid Date."),
  e.expectEval("takeDate(new Date(1/0))",
               "throws",
               "TypeError: The value cannot be converted because it is not a valid Date."),
  e.expectEval("takeDate(new Date('1800-01-22T00:54:57.893Z')).toUTCString()",
               "string",
               "Wed, 22 Jan 1800 00:54:57 GMT");
  e.expectEval("takeDate('2022-01-22T00:54:57.893Z')",
               "throws",
               "TypeError: Failed to execute 'takeDate' on 'DateContext': parameter "
               "1 is not of type 'date'.");
}

// ========================================================================================

struct ArrayContext: public ContextGlobalObject {
  kj::Array<int> takeArray(kj::Array<int> array) {
    // The ArrayWrapper uses a stack array with a max size of 64. This is just a
    // quick test to ensure that arrays larger than that are properly supported.
    KJ_ASSERT(array.size() == 65);
    KJ_ASSERT(array[64] == 1);
    return kj::mv(array);
  }
  kj::Array<int> takeArguments(int i, Arguments<int> array) {
    KJ_ASSERT(i == 123);
    return kj::mv(array);
  }
  JSG_RESOURCE_TYPE(ArrayContext) {
    JSG_METHOD(takeArray);
    JSG_METHOD(takeArguments);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ArrayIsolate, ArrayContext);

KJ_TEST("Array Values") {
  Evaluator<ArrayContext, ArrayIsolate> e(v8System);
  e.expectEval("m = Array(65); m[64] = 1; takeArray(m)[64]", "number", "1");

  e.expectEval("takeArguments(123, 456, 789, 321).join(', ')", "string", "456, 789, 321");
}

// ========================================================================================

struct SequenceContext: public ContextGlobalObject {
  Sequence<kj::String> testSequence(Sequence<kj::String> sequence) {
    KJ_ASSERT(sequence.size() == 2);
    KJ_ASSERT(sequence[0] == "a");
    KJ_ASSERT(sequence[1] == "b");
    return kj::mv(sequence);
  }

  Sequence<UsvString> testUsv(Sequence<UsvString> sequence) {
    KJ_ASSERT(sequence.size() == 2);
    KJ_ASSERT(sequence[0] == usv("a"));
    KJ_ASSERT(sequence[1] == usv("b"));
    return kj::mv(sequence);
  }

  Sequence<UsvString> testUsv2(Sequence<Sequence<UsvString>> sequence) {
    KJ_ASSERT(sequence.size() == 2);
    kj::Vector<UsvString> flat;
    for (auto& s : sequence) {
      for (auto& p : s) {
        flat.add(kj::mv(p));
      }
    }
    return Sequence<UsvString>(flat.releaseAsArray());
  }

  Sequence<int> testInt(Sequence<int> sequence) {
    KJ_ASSERT(sequence.size(), 2);
    return kj::mv(sequence);
  }

  struct Foo {
    kj::String a;
    JSG_STRUCT(a);
  };

  Sequence<Foo> testFoo(Sequence<Foo> sequence) {
    KJ_ASSERT(sequence.size() == 1);
    return kj::mv(sequence);
  }

  // Because the kj::OneOf lists kj::String separately, and because JavaScript
  // strings are technically iterable, we want to make sure that the Sequence
  // ignores strings.
  bool oneof1(kj::OneOf<kj::String, Sequence<kj::String>> input) {
    KJ_SWITCH_ONEOF(input) {
      KJ_CASE_ONEOF(str, kj::String) {
        KJ_ASSERT(str == "aa");
        return true;
      }
      KJ_CASE_ONEOF(seq, Sequence<kj::String>) {
        KJ_ASSERT(seq[0] == "b");
        KJ_ASSERT(seq[1] == "b");
        return true;
      }
    }
    KJ_UNREACHABLE;
  }

  bool oneof2(kj::OneOf<UsvString, Sequence<UsvString>> input) {
    KJ_SWITCH_ONEOF(input) {
      KJ_CASE_ONEOF(str, UsvString) {
        KJ_ASSERT(str == usv("aa"));
        return true;
      }
      KJ_CASE_ONEOF(seq, Sequence<UsvString>) {
        KJ_ASSERT(seq[0] == usv("b"));
        KJ_ASSERT(seq[1] == usv("b"));
        return true;
      }
    }
    KJ_UNREACHABLE;
  }

  JSG_RESOURCE_TYPE(SequenceContext) {
    JSG_METHOD(testSequence);
    JSG_METHOD(testUsv);
    JSG_METHOD(testUsv2);
    JSG_METHOD(testInt);
    JSG_METHOD(testFoo);
    JSG_METHOD(oneof1);
    JSG_METHOD(oneof2);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SequenceIsolate, SequenceContext, SequenceContext::Foo);

KJ_TEST("Sequence Values") {
  Evaluator<SequenceContext, SequenceIsolate> e(v8System);
  e.expectEval("testSequence(['a', 'b']).join('')", "string", "ab");
  e.expectEval(
    "const val = {*[Symbol.iterator]() { yield 'a'; yield 'b'; }};"
    "testSequence(val).join('')", "string", "ab");
  e.expectEval("testUsv(['a', 'b']).join('')", "string", "ab");
  e.expectEval(
    "const val = {*[Symbol.iterator]() { yield 'a'; yield 'b'; }};"
    "testUsv(val).join('')", "string", "ab");
  e.expectEval(
    "const val = {*[Symbol.iterator]() { yield 'c', yield 'd'; }};"
    "testUsv2([['a','b'],val]).join('')", "string", "abcd");
  e.expectEval("testInt([1,2]).join('')", "string", "12");
  e.expectEval("testInt([1,'2']).join('')", "string", "12");
  e.expectEval("testInt([1,'a']).join('')", "string", "10");
  e.expectEval("testInt([1,null]).join('')", "string", "10");
  e.expectEval("testInt([1,NaN]).join('')", "string", "10");
  e.expectEval("testFoo([{a:'a'}])[0].a", "string", "a");
  e.expectEval("oneof1('aa')", "boolean", "true");
  e.expectEval("oneof1(['b', 'b'])", "boolean", "true");
  e.expectEval("oneof2('aa')", "boolean", "true");
  e.expectEval("testFoo({a:'a'})", "throws", "TypeError: Failed to execute 'testFoo' on 'SequenceContext': parameter 1 is not of type 'Sequence'.");
}

// ========================================================================================

struct NonCoercibleContext: public ContextGlobalObject {
  template <CoercibleType T>
  bool test(NonCoercible<T>) {
    return true;
  }

  template <CoercibleType T>
  bool testCoerced(T) {
    return true;
  }

  bool testMaybeString(Optional<NonCoercible<kj::String>> value) {
    KJ_IF_SOME(v, value) {
      KJ_ASSERT(v.value != "null"_kj);
    }
    return true;
  }

  bool testMaybeStringCoerced(Optional<kj::String> value) {
    KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "null"_kj);
    return true;
  }

  bool testOneOf(kj::OneOf<NonCoercible<bool>, NonCoercible<kj::String>> value) {
    return true;
  }

  JSG_RESOURCE_TYPE(NonCoercibleContext) {
    JSG_METHOD_NAMED(testString, template test<kj::String>);
    JSG_METHOD_NAMED(testStringCoerced, template testCoerced<kj::String>);
    JSG_METHOD_NAMED(testBoolean, template test<bool>);
    JSG_METHOD_NAMED(testBooleanCoerced, template testCoerced<bool>);
    JSG_METHOD_NAMED(testDouble, template test<double>);
    JSG_METHOD_NAMED(testDoubleCoerced, template testCoerced<double>);
    JSG_METHOD(testMaybeString);
    JSG_METHOD(testMaybeStringCoerced);
    JSG_METHOD(testOneOf);
  }
};
JSG_DECLARE_ISOLATE_TYPE(NonCoercibleIsolate, NonCoercibleContext);

KJ_TEST("NonCoercible Values") {
  Evaluator<NonCoercibleContext, NonCoercibleIsolate> e(v8System);
  e.expectEval("testString('')", "boolean", "true");
  e.expectEval("testString(null)", "throws",
               "TypeError: Failed to execute 'testString' on 'NonCoercibleContext': parameter 1 is "
               "not of type 'string'.");
  e.expectEval("testString({})", "throws",
               "TypeError: Failed to execute 'testString' on 'NonCoercibleContext': parameter 1 is "
               "not of type 'string'.");
  e.expectEval("testString(1)", "throws",
               "TypeError: Failed to execute 'testString' on 'NonCoercibleContext': parameter 1 is "
               "not of type 'string'.");
  e.expectEval("testStringCoerced('')", "boolean", "true");
  e.expectEval("testStringCoerced(null)", "boolean", "true");
  e.expectEval("testStringCoerced({})", "boolean", "true");
  e.expectEval("testStringCoerced(1)", "boolean", "true");

  e.expectEval("testBoolean(true)", "boolean", "true");
  e.expectEval("testBoolean(null)", "throws",
               "TypeError: Failed to execute 'testBoolean' on 'NonCoercibleContext': parameter 1 is"
               " not of type 'boolean'.");
  e.expectEval("testBooleanCoerced(true)", "boolean", "true");
  e.expectEval("testBooleanCoerced(null)", "boolean", "true");

  e.expectEval("testDouble(1.1)", "boolean", "true");
  e.expectEval("testDouble(Infinity)", "boolean", "true");
  e.expectEval("testDouble(NaN)", "boolean", "true");
  e.expectEval("testDouble(null)", "throws",
               "TypeError: Failed to execute 'testDouble' on 'NonCoercibleContext': parameter 1 is"
               " not of type 'number'.");
  e.expectEval("testDoubleCoerced(1.1)", "boolean", "true");
  e.expectEval("testDoubleCoerced(null)", "boolean", "true");

  e.expectEval("testMaybeString('')", "boolean", "true");
  e.expectEval("testMaybeString(undefined)", "boolean", "true");
  e.expectEval("testMaybeString(null)", "throws",
               "TypeError: Failed to execute 'testMaybeString' on 'NonCoercibleContext': parameter"
               " 1 is not of type 'string'.");
  e.expectEval("testMaybeString(1)", "throws",
               "TypeError: Failed to execute 'testMaybeString' on 'NonCoercibleContext': parameter"
               " 1 is not of type 'string'.");

  e.expectEval("testMaybeStringCoerced(null)", "boolean", "true");

  e.expectEval("testOneOf(false)", "boolean", "true");
  e.expectEval("testOneOf('')", "boolean", "true");
  e.expectEval("testOneOf(new String(''))", "throws",
               "TypeError: Failed to execute 'testOneOf' on 'NonCoercibleContext': parameter 1 is"
               " not of type 'boolean or string'.");
}

// ========================================================================================

struct MemoizedIdentityContext: public ContextGlobalObject {
  static constexpr kj::Date DATE = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS;
  MemoizedIdentity<kj::Date> date = DATE;

  kj::Date getDate() {
    return DATE;
  }

  MemoizedIdentity<kj::Date>& getDateMemoized() {
    return date;
  }

  JSG_RESOURCE_TYPE(MemoizedIdentityContext) {
    JSG_METHOD(getDate);
    JSG_METHOD(getDateMemoized);
  }
};
JSG_DECLARE_ISOLATE_TYPE(MemoizedIdentityIsolate, MemoizedIdentityContext);

KJ_TEST("MemoizedIdentity Values") {
  Evaluator<MemoizedIdentityContext, MemoizedIdentityIsolate> e(v8System);
  e.expectEval("getDate() === getDate()", "boolean", "false");
  e.expectEval("getDateMemoized() === getDateMemoized()", "boolean", "true");
}

// ========================================================================================

struct IdentifiedContext: public ContextGlobalObject {
  kj::String compare(Identified<kj::Date> a, Identified<kj::Date> b, v8::Isolate* isolate) {
    bool result = a.identity == b.identity;
    KJ_EXPECT(a.identity.hashCode() != 0);
    KJ_EXPECT(b.identity.hashCode() != 0);
    if (result) {
      KJ_EXPECT(a.identity.hashCode() == b.identity.hashCode());
    }
    KJ_EXPECT(a.identity.hashCode() == a.identity.getHandle(isolate)->GetIdentityHash());
    KJ_EXPECT(b.identity.hashCode() == b.identity.getHandle(isolate)->GetIdentityHash());

    return kj::str(result, ' ', a.unwrapped - b.unwrapped);
  }

  JSG_RESOURCE_TYPE(IdentifiedContext) {
    JSG_METHOD(compare);
  }
};
JSG_DECLARE_ISOLATE_TYPE(IdentifiedIsolate, IdentifiedContext);

KJ_TEST("Identified values") {
  Evaluator<IdentifiedContext, IdentifiedIsolate> e(v8System);

  e.expectEval("compare(new Date(123), new Date(123))", "string", "false 0ns");
  e.expectEval("compare(new Date(456), new Date(123))", "string", "false 333ms");
  e.expectEval("let d = new Date(123); compare(d, d)", "string", "true 0ns");
}

// ========================================================================================

struct ExceptionContext: public ContextGlobalObject {

  kj::String testToException(kj::Exception exception) {
    return kj::str(exception.getDescription());
  }

  kj::Exception testFromException(int n) {
    switch (n) {
      case 1:
        return JSG_KJ_EXCEPTION(FAILED, TypeError, "boom");
      case 2:
        return JSG_KJ_EXCEPTION(FAILED, DOMAbortError, "boom");
    }
    KJ_UNREACHABLE;
  }

  JSG_RESOURCE_TYPE(ExceptionContext) {
    JSG_METHOD(testToException);
    JSG_METHOD(testFromException);
    JSG_NESTED_TYPE(DOMException);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ExceptionIsolate, ExceptionContext);

KJ_TEST("kj::Exception wrapper works") {
  Evaluator<ExceptionContext, ExceptionIsolate> e(v8System);

  e.expectEval("testToException(new DOMException('boom', 'AbortError'))", "string",
               "jsg.DOMException(AbortError): boom");
  e.expectEval("testToException(new SyntaxError('boom'))", "string",
               "jsg.SyntaxError: boom");
  e.expectEval("testToException(undefined)", "string",
               "jsg.Error: undefined");
  e.expectEval("testToException(1)", "string",
               "jsg.Error: 1");

  e.expectEval("testFromException(1)", "object", "TypeError: boom");
  e.expectEval("testFromException(2)", "object", "AbortError: boom");
}

// ========================================================================================
struct NameContext: public ContextGlobalObject {
  Name name(Name value) {
    return kj::mv(value);
  }

  Name forSymbol(Lock& js, kj::String symbol) {
    return js.newSymbol(symbol);
  }

  Name forSymbolShared(Lock& js, kj::String symbol) {
    return js.newSharedSymbol(symbol);
  }

  Name forSymbolApi(Lock& js, kj::String symbol) {
    return js.newApiSymbol(symbol);
  }

  JSG_RESOURCE_TYPE(NameContext) {
    JSG_METHOD(name);
    JSG_METHOD(forSymbol);
    JSG_METHOD(forSymbolShared);
    JSG_METHOD(forSymbolApi);
  }
};
JSG_DECLARE_ISOLATE_TYPE(NameIsolate, NameContext);

KJ_TEST("jsg::Name works") {
  Evaluator<NameContext, NameIsolate> e(v8System);
  e.expectEval("name('hello')", "string", "hello");
  e.expectEval("name(Symbol('foo')).description", "string", "foo");
  e.expectEval("name(Symbol.for('foo')).description", "string", "foo");
  e.expectEval("forSymbol('foo').description", "string", "foo");
  e.expectEval("forSymbolShared('foo').description", "string", "foo");
  e.expectEval("forSymbolApi('foo').description", "string", "foo");
  e.expectEval("forSymbol('foo') !== Symbol.for('foo')", "boolean", "true");
  e.expectEval("forSymbolShared('foo') === Symbol.for('foo')", "boolean", "true");
  e.expectEval("forSymbolShared('foo') !== forSymbolApi('foo')", "boolean", "true");
}

}  // namespace
}  // namespace workerd::jsg::test
