// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for jsg-js-reentry, each a simplified past fix; js-reentry-test.sh asserts
// every expected diagnostic and the exact total.

namespace v8 {

class Context;

class Isolate {
 public:
  void Dispose();
};

class Value {};

class Object {
 public:
  bool Has(Context* context, Value* key);
};

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

[[noreturn]] void fail();

}  // namespace _
}  // namespace kj

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

// Wraps a C++ callback for JavaScript; the callback's second parameter is the call's arguments.
template <typename Func>
void wrapSimpleFunction(jsg::Lock& js, Func&& func) {
  int args = 0;
  func(js, args);
}

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

// P1, ae9b5c373 (AbortSignal::any): a listener captures the signal by reference, and aborting it
// runs listeners that can drop the last reference to it.
struct AbortSignal {
  jsg::Function<void()> onAbort;
  void triggerAbort(jsg::Lock& js) {
    onAbort(js);
  }

  static void any(jsg::Lock& js, jsg::Ref<AbortSignal>& signal) {
    wrapSimpleFunction(js, [&signal = *signal](jsg::Lock& js, auto&) {
      signal.triggerAbort(js);  // P1
    });
  }
};

// P2, 61bd42197702 (ReaderImpl::read): the user's pull() can release the reader's lock, dropping
// the only reference to the stream whose controller is still running.
struct ReaderImpl {
  struct Attached {
    jsg::Ref<Stream> stream;
  };
  kj::Maybe<Attached> state;

  void read(jsg::Lock& js) {
    KJ_IF_SOME(attached, state) {
      attached.stream->controller.pull(js);  // P2
    }
  }
};

// P3, 53a29be929be (diagnostics_channel publish): a subscriber can unsubscribe, freeing the entry
// being called and invalidating the loop's iterator.
struct Channel {
  struct Subscriber {
    jsg::Function<void()> callback;
  };
  kj::Vector<Subscriber> subscribers;

  void publish(jsg::Lock& js) {
    for (auto& sub: subscribers) sub.callback(js);  // P3
  }
};

// P4, d53b445eeb8c (ByobRequest::respond): settling a read in push() can run a `then` getter that
// cancels the reader and frees the request.
struct ByobRequest {
  kj::Maybe<ReadRequest> request;
  Queue* queue;

  bool respond(jsg::Lock& js, kj::size_t amount) {
    auto& req = KJ_ASSERT_NONNULL(request);
    queue->push(js);
    req.filled += amount;  // P4
    return true;
  }
};

// P5, 65eda74f0610 (module registry): evaluating a module re-enters resolution, which can grow the
// cache that holds the entry.
struct ModuleRegistry {
  struct Entry {
    v8::Module* module;
    bool evaluated;
  };
  kj::Vector<Entry> cache;

  void evaluate(jsg::Lock& js, v8::Module* module) {
    auto& entry = cache.add(Entry{module, false});
    module->Evaluate(js.context);
    entry.evaluated = true;  // P5
  }
};

// P6, 814f7b6ca1c3 (writeStdio): the caller passes a view of its line buffer, and a console.log
// getter can write to stdio again and reallocate the buffer the view points into.
struct Console {
  jsg::Function<void()> log;
};

kj::size_t writeStdio(jsg::Lock& js, Console& console, kj::ArrayPtr<char> bytes) {
  auto line = bytes;
  console.log(js);
  return line.size();  // P6
}

// P7: functions that call each other without taking the lock. visit() decides cycleA() first,
// which reaches cycleB() while cycleA() is undecided; cycleB() still runs JavaScript through
// cycleA()'s later call to log.
struct Cycle {
  jsg::Function<void()> log;
  jsg::Lock* lock;
  kj::Vector<int> values;

  void cycleA();
  void cycleB() {
    cycleA();
  }

  void visit() {
    cycleA();
  }

  int read() {
    auto& first = *values.begin();
    cycleB();
    return first;  // P7
  }
};

void Cycle::cycleA() {
  cycleB();
  log(*lock);
}

// P8, 61bd42197702 as it was written: the controller is reached through a pure virtual that takes
// the lock, so nothing in the translation unit shows that read() runs the user's pull().
struct ReadableStreamController {
  virtual void read(jsg::Lock& js) = 0;
};

struct ReadableStream {
  ReadableStreamController& getController();
};

struct VirtualReaderImpl {
  struct Attached {
    jsg::Ref<ReadableStream> stream;
  };
  kj::Maybe<Attached> state;

  void read(jsg::Lock& js) {
    KJ_IF_SOME(reader, state) {
      reader.stream->getController().read(js);  // P8
    }
  }
};

// P9: `in` on a proxy runs its `has` trap.
struct Options {
  kj::Vector<int> flags;

  int lookup(jsg::Lock& js, v8::Object* object, v8::Value* key) {
    auto& flag = *flags.begin();
    object->Has(js.context, key);
    return flag;  // P9
  }
};

// P10: a loop that runs more than once uses the borrow again after the call on its next pass.
struct Batch {
  kj::Maybe<ReadRequest> request;
  Queue* queue;

  void drain(jsg::Lock& js, int count) {
    auto& pending = KJ_ASSERT_NONNULL(request);
    for (int i = 0; i < count; ++i) {
      pending.filled += 1;  // P10
      queue->push(js);
    }
  }
};
