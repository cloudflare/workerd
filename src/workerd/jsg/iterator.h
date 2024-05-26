// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include "struct.h"
#include <workerd/jsg/memory.h>
#include <concepts>
#include <deque>

namespace workerd::jsg {

// -----------------------------------------------------------------------------
// Generators

template <typename TypeWrapper>
class GeneratorWrapper;

// GeneratorNext is used internally by jsg::Generator and jsg::AsyncGenerator.
template <typename T>
struct GeneratorNext {
  bool done;

  // Value should only be nullptr if done is true. It does not
  // *have* to be nullptr if done is true, however.
  kj::Maybe<T> value;
};

template <typename T>
class GeneratorContext final {
  // See the documentation in jsg.h
public:
  // Signal early return on the generator. The value given will be returned by the
  // generator's forEach once the generator completes returning.
  void return_(Lock& js, kj::Maybe<T> maybeValue = kj::none) {
    returning = true;
    if (state.template is<Init>()) {
      state = kj::mv(maybeValue);
    }
  }

  // Indicates that return_() has been called and that an early return on the generator
  // is pending. The generator can still yield additional values.
  inline bool isReturning() const { return returning; }

  // Indicates that the generator's throw() handler has been called and that the generator
  // is likely expecting a throw. The generator can still yield additional values and could
  // even exit normally without throwing.
  inline bool isErroring() const { return state.template is<Erroring>(); }

private:
  struct Init {};
  struct Erroring {};
  using Returning = kj::Maybe<T>;

  // Active = The generator is active and producing values.
  // Errored = The generator has errored and may produce one final value.
  // Returning = return_() has been called, inserting a pending return value.
  kj::OneOf<Init, Erroring, Returning> state = Init();
  bool returning = false;

  kj::Maybe<kj::Maybe<T>> tryClearPendingReturn(Lock& js) {
    KJ_IF_SOME(pending, state.template tryGet<Returning>()) {
      // pending here is a kj::Maybe<T> which might be nullptr.
      auto value = kj::mv(pending);
      state.template init<Init>();
      return kj::Maybe<kj::Maybe<T>>(kj::mv(value));
    }
    return kj::none;
  }

  inline void setErroring() { state.template init<Erroring>(); }

  friend class Generator<T>;
  friend class AsyncGenerator<T>;
};

// Provides underlying state for Generator and AsyncGenerator instances.
template <typename Generator>
class GeneratorImpl {
public:
  using T = typename Generator::Type;
  using NextSignature = typename Generator::NextSignature;
  using ReturnSignature = typename Generator::ReturnSignature;
  using ThrowSignature = typename Generator::ThrowSignature;

  // TODO(soon): It is not currently possible to use Generator<NonCoercible<Type>>
  // because the FunctionWrapper used to support the jsg::Functions here does not
  // currently like the fact that there is no wrap impl for NonCoercible<Type>.

  template <typename TypeWrapper>
  GeneratorImpl(
      v8::Isolate* isolate,
      v8::Local<v8::Object> object,
      TypeWrapper& typeWrapper)
      : state(Active {
          .maybeNext = tryGetFunction<NextSignature>(isolate, object, "next"_kj, typeWrapper),
          .maybeReturn = tryGetFunction<ReturnSignature>(isolate, object, "return"_kj, typeWrapper),
          .maybeThrow = tryGetFunction<ThrowSignature>(isolate, object, "throw"_kj, typeWrapper)
        }) {
    // If there is no next function, there's nothing to do. Go ahead and mark finished.
    if (state.template get<Active>().maybeNext == kj::none) {
      setFinished();
    }
  }

  inline bool isFinished() const { return state.template is<Finished>(); }

  void visitForGc(GcVisitor& visitor) {
    KJ_IF_SOME(active, state) {
      visitor.visit(active.maybeNext, active.maybeReturn, active.maybeThrow);
    }
  }

private:
  struct Finished {};
  struct Active {
    kj::Maybe<NextSignature> maybeNext;
    kj::Maybe<ReturnSignature> maybeReturn;
    kj::Maybe<ThrowSignature> maybeThrow;
    GeneratorContext<T> context;
  };

  kj::OneOf<Finished, Active> state;
  kj::Maybe<T> returnValue;

  template <typename Signature>
  static kj::Maybe<Signature> tryGetFunction(
      v8::Isolate* isolate,
      v8::Local<v8::Object> object,
      kj::StringPtr name,
      auto& wrapper) {
    auto context = isolate->GetCurrentContext();
    return wrapper.tryUnwrap(isolate->GetCurrentContext(),
                             check(object->Get(context, v8StrIntern(isolate, name))),
                             (Signature*)nullptr,
                             object);
  }

  void setFinished(kj::Maybe<T> maybeReturnValue = kj::none) {
    returnValue = kj::mv(maybeReturnValue);
    state.template init<Finished>();
  }

  bool processResultMaybeDone(auto& js, auto& func, auto& result) {
    auto& active = state.template get<Active>();
    if (result.done) {
      setFinished(kj::mv(result.value));
      return true;
    }
    auto& value = KJ_ASSERT_NONNULL(result.value);
    func(js, kj::mv(value), active.context);

    return false;
  };

  friend Generator;
};

template <class Func, class T>
concept GeneratorCallback = std::invocable<Func, Lock&, T, GeneratorContext<T>&>
    && std::same_as<void, std::invoke_result_t<Func, Lock&, T, GeneratorContext<T>&>>;

template <class Func, class T>
concept AsyncGeneratorCallback = std::invocable<Func, Lock&, T, GeneratorContext<T>&>
    && std::same_as<Promise<void>, std::invoke_result_t<Func, Lock&, T, GeneratorContext<T>&>>;

template <typename T>
class Generator final {
  // See the documentation in jsg.h
public:
  Generator(Generator&&) = default;
  Generator& operator=(Generator&&) = default;

  // Func should have a signature of void(Lock&, T, GeneratorContext<T>&)
  template <GeneratorCallback<T> Func>
  kj::Maybe<T> forEach(Lock& js, Func func) {
    KJ_IF_SOME(i, this->impl) {
      auto& impl = i;
      KJ_SWITCH_ONEOF(impl->state) {
        KJ_CASE_ONEOF(finished, typename Impl::Finished) {
          return kj::none;
        }
        KJ_CASE_ONEOF(active, typename Impl::Active) {
          KJ_IF_SOME(next, active.maybeNext) {
            while(!impl->isFinished()) {
              js.tryCatch([&] {
                auto result = next(js);
                if (impl->processResultMaybeDone(js, func, result)) {
                  return;
                }

                // If we got here, done has not been signaled but we need to
                // check to see if early return has been indicated. That will
                // return a result that might indicate done. If it does, we
                // return, otherwise we continue the loop until done is signaled.
                KJ_IF_SOME(returned, active.context.tryClearPendingReturn(js)) {
                  // returned here is a kj::Maybe<T> ...
                  // If a return() handler is not provided by the generator, then
                  // we will set the return value and break out early. Otherwise,
                  // we let the return() handler tell us what to do next.
                  KJ_IF_SOME(return_, active.maybeReturn) {
                    auto result = return_(js, kj::mv(returned));
                    impl->processResultMaybeDone(js, func, result);
                  } else {
                    impl->setFinished(kj::mv(returned));
                  }
                }
              }, [&](Value exception) {
                // When an exception happens, we give the throw_() handler the opportunity
                // to deal with it if we haven't done so already. The way this works with
                // generators can be a bit confusing. For instance, suppose we have a
                // generator defined like:
                //
                //  function* foo() {
                //    try {
                //      yield 'a';
                //      yield 'b';
                //    } finally {
                //      yield 'c';
                //      yield 'd';
                //    }
                //  }
                //
                // When we have an instance of foo, e.g. f = foo(), and we call f.throw('boom')
                // before we call f.next(), then 'boom' will be thrown immediately and the
                // generator will be ended immediately.
                //
                // However, if we call next() once, then call f.throw('boom'), the call to
                // throw will return a result of { done: false, value: 'c' }. Calling next()
                // again will return { done: false, value: 'd' }. Calling next() again
                // after that will result in the pending 'boom' exception being thrown.

                KJ_IF_SOME(throw_, active.maybeThrow) {
                  auto result = throw_(js, kj::mv(exception));
                  active.context.setErroring();
                  // The throw_() handler could just end up throwing the exception
                  // synchronously, in which case we break out here and do nothing
                  // else to catch it. However, it could return a result that may
                  // or may not signal the end of the iteration. If there is a throw_()
                  // handler present, it is best for us just to defer to whatever it
                  // does and process the return, if any, just as we would any other
                  // result, regardless of whether we've already done so previously.
                  // Importantly, if throw_() returns a result, whether done is
                  // indicated or not, the value must be treated as significant.
                  impl->processResultMaybeDone(js, func, result);
                } else {
                  impl->setFinished();
                  js.throwException(kj::mv(exception));
                }
              });
            }
          }
          return kj::mv(impl->returnValue);
        }
      }
      KJ_UNREACHABLE;
    } else {
      return kj::Maybe<T>();
    }
  }

  void visitForGc(GcVisitor& visitor) {
    KJ_IF_SOME(i, impl) {
      i->visitForGc(visitor);
    }
  }

private:
  using Type = T;
  using Next = GeneratorNext<T>;
  using NextSignature = Function<Next()>;
  using ReturnSignature = Function<Next(Optional<T>)>;
  using ThrowSignature = Function<Next(Optional<Value>)>;

  friend GeneratorImpl<Generator>;
  using Impl = GeneratorImpl<Generator>;

  kj::Maybe<kj::Own<Impl>> impl;

  template <typename TypeWrapper>
  Generator(v8::Isolate* isolate,
            v8::Local<v8::Object> object,
            TypeWrapper& typeWrapper)
      : impl(kj::heap<Impl>(isolate, object, typeWrapper)) {}

  template <typename TypeWrapper>
  friend class GeneratorWrapper;
};

template <typename T>
class AsyncGenerator final {
  // See the documentation in jsg.h
public:
  AsyncGenerator(AsyncGenerator&&) = default;
  AsyncGenerator& operator=(AsyncGenerator&&) = default;

  // Func should have a signature of Promise<void>(Lock&, T, GeneratorContext<T>&)
  template <AsyncGeneratorCallback<T> Func>
  Promise<kj::Maybe<T>> forEach(Lock& js, Func func) {
    KJ_IF_SOME(i, this->impl) {
      // It is important to note that the returned Promise takes over ownership
      // of the underlying Impl state here, keeping it alive until the async
      // generator completes. Once the generator is consumed once, it cannot
      // be consumed again.
      return loop(js, *i, kj::mv(func)).then(js,
          JSG_VISITABLE_LAMBDA((impl = kj::mv(i)), (impl), (auto& js) mutable {
        return js.resolvedPromise(kj::mv(impl->returnValue));
      }));
    } else {
      return js.template resolvedPromise<kj::Maybe<T>>(kj::none);
    }
    return js.template resolvedPromise<kj::Maybe<T>>(kj::none);
  }

  // Returns a promise for the next item, or a promise that resolves to kj::none if
  // the generator is finished.
  Promise<kj::Maybe<T>> next(Lock& js);
  Promise<void> return_(Lock& js, kj::Maybe<T> maybeValue = kj::none);
  Promise<void> throw_(Lock& js, Value exception);

  void visitForGc(GcVisitor& visitor) {
    KJ_IF_SOME(i, impl) {
      i.visitForGc(visitor);
    }
  }

private:
  using Type = T;
  using Next = GeneratorNext<T>;
  using NextSignature = Function<Promise<Next>()>;
  using ReturnSignature = Function<Promise<Next>(Optional<T>)>;
  using ThrowSignature = Function<Promise<Next>(Optional<Value>)>;

  friend GeneratorImpl<AsyncGenerator>;
  using Impl = GeneratorImpl<AsyncGenerator>;

  kj::Maybe<kj::Own<Impl>> impl;

  template <typename TypeWrapper>
  AsyncGenerator(v8::Isolate* isolate,
                 v8::Local<v8::Object> object,
                 TypeWrapper& typeWrapper)
      : impl(kj::heap<Impl>(isolate, object, typeWrapper)) {}

  template <AsyncGeneratorCallback<T> Func>
  static Promise<void> loop(Lock& js, Impl& impl, Func func) {
    KJ_SWITCH_ONEOF(impl.state) {
      KJ_CASE_ONEOF(finished, typename Impl::Finished) {
        return js.resolvedPromise();
      }
      KJ_CASE_ONEOF(active, typename Impl::Active) {
        KJ_IF_SOME(next, active.maybeNext) {
          // Call next to get the next item...
          return next(js).then(js, [&impl, func = kj::mv(func)](auto& js, auto result) {
            // Impl should still be active here because nothing should have been able
            // to invalidate it yet.
            auto& active = impl.state.template get<typename Impl::Active>();

            // If result.done is true, we set the impl.returnValue, set impl to finished,
            // and return a resolved promise.
            if (impl.processResultMaybeDone(js, func, result)) {
              return js.resolvedPromise();
            }

            // Otherwise, we assume that value is not nullptr.
            auto& value = KJ_ASSERT_NONNULL(result.value);

            // We pass the result on to the handler function...
            return js.evalNow([&] { return func(js, kj::mv(value), active.context); })
                .then(js, [&impl, func = kj::cp(func)](auto& js) {
              // When the handler returns, check to see if there's anything left to do...
              if (impl.isFinished()) {
                return js.resolvedPromise();
              }

              auto& active = impl.state.template get<typename Impl::Active>();

              // If we got this far, we check to see if early return was requested.
              KJ_IF_SOME(returned, active.context.tryClearPendingReturn(js)) {
                KJ_IF_SOME(return_, active.maybeReturn) {
                  return js.evalNow([&] { return return_(js, kj::mv(returned)); })
                      .then(js, [&impl, func = kj::cp(func)](auto& js, auto result) {
                    if (impl.isFinished()) {
                      return js.resolvedPromise();
                    }
                    if (impl.processResultMaybeDone(js, func, result)) {
                      return js.resolvedPromise();
                    }
                    return loop(js, impl, kj::mv(func));
                  }, [&impl, func = kj::mv(func)](auto& js, Value exception) {
                    if (!impl.isFinished()) {
                      auto& active = impl.state.template get<typename Impl::Active>();
                      KJ_IF_SOME(throw_, active.maybeThrow) {
                        active.context.setErroring();
                        return js.evalNow([&] { return throw_(js, kj::mv(exception)); })
                            .then(js, [&impl, func = kj::mv(func)](auto& js, auto result) {
                          if (impl.processResultMaybeDone(js, func, result)) {
                            return js.resolvedPromise();
                          }
                          return loop(js, impl, kj::mv(func));
                        });
                      }
                    }
                    return js.template rejectedPromise<void>(kj::mv(exception));
                  });
                }

                impl.setFinished(kj::mv(returned));
                return js.resolvedPromise();
              }

              return loop(js, impl, kj::mv(func));
            }, [&impl, func = kj::mv(func)](auto& js, Value exception) {
              if (!impl.isFinished()) {
                auto& active = impl.state.template get<typename Impl::Active>();
                KJ_IF_SOME(throw_, active.maybeThrow) {
                  active.context.setErroring();
                  return js.evalNow([&] { return throw_(js, kj::mv(exception)); })
                      .then(js, [&impl, func = kj::mv(func)](auto& js, auto result) {
                    if (impl.processResultMaybeDone(js, func, result)) {
                      return js.resolvedPromise();
                    }
                    return loop(js, impl, kj::mv(func));
                  });
                }
              }
              return js.template rejectedPromise<void>(kj::mv(exception));
            });
          });
        }
        return js.resolvedPromise();
      }
    }
    KJ_UNREACHABLE;
  }

  template <typename TypeWrapper>
  friend class GeneratorWrapper;
};

template <typename T>
Promise<kj::Maybe<T>> AsyncGenerator<T>::next(Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i->state) {
      KJ_CASE_ONEOF(finished, typename Impl::Finished) {
        return js.resolvedPromise(kj::Maybe<T>(kj::none));
      }
      KJ_CASE_ONEOF(active, typename Impl::Active) {
        KJ_IF_SOME(next, active.maybeNext) {
          return next(js).then(js, [&i=*i](auto& js, auto result)
              -> Promise<kj::Maybe<T>> {
            // If result.done is true, we set the impl.returnValue, set impl to finished,
            // and return a resolved promise.
             if (result.done) {
               i.setFinished(kj::mv(result.value));
               return js.resolvedPromise(kj::Maybe<T>(kj::none));
             }

            // And we resolve the promise with the value.
            return js.resolvedPromise(kj::mv(result.value));
          });
        } else {
          // There is no next? weird. I guess we're done then.
         return js.resolvedPromise(kj::Maybe<T>(kj::none));
        }
      }
    }
    KJ_UNREACHABLE;
  } else {
    return js.resolvedPromise(kj::Maybe<T>(kj::none));
  }
}

template <typename T>
Promise<void> AsyncGenerator<T>::return_(Lock& js, kj::Maybe<T> maybeValue) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i->state) {
      KJ_CASE_ONEOF(finished, typename Impl::Finished) {
        return js.resolvedPromise();
      }
      KJ_CASE_ONEOF(active, typename Impl::Active) {
        KJ_IF_SOME(return_, active.maybeReturn) {
          return js.tryCatch([&] {
            return return_(js, kj::mv(maybeValue)).then(js,
                [&i=*i](jsg::Lock& js, auto result) -> void {
              if (i.isFinished()) return;
              if (result.done) {
                i.setFinished(kj::mv(result.value));
              }
            });
          }, [&](jsg::Value exception) {
            return throw_(js, kj::mv(exception));
          });
        }
        i->setFinished(kj::mv(maybeValue));
        return js.resolvedPromise();
      }
    }
  }
  return js.resolvedPromise();
}

template <typename T>
Promise<void> AsyncGenerator<T>::throw_(Lock& js, jsg::Value exception) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i->state) {
      KJ_CASE_ONEOF(finished, typename Impl::Finished) {
        return js.resolvedPromise();
      }
      KJ_CASE_ONEOF(active, typename Impl::Active) {
        KJ_IF_SOME(throw_, active.maybeThrow) {
          return throw_(js, kj::mv(exception)).then(js,
              [&i=*i](jsg::Lock& js, auto result) {
            if (!i.isFinished() && result.done) {
              i.setFinished(kj::mv(result.value));
            }
          });
        }
        i->setFinished();
        return js.resolvedPromise();
      }
    }
  }
  return js.resolvedPromise();
}

template <typename TypeWrapper>
class GeneratorWrapper {
public:
  template <typename T>
  static constexpr const char* getName(Generator<T>*) { return "Generator"; }

  template <typename T>
  static constexpr const char* getName(AsyncGenerator<T>*) { return "AsyncGenerator"; }

  template <typename T>
  static constexpr const char* getName(GeneratorNext<T>*) { return "GeneratorNext"; }

  template <typename T>
  v8::Local<v8::Object> wrap(
      v8::Local<v8::Context>,
      kj::Maybe<v8::Local<v8::Object>>,
      Generator<T>&&) = delete;

  template <typename T>
  v8::Local<v8::Object> wrap(
      v8::Local<v8::Context>,
      kj::Maybe<v8::Local<v8::Object>>,
      AsyncGenerator<T>&&) = delete;

  template <typename T>
  v8::Local<v8::Object> wrap(
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>>,
      GeneratorNext<T>&& next) = delete;
  // Generator, AsyncGenerator, and GeneratorNext instances should never be
  // passed back out into JavaScript. Use Iterators for that.

  template <typename T>
  kj::Maybe<GeneratorNext<T>> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      GeneratorNext<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsObject()) {
      auto isolate = context->GetIsolate();
      auto& typeWrapper = TypeWrapper::from(isolate);
      auto object = handle.template As<v8::Object>();

      bool done = typeWrapper.template unwrap<bool>(
          context,
          check(object->Get(context, v8StrIntern(isolate, "done"_kj))),
          TypeErrorContext::other());

      auto value = check(object->Get(context, v8StrIntern(isolate, "value"_kj)));

      if (done) {
        // If done is true, then it is ok if the value does not map to anything.
        // Why are we doing it this way? Currently in the Generator pattern, there
        // is no way of distinguishing between the generator not having any return
        // value or the generator having undefined as a return value. Because we
        // cannot differentiate the two, we treat undefined specially and always
        // return nullptr in this case rather than trying to map it to anything --
        // even if the thing we'd be mapping to can safely handle undefined as
        // a value.
        if (value->IsUndefined()) {
          return GeneratorNext<T> {
            .done = true,
            .value = kj::none,
          };
        } else {
          return GeneratorNext<T> {
            .done = true,
            .value = typeWrapper.tryUnwrap(context, value, (T*) nullptr, parentObject),
          };
        }
      }

      KJ_IF_SOME(v, typeWrapper.tryUnwrap(context, value, (T*)nullptr, parentObject)) {
        return GeneratorNext<T> {
          .done = false,
          .value = kj::mv(v),
        };
      } else {
        throwTypeError(context->GetIsolate(),
                       TypeErrorContext::other(),
                       TypeWrapper::getName((T*)nullptr));
      }
    }

    return kj::none;
  }

  template <typename T>
  kj::Maybe<Generator<T>> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Generator<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsObject()) {
      auto isolate = context->GetIsolate();
      auto object = handle.As<v8::Object>();
      auto iter = check(object->Get(context, v8::Symbol::GetIterator(isolate)));
      if (iter->IsFunction()) {
        auto func = iter.As<v8::Function>();
        auto iterObj = check(func->Call(context, object, 0, nullptr));
        if (iterObj->IsObject()) {
          return Generator<T>(isolate, iterObj.As<v8::Object>(), TypeWrapper::from(isolate));
        }
      }
    }
    return kj::none;
  }

  template <typename T>
  kj::Maybe<AsyncGenerator<T>> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      AsyncGenerator<T>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (handle->IsObject()) {
      auto isolate = context->GetIsolate();
      auto object = handle.As<v8::Object>();
      auto iter = check(object->Get(context, v8::Symbol::GetAsyncIterator(isolate)));
      // If there is no async iterator, let's try a sync iterator
      if (iter->IsUndefined()) iter = check(object->Get(context, v8::Symbol::GetIterator(isolate)));
      if (iter->IsFunction()) {
        auto func = iter.As<v8::Function>();
        auto iterObj = check(func->Call(context, object, 0, nullptr));
        if (iterObj->IsObject()) {
          return AsyncGenerator<T>(isolate, iterObj.As<v8::Object>(), TypeWrapper::from(isolate));
        }
      }
    }
    return kj::none;
  }
};

// -----------------------------------------------------------------------------
// Sequences

template <typename T>
struct Sequence: public kj::Array<T> {
  // See the documentation in jsg.h
  Sequence() = default;
  Sequence(kj::Array<T> items) : kj::Array<T>(kj::mv(items)) {}
};

template <typename TypeWrapper>
class SequenceWrapper {
  // TypeWrapper mixin for jsg::Sequences.

public:
  static auto constexpr MAX_STACK = 64;

  template <typename U> static constexpr const char* getName(Sequence<U>*) {
    // TODO(later): It would be nicer if the name included the demangled name of U
    // e.g. Sequence<Foo>
    return "Sequence";
  }

  template <typename U>
  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      jsg::Sequence<U> sequence) {
    v8::Isolate* isolate = context->GetIsolate();
    v8::EscapableHandleScope handleScope(isolate);
    KJ_STACK_ARRAY(v8::Local<v8::Value>, items, sequence.size(), MAX_STACK, MAX_STACK);
    for (auto i: kj::indices(sequence)) {
      items[i] = static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::mv(sequence[i]));
    }
    return handleScope.Escape(v8::Array::New(isolate, items.begin(), items.size()));
  }

  template <typename U>
  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      jsg::Sequence<U>& sequence) {
    v8::Isolate* isolate = context->GetIsolate();
    v8::EscapableHandleScope handleScope(isolate);
    KJ_STACK_ARRAY(v8::Local<v8::Value>, items, sequence.size(), MAX_STACK, MAX_STACK);
    for (auto i: kj::indices(sequence)) {
      items[i] = static_cast<TypeWrapper*>(this)->wrap(context, creator, kj::mv(sequence[i]));
    }
    return handleScope.Escape(v8::Array::New(isolate, items.begin(), items.size()));
  }

  template <typename U>
  kj::Maybe<Sequence<U>> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      Sequence<U>*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    auto isolate = context->GetIsolate();
    auto& typeWrapper = TypeWrapper::from(isolate);
    KJ_IF_SOME(gen, typeWrapper.tryUnwrap(context, handle, (Generator<U>*)nullptr, parentObject)) {
      kj::Vector<U> items;
      // We intentionally ignore the forEach return value.
      gen.forEach(Lock::from(isolate), [&items](Lock&, U item, auto&) {
        items.add(kj::mv(item));
      });
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
  explicit IteratorBase(State state) : state(kj::mv(state)) {}
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
      return Next { .done = false, .value = kj::mv(value) };
    }
    return Next { .done = true, .value = kj::none, };
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
  std::deque<Promise<void>> pendingStack;
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
  using ReturnSignature = Promise<void>(Lock&, State&, Optional<Value>);
  using Next = AsyncIteratorImpl::Next<Type>;
  using Finished = AsyncIteratorImpl::Finished;

  explicit AsyncIteratorBase(State state) : state(InnerState { .state = kj::mv(state) }) {}

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

  void pushCurrent(jsg::Lock& js, jsg::Promise<void> promise) {
    auto& inner = state.template get<InnerState>();
    inner.impl.pushCurrent(promise.whenResolved(js).then(js,
        [this, self = JSG_THIS](jsg::Lock& js) {
      // If state is Finished, then there's nothing we need to do here.
      KJ_IF_SOME(inner, state.template tryGet<InnerState>()) {
        inner.impl.popCurrent();
      }
      return js.resolvedPromise();
    }, [this, self = JSG_THIS](jsg::Lock& js, jsg::Value value) {
      KJ_IF_SOME(inner, state.template tryGet<InnerState>()) {
        inner.impl.popCurrent();
      }
      return js.rejectedPromise<void>(kj::mv(value));
    }));
  }

  Promise<Next> nextImpl(Lock& js, NextSignature nextFunc) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(finished, Finished) {
        return js.resolvedPromise(Next { .done = true });
      }
      KJ_CASE_ONEOF(inner, InnerState) {
        // If return_() has already been called on the async iterator, we just return an immediately
        // resolved promise indicating done, regardless of whether there are still other outstanding
        // next promises or not.
        if (inner.impl.returning) {
          return js.resolvedPromise(Next { .done = true });
        }

        auto callNext =
            [this, self = JSG_THIS, nextFunc = kj::mv(nextFunc)](Lock& js) mutable {
          KJ_SWITCH_ONEOF(state) {
            KJ_CASE_ONEOF(finished, Finished) {
              return js.resolvedPromise(Next { .done = true });
            }
            KJ_CASE_ONEOF(inner, InnerState) {
              auto promise = nextFunc(js, inner.state);
              pushCurrent(js, promise.whenResolved(js));
              return promise.then(js,
                  [this, self = kj::mv(self)](Lock& js, kj::Maybe<Type> maybeResult) mutable {
                KJ_IF_SOME(result, maybeResult) {
                  return js.resolvedPromise(Next { .done = false, .value = kj::mv(result) });
                } else {
                  state.template init<Finished>();
                  return js.resolvedPromise(Next { .done = true });
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

  Promise<Next> returnImpl(
      Lock& js,
      Optional<Value> value,
      ReturnSignature returnFunc) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(finished, Finished) {
         return js.resolvedPromise(Next { .done = true, .value = kj::mv(value) });
      }
      KJ_CASE_ONEOF(inner, InnerState) {

        // When inner.returning is true, return_() has already been called on the iterator.
        // Any further calls to either next() or return_() will result in immediately resolved
        // promises indicating a done status being returned, regardless of any other promises
        // that may be pending.
        if (inner.impl.returning) {
          return js.resolvedPromise(Next { .done = true, .value = kj::mv(value) });
        }

        inner.impl.returning = true;

        auto callReturn = [this,
                           self = JSG_THIS,
                           value = kj::mv(value),
                           returnFunc = kj::mv(returnFunc)](Lock& js) mutable {
          KJ_SWITCH_ONEOF(state) {
            KJ_CASE_ONEOF(finished, Finished) {
              return js.resolvedPromise(Next { .done = true, .value = kj::mv(value) });
            }
            KJ_CASE_ONEOF(inner, InnerState) {
              return returnFunc(js, inner.state,
                                value.map([&](Value& v) { return v.addRef(js.v8Isolate); }))
                  .then(js, [this, self = kj::mv(self), value = kj::mv(value)]
                            (Lock& js) mutable {
                state.template init<Finished>();
                return js.resolvedPromise(Next { .done = true, .value = kj::mv(value) });
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
//     return jsg::alloc<MyApiTypeIterator>(IteratorState { /* any necessary state init */ });
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
#define JSG_ITERATOR(Name, Label, Type, State, NextFunc)                                        \
  class Name final: public jsg::IteratorBase<Name, Type, State> {                               \
  public:                                                                                       \
    using jsg::IteratorBase<Name, Type, State>::IteratorBase;                                   \
    inline Next next(jsg::Lock& js) { return nextImpl(js, NextFunc); }                          \
    JSG_RESOURCE_TYPE(Name) {                                                                   \
      JSG_INHERIT_INTRINSIC(v8::kIteratorPrototype);                                            \
      JSG_METHOD(next);                                                                         \
      JSG_ITERABLE(self);                                                                       \
    }                                                                                           \
  };                                                                                            \
  jsg::Ref<Name> Label(jsg::Lock&);

#define JSG_ASYNC_ITERATOR_TYPE(Name, Type, State, NextFunc, ReturnFunc)                       \
  class Name final: public jsg::AsyncIteratorBase<Name, Type, State> {                         \
  public:                                                                                       \
    using jsg::AsyncIteratorBase<Name, Type, State>::AsyncIteratorBase;                        \
    inline jsg::Promise<Next> next(jsg::Lock& js) { return nextImpl(js, NextFunc); }          \
    inline jsg::Promise<Next> return_(jsg::Lock& js, jsg::Optional<jsg::Value> value) {     \
      return returnImpl(js, kj::mv(value), ReturnFunc);                                         \
    }                                                                                           \
    JSG_RESOURCE_TYPE(Name) {                                                                  \
      JSG_INHERIT_INTRINSIC(v8::kAsyncIteratorPrototype);                                      \
      JSG_METHOD(next);                                                                        \
      JSG_METHOD_NAMED(return, return_);                                                       \
      JSG_ASYNC_ITERABLE(self);                                                                \
    }                                                                                           \
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
//     return jsg::alloc<MyApiTypeIterator>(IteratorState { /* any necessary state init */ });
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
#define JSG_ASYNC_ITERATOR(Name, Label, Type, State, NextFunc, ReturnFunc)                     \
  JSG_ASYNC_ITERATOR_TYPE(Name, Type, State, NextFunc, ReturnFunc)                             \
  jsg::Ref<Name> Label(jsg::Lock&);

#define JSG_ASYNC_ITERATOR_WITH_OPTIONS(Name, Label, Type, State, NextFunc,                    \
                                         ReturnFunc, Options)                                   \
  JSG_ASYNC_ITERATOR_TYPE(Name, Type, State, NextFunc, ReturnFunc)                             \
  jsg::Ref<Name> Label(jsg::Lock&, jsg::Optional<Options>);

}  // namespace workerd::jsg
