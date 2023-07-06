// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Common JSG testing infrastructure

#include "jsg.h"
#include "setup.h"
#include <kj/test.h>

namespace workerd::jsg::test {

template <typename ContextType, typename IsolateType,
          typename ConfigurationType = decltype(nullptr)>
class Evaluator {
  // Checks the evaluation of a blob of JS code under the given context and isolate types.
  //
  // TODO(cleanup): `ConfigurationType` currently can optionally be specified to fix the build
  //   in cases that the isolate includes types that require configuration, but currently the
  //   type is always default-constructed. What if you want to specify a test config?
public:
  explicit Evaluator(V8System& v8System) : v8System(v8System) {}

  IsolateType& getIsolate() {
    // Slightly more efficient to only instantiate each isolate type once (17s vs. 20s):
    static IsolateType isolate(v8System, ConfigurationType(), kj::heap<IsolateObserver>());
    return isolate;
  }

  void expectEval(kj::StringPtr code, kj::StringPtr expectedType, kj::StringPtr expectedValue) {
    V8StackScope stackScope;
    typename IsolateType::Lock lock(getIsolate(), stackScope);

    // Create a stack-allocated handle scope.
    v8::HandleScope handleScope(lock.v8Isolate);

    // Create a new context.
    v8::Local<v8::Context> context =
        lock.template newContext<ContextType>().getHandle(lock.v8Isolate);

    // Enter the context for compiling and running the hello world script.
    v8::Context::Scope contextScope(context);

    // Create a string containing the JavaScript source code.
    v8::Local<v8::String> source = jsg::v8Str(lock.v8Isolate, code);

    // Compile the source code.
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context, source).ToLocal(&script)) {
      KJ_FAIL_EXPECT("code didn't parse", code);
      return;
    }

    v8::TryCatch catcher(lock.v8Isolate);

    // Run the script to get the result.
    v8::Local<v8::Value> result;
    if (script->Run(context).ToLocal(&result)) {
      v8::String::Utf8Value type(lock.v8Isolate, result->TypeOf(lock.v8Isolate));
      v8::String::Utf8Value value(lock.v8Isolate, result);

      KJ_EXPECT(*type == expectedType, *type, expectedType);
      KJ_EXPECT(*value == expectedValue, *value, expectedValue);
    } else if (catcher.HasCaught()) {
      v8::String::Utf8Value message(lock.v8Isolate, catcher.Exception());

      KJ_EXPECT(expectedType == "throws", expectedType, catcher.Exception());
      KJ_EXPECT(*message == expectedValue, *message, expectedValue);
    } else {
      KJ_FAIL_EXPECT("returned empty handle but didn't throw exception?");
    }
  }

  void setAllowEval(bool b) {
    V8StackScope stackScope;
    typename IsolateType::Lock lock(getIsolate(), stackScope);
    lock.setAllowEval(b);
  }

  void setCaptureThrowsAsRejections(bool b) {
    V8StackScope stackScope;
    typename IsolateType::Lock lock(getIsolate(), stackScope);
    lock.setCaptureThrowsAsRejections(b);
  }

  void runMicrotasks() {
    V8StackScope stackScope;
    typename IsolateType::Lock lock(getIsolate(), stackScope);
    lock.v8Isolate->PerformMicrotaskCheckpoint();
  }

  void runMicrotasks(typename IsolateType::Lock& lock) {
    lock.v8Isolate->PerformMicrotaskCheckpoint();
  }

private:
  V8System& v8System;
};

struct NumberBox: public Object {
  double value;

  explicit NumberBox(double value): value(value) {}
  NumberBox() = default;

  static Ref<NumberBox> constructor(double value) {
    return jsg::alloc<NumberBox>(value);
  }

  void increment() { value += 1; }
  void incrementBy(double amount) { value += amount; }
  void incrementByBox(NumberBox& amount) { value += amount.value; }

  double add(double other) { return value + other; }
  double addBox(NumberBox& other) { return value + other.value; }
  Ref<NumberBox> addReturnBox(double other) { return jsg::alloc<NumberBox>(value + other); }
  double addMultiple(NumberBox& a, double b, NumberBox& c) {
    return value + a.value + b + c.value;
  }

  double getValue() { return value; }
  void setValue(double newValue) { value = newValue; }

  Ref<NumberBox> getBoxed() { return jsg::alloc<NumberBox>(value); }
  void setBoxed(NumberBox& newValue) { value = newValue.value; }

  v8::Local<v8::Value> getBoxedFromTypeHandler(
      jsg::Lock& js, v8::Isolate*, const TypeHandler<Ref<NumberBox>>& numberBoxTypeHandler) {
    // This function takes an Isolate just to prove it can take multiple value-less parameters.
    return numberBoxTypeHandler.wrap(js, alloc<NumberBox>(value));
  }

  JSG_RESOURCE_TYPE(NumberBox) {

    JSG_METHOD(increment);
    JSG_METHOD(incrementBy);
    JSG_METHOD(incrementByBox);

    JSG_METHOD(add);
    JSG_METHOD(addBox);
    JSG_METHOD(addReturnBox);
    JSG_METHOD(addMultiple);

    JSG_METHOD(getValue);
    JSG_METHOD(setValue);

    JSG_INSTANCE_PROPERTY(value, getValue, setValue);
    JSG_INSTANCE_PROPERTY(boxed, getBoxed, setBoxed);
    JSG_READONLY_INSTANCE_PROPERTY(boxedFromTypeHandler, getBoxedFromTypeHandler);
  }
};

class BoxBox: public Object {
public:
  explicit BoxBox(Ref<NumberBox> inner): inner(kj::mv(inner)) {}

  Ref<NumberBox> inner;

  static Ref<BoxBox> constructor(NumberBox& inner, double add) {
    return jsg::alloc<BoxBox>(jsg::alloc<NumberBox>(inner.value + add));
  }

  Ref<NumberBox> getInner() { return inner.addRef(); }

  JSG_RESOURCE_TYPE(BoxBox) {
    JSG_READONLY_INSTANCE_PROPERTY(inner, getInner);
  }

private:
  void visitForGc(GcVisitor& visitor) {
    visitor.visit(inner);
  }
};

struct ExtendedNumberBox: public NumberBox {
  static Ref<ExtendedNumberBox> constructor(double value, kj::String text) {
    auto result = jsg::alloc<ExtendedNumberBox>();
    result->value = value;
    result->text = kj::mv(text);
    return result;
  }

  kj::StringPtr getText() { return text; }
  void setText(kj::String newText) { text = kj::mv(newText); }

  kj::String text;

  JSG_RESOURCE_TYPE(ExtendedNumberBox) {
    JSG_INHERIT(NumberBox);

    JSG_METHOD(getText);
    JSG_METHOD(setText);
    JSG_INSTANCE_PROPERTY(text, getText, setText);
  }
};

struct TestStruct {
  kj::String str;
  double num;
  Ref<NumberBox> box;

  JSG_STRUCT(str, num, box);

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(box);
  }
};

}  // namespace workerd::jsg::test
