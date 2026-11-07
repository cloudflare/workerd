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
};

namespace _ {

template <typename T>
T* readMaybe(Maybe<T>& maybe) {
  return maybe.ptr;
}

template <typename T>
T& assertNonNull(Maybe<T>& maybe) {
  return *maybe.ptr;
}

}  // namespace _
}  // namespace kj

#define KJ_IF_SOME(name, exp)                                                                      \
  if (auto _##name = ::kj::_::readMaybe(exp))                                                      \
    if (auto& name = *_##name; false) {                                                            \
    } else
#define KJ_ASSERT_NONNULL(exp) ::kj::_::assertNonNull(exp)

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
