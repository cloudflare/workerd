// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for jsg-js-reentry: none may produce a diagnostic.

namespace v8 {

class Context;

class Isolate {
 public:
  void Dispose();
};

class Value {};

class Function {
 public:
  Value* Call(Context* context, Value* recv, int argc, Value** argv);
};

class Module {
 public:
  Value* Evaluate(Context* context);
};

class Promise {
 public:
  class Resolver {
   public:
    bool Resolve(Context* context, Value* value);
  };
};

}  // namespace v8

namespace kj {

using size_t = decltype(sizeof(0));

template <typename T>
T&& mv(T& value) {
  return static_cast<T&&>(value);
}

template <typename T>
class Maybe {
 public:
  T* ptr;
};

template <typename T>
class Vector {
 public:
  T* begin();
  T* end();
  T& add(T&& value);
};

template <typename T>
class ArrayPtr {
 public:
  ArrayPtr(T* begin, size_t size);
  size_t size() const;
  T* begin() const;
  T* end() const;
};

template <typename T>
struct DecayImpl {
  using Type = T;
};
template <typename T>
struct DecayImpl<T&> {
  using Type = T;
};
template <typename T>
using Decay = typename DecayImpl<T>::Type;

template <typename T, typename U>
constexpr bool isSame = false;
template <typename T>
constexpr bool isSame<T, T> = true;

template <typename A, typename B>
class OneOf {
 public:
  unsigned which() const;
  template <typename T>
  static constexpr unsigned tagFor() {
    return isSame<T, A> ? 1 : 2;
  }
  template <typename T>
  T& get();
  OneOf* _switchSubject() & {
    return this;
  }
};

template <typename Signature>
class Function;

template <typename Ret, typename... Args>
class Function<Ret(Args...)> {
 public:
  Ret operator()(Args... args);
};

namespace _ {

template <typename T>
T* readMaybe(Maybe<T>& maybe) {
  return maybe.ptr;
}

template <typename T>
T instance();

[[noreturn]] void fail();

}  // namespace _
}  // namespace kj

// As KJ defines them for clang.
#define KJ_SWITCH_ONEOF(value)                                                                     \
  switch (auto _kj_switch_subject = (value)._switchSubject(); _kj_switch_subject->which())
#define KJ_CASE_ONEOF(name, ...)                                                                   \
  break;                                                                                           \
  case ::kj::Decay<decltype(*_kj_switch_subject)>::template tagFor<__VA_ARGS__>():                 \
    for (auto &name = _kj_switch_subject->template get<__VA_ARGS__>(), *_kj_switch_done = &name;   \
         _kj_switch_done; _kj_switch_done = nullptr)
#define KJ_IF_SOME(name, exp)                                                                      \
  if (auto _##name = ::kj::_::readMaybe(exp))                                                      \
    if (auto& name = *_##name; false) {                                                            \
    } else
// As KJ defines it for clang.
#define KJ_ASSERT_NONNULL(exp)                                                                     \
  (*({                                                                                             \
    auto _kj_result = ::kj::_::readMaybe(exp);                                                     \
    if (!_kj_result) ::kj::_::fail();                                                              \
    _kj_result;                                                                                    \
  }))

namespace workerd::jsg {

class Lock {
 public:
  v8::Isolate* v8Isolate;
  v8::Context* context;
};

template <typename T>
class Ref {
 public:
  T& operator*() const;
  T* operator->() const;
  Ref addRef();
};

template <typename Signature>
class Function;

template <typename Ret, typename... Args>
class Function<Ret(Args...)> {
 public:
  __attribute__((annotate("jsg_may_run_js"))) Ret operator()(Lock& js, Args... args);
};

class Resolver {
 public:
  void resolve(Lock& js) {
    resolver->Resolve(js.context, nullptr);
  }

 private:
  v8::Promise::Resolver* resolver;
};

}  // namespace workerd::jsg

namespace jsg = workerd::jsg;

// A stream controller whose pull() runs the user's pull algorithm.
struct Controller {
  jsg::Function<void()> pullAlgorithm;
  void pull(jsg::Lock& js) {
    pullAlgorithm(js);
  }
};

struct Stream {
  Controller controller;
};

// A queue whose push() settles a read, which reads the result's `then`.
struct Queue {
  jsg::Resolver resolver;
  void push(jsg::Lock& js) {
    resolver.resolve(js);
  }
};

struct ReadRequest {
  kj::size_t filled;
};

void helper();

struct ReaderImpl {
  struct Attached {
    jsg::Ref<Stream> stream;
  };
  kj::Maybe<Attached> state;

  // N1: a strong reference held across the call, as in the fix for P2.
  void readWithRef(jsg::Lock& js) {
    KJ_IF_SOME(attached, state) {
      auto ref = attached.stream.addRef();
      ref->controller.pull(js);
    }
  }

  // N2: a local owning pointer dereferenced directly keeps its target alive.
  void readThroughLocal(jsg::Lock& js, jsg::Ref<Stream> stream) {
    auto& s = *stream;
    s.controller.pull(js);
  }
};

// N3: iterating a copy, as in the fix for P3.
struct Channel {
  kj::Vector<jsg::Function<void()>> copyCallbacks();

  void publish(jsg::Lock& js) {
    auto snapshot = copyCallbacks();
    for (auto& callback: snapshot) callback(js);
  }
};

struct ByobRequest {
  kj::Maybe<ReadRequest> request;
  Queue* queue;

  // N4: the borrow is used only before the call.
  void respondBefore(jsg::Lock& js, kj::size_t amount) {
    auto& req = KJ_ASSERT_NONNULL(request);
    req.filled += amount;
    queue->push(js);
  }

  // N5: the request is looked up again after the call.
  void respondLookUpAgain(jsg::Lock& js, kj::size_t amount) {
    queue->push(js);
    KJ_IF_SOME(req, request) {
      req.filled += amount;
    }
  }

  // N6: a pointer bound again after the call.
  void respondRebind(jsg::Lock& js) {
    ReadRequest* req = &KJ_ASSERT_NONNULL(request);
    queue->push(js);
    req = &KJ_ASSERT_NONNULL(request);
    req->filled = 1;
  }

  // N7: a lambda that runs JavaScript, created but not called here.
  void respondLater(jsg::Lock& js) {
    auto& req = KJ_ASSERT_NONNULL(request);
    auto later = [this](jsg::Lock& js) { queue->push(js); };
    req.filled = 1;
  }

  // N8: no call in between can run JavaScript.
  void respondNoJs() {
    auto& req = KJ_ASSERT_NONNULL(request);
    helper();
    req.filled = 1;
  }

  // N9: a suppressed site.
  void respondSuppressed(jsg::Lock& js) {
    auto& req = KJ_ASSERT_NONNULL(request);
    queue->push(js);
    req.filled = 1;  // NOLINT(jsg-js-reentry): the queue has no consumers that can run JS.
  }
};

// N10: the isolate outlives any JavaScript it runs.
struct UsesIsolate {
  jsg::Function<void()> callback;

  void run(jsg::Lock& js) {
    auto* isolate = js.v8Isolate;
    callback(js);
    isolate->Dispose();
  }
};

// N11: jsg::Function::operator() keeps its own target alive while it runs.
struct Callbacks {
  kj::Vector<jsg::Function<void()>> callbacks;

  void callFirst(jsg::Lock& js) {
    auto& callback = *callbacks.begin();
    callback(js);
  }
};

// N12: a callee that takes the lock but is marked as never running JavaScript.
__attribute__((annotate("jsg_no_js"))) int handleCount(jsg::Lock& js);

struct Handles {
  kj::Maybe<ReadRequest> request;

  void count(jsg::Lock& js) {
    auto& req = KJ_ASSERT_NONNULL(request);
    req.filled = handleCount(js);
    req.filled += 1;
  }
};

// N13: a call graph where every function calls the next two and the last calls the first, none of
// them running JavaScript. Deciding it must not walk every path through the cycle.
void c0();
void c1();
void c2();
void c3();
void c4();
void c5();
void c6();
void c7();
void c8();
void c9();
void c10();
void c11();
void c12();
void c13();
void c14();
void c15();
void c16();
void c17();
void c18();
void c19();
void c20();
void c21();
void c22();
void c23();
void c24();
void c25();
void c26();
void c27();
void c28();
void c29();
void c0() {
  c1();
  c2();
}
void c1() {
  c2();
  c3();
}
void c2() {
  c3();
  c4();
}
void c3() {
  c4();
  c5();
}
void c4() {
  c5();
  c6();
}
void c5() {
  c6();
  c7();
}
void c6() {
  c7();
  c8();
}
void c7() {
  c8();
  c9();
}
void c8() {
  c9();
  c10();
}
void c9() {
  c10();
  c11();
}
void c10() {
  c11();
  c12();
}
void c11() {
  c12();
  c13();
}
void c12() {
  c13();
  c14();
}
void c13() {
  c14();
  c15();
}
void c14() {
  c15();
  c16();
}
void c15() {
  c16();
  c17();
}
void c16() {
  c17();
  c18();
}
void c17() {
  c18();
  c19();
}
void c18() {
  c19();
  c20();
}
void c19() {
  c20();
  c21();
}
void c20() {
  c21();
  c22();
}
void c21() {
  c22();
  c23();
}
void c22() {
  c23();
  c24();
}
void c23() {
  c24();
  c25();
}
void c24() {
  c25();
  c26();
}
void c25() {
  c26();
  c27();
}
void c26() {
  c27();
  c28();
}
void c27() {
  c28();
  c29();
}
void c28() {
  c29();
  c0();
}
void c29() {
  c0();
}

struct CycleUser {
  kj::Maybe<ReadRequest> request;

  void run() {
    auto& req = KJ_ASSERT_NONNULL(request);
    c0();
    req.filled = 1;
  }
};

struct Exception {
  int code;
};
struct Value {
  int code;
};

struct Settler {
  kj::OneOf<Exception, Value> outcome;
  jsg::Resolver resolver;

  // N14: a KJ_CASE_ONEOF borrow passed to the call, then left. The case's `for` runs once, so the
  // call's own argument is not a use after it.
  void settle(jsg::Lock& js) {
    KJ_SWITCH_ONEOF(outcome) {
      KJ_CASE_ONEOF(exception, Exception) {
        settleWith(js, exception);
      }
      KJ_CASE_ONEOF(value, Value) {
        settleWith(js, value.code);
      }
    }
  }
  void settleWith(jsg::Lock& js, Exception& exception);
  void settleWith(jsg::Lock& js, int code);
};

// N15: a call through a kj::Function held in C++ state.
struct Logger {
  kj::Maybe<kj::Function<void(jsg::Lock&, int)>> maybeLogger;

  void log(jsg::Lock& js, int message) {
    KJ_IF_SOME(logger, maybeLogger) {
      logger(js, message);
    }
  }
};

// N16: a continuation named only in an unevaluated operand, as kj::Promise::then() does to find
// its start address. Registering it does not run it.
template <typename Func>
void registerContinuation(Func&& func) {
  using Result = decltype(func(kj::_::instance<jsg::Lock&>()));
  (void)sizeof(Result*);
}

struct Gate {
  kj::Maybe<ReadRequest> request;
  Queue* queue;

  void wait() {
    auto& req = KJ_ASSERT_NONNULL(request);
    registerContinuation([this](jsg::Lock& js) { queue->push(js); });
    req.filled = 1;
  }
};

// N17: a lambda passed the lock that only builds a value, as kv.c++ passes to jsg::Lock::arr().
template <typename T, typename Func>
int mapEach(jsg::Lock& js, kj::ArrayPtr<T> values, Func fn) {
  int total = 0;
  for (const T& value: values) total += fn(js, value);
  return total;
}

int countNames(jsg::Lock& js, kj::ArrayPtr<ReadRequest> names) {
  return mapEach(js, names, [](jsg::Lock&, const ReadRequest& name) { return int(name.filled); });
}
