// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include <workerd/jsg/resource-test.capnp.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct BoxContext: public Object {
  JSG_RESOURCE_TYPE(BoxContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_NESTED_TYPE(BoxBox);
  }
};
JSG_DECLARE_ISOLATE_TYPE(BoxIsolate, BoxContext, NumberBox, BoxBox);

KJ_TEST("constructors and properties") {
  Evaluator<BoxContext, BoxIsolate> e(v8System);
  e.expectEval("new NumberBox(123).value", "number", "123");
  e.expectEval("new NumberBox(123).boxed.value", "number", "123");
  e.expectEval("new BoxBox(new NumberBox(123), 321).inner.value", "number", "444");
  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.value = 321;\n"
      "n.getValue()", "number", "321");
  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.boxed = new NumberBox(321);\n"
      "n.getValue()", "number", "321");
  e.expectEval("new NumberBox(123) instanceof NumberBox", "boolean", "true");
  e.expectEval("new NumberBox(123) instanceof BoxBox", "boolean", "false");
}

KJ_TEST("methods") {
  Evaluator<BoxContext, BoxIsolate> e(v8System);
  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.increment();\n"
      "n.getValue()", "number", "124");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.incrementBy(321);\n"
      "n.getValue()", "number", "444");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.incrementByBox(new NumberBox(321));\n"
      "n.getValue()", "number", "444");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.add(321)", "number", "444");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.addBox(new NumberBox(321))", "number", "444");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.addReturnBox(321).value", "number", "444");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "n.addMultiple(new NumberBox(321), 111, new NumberBox(2222))", "number", "2777");

  e.expectEval(
      "var n = new NumberBox(123);\n"
      "new n.increment();", "throws", "TypeError: n.increment is not a constructor");
}

// ========================================================================================

struct Mixin {
  int getValue() { return i; }
  Mixin(int i): i(i) {}
  int i;
};
struct InheritsMixin: public Object, public Mixin {
  InheritsMixin(int i): Mixin(i) {}

  JSG_RESOURCE_TYPE(InheritsMixin) {
    JSG_METHOD(getValue);
  }
};
struct InheritsMixinContext: public Object {
  Ref<InheritsMixin> makeInheritsMixin(int i) {
    return jsg::alloc<InheritsMixin>(i);
  }

  JSG_RESOURCE_TYPE(InheritsMixinContext) {
    JSG_METHOD(makeInheritsMixin);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InheritsMixinIsolate, InheritsMixinContext, InheritsMixin);

KJ_TEST("JSG_METHODs can be implemented by mixins") {
  Evaluator<InheritsMixinContext, InheritsMixinIsolate> e(v8System);
  e.expectEval(
      "makeInheritsMixin(12345).getValue()", "number", "12345");
}

// ========================================================================================

struct PrototypePropertyObject: public Object {
  double value;

  PrototypePropertyObject(double value): value(value) {}

  static Ref<PrototypePropertyObject> constructor(double value) {
    return alloc<PrototypePropertyObject>(value);
  }

  double getValue() { return value; }
  void setValue(double v) { value = v; }

  JSG_RESOURCE_TYPE(PrototypePropertyObject) {
    JSG_PROTOTYPE_PROPERTY(value, getValue, setValue);
  }
};

struct PropContext: public Object {
  PropContext(): contextProperty(kj::str("default-context-property-value")) {}

  kj::StringPtr getContextProperty() { return contextProperty; }
  void setContextProperty(kj::String s) { contextProperty = kj::mv(s); }

  JSG_RESOURCE_TYPE(PropContext) {
    JSG_METHOD(getContextProperty);
    JSG_METHOD(setContextProperty);
    JSG_INSTANCE_PROPERTY(contextProperty, getContextProperty, setContextProperty);

    JSG_NESTED_TYPE(PrototypePropertyObject);
  }

private:
  kj::String contextProperty;
};
JSG_DECLARE_ISOLATE_TYPE(PropIsolate, PropContext, PrototypePropertyObject);

KJ_TEST("context methods and properties") {
  Evaluator<PropContext, PropIsolate> e(v8System);
  e.expectEval(
      "getContextProperty()", "string", "default-context-property-value");
  e.expectEval(
      "setContextProperty('foo');\n"
      "getContextProperty()", "string", "foo");

  e.expectEval(
      "contextProperty", "string", "default-context-property-value");
  e.expectEval(
      "contextProperty = 'foo'; getContextProperty()", "string", "foo");

  e.expectEval(
      "this.getContextProperty()", "string", "default-context-property-value");
  e.expectEval(
      "this.setContextProperty('foo');\n"
      "getContextProperty()", "string", "foo");

  e.expectEval(
      "this.contextProperty", "string", "default-context-property-value");
  e.expectEval(
      "this.contextProperty = 'foo'; getContextProperty()", "string", "foo");

  e.expectEval(
      "let p = new PrototypePropertyObject(123);\n"
      "let o = {};\n"
      "o.__proto__ = p.__proto__;\n"
      "o.value",
      "throws",
      "TypeError: Illegal invocation");
  e.expectEval(
      "let p = new PrototypePropertyObject(123);\n"
      "let o = {};\n"
      "o.__proto__ = p.__proto__;\n"
      "o.value = 123",
      "throws",
      "TypeError: Illegal invocation");

  e.expectEval(
      "class P2 extends PrototypePropertyObject {\n"
      "  constructor(v) { super(v); }\n"
      "}\n"
      "let p = new P2(123);\n"
      "p.value", "number", "123");
}

// ========================================================================================

struct NonConstructibleContext: public Object {
  struct NonConstructible: public Object {
    NonConstructible(double x): x(x) {}

    double x;

    double method() {
      return x;
    }

    JSG_RESOURCE_TYPE(NonConstructible) {
      JSG_METHOD(method);
    }
  };

  Ref<NonConstructible> getNonConstructible(double x) {
    return jsg::alloc<NonConstructible>(x);
  }

  JSG_RESOURCE_TYPE(NonConstructibleContext) {
    JSG_NESTED_TYPE(NonConstructible);
    JSG_METHOD(getNonConstructible);
  }
};
JSG_DECLARE_ISOLATE_TYPE(NonConstructibleIsolate, NonConstructibleContext,
    NonConstructibleContext::NonConstructible);

KJ_TEST("non-constructible types can't be constructed") {
  Evaluator<NonConstructibleContext, NonConstructibleIsolate> e(v8System);
  e.expectEval(
      "new NonConstructible().method()",
      "throws", "TypeError: Illegal constructor");

  e.expectEval(
      "getNonConstructible(12321).method()",
      "number", "12321");

  e.expectEval(
      "getNonConstructible(12321) instanceof NonConstructible",
      "boolean", "true");
}

// ========================================================================================

struct IterableContext: public Object {
  class Iterable: public Object {
  public:
    static Ref<Iterable> constructor() { return jsg::alloc<Iterable>(); }

    class Iterator: public Object {
    public:
      struct NextValue {
        bool done;
        Optional<int> value;
        JSG_STRUCT(done, value);
      };

      explicit Iterator(Ref<Iterable> parentParam)
          : parent(kj::mv(parentParam)), cursor(parent->values) {}

      NextValue next() {
        if (cursor == parent->values + sizeof(parent->values) / sizeof(*parent->values)) {
          return {.done = true, .value = nullptr};
        }
        return {.done = false, .value = *cursor++};
      }

      v8::Local<v8::Object> self(const v8::FunctionCallbackInfo<v8::Value>& info) {
        // Helper to make this iterator itself iterable. This allows code like
        // `for (let k of iterable.entries())` to work.
        return info.This();
      }

      JSG_RESOURCE_TYPE(Iterator) {
        JSG_INHERIT_INTRINSIC(v8::kIteratorPrototype);
        JSG_METHOD(next);
        JSG_ITERABLE(self);
      }

    private:
      Ref<Iterable> parent;
      int* cursor;
    };

    Ref<Iterator> entries(const v8::FunctionCallbackInfo<v8::Value>& info) {
      return jsg::alloc<Iterator>(JSG_THIS);
    }

    JSG_RESOURCE_TYPE(Iterable) {
      JSG_METHOD(entries);
      JSG_ITERABLE(entries);
    }

  private:
    int values[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    // In real code, this data structure could be more complex, and we would need to think about
    // iterator invalidation, which might require storing back-references from the parent iterable
    // to all of its live iterators to make sure they can be nulled out if necessary. But then we
    // need to worry about circular references ...
  };

  JSG_RESOURCE_TYPE(IterableContext) {
    JSG_NESTED_TYPE(Iterable);
  }
};
JSG_DECLARE_ISOLATE_TYPE(IterableIsolate, IterableContext, IterableContext::Iterable,
    IterableContext::Iterable::Iterator, IterableContext::Iterable::Iterator::NextValue);

KJ_TEST("Iterables can be iterated") {
  Evaluator<IterableContext, IterableIsolate> e(v8System);
  e.expectEval(
      "let results = [];"
      "for (let n of new Iterable()) { results.push(n); };"
      "'' + results.join('')",
      "string", "0123456789"
  );
  e.expectEval(
      "let results = [];"
      "for (let n of new Iterable().entries()) { results.push(n); };"
      "'' + results.join('')",
      "string", "0123456789"
  );
  e.expectEval(
      "let arrayIterator = [][Symbol.iterator]();"
      "let arrayIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(arrayIterator));"
      "let iterator = new Iterable().entries();"
      "let iteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(iterator));"
      "iteratorPrototype === arrayIteratorPrototype",
      "boolean", "true"
  );
}

// ========================================================================================

struct StaticContext: public Object {
  struct StaticConstants: public Object {
    static Ref<StaticConstants> constructor() { return jsg::alloc<StaticConstants>(); }

    static constexpr double        DOUBLE = 1.5;
    static constexpr int           INT = 123;
    static constexpr bool          BOOL = true;
    static constexpr kj::StringPtr STRING = "a static constant string"_kj;

    JSG_RESOURCE_TYPE(StaticConstants) {
      JSG_STATIC_CONSTANT(DOUBLE);
      JSG_STATIC_CONSTANT(INT);
      JSG_STATIC_CONSTANT(BOOL);
      JSG_STATIC_CONSTANT(STRING);
    }
  };

  struct StaticMethods: public Object {
    static Ref<StaticMethods> constructor() { return jsg::alloc<StaticMethods>(); }

    static v8::Local<v8::Value> passThrough(v8::Local<v8::Value> arg) { return arg; }
    static v8::Local<v8::Value> passThroughWithInfo(const v8::FunctionCallbackInfo<v8::Value>& info,
                                                    v8::Local<v8::Value> arg) { return arg; }

    static void voidCall() {}
    static void voidCallWithInfo(const v8::FunctionCallbackInfo<v8::Value>& info) {}

    static Unimplemented unimplementedStaticMethod() { return {}; }

    static void delete_() {}

    JSG_RESOURCE_TYPE(StaticMethods) {
      JSG_STATIC_METHOD(passThrough);
      JSG_STATIC_METHOD(passThroughWithInfo);
      JSG_STATIC_METHOD(voidCall);
      JSG_STATIC_METHOD(voidCallWithInfo);
      JSG_STATIC_METHOD_NAMED(delete, delete_);
      JSG_STATIC_METHOD(unimplementedStaticMethod);
    }
  };

  JSG_RESOURCE_TYPE(StaticContext) {
    JSG_NESTED_TYPE(StaticConstants);
    JSG_NESTED_TYPE(StaticMethods);
  }
};
JSG_DECLARE_ISOLATE_TYPE(StaticIsolate, StaticContext, StaticContext::StaticConstants,
    StaticContext::StaticMethods);

KJ_TEST("Static constants are exposed as constructor properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "StaticConstants.DOUBLE === 1.5",
      "boolean", "true"
  );
  e.expectEval(
      "StaticConstants.INT === 123",
      "boolean", "true"
  );
  e.expectEval(
      "StaticConstants.BOOL === true",
      "boolean", "true"
  );
  e.expectEval(
      "StaticConstants.STRING === 'a static constant string'",
      "boolean", "true"
  );
}
KJ_TEST("Static constants are exposed as constructor prototype properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "StaticConstants.prototype.DOUBLE === 1.5",
      "boolean", "true"
  );
  e.expectEval(
      "StaticConstants.prototype.INT === 123",
      "boolean", "true"
  );
  e.expectEval(
      "StaticConstants.prototype.BOOL === true",
      "boolean", "true"
  );
  e.expectEval(
      "StaticConstants.prototype.STRING === 'a static constant string'",
      "boolean", "true"
  );
}
KJ_TEST("Static constants are exposed as object instance properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "new StaticConstants().DOUBLE === 1.5",
      "boolean", "true"
  );
  e.expectEval(
      "new StaticConstants().INT === 123",
      "boolean", "true"
  );
  e.expectEval(
      "new StaticConstants().BOOL === true",
      "boolean", "true"
  );
  e.expectEval(
      "new StaticConstants().STRING === 'a static constant string'",
      "boolean", "true"
  );
}
KJ_TEST("Static constants are exposed as object instance prototype properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "Object.getPrototypeOf(new StaticConstants()).DOUBLE === 1.5",
      "boolean", "true"
  );
  e.expectEval(
      "Object.getPrototypeOf(new StaticConstants()).INT === 123",
      "boolean", "true"
  );
  e.expectEval(
      "Object.getPrototypeOf(new StaticConstants()).BOOL === true",
      "boolean", "true"
  );
  e.expectEval(
      "Object.getPrototypeOf(new StaticConstants()).STRING === 'a static constant string'",
      "boolean", "true"
  );
}
KJ_TEST("Static methods are exposed as constructor properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval("StaticMethods.passThrough(true)", "boolean", "true");
  e.expectEval("StaticMethods.passThroughWithInfo(true)", "boolean", "true");
  e.expectEval("StaticMethods.voidCall(); true;", "boolean", "true");
  e.expectEval("StaticMethods.voidCallWithInfo(); true;", "boolean", "true");
  e.expectEval("StaticMethods.delete(); true;", "boolean", "true");
  e.expectEval("StaticMethods.unimplementedStaticMethod()",
      "throws", "Error: Failed to execute 'unimplementedStaticMethod' on 'StaticMethods': "
                "the method is not implemented.");
  e.expectEval("new StaticMethods.passThrough(true);",
      "throws", "TypeError: StaticMethods.passThrough is not a constructor");
}
KJ_TEST("Static methods are not exposed as constructor prototype properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "typeof StaticMethods.prototype.passThrough === 'undefined'\n"
      "&& typeof StaticMethods.prototype.passThroughWithInfo === 'undefined'\n"
      "&& typeof StaticMethods.prototype.voidCall === 'undefined'\n"
      "&& typeof StaticMethods.prototype.voidCallWithInfo === 'undefined'\n"
      "&& typeof StaticMethods.prototype.delete === 'undefined'\n"
      "&& typeof StaticMethods.prototype.unimplementedStaticMethod === 'undefined'",
      "boolean", "true"
  );
}
KJ_TEST("Static methods are not exposed as object instance properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "let obj = new StaticMethods();\n"
      "typeof obj.passThrough === 'undefined'\n"
      "&& typeof obj.passThroughWithInfo === 'undefined'\n"
      "&& typeof obj.voidCall === 'undefined'\n"
      "&& typeof obj.voidCallWithInfo === 'undefined'\n"
      "&& typeof obj.delete === 'undefined'\n"
      "&& typeof obj.unimplementedStaticMethod === 'undefined'",
      "boolean", "true"
  );
}
KJ_TEST("Static methods are not exposed as object instance prototype properties") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "let objProto = Object.getPrototypeOf(new StaticMethods());\n"
      "typeof objProto.passThrough === 'undefined'\n"
      "&& typeof objProto.passThroughWithInfo === 'undefined'\n"
      "&& typeof objProto.voidCall === 'undefined'\n"
      "&& typeof objProto.voidCallWithInfo === 'undefined'\n"
      "&& typeof objProto.delete === 'undefined'\n"
      "&& typeof objProto.unimplementedStaticMethod === 'undefined'",
      "boolean", "true"
  );
}
KJ_TEST("Static methods can be monkey-patched") {
  Evaluator<StaticContext, StaticIsolate> e(v8System);
  e.expectEval(
      "StaticMethods.passThrough = function(a) { return false; };\n"
      "StaticMethods.passThrough(true)",
      "boolean", "false"
  );
}

// ========================================================================================

struct ReflectionContext: public Object {
  struct Super: public Object {
    JSG_RESOURCE_TYPE(Super) {}
  };

  struct Reflector: public Super {
    static jsg::Ref<Reflector> constructor(v8::Isolate* isolate) {
      auto result = jsg::alloc<Reflector>();

      // Check reflection returns null when wrapper isn't allocated.
      KJ_EXPECT(result->intReflector.get(isolate, "foo") == nullptr);
      KJ_EXPECT(result->stringReflector.get(isolate, "foo") == nullptr);

      return result;
    }

    kj::Maybe<int> getIntReflection(kj::String name, v8::Isolate* isolate) {
      return intReflector.get(isolate, name);
    }
    kj::Maybe<kj::String> getStringReflection(kj::String name, v8::Isolate* isolate) {
      return stringReflector.get(isolate, name);
    }

    PropertyReflection<int> intReflector;
    PropertyReflection<kj::String> stringReflector;

    JSG_RESOURCE_TYPE(Reflector) {
      JSG_INHERIT(Super);
      JSG_METHOD(getIntReflection);
      JSG_METHOD(getStringReflection);
    }
    JSG_REFLECTION(intReflector, stringReflector);
  };

  jsg::Ref<Reflector> makeReflector() {
    return jsg::alloc<Reflector>();
  }

  jsg::Ref<Super> makeSuper() {
    return jsg::alloc<Reflector>();
  }

  JSG_RESOURCE_TYPE(ReflectionContext) {
    JSG_NESTED_TYPE(Reflector);
    JSG_METHOD(makeReflector);
    JSG_METHOD(makeSuper);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ReflectionIsolate, ReflectionContext,
                          ReflectionContext::Super, ReflectionContext::Reflector);

KJ_TEST("PropertyReflection works") {
  Evaluator<ReflectionContext, ReflectionIsolate> e(v8System);
  e.expectEval("let r = new Reflector; r.getIntReflection('foo')", "object", "null");
  e.expectEval("let r = new Reflector; r.foo = 123; r.getIntReflection('foo')", "number", "123");
  e.expectEval("let r = new Reflector; r.foo = 123; r.getStringReflection('foo')", "string", "123");

  e.expectEval("let r = makeReflector(); r.foo = 123; r.getIntReflection('foo')", "number", "123");
  e.expectEval("let r = makeSuper(); r.foo = 123; r.getIntReflection('foo')", "number", "123");
}

// ========================================================================================

struct InjectLockContext: public Object {
  struct Thingy: public Object {
    Thingy(int val, v8::Isolate* v8Isolate): val(val), v8Isolate(v8Isolate) {}
    int val;
    v8::Isolate* v8Isolate;

    static Ref<Thingy> constructor(Lock& js, int val, v8::Isolate* v8Isolate) {
      KJ_ASSERT(js.v8Isolate == v8Isolate);
      return alloc<Thingy>(val, v8Isolate);
    }

    int frob(Lock& js, int val2) {
      KJ_ASSERT(js.v8Isolate == v8Isolate);
      return val + val2;
    }

    int getVal(Lock& js) {
      KJ_ASSERT(js.v8Isolate == v8Isolate);
      return val;
    }

    void setVal(Lock& js, int val) {
      KJ_ASSERT(js.v8Isolate == v8Isolate);
      this->val = val;
    }

    static int borf(Lock& js, int val, v8::Isolate* v8Isolate) {
      KJ_ASSERT(js.v8Isolate == v8Isolate);
      return val * 2;
    }

    JSG_RESOURCE_TYPE(Thingy) {
      JSG_METHOD(frob);
      JSG_PROTOTYPE_PROPERTY(val, getVal, setVal);
      JSG_STATIC_METHOD(borf);
    }
  };


  JSG_RESOURCE_TYPE(InjectLockContext) {
    JSG_NESTED_TYPE(Thingy);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InjectLockIsolate, InjectLockContext, InjectLockContext::Thingy);

KJ_TEST("Methods can take Lock& as first parameter") {
  Evaluator<InjectLockContext, InjectLockIsolate> e(v8System);
  e.expectEval("let t = new Thingy(123); t.val", "number", "123");
}

struct JsBootstrap: public Object {
  JSG_RESOURCE_TYPE(JsBootstrap) {
    JSG_JS_BOOTSTRAP_CONTEXT(BOOTSTRAP_BUNDLE, "bootstrap:main");
  }
};
JSG_DECLARE_ISOLATE_TYPE(JsBootstrapIsolate, JsBootstrap);

KJ_TEST("js bootstrap is executed during context creation") {
  Evaluator<JsBootstrap, JsBootstrapIsolate> e(v8System);
  e.expectEval("new BootstrapClass().foo()", "string", "foo");
}

}  // namespace
}  // namespace workerd::jsg::test
