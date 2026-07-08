// Test cases for the workerd-unsafe-continuation-capture check.
//
// This file is *not* part of the regular build. To run the check against
// it manually:
//
//   bazel build //tools/clang-tidy:workerd-lint
//   clang-tidy --load=bazel-bin/tools/clang-tidy/libworkerd-lint.so \
//       --checks=-*,workerd-unsafe-continuation-capture \
//       tools/clang-tidy/tests/unsafe-continuation-capture-test.c++ -- -std=c++23
//
// Comments of the form `// expected: ...` describe the diagnostic the
// check should (or should not) emit on the following lambda.

// ---------------------------------------------------------------------------
// Minimal stand-ins for kj / jsg / workerd types. These only need enough
// shape for the AST matcher to recognize the sink callees and capture
// types -- no real semantics.

namespace kj {
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
class WeakRef {};
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
template <typename T>
class Promise {
public:
  template <typename F>
  Promise then(F &&);
  template <typename F>
  Promise catch_(F &&);
};
template <typename F>
auto evalLater(F &&f) -> Promise<int>;
template <typename T, typename... Attachments>
T mv(T &&v) {
  return static_cast<T &&>(v);
}
template <typename T>
ArrayBuilder<T> heapArrayBuilder(unsigned long n);
template <typename T, typename... U>
Array<T> arr(T &&, U &&...);
template <typename T>
Promise<void> joinPromisesFailFast(Array<Promise<T>>);
template <typename T>
Promise<void> joinPromises(Array<Promise<T>>);
} // namespace kj

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
  Promise then(Lock &, F &&);
};
template <typename Self>
Ref<Self> _jsgThis(Self *);
} // namespace jsg

class IoContext {
public:
  template <typename Func>
  auto run(Func &&) -> kj::Promise<int>;
  void addTask(kj::Promise<void>);
  template <typename Func>
  auto addFunctor(Func &&) -> Func &&;
  template <typename T, typename Func>
  jsg::Promise<T> awaitIo(jsg::Lock &, kj::Promise<T>, Func &&);
};

template <typename T>
class IoOwn {};
template <typename T>
class IoPtr {};

// Minimal stand-in for the workerd test harness fixture. Real
// definition lives in src/workerd/tests/test-fixture.h. Its callback is
// invoked synchronously and the returned promise is `.wait()`-ed before
// `runInIoContext` returns -- so lambdas passed to it (and any nested
// continuations whose escape route is `return` from the callback) are
// treated as locally consumed.
class TestFixture {
public:
  template <typename Func>
  auto runInIoContext(Func &&) -> int;
};
} // namespace workerd

#define JSG_THIS (::workerd::jsg::_jsgThis(this))

// ---------------------------------------------------------------------------
// Example sink we expect the check to *not* recognize.

template <typename Container, typename F>
auto KJ_MAP(Container &&c, F &&f) -> int {
  return 0;
}

// ---------------------------------------------------------------------------
// POSITIVE CASES: should trigger diagnostics.

struct Resource;

kj::Promise<int> bareReferenceCapture(Resource &r) {
  kj::Promise<int> p;
  // expected: unsafe by-reference capture of `r`
  return p.then([&r](int x) { return x; });
}

kj::Promise<int> bareReferenceDefault(Resource &r) {
  kj::Promise<int> p;
  // expected: unsafe by-reference capture (implicit `[&]`)
  return p.then([&](int x) { (void)r; return x; });
}

kj::Promise<int> rawPointerCapture(Resource *r) {
  kj::Promise<int> p;
  // expected: unsafe raw pointer capture of `r`
  return p.then([r](int x) { (void)r; return x; });
}

kj::Promise<int> arrayPtrCapture(kj::ArrayPtr<int> data) {
  kj::Promise<int> p;
  // expected: unsafe non-owning view capture of `data`
  return p.then([data](int x) { (void)data; return x; });
}

kj::Promise<int> stringPtrCapture(kj::StringPtr s) {
  kj::Promise<int> p;
  // expected: unsafe non-owning view capture of `s`
  return p.then([s](int x) { (void)s; return x; });
}

struct JsgThing {
  jsg::Promise<int> doIt(jsg::Lock &js, jsg::Promise<int> p) {
    // expected: unsafe `this` capture; use JSG_THIS
    return p.then(js, [this](jsg::Lock &, int x) { (void)this; return x; });
  }
};

kj::Promise<int> ioContextRun(workerd::IoContext &ctx, kj::Own<Resource> r) {
  // Mirror of streams/standard.c++:3789. Expected diagnostic on `&r`.
  return ctx.run([&r](workerd::jsg::Lock &) -> int { (void)r; return 0; });
}

// ---------------------------------------------------------------------------
// NEGATIVE CASES: should NOT trigger diagnostics.

kj::Promise<int> ownedCapture(kj::Own<Resource> r) {
  kj::Promise<int> p;
  // safe: kj::Own transfers ownership.
  return p.then([r = kj::mv(r)](int x) { (void)r; return x; });
}

kj::Promise<int> jsgRefCapture(workerd::jsg::Ref<Resource> ref,
                               kj::Promise<int> p) {
  // safe: jsg::Ref is an owning JS heap root.
  return p.then([ref = kj::mv(ref)](int x) { (void)ref; return x; });
}

jsg::Promise<int> jsgThisOk(JsgThing *t, jsg::Lock &js, jsg::Promise<int> p) {
  // safe: JSG_THIS is a jsg::Ref<Self>.
  return p.then(js, [self = JSG_THIS](jsg::Lock &, int x) {
    (void)self;
    return x;
  });
}

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
  return p.then([&r](int x) { (void)r; return x; }).wait(*ws);
}

kj::Promise<int> promiseAwaitedInCoroutine(Resource &r, kj::Promise<int> p) {
  // safe: the .then's promise is co_await-ed; the coroutine frame keeps
  // `r` alive through the suspension point.
  int v = co_await p.then([&r](int x) { (void)r; return x; });
  co_return v;
}

kj::Promise<int> promiseChainWaitedLater(Resource &r, kj::Promise<int> p,
                                         kj::WaitScope &ws) {
  // safe: result is bound to a local, then .wait()-ed.
  auto chained = p.then([&r](int x) { (void)r; return x; });
  return chained.wait(ws);
}

kj::Promise<int> promiseChainedThenReturned(Resource &r, kj::Promise<int> p) {
  // unsafe: outer .then's result is returned; the captures of *both*
  // lambdas escape. expected diagnostic on `&r`.
  return p.then([&r](int x) { (void)r; return x; })
      .then([](int y) { return y + 1; });
}

kj::Promise<int> promiseDiscarded(kj::Promise<int> p, Resource &r) {
  // The .then result is discarded as a full-expression. The promise's
  // destructor runs in this scope; the captures don't outlive the
  // function. Safe.
  p.then([&r](int x) { (void)r; return x; });
  return kj::Promise<int>();
}

// Escape-analysis through a local promise *container* that is finalized,
// joined, and co_await-ed in the same coroutine: the continuation cannot
// outlive the container, so by-reference captures are safe.

kj::Promise<void> addedToArrayBuilderThenJoined(Resource &r, kj::Promise<int> p) {
  // safe: continuation is added to a local kj::ArrayBuilder, finished, joined,
  // and co_await-ed in this coroutine.
  auto promises = kj::heapArrayBuilder<kj::Promise<int>>(1);
  promises.add(p.then([&r](int x) { (void)r; return x; }));
  co_await kj::joinPromisesFailFast(promises.finish());
}

kj::Promise<void> addedToVectorThenJoined(Resource &r, kj::Promise<int> p) {
  // safe: continuation is added to a local kj::Vector, released, joined, and
  // co_await-ed in this coroutine.
  kj::Vector<kj::Promise<int>> promises;
  promises.add(p.then([&r](int x) { (void)r; return x; }));
  co_await kj::joinPromisesFailFast(promises.releaseAsArray());
}

kj::Promise<void> addedToVectorWithEmptyCheck(Resource &r, kj::Promise<int> p) {
  // safe: the container is queried via .empty() (a benign query) and then
  // released, joined, and co_await-ed -- the continuation still cannot escape.
  kj::Vector<kj::Promise<int>> promises;
  promises.add(p.then([&r](int x) { (void)r; return x; }));
  if (!promises.empty()) {
    co_await kj::joinPromisesFailFast(promises.releaseAsArray());
  }
}

kj::Promise<void> localTaskIntoArrThenJoined(Resource &r, kj::Promise<int> p) {
  // safe: continuation bound to a local, moved into kj::arr(...), joined, and
  // co_await-ed in this coroutine.
  auto task = p.then([&r](int x) { (void)r; return x; });
  co_await kj::joinPromisesFailFast(kj::arr(kj::mv(task)));
}

kj::Array<kj::Promise<int>> addedToArrayThenReturned(Resource &r,
                                                     kj::Promise<int> p) {
  // unsafe: the promise array is returned to the caller, so the continuation
  // escapes this function. expected diagnostic on `&r`.
  auto promises = kj::heapArrayBuilder<kj::Promise<int>>(1);
  promises.add(p.then([&r](int x) { (void)r; return x; }));
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
  return p.then([s = kj::mv(s)](int x) { (void)s; return x; });
}

kj::Promise<int> maybeOwnCapture(kj::Maybe<kj::Own<Resource>> maybe,
                                 kj::Promise<int> p) {
  // safe: kj::Maybe<kj::Own<T>> -- transparent container around an
  // owning type.
  return p.then([maybe = kj::mv(maybe)](int x) { (void)maybe; return x; });
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
  Promise2 then(F &&);
  template <typename... Attachments>
  Promise2 attach(Attachments &&...);
};
} // namespace kj

kj::Promise2<int> attachOwnSuppresses(kj::Own<Resource> r, kj::Promise2<int> p) {
  // safe: r is bound to the chain via .attach(kj::mv(r)), so the
  // by-reference capture of r in the inner lambda cannot dangle.
  return p.then([&r](int x) { (void)r; return x; }).attach(kj::mv(r));
}

struct PromiseAndFulfiller {
  kj::Own<Resource> fulfiller;
};

kj::Promise2<int> attachMemberInitCapture(kj::Promise2<int> p) {
  PromiseAndFulfiller paf;
  // safe: init-capture binds a reference to `paf.fulfiller`, and the
  // chain attaches `paf.fulfiller` -- the bound (base, member) pair
  // matches, so the capture is treated as safe.
  return p.then([&f = *paf.fulfiller](int x) { (void)f; return x; })
      .attach(kj::mv(paf.fulfiller));
}

kj::Promise2<int> attachThisSuppresses(kj::Promise2<int> p);
struct AttachThisHolder {
  kj::Promise2<int> p;
  kj::Promise2<int> doIt() {
    // safe: `*this` is attached to the chain.
    return p.then([this](int x) { (void)this; return x; }).attach(*this);
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
    task = p.then([this](int x) { (void)this; return x; });
  }
  void startBadRef(Resource &r) {
    kj::Promise<int> p;
    // expected: unsafe by-reference capture of `r` -- the chain may
    // outlive r even though it's stored on *this.
    task = p.then([&r](int x) { (void)r; return x; });
  }
};

// ---------------------------------------------------------------------------
// Free-function async sinks (`kj::evalLater`, `kj::evalLast`).

namespace kj {
template <typename F>
auto evalLast(F &&) -> Promise<int>;
} // namespace kj

kj::Promise<int> evalLaterBadCapture(Resource &r) {
  // expected: unsafe by-reference capture passed to kj::evalLater.
  return kj::evalLater([&r]() { (void)r; return 0; });
}

kj::Promise<int> evalLastBadCapture(Resource &r) {
  // expected: unsafe by-reference capture passed to kj::evalLast.
  return kj::evalLast([&r]() { (void)r; return 0; });
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
auto mySink(F &&) -> kj::Promise<int>;

template <typename T>
class MyOwn {};
} // namespace myproject

kj::Promise<int> extraSinkBadCapture(Resource &r) {
  // expected (when ExtraSinks contains "myproject::mySink"): unsafe
  // by-reference capture passed to myproject::mySink.
  return myproject::mySink([&r]() { (void)r; return 0; });
}

kj::Promise<int> extraOwningTypeOk(myproject::MyOwn<Resource> own,
                                   kj::Promise<int> p) {
  // safe (when OwningCaptureTypes contains "myproject::MyOwn"):
  // capturing a MyOwn by value transfers ownership.
  return p.then([own = kj::mv(own)](int x) { (void)own; return x; });
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
  return fx.runInIoContext([&r]() { (void)r; return 0; });
}

int syncSinkNestedThenIsExempt(workerd::TestFixture &fx, Resource &r,
                               kj::Promise<int> p) {
  // safe: the inner .then's promise is returned from a lambda that is
  // passed directly to a synchronous sink. The sink will .wait() on the
  // returned promise before returning, so `&r` cannot dangle.
  return fx.runInIoContext([&r, p = kj::mv(p)]() mutable {
    return p.then([&r](int x) { (void)r; return x; });
  });
}

int syncSinkNestedThenChainIsExempt(workerd::TestFixture &fx, Resource &r,
                                    kj::Promise<int> p) {
  // safe: same as above but with a longer .then chain. Every link
  // ultimately escapes via `return` from the outer sync-sink lambda.
  return fx.runInIoContext([&r, p = kj::mv(p)]() mutable {
    return p.then([&r](int x) { (void)r; return x; })
        .then([&r](int y) { (void)r; return y + 1; });
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
      task = p.then([&r](int x) { (void)r; return x; });
      return 0;
    });
  }
};
