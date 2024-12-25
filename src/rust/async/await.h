#include <workerd/rust/async/future.h>
#include <workerd/rust/async/waker.h>

#include <kj/debug.h>
#include <kj/mutex.h>

namespace workerd::rust::async {

// TODO(cleanup): Make a common base class and move implementation into a .c++ file.
class BoxFutureVoidAwaiter: public kj::_::Event {
public:
  BoxFutureVoidAwaiter(kj::_::CoroutineBase& coroutine, BoxFutureVoid&& future, kj::SourceLocation location = {})
      : Event(location),
        executor(kj::getCurrentThreadExecutor()),
        coroutine(coroutine),
        future(kj::mv(future)) {}
  ~BoxFutureVoidAwaiter() noexcept(false) {
    coroutine.awaitEnd();
    // TODO(cleanup): We should probably also nullify afp.node, the same way PromiseAwaiter does.
    //   Maybe PromiseAwaiter should take an abstract Event + canImmediatelyResume() + awaitBegin()
    //   + awaitEnd() interface, which this class would also implement.
  }

  bool await_ready() {
    // TODO(perf): Check if we already have an ArcWaker from a previous suspension and give it to
    //   AwaitWaker for cloning if we have the last reference to it at this point. This could save
    //   memory allocations, but would depend on making XThreadFulfiller and XThreadPaf resettable
    //   to really benefit.
    AwaitWaker waker;

    if (future.poll(waker)) {
      // Future is ready, we're done.
      return true;
    }

    auto state = waker.reset();

    if (state.wakeCount > 0) {
      // The future returned Pending, but synchronously called `wake_by_ref()` on the AwaitWaker,
      // indicating it wants to immediately be polled again. We should arm our event right now.
      armDepthFirst();
    } else KJ_IF_SOME(wakeAfter, state.wakeAfter) {
      // Optimized path for waiting on a Rust Future which is in turn waiting on a kj::Promise.
      wakeAfter->setSelfPointer(&wakeAfter);
      wakeAfter->onReady(this);
      coroutine.awaitBegin(wakeAfter);
    } else KJ_IF_SOME(cloned, state.cloned) {
      // The future returned Pending and cloned an ArcWaker to notify us later. We'll arrange for
      // the ArcWaker's promise to arm our event once it's fulfilled.
      auto& nodeOwn = node.emplace(kj::_::PromiseNode::from(kj::mv(cloned.promise)));
      nodeOwn->setSelfPointer(&nodeOwn);
      nodeOwn->onReady(this);
      coroutine.awaitBegin(nodeOwn);
    } else {
      // The future returned Pending, did not call `wake_by_ref()` on the AwaitWaker, and did not
      // clone an ArcWaker. We are awaiting the Rust equivalent of kj::NEVER_DONE.
    }

    return false;
  }

  // We already arranged to be scheduled in await_ready(), nothing to do here.
  void await_suspend(kj::_::stdcoro::coroutine_handle<>) {}

  // Validity-check the Promise's result, then poll our wrapped Future again. If it's ready, or if
  // there was an exception, schedule the coroutine by arming its event.
  kj::Maybe<kj::Own<kj::_::Event>> fire() override {
    coroutine.awaitEnd();

    KJ_IF_SOME(nodeOwn, node) {
      nodeOwn->get(result);

      // We should only ever receive a WakeInstruction, never an exception. But if we do, propagate
      // it to the coroutine.
      if (result.exception != kj::none) {
        coroutine.armDepthFirst();
        return kj::none;
      } else KJ_IF_SOME(value, result.value) {
        if (value == WakeInstruction::IGNORE) {
          // All of our Wakers were dropped. We are awaiting the Rust equivalent of kj::NEVER_DONE,
          // and will never fire again.
          return kj::none;
        }
      }
    }

    if (await_ready()) {
      coroutine.armDepthFirst();
    }

    return kj::none;
  }

  // Unit futures return void.
  void await_resume() {
    KJ_IF_SOME(exception, result.exception) {
      kj::throwFatalException(kj::mv(exception));
    }
  }

  void traceEvent(kj::_::TraceBuilder& builder) override {
    // TODO(now): Think about safety.
    KJ_IF_SOME(nodeOwn, node) {
      nodeOwn->tracePromise(builder, true);
    }
    // TODO(cleanup): Just make traceEvent() public on CoroutineBase.
    static_cast<Event&>(coroutine).traceEvent(builder);
  }

private:
  const kj::Executor& executor;
  kj::_::CoroutineBase& coroutine;
  BoxFutureVoid future;
  // TODO(soon): PromiseAwaiter instead
  kj::Maybe<kj::_::OwnPromiseNode> node;
  kj::_::ExceptionOr<WakeInstruction> result;
};

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await);
BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await);

}  // namespace workerd::rust::async
