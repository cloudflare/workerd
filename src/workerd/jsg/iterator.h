// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/memory.h>
#include <workerd/jsg/struct.h>
#include <workerd/util/weak-refs.h>

#include <list>

namespace workerd::jsg {

// -----------------------------------------------------------------------------
// Generators

template <typename TypeWrapper>
class GeneratorWrapper;

template <typename T>
struct GeneratorNext {
  bool done;

  // Value should only be nullptr if done is true. It does not
  // *have* to be nullptr if done is true, however.
  kj::Maybe<T> value;
};

template <typename Signature, typename TypeWrapper>
static kj::Maybe<Signature> tryGetGeneratorFunction(
    Lock& js, JsObject& object, kj::StringPtr name) {
  auto value = object.get(js, name);
  return TypeWrapper::from(js.v8Isolate)
      .tryUnwrap(js, js.v8Context(), value, static_cast<Signature*>(nullptr),
          kj::Maybe<v8::Local<v8::Object>>(object));
}

template <typename T>
class Generator final {
  // See the documentation in jsg.h
 public:
  template <typename TypeWrapper>
  Generator(Lock& js, JsObject object, TypeWrapper*)
      : maybeActive(Active(js, object, static_cast<TypeWrapper*>(nullptr))) {}
  Generator(Generator&&) = default;
  Generator& operator=(Generator&&) = default;
  KJ_DISALLOW_COPY(Generator);

  // If nothing is returned, the generator is complete.
  kj::Maybe<T> next(Lock& js) {
    KJ_IF_SOME(active, maybeActive) {
      KJ_IF_SOME(nextfn, active.maybeNext) {
        return js.tryCatch([&] {
          auto result = nextfn(js);
          if (result.done || result.value == kj::none) {
            maybeActive = kj::none;
          }
          return kj::mv(result.value);
        }, [&](Value exception) { return throw_(js, kj::mv(exception)); });
      }
      maybeActive = kj::none;
    }
    return kj::none;
  }

  // If nothing is returned, the generator is complete.
  kj::Maybe<T> return_(Lock& js, kj::Maybe<T> maybeValue = kj::none) {
    KJ_IF_SOME(active, maybeActive) {
      KJ_IF_SOME(returnFn, active.maybeReturn) {
        return js.tryCatch([&] {
          auto result = returnFn(js, kj::mv(maybeValue));
          if (result.done || result.value == kj::none) {
            maybeActive = kj::none;
          }
          return kj::mv(result.value);
        }, [&](Value exception) { return throw_(js, kj::mv(exception)); });
      }
      maybeActive = kj::none;
    }
    return kj::none;
  }

  // If nothing is returned, the generator is complete. If there
  // is no throw handler in the generator, the method will throw.
  // It's also possible (and even likely) that the throw handler
  // will just re-throw the exception.
  kj::Maybe<T> throw_(Lock& js, Value exception) {
    KJ_IF_SOME(active, maybeActive) {
      KJ_IF_SOME(throwFn, active.maybeThrow) {
        return js.tryCatch([&] -> kj::Maybe<T> {
          auto result = throwFn(js, kj::mv(exception));
          if (result.done || result.value == kj::none) {
            maybeActive = kj::none;
          }
          return kj::mv(result.value);
        }, [&](Value exception) -> kj::Maybe<T> {
          maybeActive = kj::none;
          js.throwException(kj::mv(exception));
        });
      }
    }
    js.throwException(kj::mv(exception));
  }

  void visitForGc(GcVisitor& visitor) {
    visitForGc(maybeActive);
  }

 private:
  using Next = GeneratorNext<T>;
  using NextSignature = Function<Next()>;
  using ReturnSignature = Function<Next(Optional<T>)>;
  using ThrowSignature = Function<Next(Optional<Value>)>;

  struct Active final {
    kj::Maybe<NextSignature> maybeNext;
    kj::Maybe<ReturnSignature> maybeReturn;
    kj::Maybe<ThrowSignature> maybeThrow;

    template <typename TypeWrapper>
    Active(Lock& js, JsObject object, TypeWrapper*)
        : maybeNext(tryGetGeneratorFunction<NextSignature, TypeWrapper>(js, object, "next"_kj)),
          maybeReturn(
              tryGetGeneratorFunction<ReturnSignature, TypeWrapper>(js, object, "return"_kj)),
          maybeThrow(tryGetGeneratorFunction<ThrowSignature, TypeWrapper>(js, object, "throw"_kj)) {
    }
    Active(Active&&) = default;
    Active& operator=(Active&&) = default;
    KJ_DISALLOW_COPY(Active);

    void visitForGc(GcVisitor& visitor) {
      visitor.visit(maybeNext, maybeReturn, maybeThrow);
    }
  };
  kj::Maybe<Active> maybeActive;
};

template <typename T>
class AsyncGenerator final {
  // See the documentation in jsg.h
 public:
  template <typename TypeWrapper>
  AsyncGenerator(Lock& js, JsObject object, TypeWrapper*)
      : maybeActive(Active(js, object, static_cast<TypeWrapper*>(nullptr))),
        maybeSelfRef(kj::rc<WeakRef<AsyncGenerator>>(kj::Badge<AsyncGenerator>{}, *this)) {}
  AsyncGenerator(AsyncGenerator&& other)
      : maybeActive(kj::mv(other.maybeActive)),
        maybeSelfRef(kj::rc<WeakRef<AsyncGenerator>>(kj::Badge<AsyncGenerator>{}, *this)) {
    // Invalidate the old WeakRef since it's being moved.
    KJ_IF_SOME(selfRef, other.maybeSelfRef) {
      selfRef->invalidate();
    }
  }
  AsyncGenerator& operator=(AsyncGenerator&& other) {
    if (this != &other) {
      KJ_IF_SOME(selfRef, maybeSelfRef) {
        selfRef->invalidate();
      }
      KJ_IF_SOME(selfRef, other.maybeSelfRef) {
        selfRef->invalidate();
      }
      maybeActive = kj::mv(other.maybeActive);
      maybeSelfRef = kj::rc<WeakRef<AsyncGenerator>>(kj::Badge<AsyncGenerator>{}, *this);
    }
    return *this;
  }
  KJ_DISALLOW_COPY(AsyncGenerator);
  ~AsyncGenerator() noexcept(false) {
    KJ_IF_SOME(selfRef, maybeSelfRef) {
      selfRef->invalidate();
    }
  }

  // If nothing is returned, the generator is complete.
  Promise<kj::Maybe<T>> next(Lock& js) {
    KJ_IF_SOME(active, maybeActive) {
      KJ_IF_SOME(next, active.maybeNext) {
        auto& selfRef = KJ_ASSERT_NONNULL(maybeSelfRef);
        return js.tryCatch([&] {
          return next(js).then(js, [ref = selfRef.addRef()](Lock& js, auto result) {
            if (result.done || result.value == kj::none) {
              ref->runIfAlive([&](AsyncGenerator& self) { self.maybeActive = kj::none; });
            }
            return js.resolvedPromise<kj::Maybe<T>>(kj::mv(result.value));
          }, [ref = selfRef.addRef()](Lock& js, Value exception) {
            Promise<kj::Maybe<T>> retPromise = nullptr;
            if (ref->runIfAlive([&](AsyncGenerator& self) {
              retPromise = self.throw_(js, kj::mv(exception));
            })) {
              return kj::mv(retPromise);
            }
            return js.rejectedPromise<kj::Maybe<T>>(kj::mv(exception));
          });
        }, [&](Value exception) {
          maybeActive = kj::none;
          return throw_(js, kj::mv(exception));
        });
      }
      maybeActive = kj::none;
    }

    return js.resolvedPromise(kj::Maybe<T>(kj::none));
  }

  // If nothing is returned, the generator is complete.
  // Per GetMethod spec (https://262.ecma-international.org/#sec-getmethod), if the 'return'
  // property exists but is not callable, we throw a TypeError.
  Promise<kj::Maybe<T>> return_(Lock& js, kj::Maybe<T> maybeValue = kj::none) {
    KJ_IF_SOME(active, maybeActive) {
      // Per GetMethod spec: if property exists but is not callable, throw TypeError
      if (active.returnExistsButNotCallable) {
        maybeActive = kj::none;
        return js.rejectedPromise<kj::Maybe<T>>(
            js.typeError("property 'return' is not a function"_kj));
      }

      KJ_IF_SOME(return_, active.maybeReturn) {
        auto& selfRef = KJ_ASSERT_NONNULL(maybeSelfRef);
        return js.tryCatch([&] {
          return return_(js, kj::mv(maybeValue))
              .then(js, [ref = selfRef.addRef()](Lock& js, auto result) {
            if (result.done || result.value == kj::none) {
              ref->runIfAlive([&](AsyncGenerator& self) { self.maybeActive = kj::none; });
            }
            return js.resolvedPromise(kj::mv(result.value));
          }, [ref = selfRef.addRef()](Lock& js, Value exception) {
            // Per spec, rejections from return() should be propagated directly
            ref->runIfAlive([&](AsyncGenerator& self) { self.maybeActive = kj::none; });
            return js.rejectedPromise<kj::Maybe<T>>(kj::mv(exception));
          });
        }, [&](Value exception) {
          maybeActive = kj::none;
          return js.rejectedPromise<kj::Maybe<T>>(kj::mv(exception));
        });
      }
      maybeActive = kj::none;
    }
    return js.resolvedPromise(kj::Maybe<T>(kj::none));
  }

  // If nothing is returned, the generator is complete. If there
  // is no throw handler in the generator, the method will throw.
  // It's also possible (and even likely) that the throw handler
  // will just re-throw the exception.
  Promise<kj::Maybe<T>> throw_(Lock& js, Value exception) {
    KJ_IF_SOME(active, maybeActive) {
      KJ_IF_SOME(throw_, active.maybeThrow) {
        auto& selfRef = KJ_ASSERT_NONNULL(maybeSelfRef);
        return js.tryCatch([&] {
          return throw_(js, kj::mv(exception))
              .then(js, [ref = selfRef.addRef()](Lock& js, auto result) {
            if (result.done || result.value == kj::none) {
              ref->runIfAlive([&](AsyncGenerator& self) { self.maybeActive = kj::none; });
            }
            // In this case, the exception was handled and we might have a value to return.
            // The generator might still be active.
            return js.resolvedPromise(kj::mv(result.value));
          }, [ref = selfRef.addRef()](Lock& js, Value exception) {
            ref->runIfAlive([&](AsyncGenerator& self) { self.maybeActive = kj::none; });
            return js.rejectedPromise<kj::Maybe<T>>(kj::mv(exception));
          });
        }, [&](Value exception) {
          maybeActive = kj::none;
          return js.rejectedPromise<kj::Maybe<T>>(kj::mv(exception));
        });
      }
      maybeActive = kj::none;
    }
    return js.rejectedPromise<kj::Maybe<T>>(kj::mv(exception));
  }

 private:
  using Next = GeneratorNext<T>;
  using NextSignature = Function<Promise<Next>()>;
  using ReturnSignature = Function<Promise<Next>(Optional<T>)>;
  using ThrowSignature = Function<Promise<Next>(Optional<Value>)>;

  struct Active final {
    kj::Maybe<NextSignature> maybeNext;
    kj::Maybe<ReturnSignature> maybeReturn;
    kj::Maybe<ThrowSignature> maybeThrow;
    // Per GetMethod spec, if property exists but is not callable, we should throw TypeError.
    // We track this state to defer the error to when return_() is actually called.
    bool returnExistsButNotCallable = false;

    template <typename TypeWrapper>
    Active(Lock& js, JsObject object, TypeWrapper*)
        : maybeNext(tryGetGeneratorFunction<NextSignature, TypeWrapper>(js, object, "next"_kj)),
          maybeReturn(
              tryGetGeneratorFunction<ReturnSignature, TypeWrapper>(js, object, "return"_kj)),
          maybeThrow(tryGetGeneratorFunction<ThrowSignature, TypeWrapper>(js, object, "throw"_kj)) {
      // Check if return property exists but isn't callable (per GetMethod spec)
      returnExistsButNotCallable =
          maybeReturn == kj::none && !object.get(js, "return"_kj).isNullOrUndefined();
    }
    Active(Active&&) = default;
    Active& operator=(Active&&) = default;
    KJ_DISALLOW_COPY(Active);

    void visitForGc(GcVisitor& visitor) {
      visitor.visit(maybeNext, maybeReturn, maybeThrow);
    }
  };
  kj::Maybe<Active> maybeActive;
  kj::Maybe<kj::Rc<WeakRef<AsyncGenerator>>> maybeSelfRef;
};

template <typename T>
class AsyncGeneratorIgnoringStrings final {
 public:
  template <typename TypeWrapper>
  AsyncGeneratorIgnoringStrings(Lock& js, JsObject object, TypeWrapper* ptr)
      : inner(AsyncGenerator<T>(js, object, ptr)) {}

  AsyncGenerator<T> release() {
    return kj::mv(inner);
  }

 private:
  AsyncGenerator<T> inner;
};

template <typename TypeWrapper>
class GeneratorWrapper {
 public:
  GeneratorWrapper(const auto& config): config(getConfig(config)) {}

  template <typename T>
  static constexpr const char* getName(Generator<T>*) {
    return "Generator";
  }

  template <typename T>
  static constexpr const char* getName(AsyncGenerator<T>*) {
    return "AsyncGenerator";
  }

  template <typename T>
  static constexpr const char* getName(AsyncGeneratorIgnoringStrings<T>*) {
    return "AsyncGenerator";
  }

  template <typename T>
  static constexpr const char* getName(GeneratorNext<T>*) {
    return "GeneratorNext";
  }

  template <typename T>
  v8::Local<v8::Object> wrap(
      Lock& js, v8::Local<v8::Context>, kj::Maybe<v8::Local<v8::Object>>, Generator<T>&&) {
    KJ_FAIL_ASSERT("Generator instances do not support wrap");
  }

  template <typename T>
  v8::Local<v8::Object> wrap(
      Lock& js, v8::Local<v8::Context>, kj::Maybe<v8::Local<v8::Object>>, AsyncGenerator<T>&&) {
    KJ_FAIL_ASSERT("AsyncGenerator instances do not support wrap");
  }

  template <typename T>
  v8::Local<v8::Object> wrap(Lock& js,
      v8::Local<v8::Context>,
      kj::Maybe<v8::Local<v8::Object>>,
      AsyncGeneratorIgnoringStrings<T>&&) {
    KJ_FAIL_ASSERT("AsyncGenerator instances do not support wrap");
  }

  template <typename T>
  v8::Local<v8::Object> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>>,
      GeneratorNext<T>&& next) {
    KJ_FAIL_ASSERT("GeneratorNext instances do not support wrap");
  }
  // Generator, AsyncGenerator, and GeneratorNext instances should never be
  // passed back out into JavaScript. Use Iterators for that.

  template <typename T>
  kj::Maybe<GeneratorNext<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      GeneratorNext<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsObject()) {
      auto isolate = js.v8Isolate;
      auto& typeWrapper = TypeWrapper::from(isolate);
      auto object = handle.template As<v8::Object>();

      bool done = typeWrapper.template unwrap<bool>(js, context,
          check(object->Get(context, v8StrIntern(isolate, "done"_kj))), TypeErrorContext::other());

      auto value = check(object->Get(context, v8StrIntern(isolate, "value"_kj)));

      if (done) {
        // If done is true, then it is OK if the value does not map to anything.
        // Why are we doing it this way? Currently in the Generator pattern, there
        // is no way of distinguishing between the generator not having any return
        // value or the generator having undefined as a return value. Because we
        // cannot differentiate the two, we treat undefined specially and always
        // return nullptr in this case rather than trying to map it to anything --
        // even if the thing we'd be mapping to can safely handle undefined as
        // a value.
        if (value->IsUndefined()) {
          return GeneratorNext<T>{
            .done = true,
            .value = kj::none,
          };
        } else {
          return GeneratorNext<T>{
            .done = true,
            .value =
                typeWrapper.tryUnwrap(js, context, value, static_cast<T*>(nullptr), parentObject),
          };
        }
      }

      KJ_IF_SOME(v, typeWrapper.tryUnwrap(js, context, value, (T*)nullptr, parentObject)) {
        return GeneratorNext<T>{
          .done = false,
          .value = kj::mv(v),
        };
      } else {
        throwTypeError(js.v8Isolate, TypeErrorContext::other(),
            TypeWrapper::getName(static_cast<T*>(nullptr)));
      }
    }

    return kj::none;
  }

  template <typename T>
  kj::Maybe<Generator<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Generator<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsString()) {
      // In order to be able to treat a string as a generator, we need to first
      // convert it to a String object. Yes, this means that each call to next
      // will yield a single character from the string, which is terrible but
      // that's the spec.
      handle = check(handle->ToObject(context));
    }
    if (handle->IsObject()) {
      auto isolate = js.v8Isolate;
      auto object = handle.As<v8::Object>();
      auto iter = check(object->Get(context, v8::Symbol::GetIterator(isolate)));
      if (iter->IsFunction()) {
        auto func = iter.As<v8::Function>();
        auto iterObj = check(func->Call(context, object, 0, nullptr));
        if (iterObj->IsObject()) {
          return Generator<T>(
              js, JsObject(iterObj.As<v8::Object>()), static_cast<TypeWrapper*>(nullptr));
        }
      }
    }
    return kj::none;
  }

  template <typename T>
  kj::Maybe<AsyncGenerator<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      AsyncGenerator<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsString()) {
      // In order to be able to treat a string as a generator, we need to first
      // convert it to a String object. Yes, this means that each call to next
      // will yield a single character from the string, which is terrible but
      // that's the spec.
      handle = check(handle->ToObject(context));
    }
    if (handle->IsObject()) {
      auto isolate = js.v8Isolate;
      auto object = handle.As<v8::Object>();
      auto iter = check(object->Get(context, v8::Symbol::GetAsyncIterator(isolate)));
      // If there is no async iterator, let's try a sync iterator.
      if (iter->IsNullOrUndefined()) {
        iter = check(object->Get(context, v8::Symbol::GetIterator(isolate)));
      }
      if (iter->IsFunction()) {
        auto func = iter.As<v8::Function>();
        auto iterObj = check(func->Call(context, object, 0, nullptr));
        if (iterObj->IsObject()) {
          return AsyncGenerator<T>(
              js, JsObject(iterObj.As<v8::Object>()), static_cast<TypeWrapper*>(nullptr));
        }
      }
    }
    return kj::none;
  }

  template <typename T>
  kj::Maybe<AsyncGeneratorIgnoringStrings<T>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      AsyncGeneratorIgnoringStrings<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // This variation of the wrapper is used in cases where Strings should not be treated
    // as iterators. Specifically, for cases like `kj::OneOf<kj::String,AsyncGenerator<T>>`
    // where we want to allow strings to be passed through as strings but also want to allow
    // sync and async generators to be handled as well. Without this, the strings would be
    // treated as sync iterables.
    if (config.fetchIterableTypeSupport && handle->IsObject() && !handle->IsStringObject()) {
      auto isolate = js.v8Isolate;
      auto object = handle.As<v8::Object>();

      auto iter = check(object->Get(context, v8::Symbol::GetAsyncIterator(isolate)));
      // If there is no async iterator, let's try a sync iterator.
      if (iter->IsNullOrUndefined()) {
        // Before checking for the sync iterator, let's also check to see if the object
        // implements a custom toString to Symbol.toPrimitive method that is not the default
        // Object.prototype.toString. If it does, then we won't treat it as
        // an iterator either. If the object is an Array, then we skip this check since
        // it's exceedingly uncommon for arrays to be subclassed with a custom toString method,
        // so much that it's not worth handling the extreme edge case.
        // This is to deal with edge cases around objects with customized stringify methods,
        // which are likely more common than those with customized iterator methods. While
        // these are both rare cases, it's better to err on the side of custom stringification
        // rather than custom iteration.
        if (config.fetchIterableTypeSupportOverrideAdjustment && !object->IsArray()) {
          if (protoToString == kj::none) {
            // TODO(cleanup): In several places in the codebase we have this pattern of
            // lazily grabbing the object prototype. We should probably centralize this
            // an cache it in the IsolateBase or something.
            auto obj = js.obj();
            auto proto = obj.getPrototype(js);
            protoToString = jsg::JsRef(
                js, KJ_ASSERT_NONNULL(proto.tryCast<jsg::JsObject>()).get(js, "toString"_kj));
            toPrimitiveString = jsg::JsRef(js,
                KJ_ASSERT_NONNULL(proto.tryCast<jsg::JsObject>()).get(js, js.symbolToPrimitive()));
          }

          // We only check that the toString/Symbol.toPrimitive is the same value as
          // Object.prototype.toString/Symbol.toPrimitive. This does not guarantee every
          // possible edge case but should be sufficient for our purposes.
          auto jsobj = JsObject(object);
          if (jsobj.get(js, "toString"_kj) != KJ_ASSERT_NONNULL(protoToString).getHandle(js) ||
              jsobj.get(js, js.symbolToPrimitive()) !=
                  KJ_ASSERT_NONNULL(toPrimitiveString).getHandle(js)) {
            return kj::none;
          }
        }

        iter = check(object->Get(context, v8::Symbol::GetIterator(isolate)));
      }
      if (iter->IsFunction()) {
        auto func = iter.As<v8::Function>();
        auto iterObj = check(func->Call(context, object, 0, nullptr));
        if (iterObj->IsObject()) {
          return AsyncGeneratorIgnoringStrings<T>(
              js, JsObject(iterObj.As<v8::Object>()), static_cast<TypeWrapper*>(nullptr));
        }
      }
    }
    return kj::none;
  }

 private:
  const JsgConfig config;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> protoToString;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> toPrimitiveString;
};

// -----------------------------------------------------------------------------
// Sequences

template <typename T>
struct Sequence: public kj::Array<T> {
  // See the documentation in jsg.h
  Sequence() = default;
  Sequence(kj::Array<T> items): kj::Array<T>(kj::mv(items)) {}
};

template <typename TypeWrapper>
class SequenceWrapper {
  // TypeWrapper mixin for Sequences.

 public:
  template <typename U>
  static constexpr const char* getName(Sequence<U>*) {
    // TODO(later): It would be nicer if the name included the demangled name of U
    // e.g. Sequence<Foo>
    return "Sequence";
  }

  template <typename U>
  v8::Local<v8::Value> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Sequence<U> sequence) {
    v8::Isolate* isolate = js.v8Isolate;
    v8::EscapableHandleScope handleScope(isolate);
    v8::LocalVector<v8::Value> items(isolate, sequence.size());
    for (auto i: kj::indices(sequence)) {
      items[i] = static_cast<TypeWrapper*>(this)->wrap(js, context, creator, kj::mv(sequence[i]));
    }
    return handleScope.Escape(v8::Array::New(isolate, items.data(), items.size()));
  }

  template <typename U>
  v8::Local<v8::Value> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      Sequence<U>& sequence) {
    v8::Isolate* isolate = js.v8Isolate;
    v8::EscapableHandleScope handleScope(isolate);
    v8::LocalVector<v8::Value> items(isolate, sequence.size());
    for (auto i: kj::indices(sequence)) {
      items[i] = static_cast<TypeWrapper*>(this)->wrap(js, context, creator, kj::mv(sequence[i]));
    }
    return handleScope.Escape(v8::Array::New(isolate, items.data(), items.size()));
  }

  template <typename U>
  kj::Maybe<Sequence<U>> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Sequence<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto isolate = js.v8Isolate;
    auto& typeWrapper = TypeWrapper::from(isolate);
    // In this case, if handle is a string, we likely do not want to treat it as
    // a sequence of characters, which the Generator case would do. If someone
    // really wants to treat a string as a sequence of characters, then they
    // should use the Generator interface directly.
    if (handle->IsString()) return kj::none;
    KJ_IF_SOME(gen,
        typeWrapper.tryUnwrap(js, context, handle, (Generator<U>*)nullptr, parentObject)) {
      // The generator gives us no indication of how many items there might be, so we
      // have to just keep pulling them until it says it's done.
      kj::Vector<U> items;
      while (true) {
        KJ_IF_SOME(item, gen.next(js)) {
          items.add(kj::mv(item));
        } else {
          gen.return_(js, kj::none);
          break;
        }
      }
      return Sequence<U>(items.releaseAsArray());
    }
    return kj::none;
  }
};

// -----------------------------------------------------------------------------

template <typename SelfType, typename Type, typename State>
class IteratorBase: public Object {
  // Provides the base implementation of JSG_ITERATOR types. See the documentation
  // for JSG_ITERATOR for details.
 public:
  using NextSignature = kj::Maybe<Type>(Lock&, State&);
  explicit IteratorBase(State state): state(kj::mv(state)) {}
  struct Next {
    bool done;
    Optional<Type> value;
    JSG_STRUCT(done, value);
  };

  v8::Local<v8::Object> self(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return info.This();
  }

  void visitForGc(GcVisitor& visitor) {
    if constexpr (hasPublicVisitForGc<State>()) {
      visitor.visit(state);
    }
  }

  JSG_MEMORY_INFO(IteratorBase) {
    if constexpr (MemoryRetainer<State>) {
      tracker.trackField("state", state);
    } else {
      tracker.trackFieldWithSize("state", sizeof(State));
    }
  }

 private:
  State state;

  Next nextImpl(Lock& js, NextSignature nextFunc) {
    KJ_IF_SOME(value, nextFunc(js, state)) {
      return Next{.done = false, .value = kj::mv(value)};
    }
    return Next{
      .done = true,
      .value = kj::none,
    };
  }

  friend SelfType;
};

class AsyncIteratorImpl {
 public:
  struct Finished {};

  kj::Maybe<Promise<void>&> maybeCurrent();

  void pushCurrent(Promise<void> promise);

  void popCurrent();

  bool returning = false;

  void visitForGc(GcVisitor& visitor);

  template <typename Type>
  struct Next {
    bool done;
    Optional<Type> value;
    JSG_STRUCT(done, value);
  };

  JSG_MEMORY_INFO(AsyncIteratorImpl) {
    // TODO(soon): Implement memory tracking
  }

 private:
  std::list<Promise<void>> pendingStack;
};

// Provides the base implementation of JSG_ASYNC_ITERATOR types. See the documentation
// for JSG_ASYNC_ITERATOR for details.
//
// Objects that use AsyncIteratorBase will be usable with the for await syntax in
// JavaScript, e.g.:
//
//   const obj = new MyNewAsyncIterableObject();
//   for await (const chunk of obj) {
//     console.log(chunk);
//   }
//
// The for await syntax is just sugar for using an async generator object.
// All async iterable objects will have a method that will return an instance
// of the AsyncIteratorBase. This is typically a method named values() or
// entries().
//
// const obj = new MyNewAsyncIterableObject();
// const gen = obj.values();
//
// The async generator object has two methods: next() and return()
// next() is called to fetch the next item from the iterator, and should
// be called until there is no more data to return. The return() method
// is called to signal early termination of the iterator. Both methods
// return a JavaScript promise that resolves to an IteratorResult object
// (an ordinary JavaScript object with a done and value property).
//
// const result = await gen.next();
// console.log(result.done);   // true or false
// console.log(result.value);  // the value yielded in this iteration.
//
// const result = await gen.return("foo");
// console.log(result.done);   // true
// console.log(result.value);  // "foo" ... whatever value was passed in.
//
// It is important for the generator to queue and properly sequence concurrent
// next() and return() calls. Specifically, the following pattern should read
// five elements off the iterator before terminating it early:
//
// await Promise.all([
//   gen.next(),         // must resolve to the first item
//   gen.next(),         // must resolve to the second item
//   gen.next(),         // must resolve to the third item
//   gen.next(),         // must resolve to the fourth item
//   gen.next(),         // must resolve to the fifth item
//   gen.return("boom"), // must not be processed until after the fifth next()
// ]);
//
// Once return() is called, all subsequent next() and return() calls must just
// return an immediately resolved promise indicating that the iterator is done.
template <typename SelfType, typename Type, typename State>
class AsyncIteratorBase: public Object {
 public:
  using NextSignature = Promise<kj::Maybe<Type>>(Lock&, State&);
  using ReturnSignature = Promise<void>(Lock&, State&, Optional<Type>&);
  using Next = AsyncIteratorImpl::Next<Type>;
  using Finished = AsyncIteratorImpl::Finished;

  explicit AsyncIteratorBase(State state): state(InnerState{.state = kj::mv(state)}) {}

  v8::Local<v8::Object> self(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return info.This();
  }

  void visitForGc(GcVisitor& visitor) {
    KJ_IF_SOME(inner, state.template tryGet<InnerState>()) {
      if constexpr (hasPublicVisitForGc<State>()) {
        visitor.visit(inner.state);
      }
      visitor.visit(inner.impl);
    }
  }

  JSG_MEMORY_INFO(AsyncIteratorBase) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(fin, Finished) {
        tracker.trackFieldWithSize("state", sizeof(Finished));
      }
      KJ_CASE_ONEOF(state, InnerState) {
        tracker.trackField("state", state);
      }
    }
  }

 private:
  struct InnerState {
    State state;
    AsyncIteratorImpl impl;

    JSG_MEMORY_INFO(InnerState) {
      if constexpr (MemoryRetainer<State>) {
        tracker.trackField("state", state);
      } else {
        tracker.trackFieldWithSize("state", sizeof(State));
      }
      tracker.trackField("impl", impl);
    }
  };

  kj::OneOf<Finished, InnerState> state;

  void pushCurrent(Lock& js, Promise<void> promise) {
    auto& inner = state.template get<InnerState>();
    inner.impl.pushCurrent(promise.whenResolved(js).then(js, [this, self = JSG_THIS](Lock& js) {
      // If state is Finished, then there's nothing we need to do here.
      KJ_IF_SOME(inner, state.template tryGet<InnerState>()) {
        inner.impl.popCurrent();
      }
      return js.resolvedPromise();
    }, [this, self = JSG_THIS](Lock& js, Value value) {
      KJ_IF_SOME(inner, state.template tryGet<InnerState>()) {
        inner.impl.popCurrent();
      }
      return js.rejectedPromise<void>(kj::mv(value));
    }));
  }

  Promise<Next> nextImpl(Lock& js, NextSignature nextFunc) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(finished, Finished) {
        return js.resolvedPromise(Next{.done = true});
      }
      KJ_CASE_ONEOF(inner, InnerState) {
        // If return_() has already been called on the async iterator, we just return an immediately
        // resolved promise indicating done, regardless of whether there are still other outstanding
        // next promises or not.
        if (inner.impl.returning) {
          return js.resolvedPromise(Next{.done = true});
        }

        auto callNext = [this, self = JSG_THIS, nextFunc = kj::mv(nextFunc)](Lock& js) mutable {
          KJ_SWITCH_ONEOF(state) {
            KJ_CASE_ONEOF(finished, Finished) {
              return js.resolvedPromise(Next{.done = true});
            }
            KJ_CASE_ONEOF(inner, InnerState) {
              auto promise = nextFunc(js, inner.state);
              pushCurrent(js, promise.whenResolved(js));
              return promise.then(
                  js, [this, self = kj::mv(self)](Lock& js, kj::Maybe<Type> maybeResult) mutable {
                KJ_IF_SOME(result, maybeResult) {
                  return js.resolvedPromise(Next{.done = false, .value = kj::mv(result)});
                } else {
                  state.template init<Finished>();
                  return js.resolvedPromise(Next{.done = true});
                }
              });
            }
          }
          KJ_UNREACHABLE;
        };

        KJ_IF_SOME(current, inner.impl.maybeCurrent()) {
          auto promise = current.whenResolved(js).then(js, kj::mv(callNext));
          pushCurrent(js, promise.whenResolved(js));
          return kj::mv(promise);
        }

        // Otherwise, call the next function and handle the result.
        return callNext(js);
      }
    }
    KJ_UNREACHABLE;
  }

  Promise<Next> returnImpl(Lock& js, Optional<Type> value, ReturnSignature returnFunc) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(finished, Finished) {
        return js.resolvedPromise(Next{.done = true, .value = kj::mv(value)});
      }
      KJ_CASE_ONEOF(inner, InnerState) {

        // When inner.returning is true, return_() has already been called on the iterator.
        // Any further calls to either next() or return_() will result in immediately resolved
        // promises indicating a done status being returned, regardless of any other promises
        // that may be pending.
        if (inner.impl.returning) {
          return js.resolvedPromise(Next{.done = true, .value = kj::mv(value)});
        }

        inner.impl.returning = true;

        auto callReturn = [this, self = JSG_THIS, value = kj::mv(value),
                              returnFunc = kj::mv(returnFunc)](Lock& js) mutable {
          KJ_SWITCH_ONEOF(state) {
            KJ_CASE_ONEOF(finished, Finished) {
              return js.resolvedPromise(Next{.done = true, .value = kj::mv(value)});
            }
            KJ_CASE_ONEOF(inner, InnerState) {
              return returnFunc(js, inner.state, value)
                  .then(js, [this, self = kj::mv(self), value = kj::mv(value)](Lock& js) mutable {
                state.template init<Finished>();
                return js.resolvedPromise(Next{.done = true, .value = kj::mv(value)});
              });
            }
          }
          KJ_UNREACHABLE;
        };

        // If there is something on the pending stack, we are going to wait for that promise
        // to resolve then call callReturn.
        KJ_IF_SOME(current, inner.impl.maybeCurrent()) {
          return current.whenResolved(js).then(js, kj::mv(callReturn));
        }

        // Otherwise, we call callReturn immediately.
        return callReturn(js);
      }
    }
    KJ_UNREACHABLE;
  }

  friend SelfType;
};

// The JSG_ITERATOR macro provides a mechanism for easily implementing JavaScript-style iterators
// for JSG_RESOURCE_TYPES.
//
// Example usage:
//
// class MyApiType: public jsg::Object {
// private:
//   struct IteratorState {
//     // The iterator's internal state.
//     // Implement visitForGc here if the state stores any visitable references.
//   };
//
//   static kj::Maybe<kj::String> nextFunction(jsg::Lock& js, IteratorState& state) {
//     // Return nullptr to indicate we've reached the end of the iterator.
//     // Otherwise, return the next iterator value.
//   }
// public:
//   JSG_ITERATOR(MyApiTypeIterator,
//                 entries,
//                 kj::String,
//                 IteratorState,
//                 nextFunction);
//
//   JSG_RESOURCE_TYPE(MyApiType) {
//     JSG_METHOD(entries);
//     JSG_ITERABLE(entries);
//   }
//
//   jsg::Ref<MyApiTypeIterator> entries(jsg::Lock& js) {
//     return js.alloc<MyApiTypeIterator>(IteratorState { /* any necessary state init */ });
//   }
// };
//
// In this example, instances of MyApiType will support the JavaScript synchronous iterator
// pattern (e.g. for (const item of myApiType) {}).
//
// The actual iterator instance is defined by the type MyApiType::MyApiTypeIterator, which
// will use the IteratorState struct to store internal state and the nextFunction to yield
// the next value for the iterator.
//
// A member function named entries(Lock&) will be added to MyApiType that returns a
// jsg::Ref<MyApiTypeIterator>() instance. It will be necessary for uses to provide the
// implementation of the entries(Lock&) member function.
#define JSG_ITERATOR(Name, Label, Type, State, NextFunc)                                           \
  class Name final: public jsg::IteratorBase<Name, Type, State> {                                  \
   public:                                                                                         \
    using jsg::IteratorBase<Name, Type, State>::IteratorBase;                                      \
    inline Next next(jsg::Lock& js) {                                                              \
      return nextImpl(js, NextFunc);                                                               \
    }                                                                                              \
    JSG_RESOURCE_TYPE(Name) {                                                                      \
      JSG_INHERIT_INTRINSIC(v8::kIteratorPrototype);                                               \
      JSG_METHOD(next);                                                                            \
      JSG_ITERABLE(self);                                                                          \
    }                                                                                              \
  };                                                                                               \
  jsg::Ref<Name> Label(jsg::Lock&);

// Like JSG_ITERATOR but don't declare the method name automatically.
//
// TODO(cleanup): Change all JSG_ITERATOR usages to this. It's confusing for the macro to declare
//   the method.
#define JSG_ITERATOR_TYPE(Name, Type, State, NextFunc)                                             \
  class Name final: public jsg::IteratorBase<Name, Type, State> {                                  \
   public:                                                                                         \
    using jsg::IteratorBase<Name, Type, State>::IteratorBase;                                      \
    inline Next next(jsg::Lock& js) {                                                              \
      return nextImpl(js, NextFunc);                                                               \
    }                                                                                              \
    JSG_RESOURCE_TYPE(Name) {                                                                      \
      JSG_INHERIT_INTRINSIC(v8::kIteratorPrototype);                                               \
      JSG_METHOD(next);                                                                            \
      JSG_ITERABLE(self);                                                                          \
    }                                                                                              \
  };

#define JSG_ASYNC_ITERATOR_TYPE(Name, Type, State, NextFunc, ReturnFunc)                           \
  class Name final: public jsg::AsyncIteratorBase<Name, Type, State> {                             \
   public:                                                                                         \
    using jsg::AsyncIteratorBase<Name, Type, State>::AsyncIteratorBase;                            \
    inline jsg::Promise<Next> next(jsg::Lock& js) {                                                \
      return nextImpl(js, NextFunc);                                                               \
    }                                                                                              \
    inline jsg::Promise<Next> return_(jsg::Lock& js, jsg::Optional<Type> value) {                  \
      return returnImpl(js, kj::mv(value), ReturnFunc);                                            \
    }                                                                                              \
    JSG_RESOURCE_TYPE(Name) {                                                                      \
      JSG_INHERIT_INTRINSIC(v8::kAsyncIteratorPrototype);                                          \
      JSG_METHOD(next);                                                                            \
      JSG_METHOD_NAMED(return, return_);                                                           \
      JSG_ASYNC_ITERABLE(self);                                                                    \
    }                                                                                              \
  };

// The JSG_ASYNC_ITERATOR and JSG_ASYNC_ITERATOR_WITH_OPTIONS macros provide a mechanism for
// easily implementing JavaScript-style asynchronous iterators for JSG_RESOURCE_TYPES.
//
// Example usage:
//
// class MyApiType: public jsg::Object {
// private:
//   struct IteratorState {
//     // The iterator's internal state.
//     // Implement visitForGc here if the state stores any visitable references.
//   };
//
//   static jsg::Promise<kj::Maybe<kj::String>> nextFunction(
//       jsg::Lock& js,
//       IteratorState& state) {
//     // Called to asynchronous get the next item for the iterator.
//     // Return nullptr to indicate we've reached the end of the iterator.
//     // Otherwise, return the next iterator value.
//   }
//
//   static jsg::Promise<void> returnFunction(
//       jsg::Lock& js,
//       IteratorState& state,
//       jsg::Optional<jsg::Value> value) {
//     // Called when the iterator is abruptly terminated or when the
//     // iterator generator's return() method is called. On success,
//     // an immediately resolved promise should be returned.
//   }
//
// public:
//   JSG_ASYNC_ITERATOR(MyApiTypeIterator,
//                       entries,
//                       kj::String,
//                       IteratorState,
//                       nextFunction,
//                       returnFunction);
//
//   JSG_RESOURCE_TYPE(MyApiType) {
//     JSG_METHOD(entries);
//     JSG_ASYNC_ITERABLE(entries);
//   }
//
//   jsg::Ref<MyApiTypeIterator> entries(jsg::Lock& js) {
//     return js.alloc<MyApiTypeIterator>(IteratorState { /* any necessary state init */ });
//   }
// };
//
// In this example, instances of MyApiType will support the JavaScript asynchronous iterator
// pattern (e.g. for await (const item of myApiType) {}).
//
// The actual iterator instance is defined by the type MyApiType::MyApiTypeIterator, which
// will use the IteratorState struct to store internal state and the nextFunction to yield
// the next value for the iterator.
//
// A member function named entries(Lock&) will be added to MyApiType that returns a
// jsg::Ref<MyApiTypeIterator>() instance. It will be necessary for uses to provide the
// implementation of the entries(Lock&) member function.
#define JSG_ASYNC_ITERATOR(Name, Label, Type, State, NextFunc, ReturnFunc)                         \
  JSG_ASYNC_ITERATOR_TYPE(Name, Type, State, NextFunc, ReturnFunc)                                 \
  jsg::Ref<Name> Label(jsg::Lock&);

#define JSG_ASYNC_ITERATOR_WITH_OPTIONS(Name, Label, Type, State, NextFunc, ReturnFunc, Options)   \
  JSG_ASYNC_ITERATOR_TYPE(Name, Type, State, NextFunc, ReturnFunc)                                 \
  jsg::Ref<Name> Label(jsg::Lock&, jsg::Optional<Options>);

}  // namespace workerd::jsg
