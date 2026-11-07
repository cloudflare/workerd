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

// P1, ae9b5c373 (AbortSignal::any): a listener captures the signal by reference, and aborting it
// runs listeners that can drop the last reference to it.
struct AbortSignal {
  jsg::Function<void()> onAbort;
  void triggerAbort(jsg::Lock& js) {
    onAbort(js);
  }

  static auto any(jsg::Ref<AbortSignal>& signal) {
    return [&signal = *signal](jsg::Lock& js) {
      signal.triggerAbort(js);  // P1
    };
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

// P6, 814f7b6ca1c3 (writeStdio): a console.log getter can write to stdio again and reallocate the
// line buffer a view points into.
struct StdioFile {
  kj::Vector<char> lineBuffer;
  jsg::Function<void()> log;

  kj::size_t write(jsg::Lock& js) {
    kj::ArrayPtr<char> line(lineBuffer.begin(), 0);
    log(js);
    return line.size();  // P6
  }
};
