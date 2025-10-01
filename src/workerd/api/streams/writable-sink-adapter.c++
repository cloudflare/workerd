#include "writable-sink-adapter.h"

#include "writable.h"

#include <workerd/api/system-streams.h>
#include <workerd/util/checked-queue.h>

namespace workerd::api::streams {

// The Active state maintains a queue of tasks, such as write or flush operations. Each task
// contains a promise-returning function object and a fulfiller. When the first task is
// enqueued, the active state begins processing the queue asynchronously. Each function
// is invoked in order, its promise awaited, and the result passed to the fulfiller. The
// fulfiller notifies the code which enqueued the task that the task has completed. In
// this way, read and close operations are safely executed in serial, even if one operation
// is called before the previous completes. This mechanism satisfies KJ's restriction on
// concurrent operations on streams.
struct WritableStreamSinkJsAdapter::Active final {
  struct Task {
    kj::Function<kj::Promise<void>()> task;
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
    Task(kj::Function<kj::Promise<void>()> task, kj::Own<kj::PromiseFulfiller<void>> fulfiller)
        : task(kj::mv(task)),
          fulfiller(kj::mv(fulfiller)) {}
    KJ_DISALLOW_COPY_AND_MOVE(Task);
  };
  using TaskQueue = workerd::util::Queue<kj::Own<Task>>;

  kj::Own<WritableStreamSink> sink;
  const Options options;
  kj::Canceler canceler;
  TaskQueue queue;
  bool aborted = false;
  bool running = false;
  bool closePending = false;
  size_t bytesInFlight = 0;
  kj::Maybe<kj::Exception> pendingAbort;

  Active(kj::Own<WritableStreamSink> sink, Options options)
      : sink(kj::mv(sink)),
        options(kj::mv(options)) {
    KJ_DASSERT(this->sink.get() != nullptr, "WritableStreamSink cannot be null");
  }

  KJ_DISALLOW_COPY_AND_MOVE(Active);
  ~Active() noexcept(false) {
    // When the Active is dropped, we cancel any remaining pending writes and
    // abort the sink.
    abort(KJ_EXCEPTION(FAILED, "jsg.Error: Writable stream is canceled or closed."));

    // Check invariants for safety.
    // 1. Our canceler should be empty because we canceled it.
    KJ_DASSERT(canceler.isEmpty());
    // 2. The write queue should be empty.
    KJ_DASSERT(queue.empty());
  }

  // Explicitly cancel all in-flight and pending tasks in the queue.
  // This is a non-op if cancel has already been called.
  void abort(kj::Exception&& exception) {
    if (aborted) return;
    aborted = true;
    // 1. Cancel our in-flight "runLoop", if any.
    pendingAbort = kj::cp(exception);
    canceler.cancel(kj::cp(exception));
    // 2. Drop our queue of pending tasks.
    queue.drainTo(
        [&exception](kj::Own<Task>&& task) { task->fulfiller->reject(kj::cp(exception)); });
    // 3. Abort and drop the sink itself. We're done with it.
    sink->abort(kj::mv(exception));
    auto dropped KJ_UNUSED = kj::mv(sink);
  }

  // Get the desired size based on the configured high water mark and
  // the number of bytes currently in flight.
  ssize_t getDesiredSize() const {
    return options.highWaterMark - bytesInFlight;
  }

  kj::Promise<void> enqueue(kj::Function<kj::Promise<void>()> task) {
    KJ_DASSERT(!aborted, "cannot enqueue tasks on an aborted queue");
    auto paf = kj::newPromiseAndFulfiller<void>();
    queue.push(kj::heap<Task>(kj::mv(task), kj::mv(paf.fulfiller)));
    if (!running) {
      IoContext::current().addTask(canceler.wrap(run()));
    }
    return kj::mv(paf.promise);
  }

  kj::Promise<void> run() {
    KJ_DEFER(running = false);
    running = true;
    while (!queue.empty() && !aborted) {
      auto task = KJ_ASSERT_NONNULL(queue.pop());
      KJ_DEFER({
        if (task->fulfiller->isWaiting()) {
          KJ_IF_SOME(pending, pendingAbort) {
            task->fulfiller->reject(kj::mv(pending));
          } else {
            task->fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "Task was canceled."));
          }
        }
      });
      bool taskFailed = false;
      try {
        co_await task->task();
        task->fulfiller->fulfill();
      } catch (...) {
        auto ex = kj::getCaughtExceptionAsKj();
        task->fulfiller->reject(kj::mv(ex));
        taskFailed = true;
      }
      // If the task failed, we exit the loop. We're going to abort the
      // entire remaining queue anyway so there's no point in continuing.
      if (taskFailed) co_return;
    }
  }
};

WritableStreamSinkJsAdapter::WritableStreamSinkJsAdapter(jsg::Lock& js,
    IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<Options> options)
    : state(ioContext.addObject(kj::heap<Active>(kj::mv(sink), kj::mv(options).orDefault({})))),
      backpressureState(newBackpressureState(js)),
      selfRef(kj::rc<WeakRef<WritableStreamSinkJsAdapter>>(
          kj::Badge<WritableStreamSinkJsAdapter>{}, *this)) {
  // We want the initial backpressure state to be "ready".
  backpressureState.release(js);
}

WritableStreamSinkJsAdapter::WritableStreamSinkJsAdapter(jsg::Lock& js,
    IoContext& ioContext,
    kj::Own<kj::AsyncOutputStream> stream,
    StreamEncoding encoding,
    kj::Maybe<Options> options)
    : WritableStreamSinkJsAdapter(
          js, ioContext, newSystemStream(kj::mv(stream), encoding, ioContext), kj::mv(options)) {}

WritableStreamSinkJsAdapter::~WritableStreamSinkJsAdapter() noexcept(false) {
  selfRef->invalidate();
}

kj::Maybe<const kj::Exception&> WritableStreamSinkJsAdapter::isErrored() {
  return state.tryGet<kj::Exception>();
}

bool WritableStreamSinkJsAdapter::isClosed() {
  return state.is<Closed>();
}

bool WritableStreamSinkJsAdapter::isClosing() {
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    return active->closePending;
  } else {
    return false;
  }
}

kj::Maybe<ssize_t> WritableStreamSinkJsAdapter::getDesiredSize() {
  return state.tryGet<IoOwn<Active>>().map(
      [](IoOwn<Active>& active) { return active->getDesiredSize(); });
}

jsg::Promise<void> WritableStreamSinkJsAdapter::write(jsg::Lock& js, const jsg::JsValue& value) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      // Deference the IoOwn once to get the active state.
      auto& active = *activeOwn;

      // If close is pending, we cannot accept any more writes.
      if (active.closePending) {
        auto exc = js.typeError("Write after close is not allowed");
        return js.rejectedPromise<void>(exc);
      }

      // Ok, we are in a writable state, there are no pending closes.
      // Let's process our data and write it!
      auto& ioContext = IoContext::current();

      // We know that a WritableStreamSink only accepts bytes, so we need to
      // verify that the value is a source of bytes. We accept three possible
      // types: ArrayBuffer, ArrayBufferView, and String. If it is a string,
      // we convert it to UTF-8 bytes. Anything else is an error.
      if (value.isArrayBufferView() || value.isArrayBuffer() || value.isSharedArrayBuffer()) {
        // We can just wrap the value with a jsg::BufferSource and write it.
        jsg::BufferSource source(js, value);
        if (active.options.detachOnWrite && source.canDetach(js)) {
          // Detach from the original ArrayBuffer...
          // ... and re-wrap it with a new BufferSource that we own.
          source = jsg::BufferSource(js, source.detach(js));
        }

        // Zero-length writes are a no-op.
        if (source.size() == 0) {
          return js.resolvedPromise();
        }

        active.bytesInFlight += source.size();
        maybeSignalBackpressure(js);
        // Enqueue the actual write operation into the write queue. We pass in
        // two lambdas, one that does the actual write, and one that handles
        // errors. If the write fails, we need to transition the adapter to the
        // errored state. If the write succeeds, we need to decrement the
        // bytesInFlight counter.
        //
        // The promise returned by enqueue is not the actual write promise but
        // a branch forked off of it. We wrap that with a JS promise that waits
        // for it to complete. Once it does, we check if we can release backpressure.
        // This has to be done within an Isolate lock because we need to be able
        // to resolve or reject the JS promises. If the write fails, we instead
        // abort the backpressure state.
        //
        // This slight indirection does mean that the backpressure state change
        // may be slightly delayed after the actual write completes but that's
        // ok.
        //
        // Capturing active by reference here is safe because the lambda is
        // held by the write queue, which is itself held by Active. If active
        // is destroyed, the write queue is destroyed along with the lambda.
        auto promise =
            active.enqueue(kj::coCapture([&active, source = kj::mv(source)]() -> kj::Promise<void> {
          co_await active.sink->write(source.asArrayPtr());
          active.bytesInFlight -= source.size();
        }));
        return ioContext
            .awaitIo(js, kj::mv(promise), [self = selfRef.addRef()](jsg::Lock& js) {
          // Why do we need a weak ref here? Well, because this is a JavaScript
          // promise continuation. It is possible that the kj::Own holding our
          // adapter can be dropped while we are waiting for the continuation
          // to run. If that happens, we don't want to delay cleanup of the
          // adapter just because of backpressure state management that would
          // not be needed anymore, so we use a weak ref to update the backpressure
          // state only if we are still alive.
          self->runIfAlive(
              [&](WritableStreamSinkJsAdapter& self) { self.maybeReleaseBackpressure(js); });
        }).catch_(js, [self = selfRef.addRef()](jsg::Lock& js, jsg::Value exception) {
          auto error = jsg::JsValue(exception.getHandle(js));
          self->runIfAlive([&](WritableStreamSinkJsAdapter& self) {
            self.abort(js, error);
            self.backpressureState.abort(js, error);
          });
          js.throwException(kj::mv(exception));
        });
      } else if (value.isString()) {
        // Also super easy! Let's just convert the string to UTF-8
        auto str = value.toString(js);

        // Zero-length writes are a no-op.
        if (str.size() == 0) {
          return js.resolvedPromise();
        }

        active.bytesInFlight += str.size();
        // Make sure to account for the memory used by the string while the
        // write is in-flight/pending
        auto accounting = js.getExternalMemoryAdjustment(str.size());
        maybeSignalBackpressure(js);
        // Just like above, enqueue the write operation into the write queue,
        // ensuring that we handle both the success and failure cases.
        auto promise = active.enqueue(kj::coCapture(
            [&active, str = kj::mv(str), accounting = kj::mv(accounting)]() -> kj::Promise<void> {
          co_await active.sink->write(str.asBytes());
          active.bytesInFlight -= str.size();
        }));
        return ioContext
            .awaitIo(js, kj::mv(promise), [self = selfRef.addRef()](jsg::Lock& js) {
          self->runIfAlive(
              [&](WritableStreamSinkJsAdapter& self) { self.maybeReleaseBackpressure(js); });
        }).catch_(js, [self = selfRef.addRef()](jsg::Lock& js, jsg::Value exception) {
          auto error = jsg::JsValue(exception.getHandle(js));
          self->runIfAlive([&](WritableStreamSinkJsAdapter& self) {
            self.abort(js, error);
            self.backpressureState.abort(js, error);
          });
          js.throwException(kj::mv(exception));
        });
      } else {
        auto err = js.typeError("This WritableStream only supports writing byte types."_kj);
        return js.rejectedPromise<void>(err);
      }
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      // Really should not have been called if errored but just in case,
      // return a rejected promise.
      return js.rejectedPromise<void>(js.exceptionToJs(kj::cp(exc)));
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Really should not have been called if closed but just in case,
      // return a rejected promise.
      return js.rejectedPromise<void>(js.typeError("Write after close is not allowed"));
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> WritableStreamSinkJsAdapter::flush(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      // Deference the IoOwn once to get the active state.
      auto& active = *activeOwn;

      // If close is pending, we cannot accept any more writes.
      if (active.closePending) {
        auto exc = js.typeError("Flush after close is not allowed");
        return js.rejectedPromise<void>(exc);
      }

      // Ok, we are in a writable state, there are no pending closes.
      // Let's enqueue our flush signal.
      auto& ioContext = IoContext::current();
      // Flushing is really just a non-op write. We enqueue a no-op task
      // into the write queue and wait for it to complete.
      auto promise = active.enqueue([]() -> kj::Promise<void> {
        // Non-op.
        return kj::READY_NOW;
      });
      return ioContext.awaitIo(js, kj::mv(promise));
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      // Really should not have been called if errored but just in case,
      // return a rejected promise.
      return js.rejectedPromise<void>(js.exceptionToJs(kj::cp(exc)));
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Really should not have been called if closed but just in case,
      // return a rejected promise.
      return js.rejectedPromise<void>(js.typeError("Flush after close is not allowed"));
    }
  }
  KJ_UNREACHABLE;
}

// Transitions the adapter into the closing state. Once the write queue
// is empty, we will close the sink and transition to the closed state.
jsg::Promise<void> WritableStreamSinkJsAdapter::end(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      auto& ioContext = IoContext::current();
      auto& active = *activeOwn;

      if (active.closePending) {
        return js.rejectedPromise<void>(js.typeError("Close already pending, cannot close again."));
      }

      active.closePending = true;
      auto promise = active.enqueue(
          kj::coCapture([&active]() -> kj::Promise<void> { co_await active.sink->end(); }));

      return ioContext
          .awaitIo(js, kj::mv(promise), [self = selfRef.addRef()](jsg::Lock& js) {
        // While nothing at this point should be actually waiting on the ready promise,
        // we should still resolve it just in case.
        self->runIfAlive([&](WritableStreamSinkJsAdapter& self) {
          self.state.init<Closed>();
          self.maybeReleaseBackpressure(js);
        });
      }).catch_(js, [self = selfRef.addRef()](jsg::Lock& js, jsg::Value&& exception) {
        // Likewise, while nothing should be waiting on the ready promise, we
        // should still reject it just in case.
        auto error = jsg::JsValue(exception.getHandle(js));
        self->runIfAlive([&](WritableStreamSinkJsAdapter& self) {
          self.abort(js, error);
          self.backpressureState.abort(js, error);
        });
        js.throwException(kj::mv(exception));
      });
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      // Really should not have been called if errored but just in case,
      // return a rejected promise.
      return js.rejectedPromise<void>(js.exceptionToJs(kj::cp(exc)));
    }
    KJ_CASE_ONEOF(_, Closed) {
      // We are already in a closed state. This is a no-op. This really
      // should not have been called if closed but just in case, return
      // a resolved promise.
      return js.resolvedPromise();
    }
  }
  KJ_UNREACHABLE;
}

// Transitions the adapter to the errored state, even if we are already closed.
void WritableStreamSinkJsAdapter::abort(kj::Exception&& exception) {
  // If we are in an active state, we need to cancel any in-flight and pending
  // operations in the active write queue *before* we transition to the errored
  // state. This ensures that any pending writes are interrupted and do not
  // complete.
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    active->abort(kj::cp(exception));
  }
  state = kj::mv(exception);
}

void WritableStreamSinkJsAdapter::abort(jsg::Lock& js, const jsg::JsValue& reason) {
  abort(js.exceptionToKj(reason));
}

void WritableStreamSinkJsAdapter::BackpressureState::abort(
    jsg::Lock& js, const jsg::JsValue& reason) {
  // Backpressure signaling is being aborted, likely because the adapter
  // transitioned to the errored state. Reject the ready promise with
  // the given reason.
  KJ_IF_SOME(resolver, readyResolver) {
    resolver.reject(js, reason);
    readyResolver = kj::none;
  }
}

void WritableStreamSinkJsAdapter::BackpressureState::release(jsg::Lock& js) {
  // The backppressure has been released. Resolve the ready promise.
  KJ_IF_SOME(resolver, readyResolver) {
    resolver.resolve(js);
    readyResolver = kj::none;
  }
}

bool WritableStreamSinkJsAdapter::BackpressureState::isWaiting() const {
  return readyResolver != kj::none;
}

jsg::Promise<void> WritableStreamSinkJsAdapter::BackpressureState::getReady(jsg::Lock& js) {
  return ready.whenResolved(js);
}

jsg::MemoizedIdentity<jsg::Promise<void>>& WritableStreamSinkJsAdapter::BackpressureState::
    getReadyStable() {
  return readyWatcher;
}

WritableStreamSinkJsAdapter::BackpressureState::BackpressureState(
    jsg::Promise<void>::Resolver&& resolver,
    jsg::Promise<void>&& promise,
    jsg::MemoizedIdentity<jsg::Promise<void>>&& watcher)
    : readyResolver(kj::mv(resolver)),
      ready(kj::mv(promise)),
      readyWatcher(kj::mv(watcher)) {}

void WritableStreamSinkJsAdapter::maybeSignalBackpressure(jsg::Lock& js) {
  // We should only be signaling backpressure if we are in an active state.
  KJ_ASSERT_NONNULL(state.tryGet<IoOwn<Active>>());
  // Indicate that backpressure is being applied. If we are already in a
  // backpressure state (isWaiting() is true), this is a no-op.
  if (!backpressureState.isWaiting()) {
    // We signal backpressure by replacing the backpressure state.
    // This replaces the JS promises and resolvers with a new set.
    backpressureState = newBackpressureState(js);
  }
}

void WritableStreamSinkJsAdapter::maybeReleaseBackpressure(jsg::Lock& js) {
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    if (active->getDesiredSize() > 0) {
      // The desired size is now > 0, so we can release backpressure.
      // If backpressure is already released or aborted, this is a non-op.
      backpressureState.release(js);
    }
  }
}

WritableStreamSinkJsAdapter::BackpressureState WritableStreamSinkJsAdapter::newBackpressureState(
    jsg::Lock& js) {
  jsg::PromiseResolverPair<void> pair = js.newPromiseAndResolver<void>();
  pair.promise.markAsHandled(js);
  auto watcher = jsg::MemoizedIdentity<jsg::Promise<void>>(pair.promise.whenResolved(js));
  return BackpressureState(kj::mv(pair.resolver), kj::mv(pair.promise), kj::mv(watcher));
}

jsg::Promise<void> WritableStreamSinkJsAdapter::getReady(jsg::Lock& js) {
  return backpressureState.getReady(js);
}

jsg::MemoizedIdentity<jsg::Promise<void>>& WritableStreamSinkJsAdapter::getReadyStable() {
  return backpressureState.getReadyStable();
}

void WritableStreamSinkJsAdapter::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(
      backpressureState.readyResolver, backpressureState.ready, backpressureState.readyWatcher);
}

void WritableStreamSinkJsAdapter::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("backpressureState.readyResolver", backpressureState.readyResolver);
  tracker.trackField("backpressureState.ready", backpressureState.ready);
  tracker.trackField("backpressureState.readyWatcher", backpressureState.readyWatcher);
}

kj::Maybe<const WritableStreamSinkJsAdapter::Options&> WritableStreamSinkJsAdapter::getOptions() {
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    return active->options;
  } else {
    return kj::none;
  }
}

// ================================================================================================

struct WritableStreamSinkKjAdapter::Active {
  IoContext& ioContext;
  jsg::Ref<WritableStream> stream;
  jsg::Ref<WritableStreamDefaultWriter> writer;
  kj::Canceler canceler;

  // The contract of WritableStreamSink is that there can only be one
  // write in-flight at a time.
  bool writePending = false;

  bool closePending = false;
  kj::Maybe<kj::Exception> pendingAbort;

  // Prevent abort() from being called multiple times.
  bool aborted = false;

  Active(jsg::Lock& js, IoContext& ioContext, jsg::Ref<WritableStream> stream);
  KJ_DISALLOW_COPY_AND_MOVE(Active);
  ~Active() noexcept(false);

  void abort(kj::Exception reason);
};

namespace {
jsg::Ref<WritableStreamDefaultWriter> initWriter(jsg::Lock& js, jsg::Ref<WritableStream>& stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError, "WritableStream is locked.");
  return stream->getWriter(js);
}
}  // namespace

WritableStreamSinkKjAdapter::Active::Active(
    jsg::Lock& js, IoContext& ioContext, jsg::Ref<WritableStream> stream)
    : ioContext(ioContext),
      stream(kj::mv(stream)),
      writer(initWriter(js, this->stream)) {}

WritableStreamSinkKjAdapter::Active::~Active() noexcept(false) {
  abort(KJ_EXCEPTION(DISCONNECTED, "WritableStreamSinkKjAdapter is canceled."));
}

void WritableStreamSinkKjAdapter::Active::abort(kj::Exception reason) {
  if (aborted) return;
  aborted = true;
  canceler.cancel(kj::cp(reason));
  ioContext.addTask(ioContext.run([writable = kj::mv(stream), writer = kj::mv(writer),
                                      exception = kj::cp(reason)](jsg::Lock& js) mutable {
    auto& ioContext = IoContext::current();
    auto error = js.exceptionToJsValue(kj::mv(exception));
    auto promise = writer->abort(js, error.getHandle(js));
    return ioContext.awaitJs(js, kj::mv(promise));
  }));
}

WritableStreamSinkKjAdapter::WritableStreamSinkKjAdapter(
    jsg::Lock& js, IoContext& ioContext, jsg::Ref<WritableStream> stream)
    : state(kj::heap<Active>(js, ioContext, kj::mv(stream))),
      selfRef(kj::rc<WeakRef<WritableStreamSinkKjAdapter>>(
          kj::Badge<WritableStreamSinkKjAdapter>{}, *this)) {}

WritableStreamSinkKjAdapter::~WritableStreamSinkKjAdapter() noexcept(false) {
  selfRef->invalidate();
}

kj::Promise<void> WritableStreamSinkKjAdapter::write(kj::ArrayPtr<const byte> buffer) {
  auto pieces = kj::arr(buffer);
  co_await write(pieces);
}

kj::Promise<void> WritableStreamSinkKjAdapter::write(
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(active, kj::Own<Active>) {
      KJ_REQUIRE(!active->writePending, "Cannot have multiple concurrent writes.");
      KJ_IF_SOME(exception, active->pendingAbort) {
        auto exc = kj::cp(exception);
        state = kj::cp(exc);
        return kj::mv(exc);
      }
      if (active->closePending) {
        state.init<Closed>();
        KJ_FAIL_REQUIRE("Cannot write after close.");
      }
      active->writePending = true;

      return active->canceler
          .wrap(active->ioContext.run(
              [self = selfRef.addRef(), writer = active->writer.addRef(), pieces = pieces](
                  jsg::Lock& js) mutable -> kj::Promise<void> {
        size_t totalAmount = 0;
        for (auto piece: pieces) {
          totalAmount += piece.size();
        }
        if (totalAmount == 0) {
          return kj::READY_NOW;
        }

        // We collapse our pieces into a single ArrayBuffer for efficiency. The
        // WritableStream API has no concept of a vector write, so each write
        // would incur the overhead of a separate promise and microtask checkpoint.
        // By collapsing into a single write we reduce that overhead.
        auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, totalAmount);
        auto ptr = backing.asArrayPtr();
        for (auto piece: pieces) {
          ptr.first(piece.size()).copyFrom(piece);
          ptr = ptr.slice(piece.size());
        }
        jsg::BufferSource source(js, kj::mv(backing));

        auto ready = KJ_ASSERT_NONNULL(writer->isReady(js));
        auto promise = ready.then(
            js, [writer = writer.addRef(), source = kj::mv(source)](jsg::Lock& js) mutable {
          return writer->write(js, source.getHandle(js));
        });
        return IoContext::current().awaitJs(js, kj::mv(promise));
      })).then([self = selfRef.addRef()]() {
        self->runIfAlive([&](WritableStreamSinkKjAdapter& self) {
          KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
            active->writePending = false;
          }
        });
      }, [self = selfRef.addRef()](kj::Exception exception) {
        self->runIfAlive([&](WritableStreamSinkKjAdapter& self) {
          KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
            active->writePending = false;
            active->pendingAbort = kj::cp(exception);
          }
        });
        kj::throwFatalException(kj::mv(exception));
      });
    }
    KJ_CASE_ONEOF(_, Closed) {
      KJ_FAIL_REQUIRE("Cannot write after close.");
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      kj::throwFatalException(kj::cp(exc));
    }
  }
  KJ_UNREACHABLE;
}

kj::Promise<void> WritableStreamSinkKjAdapter::end() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(active, kj::Own<Active>) {
      KJ_REQUIRE(!active->writePending, "Cannot have multiple concurrent writes.");
      KJ_IF_SOME(exception, active->pendingAbort) {
        auto exc = kj::mv(exception);
        state = kj::cp(exc);
        return kj::mv(exc);
      }
      if (active->closePending) {
        state.init<Closed>();
        return kj::READY_NOW;
      }
      active->closePending = true;
      return active->canceler
          .wrap(active->ioContext.run(
              [self = selfRef.addRef(), writer = active->writer.addRef()](jsg::Lock& js) mutable {
        auto promise = writer->close(js);
        return IoContext::current().awaitJs(js, kj::mv(promise));
      })).catch_([self = selfRef.addRef()](kj::Exception exception) {
        self->runIfAlive([&](WritableStreamSinkKjAdapter& self) {
          KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
            active->pendingAbort = kj::cp(exception);
          }
        });
        kj::throwFatalException(kj::mv(exception));
      });
    }
    KJ_CASE_ONEOF(_, Closed) {
      return kj::READY_NOW;
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      return kj::cp(exc);
    }
  }
  return kj::READY_NOW;
}

kj::Promise<void> WritableStreamSinkKjAdapter::pumpFromImpl(ReadableStreamSource& input, bool end) {

  static constexpr size_t kMinRead = 8192;
  static constexpr size_t kMaxRead = 16384;
  kj::FixedArray<kj::byte, kMaxRead> buffer;
  // Let's make sure we're in the right state before we start.
  KJ_DASSERT(state.is<kj::Own<Active>>());

  while (true) {
    // Check our state before each iteration of the loop. This
    // is a bit redundant since the canceler should take care
    // of aborting the loop if we are canceled, but it's good
    // to be extra careful.
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(_, kj::Own<Active>) {
        // This is what we expect. Continue.
      }
      KJ_CASE_ONEOF(_, Closed) {
        input.cancel(KJ_EXCEPTION(DISCONNECTED, "WritableStreamSinkKjAdapter is closed."));
        co_return;
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        input.cancel(kj::cp(exception));
        kj::throwFatalException(kj::cp(exception));
      }
    }

    // Now perform the read.
    size_t bytesRead = 0;
    try {
      bytesRead = co_await input.tryRead(buffer.begin(), kMinRead, buffer.size());
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      // The read failed. We need to abort the write side and exit.
      // But why do we need to check the state again? Well, because it may
      // have changed while we were reading.
      KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
        // We can't actually change the state here because we are in a
        // promise that would destroy itself if we did. Instead we
        // set the pendingAbort field, which will be picked up the next
        // time we check the state.
        active->pendingAbort = kj::cp(exception);
      }
      kj::throwFatalException(kj::cp(exception));
    }

    // If the read returned fewer than kMinRead bytes, we treat that
    // as a signal that the source is done and we should close the
    // sink after writing the data we have, but only if "end" is true.
    bool closing = end && bytesRead < kMinRead;

    // We have to check our state again after the read since it may have changed.
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(active, kj::Own<Active>) {
        if (bytesRead > 0 || closing) {
          try {
            co_await active->ioContext.run(
                [&buffer, bytesRead, closing, writer = active->writer.addRef(),
                    self = selfRef.addRef()](jsg::Lock& js) mutable -> kj::Promise<void> {
              // Just a quick check, is our adapter still alive?
              if (!self->isValid()) {
                // We're not alive anymore, just exit. There's really nothing
                // we can do at this point.
                auto exception =
                    KJ_EXCEPTION(DISCONNECTED, "WritableStreamSinkKjAdapter is closed.");
                return kj::mv(exception);
              }
              auto& ioContext = IoContext::current();
              if (bytesRead > 0) {
                auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, bytesRead);
                auto ptr = backing.asArrayPtr();
                ptr.copyFrom(buffer.asPtr().first(bytesRead));
                jsg::BufferSource source(js, kj::mv(backing));

                auto ready = KJ_ASSERT_NONNULL(writer->isReady(js));
                auto promise = ready.then(
                    js, [writer = writer.addRef(), source = kj::mv(source)](jsg::Lock& js) mutable {
                  return writer->write(js, source.getHandle(js));
                });

                if (closing) {
                  return ioContext.awaitJs(js,
                      promise.then(js,
                          ioContext.addFunctor([writer = kj::mv(writer), self = kj::mv(self)](
                                                   jsg::Lock& js) mutable {
                    // We're done writing and we are closing.
                    self->runIfAlive([](WritableStreamSinkKjAdapter& self) {
                      KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
                        active->writePending = false;
                        active->closePending = true;
                      }
                    });
                    return writer->close(js);
                  })));
                } else {
                  return ioContext.awaitJs(js, kj::mv(promise));
                }
              } else {
                // If bytesRead is zero, and we made it here, it means
                // that closing is true. We need to close the writer.
                self->runIfAlive([](WritableStreamSinkKjAdapter& self) {
                  KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
                    active->writePending = false;
                    active->closePending = true;
                  }
                });
                return ioContext.awaitJs(js, writer->close(js));
              }
            });
          } catch (...) {
            auto exception = kj::getCaughtExceptionAsKj();
            // The write failed. We need to abort the read side and exit.
            // But why do we need to check the state again? Well, because it may
            // have changed while we were writing.
            KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
              // We can't actually change the state here because we are in a
              // promise that would destroy itself if we did. Instead we
              // set the pendingAbort field, which will be picked up the next
              // time we check the state.
              active->pendingAbort = kj::cp(exception);
            }
            input.cancel(kj::cp(exception));
            kj::throwFatalException(kj::mv(exception));
          }
          if (closing) co_return;
          // When we get here, we loop around again since we're still in the
          // while loop.
        } else {
          // We read zero bytes and we are not closing. This is a signal
          // that the stream is done but end is false, so we're done and
          // we just exit without doing anything further.
          co_return;
        }
      }
      KJ_CASE_ONEOF(_, Closed) {
        // Our state changed while we were reading, cancel the input and exit.
        input.cancel(KJ_EXCEPTION(DISCONNECTED, "WritableStreamSinkKjAdapter is closed."));
        co_return;
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        // Our state changed while we were reading, cancel the input and exit.
        input.cancel(kj::cp(exception));
        kj::throwFatalException(kj::mv(exception));
      }
    }
  }  // while (true)
  KJ_UNREACHABLE;
}

kj::Maybe<kj::Promise<DeferredProxy<void>>> WritableStreamSinkKjAdapter::tryPumpFrom(
    ReadableStreamSource& input, bool end) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(active, kj::Own<Active>) {
      KJ_REQUIRE(!active->writePending, "Cannot have multiple concurrent writes.");
      KJ_IF_SOME(exc, active->pendingAbort) {
        auto exception = kj::cp(exc);
        state = kj::cp(exc);
        return addNoopDeferredProxy(kj::mv(exception));
      }
      if (active->closePending) {
        state.init<Closed>();
        return addNoopDeferredProxy(kj::READY_NOW);
      }
      active->writePending = true;

      return addNoopDeferredProxy(active->canceler.wrap(pumpFromImpl(input, end))
                                      .then([self = selfRef.addRef()]() -> kj::Promise<void> {
        self->runIfAlive([](WritableStreamSinkKjAdapter& self) { self.state.init<Closed>(); });
        return kj::READY_NOW;
      }, [self = selfRef.addRef()](kj::Exception exception) -> kj::Promise<void> {
        self->runIfAlive([&](WritableStreamSinkKjAdapter& self) {
          KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
            active->abort(kj::cp(exception));
          }
          self.state = kj::cp(exception);
        });
        return kj::mv(exception);
      }));
    }
    KJ_CASE_ONEOF(_, Closed) {
      return kj::Promise<DeferredProxy<void>>(newNoopDeferredProxy());
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      return kj::Promise<DeferredProxy<void>>(kj::cp(exc));
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamSinkKjAdapter::abort(kj::Exception reason) {
  KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
    active->abort(kj::cp(reason));
  }
  state = kj::mv(reason);
}

}  // namespace workerd::api::streams
