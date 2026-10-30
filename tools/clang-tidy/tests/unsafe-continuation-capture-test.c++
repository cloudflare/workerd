// Test cases for the workerd-unsafe-continuation-capture check.
//
// //tools/clang-tidy:unsafe-continuation-capture-test runs the check against
// this file and verifies its annotations; see that script for their syntax.
// Comments of the form `// expected: ...` describe the diagnostic the
// check should emit on the following lambda; comments starting with
// `// safe` describe a lambda the check must not flag.

#include <coroutine>
#include <type_traits>

// ---------------------------------------------------------------------------
// Minimal stand-ins for kj / jsg / workerd types. These only need enough
// shape for the AST matcher to recognize the sink callees and capture
// types -- no real semantics.

namespace kj {
template <typename T>
class Promise;
template <typename T>
class Own {
 public:
  T *operator->() const;
  T &operator*() const;
};
template <typename T>
class Rc {};
template <typename T>
class Arc {};
template <typename T>
class Weak {};
template <typename T>
class WeakRc {};
template <typename T>
class Maybe {};
template <typename T>
class Array {};
template <typename T>
class Vector {
 public:
  template <typename U>
  void add(U &&);
  Array<T> releaseAsArray();
  bool empty() const;
};
template <typename T>
class ArrayBuilder {
 public:
  template <typename U>
  void add(U &&);
  Array<T> finish();
};
class TaskSet {
 public:
  template <typename U>
  void add(U &&);
  Promise<void> onEmpty();
};
template <typename T>
class ArrayPtr {
 public:
  ArrayPtr() = default;
  ArrayPtr(T *, unsigned long);
};
class StringPtr {
 public:
  StringPtr() = default;
  StringPtr(const char *);
};
class String {};
class Date {};
class WaitScope {};
template <typename T>
struct CoroutineReturn {
  void return_value(T);
};
template <>
struct CoroutineReturn<void> {
  void return_void();
};
template <typename T>
class Promise {
 public:
  struct promise_type: CoroutineReturn<T> {
    Promise get_return_object();
    std::suspend_never initial_suspend();
    std::suspend_never final_suspend() noexcept;
    void unhandled_exception();
  };
  template <typename F>
  Promise then(F &&) {
    return {};
  }
  template <typename F, typename E>
  Promise then(F &&, E &&) {
    return {};
  }
  template <typename F>
  Promise catch_(F &&) {
    return {};
  }
  template <typename... Attachments>
  Promise attach(Attachments &&...) {
    return {};
  }
  void detach();
  T wait(WaitScope &);
  bool await_ready();
  void await_suspend(std::coroutine_handle<>);
  T await_resume();
};
template <typename T>
struct UnwrapPromise {
  using Type = T;
};
template <typename T>
struct UnwrapPromise<Promise<T>> {
  using Type = T;
};
template <typename F>
auto evalLater(F &&f) -> Promise<typename UnwrapPromise<decltype(f())>::Type> {
  return {};
}
template <typename F>
auto evalNow(F &&f) -> decltype(f()) {
  return f();
}
template <typename T>
constexpr std::remove_reference_t<T> &&mv(T &&v) {
  return static_cast<std::remove_reference_t<T> &&>(v);
}
template <typename T>
ArrayBuilder<T> heapArrayBuilder(unsigned long n);
template <typename T, typename... U>
Array<T> arr(T &&, U &&...);
template <typename T>
Promise<void> joinPromisesFailFast(Array<Promise<T>>);
template <typename T>
Promise<void> joinPromises(Array<Promise<T>>);
}  // namespace kj

namespace workerd {
namespace jsg {
class Lock {};
template <typename T>
class Ref {};
template <typename T>
class V8Ref {};
template <typename T>
class JsRef {};
template <typename T>
class WeakRef {};
template <typename T>
class Promise {
 public:
  template <typename F>
  Promise then(Lock &, F &&) {
    return {};
  }
};
template <typename Self>
Ref<Self> _jsgThis(Self *);
}  // namespace jsg

class IoContext {
 public:
  template <typename Func>
  auto run(Func &&) -> kj::Promise<int> {
    return {};
  }
  void addTask(kj::Promise<void>);
  template <typename Func>
  auto addFunctor(Func &&f) -> Func && {
    return static_cast<Func &&>(f);
  }
  template <typename T, typename Func>
  jsg::Promise<T> awaitIo(jsg::Lock &, kj::Promise<T>, Func &&) {
    return {};
  }
};

template <typename T>
class IoOwn {};
template <typename T>
class IoPtr {};
template <typename T>
class WeakRef {};

// Minimal stand-in for the workerd test harness fixture. Real
// definition lives in src/workerd/tests/test-fixture.h. Its callback is
// invoked synchronously and the returned promise is `.wait()`-ed before
// `runInIoContext` returns -- so lambdas passed to it (and any nested
// continuations whose escape route is `return` from the callback) are
// treated as locally consumed.
class TestFixture {
 public:
  template <typename Func>
  auto runInIoContext(Func &&) -> int {
    return {};
  }
};
}  // namespace workerd

namespace jsg = workerd::jsg;

#define JSG_THIS (::workerd::jsg::_jsgThis(this))

// ---------------------------------------------------------------------------
// Example sink we expect the check to *not* recognize.

template <typename Container, typename F>
auto KJ_MAP(Container &&c, F && f) -> int {
  return 0;
}

// ---------------------------------------------------------------------------
// POSITIVE CASES: should trigger diagnostics.

struct Resource;

// The positive cases capture locals that refer to unknown storage. Parameters
// of a promise-returning function are covered by the promise parameter
// contract (see below).
Resource &lookupResource();
kj::ArrayPtr<int> lookupData();
kj::StringPtr lookupString();

kj::Promise<int> bareReferenceCapture() {
  Resource &r = lookupResource();
  kj::Promise<int> p;
  // expected: unsafe by-reference capture of `r`
  return p.then([&r](int x) { return x; });
}

kj::Promise<int> bareReferenceDefault() {
  Resource &r = lookupResource();
  kj::Promise<int> p;
  // expected: unsafe by-reference capture (implicit `[&]`)
  return p.then([&](int x) {
    (void)r;
    return x;
  });
}

kj::Promise<int> rawPointerCapture() {
  Resource *r = &lookupResource();
  kj::Promise<int> p;
  // expected: unsafe raw pointer capture of `r`
  return p.then([r](int x) {
    (void)r;
    return x;
  });
}

kj::Promise<int> arrayPtrCapture() {
  kj::ArrayPtr<int> data = lookupData();
  kj::Promise<int> p;
  // expected: unsafe non-owning view capture of `data`
  return p.then([data](int x) {
    (void)data;
    return x;
  });
}

kj::Promise<int> stringPtrCapture() {
  kj::StringPtr s = lookupString();
  kj::Promise<int> p;
  // expected: unsafe non-owning view capture of `s`
  return p.then([s](int x) {
    (void)s;
    return x;
  });
}

struct JsgThing {
  jsg::Promise<int> doIt(jsg::Lock &js, jsg::Promise<int> p) {
    // expected: unsafe `this` capture; use JSG_THIS
    return p.then(js, [this](jsg::Lock &, int x) {
      (void)this;
      return x;
    });
  }
};

kj::Promise<int> ioContextRun(workerd::IoContext &ctx, kj::Own<Resource> r) {
  // Mirror of streams/standard.c++:3789. Expected diagnostic on `&r`.
  return ctx.run([&r](workerd::jsg::Lock &) -> int {
    (void)r;
    return 0;
  });
}

// ---------------------------------------------------------------------------
// NEGATIVE CASES: should NOT trigger diagnostics.

kj::Promise<int> ownedCapture(kj::Own<Resource> r) {
  kj::Promise<int> p;
  // safe: kj::Own transfers ownership.
  return p.then([r = kj::mv(r)](int x) {
    (void)r;
    return x;
  });
}

kj::Promise<int> weakCaptures(kj::Weak<Resource> weak,
    kj::WeakRc<Resource> weakRc,
    workerd::WeakRef<Resource> weakRef,
    kj::Promise<int> p) {
  // safe: weak handles go null when the referent dies, so use fails loudly.
  return p.then([weak = kj::mv(weak), weakRc = kj::mv(weakRc), weakRef = kj::mv(weakRef)](int x) {
    (void)weak;
    (void)weakRc;
    (void)weakRef;
    return x;
  });
}

kj::Promise<int> jsgRefCapture(workerd::jsg::Ref<Resource> ref, kj::Promise<int> p) {
  // safe: jsg::Ref is an owning JS heap root.
  return p.then([ref = kj::mv(ref)](int x) {
    (void)ref;
    return x;
  });
}

struct JsgThingWithRef {
  jsg::Promise<int> jsgThisOk(jsg::Lock &js, jsg::Promise<int> p) {
    // safe: JSG_THIS is a jsg::Ref<Self>.
    return p.then(js, [self = JSG_THIS](jsg::Lock &, int x) {
      (void)self;
      return x;
    });
  }
};

int immediateLambda(int x, int y) {
  int &refY = y;
  // safe: invoked immediately, not stored. The matcher will see this is
  // not passed to an async-sink callee.
  return [&]() { return x + refY; }();
}

// Escape-analysis cases: the same lambda capturing bare references is
// safe when the continuation's result is consumed locally, unsafe when
// it escapes.

int promiseWaitedSynchronously(Resource &r) {
  kj::Promise<int> p;
  kj::WaitScope *ws = nullptr;
  // safe: .then's promise is .wait()'d immediately. `r` cannot dangle.
  return p
      .then([&r](int x) {
    (void)r;
    return x;
  }).wait(*ws);
}

kj::Promise<int> promiseAwaitedInCoroutine(Resource &r, kj::Promise<int> p) {
  // safe: the .then's promise is co_await-ed; the coroutine frame keeps
  // `r` alive through the suspension point.
  int v = co_await p.then([&r](int x) {
    (void)r;
    return x;
  });
  co_return v;
}

int promiseChainWaitedLater(Resource &r, kj::Promise<int> p, kj::WaitScope &ws) {
  // safe: result is bound to a local, then .wait()-ed.
  auto chained = p.then([&r](int x) {
    (void)r;
    return x;
  });
  return chained.wait(ws);
}

kj::Promise<int> promiseChainedThenReturned(kj::Promise<int> p) {
  Resource &r = lookupResource();
  // unsafe: outer .then's result is returned; the captures of *both*
  // lambdas escape. expected diagnostic on `&r`.
  return p
      .then([&r](int x) {
    (void)r;
    return x;
  }).then([](int y) { return y + 1; });
}

kj::Promise<int> promiseDiscarded(kj::Promise<int> p, Resource &r) {
  // The .then result is discarded as a full-expression. The promise's
  // destructor runs in this scope; the captures don't outlive the
  // function. Safe.
  p.then([&r](int x) {
    (void)r;
    return x;
  });
  return kj::Promise<int>();
}

// Escape-analysis through a local promise *container* that is finalized,
// joined, and co_await-ed in the same coroutine: the continuation cannot
// outlive the container, so by-reference captures are safe.

kj::Promise<void> addedToArrayBuilderThenJoined(Resource &r, kj::Promise<int> p) {
  // safe: continuation is added to a local kj::ArrayBuilder, finished, joined,
  // and co_await-ed in this coroutine.
  auto promises = kj::heapArrayBuilder<kj::Promise<int>>(1);
  promises.add(p.then([&r](int x) {
    (void)r;
    return x;
  }));
  co_await kj::joinPromisesFailFast(promises.finish());
}

kj::Promise<void> addedToVectorThenJoined(Resource &r, kj::Promise<int> p) {
  // safe: continuation is added to a local kj::Vector, released, joined, and
  // co_await-ed in this coroutine.
  kj::Vector<kj::Promise<int>> promises;
  promises.add(p.then([&r](int x) {
    (void)r;
    return x;
  }));
  co_await kj::joinPromisesFailFast(promises.releaseAsArray());
}

kj::Promise<void> addedToVectorWithEmptyCheck(Resource &r, kj::Promise<int> p) {
  // safe: the container is queried via .empty() (a benign query) and then
  // released, joined, and co_await-ed -- the continuation still cannot escape.
  kj::Vector<kj::Promise<int>> promises;
  promises.add(p.then([&r](int x) {
    (void)r;
    return x;
  }));
  if (!promises.empty()) {
    co_await kj::joinPromisesFailFast(promises.releaseAsArray());
  }
}

kj::Promise<void> localTaskIntoArrThenJoined(Resource &r, kj::Promise<int> p) {
  // safe: continuation bound to a local, moved into kj::arr(...), joined, and
  // co_await-ed in this coroutine.
  auto task = p.then([&r](int x) {
    (void)r;
    return x;
  });
  co_await kj::joinPromisesFailFast(kj::arr(kj::mv(task)));
}

kj::Array<kj::Promise<int>> addedToArrayThenReturned(Resource &r, kj::Promise<int> p) {
  // unsafe: the promise array is returned to the caller, so the continuation
  // escapes this function. expected diagnostic on `&r`.
  auto promises = kj::heapArrayBuilder<kj::Promise<int>>(1);
  promises.add(p.then([&r](int x) {
    (void)r;
    return x;
  }));
  return promises.finish();
}

// A continuation added to a *member* promise container of `*this` (e.g. a
// `kj::TaskSet` field) is owned by that member; its destructor cancels the
// continuation before any other field of `*this` can dangle, so capturing
// `this` is safe (other captures still escape).

struct MemberTaskHolder {
  kj::TaskSet tasks;

  void addThisToMemberTaskSet(kj::Promise<void> p) {
    // safe: continuation is stored in the member `tasks`; capturing `this` is
    // safe (StoredAsSelfMember).
    tasks.add(p.catch_([this]() { (void)this; }));
  }

  void addRefToMemberTaskSet(kj::Promise<void> p, Resource &r) {
    // unsafe: storing in a member container does not save a by-reference
    // capture of a stack reference -- it dangles once this method returns.
    // expected diagnostic on `&r`.
    tasks.add(p.then([&r]() { (void)r; }));
  }
};

int synchronousCallback(int x) {
  int &refX = x;
  // safe: KJ_MAP is not in the async-sinks list.
  return KJ_MAP(refX, [&](int) { return 1; });
}

kj::Promise<int> intCapture(int n, kj::Promise<int> p) {
  // safe: int is a trivially-safe value.
  return p.then([n](int x) { return x + n; });
}

kj::Promise<int> stringCapture(kj::String s, kj::Promise<int> p) {
  // safe: kj::String owns its buffer.
  return p.then([s = kj::mv(s)](int x) {
    (void)s;
    return x;
  });
}

kj::Promise<int> maybeOwnCapture(kj::Maybe<kj::Own<Resource>> maybe, kj::Promise<int> p) {
  // safe: kj::Maybe<kj::Own<T>> -- transparent container around an
  // owning type.
  return p.then([maybe = kj::mv(maybe)](int x) {
    (void)maybe;
    return x;
  });
}

// ---------------------------------------------------------------------------
// `.attach(<owning-expr>...)` binding suppression: a continuation chain
// can bind the lifetime of arbitrary referents to itself via .attach.
// Captures of those bound names are safe even when the chain escapes.

namespace kj {
template <typename T>
class Promise2 {
 public:
  template <typename F>
  Promise2 then(F &&) {
    return {};
  }
  template <typename... Attachments>
  Promise2 attach(Attachments &&...) {
    return {};
  }
};
}  // namespace kj

kj::Promise2<int> attachOwnSuppresses(kj::Own<Resource> r, kj::Promise2<int> p) {
  // safe: r is bound to the chain via .attach(kj::mv(r)), so the
  // by-reference capture of r in the inner lambda cannot dangle.
  return p
      .then([&r](int x) {
    (void)r;
    return x;
  }).attach(kj::mv(r));
}

struct PromiseAndFulfiller {
  kj::Own<Resource> fulfiller;
};

kj::Promise2<int> attachMemberInitCapture(kj::Promise2<int> p) {
  PromiseAndFulfiller paf;
  // safe: init-capture binds a reference to `paf.fulfiller`, and the
  // chain attaches `paf.fulfiller` -- the bound (base, member) pair
  // matches, so the capture is treated as safe.
  return p
      .then([&f = *paf.fulfiller](int x) {
    (void)f;
    return x;
  }).attach(kj::mv(paf.fulfiller));
}

kj::Promise2<int> attachThisSuppresses(kj::Promise2<int> p);
struct AttachThisHolder {
  kj::Promise2<int> p;
  kj::Promise2<int> doIt() {
    // safe: `*this` is attached to the chain.
    return p
        .then([this](int x) {
      (void)this;
      return x;
    }).attach(*this);
  }
};

// ---------------------------------------------------------------------------
// StoredAsSelfMember: the continuation is assigned to a member of the
// current class. The field's destructor cancels the chain before any
// other field can be torn down, so `[this]` is safe -- but other
// captures still escape.

struct StoresChain {
  kj::Promise<int> task;
  void start() {
    kj::Promise<int> p;
    // safe: [this] is OK because `task` (the storing field) will be
    // destroyed and cancel the chain before *this dies.
    task = p.then([this](int x) {
      (void)this;
      return x;
    });
  }
  void startBadRef(Resource &r) {
    kj::Promise<int> p;
    // expected: unsafe by-reference capture of `r` -- the chain may
    // outlive r even though it's stored on *this.
    task = p.then([&r](int x) {
      (void)r;
      return x;
    });
  }
};

// ---------------------------------------------------------------------------
// Free-function async sinks (`kj::evalLater`, `kj::evalLast`).

namespace kj {
template <typename F>
auto evalLast(F &&) -> Promise<int> {
  return {};
}
}  // namespace kj

kj::Promise<int> evalLaterBadCapture() {
  Resource &r = lookupResource();
  // expected: unsafe by-reference capture passed to kj::evalLater.
  return kj::evalLater([&r]() {
    (void)r;
    return 0;
  });
}

kj::Promise<int> evalLastBadCapture() {
  Resource &r = lookupResource();
  // expected: unsafe by-reference capture passed to kj::evalLast.
  return kj::evalLast([&r]() {
    (void)r;
    return 0;
  });
}

// ---------------------------------------------------------------------------
// ExtraSinks / ExtraOwningTypes: user-configurable extensions. To
// exercise these the file should be compiled with the corresponding
// CheckOptions in a project-local .clang-tidy:
//
//   CheckOptions:
//     - key:   workerd-unsafe-continuation-capture.AsyncSinks
//       value: 'myproject::mySink'
//     - key:   workerd-unsafe-continuation-capture.OwningCaptureTypes
//       value: 'myproject::MyOwn'
//
// Both cases below are exercised through that mechanism.

namespace myproject {
template <typename F>
auto mySink(F &&) -> kj::Promise<int> {
  return {};
}

template <typename T>
class MyOwn {};
}  // namespace myproject

kj::Promise<int> extraSinkBadCapture() {
  Resource &r = lookupResource();
  // expected (when ExtraSinks contains "myproject::mySink"): unsafe
  // by-reference capture passed to myproject::mySink.
  return myproject::mySink([&r]() {
    (void)r;
    return 0;
  });
}

kj::Promise<int> extraOwningTypeOk(myproject::MyOwn<Resource> own, kj::Promise<int> p) {
  // safe (when OwningCaptureTypes contains "myproject::MyOwn"):
  // capturing a MyOwn by value transfers ownership.
  return p.then([own = kj::mv(own)](int x) {
    (void)own;
    return x;
  });
}

// ---------------------------------------------------------------------------
// Synchronous sinks: lambdas passed to a callee on `kBuiltinSynchronousSinks`
// (e.g. `workerd::TestFixture::runInIoContext`) are invoked synchronously,
// and the promise they return is `.wait()`-ed before the sink returns.
// So even nested `.then()` continuations whose escape route is `return`
// from the outer lambda are safely consumed within the caller's
// activation -- their captures of stack-local references cannot dangle.

int syncSinkDirectLambdaIsExempt(workerd::TestFixture &fx, Resource &r) {
  // safe: the lambda runs synchronously; capturing `&r` cannot dangle
  // because `runInIoContext` is not an async sink at all (and a fortiori
  // not flagged for this capture).
  return fx.runInIoContext([&r]() {
    (void)r;
    return 0;
  });
}

int syncSinkNestedThenIsExempt(workerd::TestFixture &fx, Resource &r, kj::Promise<int> p) {
  // safe: the inner .then's promise is returned from a lambda that is
  // passed directly to a synchronous sink. The sink will .wait() on the
  // returned promise before returning, so `&r` cannot dangle.
  return fx.runInIoContext([&r, p = kj::mv(p)]() mutable {
    return p.then([&r](int x) {
      (void)r;
      return x;
    });
  });
}

int syncSinkNestedThenChainIsExempt(workerd::TestFixture &fx, Resource &r, kj::Promise<int> p) {
  // safe: same as above but with a longer .then chain. Every link
  // ultimately escapes via `return` from the outer sync-sink lambda.
  return fx.runInIoContext([&r, p = kj::mv(p)]() mutable {
    return p
        .then([&r](int x) {
      (void)r;
      return x;
    }).then([&r](int y) {
      (void)r;
      return y + 1;
    });
  });
}

// And the converse: when a nested .then's promise escapes via something
// other than the sync-sink lambda's `return`, the capture is still
// flagged. The promise here is stored in a member -- not returned to
// runInIoContext -- so `&r` can outlive the sink call.
struct StoresChainInSink {
  kj::Promise<int> task;
  void start(workerd::TestFixture &fx, Resource &r, kj::Promise<int> p) {
    fx.runInIoContext([&]() mutable {
      // expected: unsafe by-reference capture of `r` -- the chain is
      // assigned to `task` (a non-local destination), not returned to
      // the synchronous sink, so the capture's lifetime is not bounded
      // by runInIoContext.
      task = p.then([&r](int x) {
        (void)r;
        return x;
      });
      return 0;
    });
  }
};

// ---------------------------------------------------------------------------
// A callee that is not a sink at all (e.g. a cache whose factory runs
// synchronously) does not get its lambda checked, but a chain that lambda
// returns is still checked: the callee keeps it, so it escapes.

namespace myproject {
struct Cache {
  template <typename F>
  kj::Promise<int> findOrCreate(F &&f) {
    return f();
  }
};
}  // namespace myproject

kj::Promise<int> factoryReturnsChain(myproject::Cache &cache, Resource &r, kj::Promise<int> p) {
  // expected: unsafe by-reference capture of `r` in the inner lambda only.
  return cache.findOrCreate([&r, &p]() {
    (void)r;
    return p.then([&r](int x) {
      (void)r;
      return x;
    });
  });
}

// ---------------------------------------------------------------------------
// Referents with static storage duration.

struct Shards {
  int n;
};
Shards globalShards;

Shards &getStaticShards() {
  static Shards shards;
  return shards;
}

Shards &getStaticShardsTwoReturns(bool b) {
  static Shards a;
  static Shards c;
  if (b) return a;
  return c;
}

Shards &getStaticOrParam(bool b, Shards &other) {
  static Shards a;
  if (b) return a;
  return other;
}

Shards &getDeclaredOnly();

struct ShardSource {
  virtual Shards &get() {
    static Shards s;
    return s;
  }
};

kj::Promise<int> staticLocalViaReference(kj::Promise<int> p) {
  static Shards shards;
  auto &ref = shards;
  // safe: `ref` is bound to a static local.
  return p.then([&ref](int x) { return x + ref.n; });
}

kj::Promise<int> staticViaFunction(kj::Promise<int> p) {
  const auto &shards = getStaticShards();
  // safe: getStaticShards() only returns a static local.
  return p.then([&shards](int x) { return x + shards.n; });
}

kj::Promise<int> staticViaFunctionTwoReturns(kj::Promise<int> p) {
  auto &shards = getStaticShardsTwoReturns(true);
  // safe: every return of getStaticShardsTwoReturns() is a static local.
  return p.then([&shards](int x) { return x + shards.n; });
}

kj::Promise<int> staticViaInitCapture(kj::Promise<int> p) {
  // safe: the init-capture binds a global.
  return p.then([&s = globalShards](int x) { return x + s.n; });
}

kj::Promise<int> staticViaReferenceChain(kj::Promise<int> p) {
  auto &first = getStaticShards();
  auto &second = first;
  // safe: `second` is bound to `first`, which is bound to a static local.
  return p.then([&second](int x) { return x + second.n; });
}

kj::Promise<int> staticOrParamNotProven(Shards &other, kj::Promise<int> p) {
  auto &shards = getStaticOrParam(false, other);
  // expected: unsafe by-reference capture of `shards` -- one return of the
  // callee is not static.
  return p.then([&shards](int x) { return x + shards.n; });
}

kj::Promise<int> declaredOnlyNotProven(kj::Promise<int> p) {
  auto &shards = getDeclaredOnly();
  // expected: unsafe by-reference capture of `shards` -- the callee's body
  // is not visible.
  return p.then([&shards](int x) { return x + shards.n; });
}

kj::Promise<int> virtualNotProven(ShardSource &source, kj::Promise<int> p) {
  auto &shards = source.get();
  // expected: unsafe by-reference capture of `shards` -- a virtual call may
  // dispatch to an override that does not return a static.
  return p.then([&shards](int x) { return x + shards.n; });
}

kj::Promise<int> copyOfStaticIsLocal(kj::Promise<int> p) {
  Shards copy = getStaticShards();
  // expected: unsafe by-reference capture of `copy` -- the copy lives on
  // the stack.
  return p.then([&copy](int x) { return x + copy.n; });
}

void referenceParameterNotProven(Shards &shards, kj::Promise<int> p) {
  // expected: unsafe by-reference capture of `shards` -- a parameter's
  // referent is unknown, and the detached chain outlives the call.
  p.then([&shards](int x) { return x + shards.n; }).detach();
}

// ---------------------------------------------------------------------------
// Chains returned from a lambda passed to a result-owning sink belong to the
// sink call's result, so that result's destination decides.

kj::Promise<void> nestedChainAwaited(Resource &r, kj::Promise<int> p, kj::Promise<int> q) {
  // safe: the inner chain is returned into the outer chain, which is
  // co_await-ed.
  auto outer = p.then([&r, q = kj::mv(q)](int) mutable {
    return q.then([&r](int y) {
      (void)r;
      return y;
    });
  });
  co_await outer;
}

kj::Promise<int> nestedChainReturned(kj::Promise<int> p, kj::Promise<int> q) {
  Resource &r = lookupResource();
  // The outer chain is returned, so both captures are unsafe; each has its own region so that
  // missing either one fails.
  return p.then(
      // expected: unsafe by-reference capture of `r` in the outer lambda.
      [&r, q = kj::mv(q)](int) mutable {
    // expected: unsafe by-reference capture of `r` in the inner lambda.
    return q.then([&r](int y) {
      (void)r;
      return y;
    });
  });
}

int nestedChainInCatchWaited(Resource &r, kj::Promise<int> p, kj::WaitScope &ws) {
  // safe: the chain returned from the catch_ handler belongs to the outer
  // chain, which is .wait()-ed.
  kj::Promise<int> other;
  auto outer = p.catch_([&other, &r]() {
    return other.then([&r](int y) {
      (void)r;
      return y;
    });
  });
  return outer.wait(ws);
}

kj::Promise<int> nestedChainInUnknownSinkLambda(Resource &r, workerd::IoContext &ctx) {
  // expected: unsafe by-reference capture of `r` -- IoContext::run is not a
  // result-owning sink, so a chain returned from its lambda escapes even
  // though the lambda's own capture is checked separately.
  kj::Promise<int> q;
  auto task = ctx.run([&q, &r](workerd::jsg::Lock &) {
    return q.then([&r](int y) {
      (void)r;
      return y;
    });
  });
  co_return co_await task;
}

// Whatever owns a chain returned from a lambda, that lambda's parameters and
// locals die when it returns. A result-owning sink also destroys the closure
// once it has run, so its by-copy captures die with it.

kj::Promise<void> nestedChainCapturesOuterParameter(kj::Promise<int> p, kj::Promise<int> q) {
  co_await p.then([&q](int x) {
    // expected: unsafe by-reference capture of `x` -- the outer chain is
    // co_await-ed, but `x` dies when the outer lambda returns.
    return q.then([&x](int y) { return x + y; });
  });
}

kj::Promise<void> nestedChainCapturesOuterInitCapture(kj::Promise<int> p, kj::Promise<int> q) {
  co_await p.then([q = kj::mv(q), n = 1](int) mutable {
    // expected: unsafe by-reference capture of `n` -- the outer closure is
    // destroyed before the inner chain runs.
    return q.then([&n](int y) { return n + y; });
  });
}

kj::Promise<int> nestedChainCapturesOuterCopyOfParameter(
    Shards &shards, kj::Promise<int> p, kj::Promise<int> q) {
  return p.then([shards, q = kj::mv(q)](int) mutable {
    // expected: unsafe by-reference capture of `shards` -- it names the outer
    // closure's copy, not the caller's object that the promise parameter
    // contract covers.
    return q.then([&shards](int y) { return y + shards.n; });
  });
}

int syncSinkNestedChainCapturesClosureCopy(
    workerd::TestFixture &fx, Shards &shards, kj::Promise<int> p) {
  // safe: the synchronous sink keeps the closure, and so its copy of
  // `shards`, alive while it waits on the chain.
  return fx.runInIoContext([shards, p = kj::mv(p)]() mutable {
    return p.then([&shards](int x) { return x + shards.n; });
  });
}

int syncSinkNestedChainCapturesInitCapture(workerd::TestFixture &fx, kj::Promise<int> p) {
  // safe: the synchronous sink keeps the closure, including its init-capture,
  // alive while it waits on the chain.
  return fx.runInIoContext(
      [n = 1, p = kj::mv(p)]() mutable { return p.then([&n](int x) { return x + n; }); });
}

kj::Promise<int> localLambdaChainCapturesClosureCopy(Shards &shards, kj::Promise<int> q) {
  auto outer = [shards, q = kj::mv(q)]() mutable {
    // expected: unsafe by-reference capture of `shards` -- `outer` and its
    // copy of `shards` are destroyed when this function returns the chain.
    return q.then([&shards](int x) { return x + shards.n; });
  };
  return outer();
}

int syncSinkNestedChainCapturesLocal(workerd::TestFixture &fx, kj::Promise<int> p) {
  return fx.runInIoContext([p = kj::mv(p)]() mutable {
    int local = 1;
    // expected: unsafe by-reference capture of `local` -- it dies when the
    // lambda returns, before the sink waits on the chain.
    return p.then([&local](int x) { return x + local; });
  });
}

namespace kj {
namespace _ {
template <typename T>
struct Mapper {
  Mapper(T);
  template <typename F>
  auto operator*(F &&f) -> Array<decltype(f(0))> {
    return {};
  }
};
}  // namespace _
}  // namespace kj
#define TEST_KJ_MAP(elementName, array)                                                            \
  ::kj::_::Mapper<decltype(array) &>(array) *[&](int elementName)

kj::Promise<void> mappedChainsKeptLocally(kj::Vector<int> &items, Resource &r) {
  int replies = 0;
  // safe: each chain is returned through evalNow and KJ_MAP into the local
  // array `promises`, which is joined and co_await-ed.
  auto promises = TEST_KJ_MAP(item, items)->kj::Promise<int> {
    return kj::evalNow([&]() -> kj::Promise<int> {
      kj::Promise<int> p;
      return p.then([&replies, &r, item](int x) {
        (void)r;
        return x + item + replies++;
      });
    });
  };
  co_await kj::joinPromises(kj::mv(promises));
}

kj::Promise<void> mappedChainsDropped(kj::Vector<int> &items, Resource &r) {
  // safe: the local array `promises` is never used; dropping it cancels the
  // chains before `r` can dangle.
  auto promises = TEST_KJ_MAP(item, items)->kj::Promise<int> {
    kj::Promise<int> p;
    return p.then([&r](int x) {
      (void)r;
      return x;
    });
  };
  co_return;
}

kj::Array<kj::Promise<int>> mappedChainsReturned(kj::Vector<int> &items, Resource &r) {
  // expected: unsafe by-reference capture of `r` -- the mapped array is
  // returned.
  return TEST_KJ_MAP(item, items)->kj::Promise<int> {
    kj::Promise<int> p;
    return p.then([&r](int x) {
      (void)r;
      return x;
    });
  };
}

// ---------------------------------------------------------------------------
// A frame-local kj::TaskSet that never escapes bounds its tasks' lifetime.

struct TaskErrorHandler {};
void registerTasks(kj::TaskSet &);

kj::Promise<void> localTaskSetAwaited(Resource &r, kj::Promise<int> p) {
  kj::TaskSet tasks;
  // safe: `tasks` is local; the coroutine awaits tasks.onEmpty().
  tasks.add(p.then([&r](int x) {
    (void)r;
    return x;
  }));
  co_await tasks.onEmpty();
}

kj::Promise<void> localTaskSetFedFromLocalLambda(Resource &r, kj::Promise<int> p) {
  kj::TaskSet tasks;
  auto startOne = [&]() {
    // safe: the lambda adds to the frame-local `tasks`.
    tasks.add(p.then([&r](int x) {
      (void)r;
      return x;
    }));
  };
  startOne();
  co_await tasks.onEmpty();
}

kj::Promise<void> localTaskSetEscapes(Resource &r, kj::Promise<int> p) {
  kj::TaskSet tasks;
  // expected: unsafe by-reference capture of `r` -- `tasks` is handed to
  // an unknown function.
  tasks.add(p.then([&r](int x) {
    (void)r;
    return x;
  }));
  registerTasks(tasks);
  co_await tasks.onEmpty();
}

// ---------------------------------------------------------------------------
// kj::Timer::timeoutAfter / kj::Canceler::wrap own the promise they take.

namespace kj {
class Duration {};
class Timer {
 public:
  template <typename T>
  Promise<T> timeoutAfter(Duration, Promise<T> &&);
};
class Canceler {
 public:
  template <typename T>
  Promise<T> wrap(Promise<T>);
};
}  // namespace kj

int timeoutAfterWaited(kj::Timer &timer, Resource &r, kj::Promise<int> p, kj::WaitScope &ws) {
  // safe: timeoutAfter() owns the chain, and its result is .wait()-ed.
  auto chain = p.then([&r](int x) {
    (void)r;
    return x;
  });
  return timer.timeoutAfter(kj::Duration(), kj::mv(chain)).wait(ws);
}

kj::Promise<int> timeoutAfterReturned(kj::Timer &timer, kj::Promise<int> p) {
  Resource &r = lookupResource();
  // expected: unsafe by-reference capture of `r` -- the timeout's result is
  // returned.
  return timer.timeoutAfter(kj::Duration(), p.then([&r](int x) {
    (void)r;
    return x;
  }));
}

kj::Promise<void> cancelerWrapAwaited(kj::Canceler &canceler, Resource &r, kj::Promise<int> p) {
  // safe: wrap() owns the chain, and its result is co_await-ed.
  co_await canceler.wrap(p.then([&r](int x) {
    (void)r;
    return x;
  }));
}

struct UnknownWrapper {
  kj::Promise<int> wrap(kj::Promise<int>);
};

kj::Promise<void> unknownWrapAwaited(UnknownWrapper &w, Resource &r, kj::Promise<int> p) {
  // expected: unsafe by-reference capture of `r` -- only kj::Canceler::wrap
  // is known to own its argument.
  co_await w.wrap(p.then([&r](int x) {
    (void)r;
    return x;
  }));
}

// ---------------------------------------------------------------------------
// Uses of a local container inside a generic lambda, where the insertion
// call is still dependent in the uninstantiated pattern.

struct Slot {
  kj::Promise<int> getPromise();
};

kj::Promise<void> genericLambdaAddsToLocalVector(Slot &a, Slot &b) {
  kj::Vector<kj::Promise<int>> promises;
  auto resolve = [&](auto &slot) {
    // safe: `promises` is local and is joined and co_await-ed below.
    promises.add(slot.getPromise().then([&slot](int x) {
      (void)slot;
      return x;
    }));
  };
  resolve(a);
  resolve(b);
  if (!promises.empty()) {
    co_await kj::joinPromisesFailFast(promises.releaseAsArray());
  }
}

kj::Array<kj::Promise<int>> genericLambdaVectorReturned(Slot &a) {
  kj::Vector<kj::Promise<int>> promises;
  auto resolve = [&](auto &slot) {
    // expected: unsafe by-reference capture of `slot` -- `promises` is
    // returned.
    promises.add(slot.getPromise().then([&slot](int x) {
      (void)slot;
      return x;
    }));
  };
  resolve(a);
  return promises.releaseAsArray();
}

// ---------------------------------------------------------------------------
// ResultOwningSinks option: with `myproject::Db::runWithConn` configured, a
// chain returned from its lambda belongs to its result.

namespace myproject {
struct Conn {};
struct Db {
  template <typename F>
  kj::Promise<int> runWithConn(F &&f) {
    return {};
  }
};
}  // namespace myproject

int resultOwningSinkWaited(myproject::Db &db, Resource &r, kj::WaitScope &ws) {
  // expected (unless ResultOwningSinks contains "myproject::Db::runWithConn"):
  // unsafe by-reference capture of `r`.
  return db
      .runWithConn([&r](myproject::Conn &) {
    kj::Promise<int> q;
    return q.then([&r](int x) {
      (void)r;
      return x;
    });
  }).wait(ws);
}

// ---------------------------------------------------------------------------
// Promise parameter contract: a function's reference, pointer and view
// parameters outlive the promise it returns (KJ style guide).

namespace kj {
class HttpHeaders {};
class AsyncInputStream {
 public:
  virtual Promise<int> tryRead(ArrayPtr<char> buffer) = 0;
};
class AsyncOutputStream {
 public:
  virtual Promise<void> write(ArrayPtr<const char> buffer) = 0;
};
class HttpService {
 public:
  class Response {};
  virtual Promise<void> request(
      StringPtr url, const HttpHeaders &headers, AsyncInputStream &body, Response &response) = 0;
};
}  // namespace kj

namespace workerd {
class WorkerInterface: public kj::HttpService {
 public:
  kj::Promise<void> request(kj::StringPtr url,
      const kj::HttpHeaders &headers,
      kj::AsyncInputStream &body,
      Response &response) override = 0;
};
}  // namespace workerd

namespace myproject {
class Interface {
 public:
  virtual kj::Promise<void> call(Resource &r) = 0;
};
}  // namespace myproject

class ContractService final: public workerd::WorkerInterface {
 public:
  kj::Promise<void> request(kj::StringPtr url,
      const kj::HttpHeaders &headers,
      kj::AsyncInputStream &body,
      Response &response) override {
    // safe: the chain is returned; `url` (a view captured by value) and the
    // reference parameters outlive it.
    return promise.then([url, &headers, &body, &response]() {
      (void)url;
      (void)headers;
      (void)body;
      (void)response;
    });
  }
  kj::Promise<void> promise;
};

class ContractStream final: public kj::AsyncOutputStream {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const char> buffer) override;
  kj::Promise<void> writeHelper(kj::ArrayPtr<const char> buffer);
  void startDetached(kj::ArrayPtr<const char> buffer);
  kj::Promise<void> promise;
  kj::Promise<void> task;
  kj::TaskSet tasks;
  int mode;
};

kj::Promise<void> ContractStream::write(kj::ArrayPtr<const char> buffer) {
  if (mode == 0) {
    // safe: the evalLater promise is returned from an out-of-line definition.
    return kj::evalLater([buffer]() { (void)buffer; });
  }
  if (mode == 1) {
    // safe: the chain reaches the returned promise as an `.attach()`
    // argument.
    auto side = promise.then([buffer]() { (void)buffer; });
    return promise.attach(kj::mv(side));
  }
  if (mode == 2) {
    // expected: unsafe non-owning view capture of `buffer` -- the chain is
    // added to a member TaskSet, so it outlives the returned promise.
    tasks.add(promise.then([buffer]() { (void)buffer; }));
    return kj::Promise<void>();
  }
  if (mode == 3) {
    // expected: unsafe non-owning view capture of `buffer` -- the chain is
    // detached.
    promise.then([buffer]() { (void)buffer; }).detach();
    return kj::Promise<void>();
  }
  if (mode == 4) {
    // expected: unsafe non-owning view capture of `buffer` -- the chain is
    // stored in a member.
    task = promise.then([buffer]() { (void)buffer; });
    return kj::Promise<void>();
  }
  if (mode == 5) {
    // expected: unsafe by-reference capture of `buffer` -- a by-value
    // parameter captured by reference lives in this function's frame.
    return promise.then([&buffer]() { (void)buffer; });
  }
  if (mode == 6) {
    // expected: unsafe this capture of `this` -- the contract covers only
    // parameters.
    return promise.then([this]() { (void)this; });
  }
  if (mode == 8) {
    // expected: unsafe this capture of `this` only -- the contract covers
    // `buffer` in both lambdas, since the inner chain is returned through
    // evalLater, whose result is returned, but not `this`.
    return kj::evalLater([this, buffer]() { return promise.then([buffer]() { (void)buffer; }); });
  }
  return kj::Promise<void>();
}

kj::Promise<void> ContractStream::writeHelper(kj::ArrayPtr<const char> buffer) {
  // safe: the contract covers every function returning a promise, not only
  // overrides.
  return promise.then([buffer]() { (void)buffer; });
}

// safe: a free function's reference and pointer parameters are covered too.
kj::Promise<void> freeContractFunction(kj::Promise<void> p, Resource &r, Resource *rp) {
  return p.then([&r, rp]() {
    (void)r;
    (void)rp;
  });
}

// safe: the local lambda's every call result is returned, so its chain is the
// returned promise.
kj::Promise<void> localLambdaReturned(kj::Promise<void> p, Resource &r, int mode) {
  auto retry = [&](kj::Promise<void> q) { return q.then([&r]() { (void)r; }); };
  if (mode == 0) return retry(kj::mv(p));
  return kj::evalNow([&retry]() { return retry(kj::Promise<void>()); });
}

// expected: unsafe by-reference capture of `r` -- one call result is
// detached.
kj::Promise<void> localLambdaDetached(kj::Promise<void> p, Resource &r) {
  auto retry = [&](kj::Promise<void> q) { return q.then([&r]() { (void)r; }); };
  retry(kj::Promise<void>()).detach();
  return retry(kj::mv(p));
}

struct ContractHolder {
  kj::Promise<void> task;
};

// expected: unsafe by-reference capture of `r` -- the function returns
// nothing, so its caller makes no promise about `r` after the call.
void nonPromiseContractFunction(ContractHolder &h, kj::Promise<void> p, Resource &r) {
  h.task = p.then([&r]() { (void)r; });
}

class ContractInput final: public kj::AsyncInputStream {
 public:
  kj::Promise<int> tryRead(kj::ArrayPtr<char> buffer) override {
    // expected: unsafe non-owning view capture of `buffer` in the inner
    // lambda only -- IoContext::run does not own the chain its lambda
    // returns.
    return ctx->run([buffer](workerd::jsg::Lock &) {
      kj::Promise<int> q;
      return q.then([buffer](int x) {
        (void)buffer;
        return x;
      });
    });
  }
  workerd::IoContext *ctx;
};

class OtherInterfaceImpl final: public myproject::Interface {
 public:
  kj::Promise<void> call(Resource &r) override {
    // safe: interfaces outside kj are covered too.
    return promise.then([&r]() { (void)r; });
  }
  kj::Promise<void> promise;
};

// ---------------------------------------------------------------------------
// Suspendable request contract: kj::HttpServer::SuspendableRequest's `url` and
// `headers` stay valid until the HttpService the factory returns is destroyed,
// so a chain owned by that returned service may capture them.

namespace kj {
template <typename T, typename... Params>
Own<T> heap(Params &&...params);
class HttpServer {
 public:
  class SuspendableRequest {
   public:
    StringPtr url;
    const HttpHeaders &headers;
    int method;
  };
};
}  // namespace kj

class PromisedService final: public kj::HttpService {
 public:
  explicit PromisedService(kj::Promise<void> promise);
};
class NotAService {
 public:
  explicit NotAService(kj::Promise<void> promise);
};

struct ServiceFactory {
  kj::Own<PromisedService> direct(kj::HttpServer::SuspendableRequest &request) {
    // safe: the chain is owned by the returned service.
    return kj::heap<PromisedService>(
        promise.then([&request, &headers = request.headers, &url = request.url]() {
      (void)request;
      (void)headers;
      (void)url;
    }));
  }

  kj::Own<PromisedService> throughLocalLambda(kj::HttpServer::SuspendableRequest &request) {
    // safe: the inner chain captures the outer lambda's reference to the
    // request headers, and the service the lambda builds is returned.
    auto build = [this, &headers = request.headers]() {
      return kj::heap<PromisedService>(promise.then([&headers]() { (void)headers; }));
    };
    return build();
  }

  kj::Own<PromisedService> otherMember(kj::HttpServer::SuspendableRequest &request) {
    // expected: unsafe by-reference capture of `method` -- the contract covers
    // only `url` and `headers`.
    return kj::heap<PromisedService>(promise.then([&method = request.method]() { (void)method; }));
  }

  void stored(kj::HttpServer::SuspendableRequest &request) {
    // expected: unsafe by-reference capture of `headers` -- the service is
    // kept, not returned, so it can outlive the request.
    kept =
        kj::heap<PromisedService>(promise.then([&headers = request.headers]() { (void)headers; }));
  }

  kj::Own<NotAService> notAService(kj::HttpServer::SuspendableRequest &request) {
    // expected: unsafe by-reference capture of `headers` -- only an
    // HttpService's lifetime is tied to the request.
    return kj::heap<NotAService>(promise.then([&headers = request.headers]() { (void)headers; }));
  }

  kj::Promise<void> promise;
  kj::Own<PromisedService> kept;
};

// WorkerEntrypoint::wrapWithCanceler returns a promise that owns its argument.

class WorkerEntrypoint final: public kj::HttpService {
 public:
  template <typename T>
  kj::Promise<T> wrapWithCanceler(kj::Promise<T> p);
  template <typename T>
  kj::Promise<T> wrapOther(kj::Promise<T> p);
  kj::Promise<void> request(kj::StringPtr url,
      const kj::HttpHeaders &headers,
      kj::AsyncInputStream &body,
      Response &response) override {
    if (mode == 0) {
      // safe: the wrapped chain is returned.
      return wrapWithCanceler(promise.then([&headers]() { (void)headers; }));
    }
    // expected: unsafe by-reference capture of `headers` -- wrapOther is not
    // known to own its argument.
    return wrapOther(promise.then([&headers]() { (void)headers; }));
  }
  kj::Promise<void> promise;
  int mode;
};
