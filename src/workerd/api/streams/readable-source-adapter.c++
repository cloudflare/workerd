#include "readable-source-adapter.h"

#include <workerd/util/checked-queue.h>

namespace workerd::api::streams {

namespace {
// Per the ReadableStream spec, when a read(buf) is performed on a BYOB reader,
// if the stream is already closed, we still need to return the allocated buffer
// back to the caller, but it must be in a zero-length view. This utility function
// does that. It takes the original allocation and wraps it into a new ArrayBuffer
// instance that is wrapped by a zero-length view of the same type as the original
// TypedArray we were given.
jsg::BufferSource transferToEmptyBuffer(jsg::Lock& js, jsg::BufferSource buffer) {
  KJ_DASSERT(!buffer.isDetached() && buffer.canDetach(js));
  auto backing = buffer.detach(js);
  backing.limit(0);
  auto buf = jsg::BufferSource(js, kj::mv(backing));
  KJ_DASSERT(buf.size() == 0);
  return kj::mv(buf);
}
}  // namespace

// The Active state maintains a queue of tasks, such as read or close operations. Each task
// contains a promise-returning function object and a fulfiller. When the first task is
// enqueued, the active state begins processing the queue asynchronously. Each function
// is invoked in order, its promise awaited, and the result passed to the fulfiller. The
// fulfiller notifies the code which enqueued the task that the task has completed. In
// this way, read and close operations are safely executed in serial, even if one operation
// is called before the previous completes. This mechanism satisfies KJ's restriction on
// concurrent operations on streams.
struct ReadableStreamSourceJsAdapter::Active {
  struct Task {
    kj::Function<kj::Promise<size_t>()> task;
    kj::Own<kj::PromiseFulfiller<size_t>> fulfiller;
    Task(kj::Function<kj::Promise<size_t>()> task, kj::Own<kj::PromiseFulfiller<size_t>> fulfiller)
        : task(kj::mv(task)),
          fulfiller(kj::mv(fulfiller)) {}
    KJ_DISALLOW_COPY_AND_MOVE(Task);
  };
  using TaskQueue = workerd::util::Queue<kj::Own<Task>>;

  kj::Own<ReadableStreamSource> source;
  kj::Canceler canceler;
  TaskQueue queue;
  bool canceled = false;
  bool running = false;
  bool closePending = false;
  kj::Maybe<kj::Exception> pendingCancel;

  Active(kj::Own<ReadableStreamSource> source): source(kj::mv(source)) {}
  KJ_DISALLOW_COPY_AND_MOVE(Active);
  ~Active() noexcept(false) {
    // When the Active is dropped, we cancel any remaining pending reads and
    // abort the sink.
    cancel(KJ_EXCEPTION(DISCONNECTED, "Writable stream is canceled or closed."));

    // Check invariants for safety.
    // 1. Our canceler should be empty because we canceled it.
    KJ_DASSERT(canceler.isEmpty());
    // 2. The write queue should be empty.
    KJ_DASSERT(queue.empty());
  }

  // Explicitly cancel all in-flight and pending tasks in the queue.
  // This is a non-op if cancel has already been called.
  void cancel(kj::Exception&& exception) {
    if (canceled) return;
    canceled = true;
    // 1. Cancel our in-flight "runLoop", if any.
    pendingCancel = kj::cp(exception);
    canceler.cancel(kj::cp(exception));
    // 2. Drop our queue of pending tasks.
    queue.drainTo(
        [&exception](kj::Own<Task>&& task) { task->fulfiller->reject(kj::cp(exception)); });
    // 3. Cancel and drop the source itself. We're done with it.
    if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
      source->cancel(kj::mv(exception));
    }
    auto dropped KJ_UNUSED = kj::mv(source);
  }

  kj::Promise<size_t> enqueue(kj::Function<kj::Promise<size_t>()> task) {
    KJ_DASSERT(!canceled, "cannot enqueue tasks on a canceled queue");
    auto paf = kj::newPromiseAndFulfiller<size_t>();
    queue.push(kj::heap<Task>(kj::mv(task), kj::mv(paf.fulfiller)));
    if (!running) {
      IoContext::current().addTask(canceler.wrap(run()));
    }
    return kj::mv(paf.promise);
  }

  kj::Promise<void> run() {
    KJ_DEFER(running = false);
    running = true;
    while (!queue.empty() && !canceled) {
      auto task = KJ_ASSERT_NONNULL(queue.pop());
      KJ_DEFER({
        if (task->fulfiller->isWaiting()) {
          KJ_IF_SOME(pending, pendingCancel) {
            task->fulfiller->reject(kj::mv(pending));
          } else {
            task->fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "Task was canceled."));
          }
        }
      });
      bool taskFailed = false;
      try {
        task->fulfiller->fulfill(co_await task->task());
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

ReadableStreamSourceJsAdapter::ReadableStreamSourceJsAdapter(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source)
    : state(ioContext.addObject(kj::heap<Active>(kj::mv(source)))),
      selfRef(kj::rc<WeakRef<ReadableStreamSourceJsAdapter>>(
          kj::Badge<ReadableStreamSourceJsAdapter>{}, *this)) {}

ReadableStreamSourceJsAdapter::~ReadableStreamSourceJsAdapter() noexcept(false) {
  selfRef->invalidate();
}

void ReadableStreamSourceJsAdapter::cancel(kj::Exception exception) {
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    active->cancel(kj::cp(exception));
  }
  state = kj::mv(exception);
}

void ReadableStreamSourceJsAdapter::cancel(jsg::Lock& js, const jsg::JsValue& reason) {
  cancel(js.exceptionToKj(reason));
}

void ReadableStreamSourceJsAdapter::shutdown(jsg::Lock& js) {
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    active->cancel(KJ_EXCEPTION(DISCONNECTED, "Stream was shut down."));
    state.init<Closed>();
  }
  // If we are are already closed or canceled, this is a no-op.
}

bool ReadableStreamSourceJsAdapter::isClosed() {
  return state.template is<Closed>();
}

kj::Maybe<const kj::Exception&> ReadableStreamSourceJsAdapter::isCanceled() {
  return state.tryGet<kj::Exception>();
}

jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> ReadableStreamSourceJsAdapter::read(
    jsg::Lock& js, ReadOptions options) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      // Deference the IoOwn once to get the active state.
      Active& active = *activeOwn;

      // If close is pending, we cannot accept any more reads.
      // Treat them as if the stream is closed.
      if (active.closePending) {
        return js.resolvedPromise(ReadResult{
          .buffer = transferToEmptyBuffer(js, kj::mv(options.buffer)),
          .done = true,
        });
      }

      // Ok, we are in a readable state, there are no pending closes.
      // Let's enqueue our read request.
      auto& ioContext = IoContext::current();

      auto buffer = kj::mv(options.buffer);
      auto elementSize = buffer.getElementSize();

      // The buffer size should always be a multiple of the element size and should
      // always be at least as large as minBytes. This should be handled for us by
      // the jsg::BufferSource, but just to be safe, we will double-check with a
      // debug assert here.
      KJ_DASSERT(buffer.size() % elementSize == 0);

      auto minBytes = kj::min(options.minBytes.orDefault(elementSize), buffer.size());
      // We want to be sure that minBytes is a multiple of the element size
      // of the buffer, otherwise we might never be able to satisfy the request
      // correcty. If the caller provided a minBytes, and it is not a multiple
      // of the element size, we will round it up to the next multiple.
      if (elementSize > 1) {
        minBytes = minBytes + (elementSize - (minBytes % elementSize)) % elementSize;
      }

      // Note: We do not enforce that the source must provide at least minBytes
      // if available here as that is part of the contract of the source itself.
      // We will simply pass minBytes along to the source and it is up to the
      // source to honor it. We do, however, enforce that the source must
      // never return more than the size of the buffer we provided.

      // We only pass a kj::ArrayPtr to the buffer into the read call, keeping
      // the actual buffer instance alive by attaching it to the JS promise
      // chain that follows the read in order to keep it alive.
      auto promise = active.enqueue(kj::coCapture(
          [&active, buffer = buffer.asArrayPtr(), minBytes]() mutable -> kj::Promise<size_t> {
        // TODO(soon): The underlying kj streams API now supports passing the
        // kj::ArrayPtr directly to the read call, but ReadableStreamSource has
        // not yet been updated to do so. When it is, we can update this read to
        // pass `buffer` directly rather than passing the begin() and size().
        co_return co_await active.source->tryRead(buffer.begin(), minBytes, buffer.size());
      }));
      return ioContext
          .awaitIo(js, kj::mv(promise),
              [buffer = kj::mv(buffer), self = selfRef.addRef()](
                  jsg::Lock& js, size_t bytesRead) mutable
              -> jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> {
        // If the bytesRead is 0, that indicates the stream is closed. We will
        // move the stream to a closed state and return the empty buffer.
        if (bytesRead == 0) {
          self->runIfAlive([](ReadableStreamSourceJsAdapter& self) {
            KJ_IF_SOME(active, self.state.tryGet<IoOwn<Active>>()) {
              active->closePending = true;
            }
          });
          return js.resolvedPromise(ReadResult{
            .buffer = transferToEmptyBuffer(js, kj::mv(buffer)),
            .done = true,
          });
        }
        KJ_DASSERT(bytesRead <= buffer.size());

        // If bytesRead is not a multiple of the element size, that indicates
        // that the source either read less than minBytes (and ended), or is
        // simply unable to satisfy the element size requirement. We cannot
        // provide a partial element to the caller, so reject the read.
        if (bytesRead % buffer.getElementSize() != 0) {
          return js.rejectedPromise<ReadResult>(
              js.typeError(kj::str("The underlying stream failed to provide a multiple of the "
                                   "target element size ",
                  buffer.getElementSize())));
        }

        auto backing = buffer.detach(js);
        backing.limit(bytesRead);
        return js.resolvedPromise(ReadResult{
          .buffer = jsg::BufferSource(js, kj::mv(backing)),
          .done = false,
        });
      })
          .catch_(js,
              [self = selfRef.addRef()](jsg::Lock& js,
                  jsg::Value exception) -> ReadableStreamSourceJsAdapter::ReadResult {
        // If an error occurred while reading, we need to transition the adapter
        // to the canceled state, but only if the adapter is still alive.
        auto error = jsg::JsValue(exception.getHandle(js));
        self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
        js.throwException(kj::mv(exception));
      });
    }
    KJ_CASE_ONEOF(closed, Closed) {
      // We are already in a closed state. This is a no-op, just return
      // an empty buffer.
      return js.resolvedPromise(ReadResult{
        .buffer = transferToEmptyBuffer(js, kj::mv(options.buffer)),
        .done = true,
      });
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      // Really should not have been called if errored but just in case,
      // return a rejected promise.
      return js.rejectedPromise<ReadResult>(js.exceptionToJs(kj::cp(exc)));
    }
  }
  KJ_UNREACHABLE;
}

// Transitions the adapter into the closing state. Once the read queue
// is empty, we will close the source and transition to the closed state.
jsg::Promise<void> ReadableStreamSourceJsAdapter::close(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      auto& ioContext = IoContext::current();
      auto& active = *activeOwn;

      if (active.closePending) {
        return js.rejectedPromise<void>(js.typeError("Close already pending, cannot close again."));
      }

      active.closePending = true;
      auto promise = active.enqueue([]() -> kj::Promise<size_t> { co_return 0; });

      return ioContext
          .awaitIo(js, kj::mv(promise), [self = selfRef.addRef()](jsg::Lock&, size_t) {
        self->runIfAlive([](ReadableStreamSourceJsAdapter& self) { self.state.init<Closed>(); });
      }).catch_(js, [self = selfRef.addRef()](jsg::Lock& js, jsg::Value&& exception) {
        // Likewise, while nothing should be waiting on the ready promise, we
        // should still reject it just in case.
        auto error = jsg::JsValue(exception.getHandle(js));
        self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
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

jsg::Promise<jsg::JsRef<jsg::JsString>> ReadableStreamSourceJsAdapter::readAllText(
    jsg::Lock& js, uint64_t limit) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      auto& ioContext = IoContext::current();
      auto& active = *activeOwn;

      if (active.closePending) {
        return js.rejectedPromise<jsg::JsRef<jsg::JsString>>(
            js.typeError("Close already pending, cannot read."));
      }
      active.closePending = true;

      struct Holder {
        kj::Maybe<kj::String> result;
      };
      auto holder = kj::heap<Holder>();

      auto promise = active.enqueue([&active, &holder = *holder, limit]() -> kj::Promise<size_t> {
        auto str = co_await active.source->readAllText(limit);
        size_t amount = str.size();
        holder.result = kj::mv(str);
        co_return amount;
      });

      return ioContext
          .awaitIo(js, kj::mv(promise),
              [self = selfRef.addRef(), holder = kj::mv(holder)](jsg::Lock& js, size_t amount) {
        self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.state.init<Closed>(); });
        KJ_IF_SOME(result, holder->result) {
          KJ_DASSERT(result.size() == amount);
          return jsg::JsRef(js, js.str(result));
        } else {
          return jsg::JsRef(js, js.str());
        }
      })
          .catch_(js,
              [self = selfRef.addRef()](
                  jsg::Lock& js, jsg::Value&& exception) -> jsg::JsRef<jsg::JsString> {
        // Likewise, while nothing should be waiting on the ready promise, we
        // should still reject it just in case.
        auto error = jsg::JsValue(exception.getHandle(js));
        self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
        js.throwException(kj::mv(exception));
      });
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      // Really should not have been called if errored but just in case,
      // return a rejected promise.
      return js.rejectedPromise<jsg::JsRef<jsg::JsString>>(js.exceptionToJs(kj::cp(exc)));
    }
    KJ_CASE_ONEOF(_, Closed) {
      // We are already in a closed state. This is a no-op. This really
      // should not have been called if closed but just in case, return
      // a resolved promise.
      return js.resolvedPromise(jsg::JsRef(js, js.str()));
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<jsg::BufferSource> ReadableStreamSourceJsAdapter::readAllBytes(
    jsg::Lock& js, uint64_t limit) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeOwn, IoOwn<Active>) {
      auto& ioContext = IoContext::current();
      auto& active = *activeOwn;

      if (active.closePending) {
        return js.rejectedPromise<jsg::BufferSource>(
            js.typeError("Close already pending, cannot read."));
      }
      active.closePending = true;

      struct Holder {
        kj::Maybe<kj::Array<kj::byte>> result;
      };
      auto holder = kj::heap<Holder>();

      auto promise = active.enqueue([&active, &holder = *holder, limit]() -> kj::Promise<size_t> {
        auto str = co_await active.source->readAllBytes(limit);
        size_t amount = str.size();
        holder.result = kj::mv(str);
        co_return amount;
      });

      return ioContext
          .awaitIo(js, kj::mv(promise),
              [self = selfRef.addRef(), holder = kj::mv(holder)](jsg::Lock& js, size_t amount) {
        self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.state.init<Closed>(); });
        KJ_IF_SOME(result, holder->result) {
          KJ_DASSERT(result.size() == amount);
          // We have to copy the data into the backing store because of the
          // v8 sandboxing rules.
          auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, amount);
          backing.asArrayPtr().copyFrom(result);
          return jsg::BufferSource(js, kj::mv(backing));
        } else {
          auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
          return jsg::BufferSource(js, kj::mv(backing));
        }
      })
          .catch_(js,
              [self = selfRef.addRef()](
                  jsg::Lock& js, jsg::Value&& exception) -> jsg::BufferSource {
        // Likewise, while nothing should be waiting on the ready promise, we
        // should still reject it just in case.
        auto error = jsg::JsValue(exception.getHandle(js));
        self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
        js.throwException(kj::mv(exception));
      });
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      // Really should not have been called if errored but just in case,
      // return a rejected promise.
      return js.rejectedPromise<jsg::BufferSource>(js.exceptionToJs(kj::cp(exc)));
    }
    KJ_CASE_ONEOF(_, Closed) {
      // We are already in a closed state. This is a no-op. This really
      // should not have been called if closed but just in case, return
      // a resolved promise.
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
      return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> ReadableStreamSourceJsAdapter::tryGetLength(StreamEncoding encoding) {
  KJ_IF_SOME(active, state.tryGet<IoOwn<Active>>()) {
    return active->source->tryGetLength(encoding);
  }
  return kj::none;
}

kj::Maybe<ReadableStreamSourceJsAdapter::Tee> ReadableStreamSourceJsAdapter::tryTee(
    jsg::Lock& js, uint64_t limit) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(activeRef, IoOwn<Active>) {
      auto& active = *activeRef;
      // If we are closing, or have pending tasks, we cannot tee.
      JSG_REQUIRE(!active.closePending && !active.running && active.queue.empty(), Error,
          "Cannot tee a stream that is closing or has pending reads.");
      KJ_IF_SOME(tee, active.source->tryTee(limit)) {
        auto& ioContext = IoContext::current();
        state.init<Closed>();
        return Tee{
          .branch1 =
              kj::heap<ReadableStreamSourceJsAdapter>(js, ioContext, kj::mv(tee.branches[0])),
          .branch2 =
              kj::heap<ReadableStreamSourceJsAdapter>(js, ioContext, kj::mv(tee.branches[1])),
        };
      }
      // Unable to tee.
      return kj::none;
    }
    KJ_CASE_ONEOF(_, Closed) {
      // We are already closed, cannot tee.
      return kj::none;
    }
    KJ_CASE_ONEOF(exc, kj::Exception) {
      js.throwException(js.exceptionToJs(kj::cp(exc)));
    }
  }
  KJ_UNREACHABLE;
}

// ===============================================================================================

struct ReadableStreamSourceKjAdapter::Active {
  IoContext& ioContext;
  jsg::Ref<ReadableStream> stream;
  jsg::Ref<ReadableStreamDefaultReader> reader;
  kj::Canceler canceler;

  bool pumping = false;

  struct Idle {};
  struct Readable {
    // Previously read but unconsumed bytes. We keep these around for the next read call.
    kj::Array<const kj::byte> data;
    kj::ArrayPtr<const kj::byte> view;
  };
  struct Reading {
    // The contract for ReadableStreamSource is that there can be only one read() in-flight
    // against the underlying stream at a time.
  };
  struct Done {
    // If a read returns fewer than the requested minBytes, that indicates the stream is done. We
    // make note of that here to prevent any further reads. We cannot transition to the closed
    // state in the promise chain of the read because the adapter will cancel the read promise
    // itself once Active is destroyed, and that would be a bad thing.
  };
  struct Canceling {
    kj::Exception exception;
  };
  struct Canceled {
    kj::Exception exception;
  };

  kj::OneOf<Idle, Readable, Reading, Done, Canceling, Canceled> state = Idle();

  Active(jsg::Lock& js, IoContext& ioContext, jsg::Ref<ReadableStream> stream);
  KJ_DISALLOW_COPY_AND_MOVE(Active);
  ~Active() noexcept(false);

  void cancel(kj::Exception reason);
};

// The ReadContext struct holds all the state needed to perform a read,
// including the JS objects that need to be kept alive during the
// read operation, the buffer we are reading into, and the total
// number of bytes read so far. This must be kept alive until the
// read is fully complete and returned back to the adapter when
// the read is complete.
//
// Ownership of the ReadContext is passed into the isolate lock and
// held by JS promise continuations, so it must not contain any
// kj I/O objects or references without an IoOwn wrapper.
struct ReadableStreamSourceKjAdapter::ReadContext {
  jsg::Ref<ReadableStream> stream;
  jsg::Ref<ReadableStreamDefaultReader> reader;
  kj::ArrayPtr<kj::byte> buffer;
  size_t totalRead = 0;
  size_t minBytes = 0;
  kj::Maybe<Active::Readable> maybeLeftOver;
  // We keep a weak reference to the adapter itself so we can track
  // whether it is still alive while we are in a JS promise chain.
  // If the adapter is gone, or transitions to a closed or canceled
  // state we will abandon the read.
  kj::Rc<WeakRef<ReadableStreamSourceKjAdapter>> adapterRef;
};

namespace {
constexpr size_t kMinRemainingForAdditionalRead = 512;

jsg::Ref<ReadableStreamDefaultReader> initReader(jsg::Lock& js, jsg::Ref<ReadableStream>& stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError, "ReadableStream is locked.");
  JSG_REQUIRE(!stream->isDisturbed(), TypeError, "ReadableStream is disturbed.");
  auto reader = stream->getReader(js, kj::none);
  return kj::mv(KJ_ASSERT_NONNULL(reader.tryGet<jsg::Ref<ReadableStreamDefaultReader>>()));
}
}  // namespace

ReadableStreamSourceKjAdapter::Active::Active(
    jsg::Lock& js, IoContext& ioContext, jsg::Ref<ReadableStream> stream)
    : ioContext(ioContext),
      stream(kj::mv(stream)),
      reader(initReader(js, this->stream)) {}

ReadableStreamSourceKjAdapter::Active::~Active() noexcept(false) {
  cancel(KJ_EXCEPTION(DISCONNECTED, "ReadableStreamSourceKjAdapter is canceled."));
}

void ReadableStreamSourceKjAdapter::Active::cancel(kj::Exception reason) {
  if (state.template is<Canceled>()) {
    return;
  }
  bool wasDone = state.template is<Done>();
  state.init<Canceled>(kj::cp(reason));
  canceler.cancel(kj::cp(reason));
  if (!wasDone) {
    // If the previous read indicated that it was the last read, then
    // the reader will have already been dropped. We do not need to
    // cancel it here.
    ioContext.addTask(ioContext.run([readable = kj::mv(stream), reader = kj::mv(reader),
                                        exception = kj::mv(reason)](jsg::Lock& js) mutable {
      auto& ioContext = IoContext::current();
      auto error = js.exceptionToJsValue(kj::mv(exception));
      auto promise = reader->cancel(js, error.getHandle(js));
      return ioContext.awaitJs(js, kj::mv(promise));
    }));
  }
}

ReadableStreamSourceKjAdapter::ReadableStreamSourceKjAdapter(
    jsg::Lock& js, IoContext& ioContext, jsg::Ref<ReadableStream> stream, Options options)
    : state(kj::heap<Active>(js, ioContext, kj::mv(stream))),
      options(options),
      selfRef(kj::rc<WeakRef<ReadableStreamSourceKjAdapter>>(
          kj::Badge<ReadableStreamSourceKjAdapter>{}, *this)) {}

ReadableStreamSourceKjAdapter::~ReadableStreamSourceKjAdapter() noexcept(false) {
  selfRef->invalidate();
}

jsg::Promise<kj::Own<ReadableStreamSourceKjAdapter::ReadContext>> ReadableStreamSourceKjAdapter::
    readInternal(jsg::Lock& js, kj::Own<ReadContext> context, MinReadPolicy minReadPolicy) {
  using ReadInternalResult = kj::Maybe<kj::OneOf<kj::String, jsg::BufferSource>>;

  auto& ioContext = IoContext::current();
  // Pay close attention to the lambda captures here. There are no raw references
  // captured! The adapter itself may be destroyed or closed while we are in the
  // promise chain below, so we have to be careful to only hold weak references
  // and pass ownership of the context along the promise chain.
  //
  // The other important thing here is to remember that everything in this function
  // is running within the isolate lock. The idea is to keep the entire read of the
  // underlying stream entirely within the lock so that we don't have to bounce
  // in and out of the isolate lock multiple times. We only return to the kj world
  // once the entire read is complete.
  //
  // Note the uses of addFunctor below. This is important because it ensures
  // that the promise continuations are run within the correct IoContext.
  return context->reader->read(js)
      .then(js,
          ioContext.addFunctor([reader = context->reader.addRef()](jsg::Lock& js,
                                   ReadResult result) mutable -> jsg::Promise<ReadInternalResult> {
    if (result.done) {
      // Stream is ended. Return kj::none indicate completion.
      return js.resolvedPromise<ReadInternalResult>(kj::none);
    }
    KJ_IF_SOME(value, result.value) {
      auto jsval = jsg::JsValue(value.getHandle(js));
      // Ok, we have some data. Let's make sure it is bytes.
      // We accept either an ArrayBuffer, ArrayBufferView, or string.
      if (jsval.isArrayBuffer() || jsval.isArrayBufferView()) {
        jsg::BufferSource source(js, jsval);
        return js.resolvedPromise<ReadInternalResult>(
            kj::Maybe(jsg::BufferSource(js, source.detach(js))));
      } else if (jsval.isString()) {
        return js.resolvedPromise<ReadInternalResult>(kj::Maybe(jsval.toString(js)));
      } else {
        // Oooo, invalid type. We cannot handle this and must treat
        // this as a fatal error. We will cancel the stream and
        // return an error.
        auto error = js.typeError("ReadableStream provided a non-bytes value. Only ArrayBuffer, "
                                  "ArrayBufferView, or string are supported.");
        reader->cancel(js, error);
        return js.rejectedPromise<ReadInternalResult>(error);
      }
    } else {
      // Done is false, but value is null/undefined? That's odd.
      // Treat is as the stream being closed.
      return js.resolvedPromise<ReadInternalResult>(kj::none);
    }
  }),
          ioContext.addFunctor([](jsg::Lock& js, jsg::Value exception) {
    // In this case, the reader should already be in an errored state
    // since it it the read that failed. Just propagate the error.
    return js.rejectedPromise<ReadInternalResult>(kj::mv(exception));
  }))
      .then(js,
          ioContext.addFunctor([context = kj::mv(context), minReadPolicy](
                                   jsg::Lock& js, ReadInternalResult maybeResult) mutable {
    KJ_IF_SOME(result, maybeResult) {
      kj::Array<const kj::byte> data = ([&] {
        KJ_SWITCH_ONEOF(result) {
          KJ_CASE_ONEOF(str, kj::String) {
            return str.asBytes().attach(kj::mv(str));
          }
          KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
            // We have to copy the data out of the buffer source
            // because of the v8 sandboxing rules.
            return kj::heapArray<kj::byte>(buffer.asArrayPtr());
          }
        }
        KJ_UNREACHABLE;
      })();
      // Ok, we have some data. Copy as much as we can into our destination
      if (data.size() == context->buffer.size()) {
        // We can fit it all! That's good because it makes things simpler.
        context->buffer.copyFrom(data);
        context->totalRead += data.size();
        context->buffer = context->buffer.slice(data.size());
        KJ_DASSERT(context->buffer.size() == 0);
        KJ_DASSERT(context->totalRead >= context->minBytes);
        // Our read is complete.
        return js.resolvedPromise(kj::mv(context));
      } else if (data.size() < context->buffer.size()) {
        // We can fit all the data we received, but we may still have
        // more room left in our destination buffer to fill and a
        // minRead requirement to satisfy. Let's copy then check.
        context->buffer.first(data.size()).copyFrom(data);
        context->totalRead += data.size();
        context->buffer = context->buffer.slice(data.size());

        // Also, we should still have some space left in our destination buffer.
        KJ_DASSERT(context->buffer.size() > 0);

        // If we have satisfied the minimum read requirement and either
        // (a) the minReadPolicy is IMMEDIATE or (b) there are fewer
        // than 512 bytes left in the buffer, we will just return what we
        // have. The idea here is that while we could just return what we have
        // and let the caller call read again, that would be inefficient if
        // the caller has a large buffer and is trying to read a lot of data.
        // Instead of returning early with a minimally filled buffer, let's
        // try to fill it up a bit more before returning. The 512 byte limit
        // is somewhat arbitrary. The risk, of course, is that the next read
        // will return too much data to fit into the buffer, which will then
        // have to be stashed away as left over data. There's also a risk that
        // the stream is slow and we end up with more latency waiting for
        // the next chunk of data to arrive. In practice, this seems unlikely
        // to be a problem. The IMMEDIATE policy is useful in the latter case,
        // when the caller wants to get whatever data is available as soon
        // as possible, even if it is just a small amount. The downside of the
        // IMMEDIATE policy is that it can lead to a lot of small reads that
        // are expensive because they have to grab the isolate lock each time.
        if (context->totalRead >= context->minBytes &&
            (minReadPolicy == MinReadPolicy::IMMEDIATE ||
                context->buffer.size() < kMinRemainingForAdditionalRead)) {
          // We have satisfied the minimum read requirement.
          KJ_DASSERT(context->totalRead >= context->minBytes);
          // Our read is complete.
          return js.resolvedPromise(kj::mv(context));
        }

        // We still have not satisfied the minimum read requirement or we are
        // trying to fill up a larger buffer. We will need to read more. Let's
        // call readInternal again to get the next chunk of data. Keep in mind
        // that this is not a true recursive call because readInternal returns
        // a jsg::Promise. We're just chaining the promises together here.
        bool continueReading = context->adapterRef->isValid();
        context->adapterRef->runIfAlive([&](ReadableStreamSourceKjAdapter& adapter) {
          continueReading = adapter.state.is<kj::Own<ReadableStreamSourceKjAdapter::Active>>();
        });
        if (!continueReading) {
          // The adapter is no longer valid, or is no longer active.
          // We have to abandon the read.
          return js.resolvedPromise(kj::mv(context));
        }

        // Ok, still active, let's continue reading.
        return readInternal(js, kj::mv(context), minReadPolicy);
      }

      // We received more data than we can fit into our destination
      // buffer. Copy what we can and stash the rest away as left
      // over data for the next read.
      context->buffer.copyFrom(data.first(context->buffer.size()));
      context->totalRead += context->buffer.size();
      auto view = data.slice(context->buffer.size());
      context->maybeLeftOver = Active::Readable{
        .data = kj::mv(data),
        .view = view,
      };
      KJ_DASSERT(context->totalRead >= context->minBytes);
      // Our read is complete.
      return js.resolvedPromise(kj::mv(context));
    } else {
      // No result, stream is done. We'll return what we've read so far,
      // even if it is less than the minBytes requirements.
      return js.resolvedPromise(kj::mv(context));
    }
  }));
}

// We separate out the actual read implementation so that it can be used by
// both tryRead and the pumpToImpl implementation.
kj::Promise<size_t> ReadableStreamSourceKjAdapter::tryReadImpl(
    Active& active, kj::ArrayPtr<kj::byte> dest, size_t minBytes) {

  KJ_IF_SOME(readable, active.state.tryGet<Active::Readable>()) {
    // We have some data left over from a previous read. Use that first.

    // If we have enough left over to fully satisfy this read,
    // Use it, then update our left over view.
    if (readable.view.size() >= dest.size()) {
      dest.copyFrom(readable.view.first(dest.size()));
      readable.view = readable.view.slice(dest.size());
      if (readable.view.size() == 0) {
        // We used up all our left over data. We can transition to the idle state.
        active.state.init<Active::Idle>();
      }
      // Otherwise we still have some left over data. That
      // is ok, we will keep it around for the next read.
      // We intentionally do not transition to the idle state
      // here because we want to keep the left over data for
      // the next read.
      return dest.size();
    }

    // Otherwise, consume what we do have left over.
    auto size = readable.view.size();
    dest.first(size).copyFrom(readable.view);
    dest = dest.slice(size);

    active.state.init<Active::Idle>();

    // Did we at least satisfy the minimum bytes?
    if (size >= minBytes) {
      // Awesome, we are technically done with this read.
      // While we might actually have more room in our buffer, and the
      // minReadyPolicy might be OPPORTUNISTIC, we will not try to
      // read more from the stream right now so that we can avoid having
      // to grab the isolate lock for this read. Instead, let's return
      // what we have and let the caller call read again if/when they want.
      return size;
    }
  }

  // If we got here, we still have not satisfied the minimum bytes,
  // so we will continue on to read more from the stream. But, we
  // also should not have any more data left over. Let's verify.
  KJ_ASSERT(active.state.is<Active::Idle>());
  active.state.init<Active::Reading>();

  // Our read context holds all the state needed to perform the read.
  // Ownership of the context is passed into the read operation and
  // returned back to us when the read is complete.
  auto context = kj::heap<ReadContext>({
    .stream = active.stream.addRef(),
    .reader = active.reader.addRef(),
    .buffer = dest,
    .totalRead = 0,
    .minBytes = minBytes,
    .adapterRef = selfRef.addRef(),
  });

  return active.canceler
      .wrap(
          // Warning: Do *not* capture "active" in this lambda! It may be destroyed
          // while we are in the promise chain. Instead, we capture a weak
          // reference to the adapter itself and check that we are still alive
          // and active before trying to update any state.
          active.ioContext.run([context = kj::mv(context), self = selfRef.addRef(),
                                   minReadPolicy = options.minReadPolicy](
                                   jsg::Lock& js) mutable -> kj::Promise<size_t> {
    auto& ioContext = IoContext::current();

    // Perform the actual read.
    return ioContext.awaitJs(js, readInternal(js, kj::mv(context), minReadPolicy))
        .then([self = kj::mv(self)](kj::Own<ReadContext> context) mutable -> kj::Promise<size_t> {
      // By the time we get here, it is possible that the adapter has been
      // destroyed. If that's the case, it's okay, that's what our weak ref
      // is here for. We will only try to update our state if we are still
      // alive and active.

      self->runIfAlive([&](ReadableStreamSourceKjAdapter& self) {
        // Ok, we're still alive! Yay! But, let's check to make sure we didn't
        // change state while we were reading.
        KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
          // Ok, we're still active. Let's see if we have any left over data
          // that we need to stash away for the next read.
          KJ_IF_SOME(leftOver, context->maybeLeftOver) {
            // We have some left over data. Stash it away for the next read.
            active->state = kj::mv(leftOver);
            // In this branch, we must have filled the entire destination
            // buffer and satisfied the minimum read requirement or else
            // we wouldn't have any left over data. Let's just assert that
            // invariant just in case.
            KJ_DASSERT(context->totalRead >= context->minBytes);
          } else if (context->totalRead < context->minBytes) {
            // We returned fewer than the minimum bytes requested. This is our
            // signal that we're done.
            active->state.init<Active::Done>();
            // We cannot change the state to Closed here because we are still
            // inside the kj::Promise chain wrapped by the canceler. If we
            // change the state to Closed, the Active would be destroyed, causing
            // this promise chain to be canceled.
            auto droppedReader KJ_UNUSED = kj::mv(active->reader);
            auto droppedStream KJ_UNUSED = kj::mv(active->stream);
            // In this branch, we should not have any left over data.
            // Let's assert that invariant just in case.
            KJ_DASSERT(context->maybeLeftOver == kj::none);
          } else {
            // Our read is complete. Return to the idle state and we're done.
            active->state.init<Active::Idle>();

            // In this branch, we must have satisfied the minimum read
            // requirement. Let's just assert that invariant just in case.
            KJ_DASSERT(context->totalRead >= context->minBytes);
            // We should not have any left over data.
            KJ_DASSERT(context->maybeLeftOver == kj::none);
          }
        } else {
          // We were closed or canceled while we were reading. Doh!
          // That's ok, there's nothing more we can or need to do
          // here. Just fall-through to the return below.
        }
      });
      return context->totalRead;
    });
  })).catch_([self = selfRef.addRef()](kj::Exception exception) -> kj::Promise<size_t> {
    self->runIfAlive([&](ReadableStreamSourceKjAdapter& self) {
      KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
        active->state.init<Active::Canceling>(Active::Canceling{
          .exception = kj::cp(exception),
        });
      }
    });
    return kj::mv(exception);
  });
}

kj::Promise<size_t> ReadableStreamSourceKjAdapter::tryRead(
    void* buffer, size_t minBytes, size_t maxBytes) {

  if (maxBytes == 0) {
    // Nothing to read. This is a no-op.
    return static_cast<size_t>(0);
  }

  // Clamp the minBytes to [1, maxBytes].
  minBytes = kj::min(maxBytes, kj::max(minBytes, 1UL));

  KJ_DASSERT(
      minBytes >= 1 && minBytes <= maxBytes, "minBytes must be less than or equal to maxBytes.");

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(active, kj::Own<Active>) {
      KJ_SWITCH_ONEOF(active->state) {
        KJ_CASE_ONEOF(_, Active::Reading) {
          KJ_FAIL_REQUIRE("Cannot have multiple concurrent reads.");
        }
        KJ_CASE_ONEOF(_, Active::Done) {
          // The previous read indicated that it was the last read by returning
          // less than the minimum bytes requested. We have to treat this as
          // the stream being closed.
          state.init<Closed>();
          return static_cast<size_t>(0);
        }
        KJ_CASE_ONEOF(canceling, Active::Canceling) {
          auto exception = kj::mv(canceling.exception);
          state = kj::cp(exception);
          return kj::mv(exception);
        }
        KJ_CASE_ONEOF(canceled, Active::Canceled) {
          auto exception = kj::cp(canceled.exception);
          state = kj::cp(exception);
          return kj::mv(exception);
        }
        KJ_CASE_ONEOF(r, Active::Readable) {
          // There is some data left over from a previous read.
          kj::ArrayPtr dest(static_cast<kj::byte*>(buffer), maxBytes);
          return tryReadImpl(*active, dest, minBytes);
        }
        KJ_CASE_ONEOF(_, Active::Idle) {
          // There are no pending reads and no left over data.
          kj::ArrayPtr dest(static_cast<kj::byte*>(buffer), maxBytes);
          return tryReadImpl(*active, dest, minBytes);
        }
      }
    }
    KJ_CASE_ONEOF(_, Closed) {
      return static_cast<size_t>(0);
    }
    KJ_CASE_ONEOF(exception, kj::Exception) {
      return kj::cp(exception);
    }
  }

  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> ReadableStreamSourceKjAdapter::tryGetLength(StreamEncoding encoding) {
  KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
    if (active->state.is<Active::Done>()) {
      // If the previous read indicated that it was the last, then
      // let's just transition to the closed state now and return kj::none.
      state.init<Closed>();
      return kj::none;
    }
    KJ_IF_SOME(canceling, active->state.tryGet<Active::Canceling>()) {
      auto exception = kj::mv(canceling.exception);
      state = kj::mv(exception);
      return kj::none;
    }
    return active->stream->tryGetLength(encoding);
  }

  // The stream is either closed or errored.
  return kj::none;
}

void ReadableStreamSourceKjAdapter::cancel(kj::Exception reason) {
  KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
    active->cancel(kj::cp(reason));
  }
  state = kj::mv(reason);
}

kj::Promise<void> ReadableStreamSourceKjAdapter::pumpToImpl(WritableStreamSink& output, bool end) {
  static constexpr size_t kMinRead = 8192;
  static constexpr size_t kMaxRead = 16384;
  kj::FixedArray<kj::byte, kMaxRead> buffer;
  // Let's make sure we're in the right state before we start.
  KJ_DASSERT(state.is<kj::Own<Active>>());
  bool writeFailed = false;

  while (true) {
    // Check our state before each iteration of the loop. This is a bit redundant since the
    // canceler should take care of aborting the loop if we are canceled, but it's good to
    // be extra careful. If this proves to be a performance problem, we can wrap the
    // KJ_SWITCH_ONEOF in an if KJ_DEBUG define so that the additional checks are only done in
    // debug builds.
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(active, kj::Own<Active>) {
        try {
          // Read at least 8192 bytes up to the size of our buffer.
          // Keep in mind that tryRead() requires acquiring the isolate lock
          // on each call, so we want to try to read a decent amount each time
          // to avoid excessive lock latency. But, we also don't want to read
          // too much and end up with too much memory pressure or lock latency.
          // The values here are somewhat arbitrary, but seem reasonable.
          auto bytesRead = co_await tryReadImpl(*active, buffer, kMinRead);

          // WARNING: do not access 'active' after this point because the stream
          // may have been closed or canceled while we were awaiting the read.
          // Nothing below depends on active, so this is currently safe, but we
          // will have to be careful. The next iteration of the loop will check
          // the state again.

          // Only write if we actually read something.
          if (bytesRead > 0) {
            KJ_ON_SCOPE_FAILURE(writeFailed = true);
            co_await output.write(buffer.asPtr().slice(0, bytesRead));
          }

          if (bytesRead < kMinRead) {
            KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
              active->state.init<Active::Done>();
              // We cannot change the state to Closed here because we are still
              // inside the kj::Promise chain wrapped by the canceler. If we
              // change the state to Closed, the Active would be destroyed, causing
              // this promise chain to be canceled. Instead, we will set a flag to
              // be checked on the next read and treat it as closed then.
            }

            // The source indicated that this was the last read by returning
            // less than the minimum bytes requested.
            if (end) {
              KJ_ON_SCOPE_FAILURE(writeFailed = true);
              co_await output.end();
            }
            co_return;
          }
        } catch (...) {
          auto exception = kj::getCaughtExceptionAsKj();
          KJ_IF_SOME(active, state.tryGet<kj::Own<Active>>()) {
            active->state.init<Active::Canceling>(Active::Canceling{
              .exception = kj::cp(exception),
            });
          }
          if (!writeFailed) {
            output.abort(kj::cp(exception));
          }
          // throw ok because we're in a coroutine
          kj::throwFatalException(kj::mv(exception));
        }
      }
      KJ_CASE_ONEOF(_, Closed) {
        co_return;
      }
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::cp(exception));
      }
    }
  }
}

kj::Promise<DeferredProxy<void>> ReadableStreamSourceKjAdapter::pumpTo(
    WritableStreamSink& output, bool end) {
  // The pumpTo operation continually reads from the stream and writes
  // to the output until the stream is closed or an error occurs. While
  // pumping, the adapter is considered active but tryRead() calls will
  // be rejected. Once pumping is complete, the adapter will be closed.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(active, kj::Own<Active>) {
      KJ_REQUIRE(!active->state.is<Active::Reading>() && !active->pumping,
          "Cannot have multiple concurrent reads.");
      active->pumping = true;

      if (active->state.is<Active::Done>()) {
        // The previous read indicated that it was the last read by returning
        // less than the minimum bytes requested. We have to treat this as
        // the stream being closed.
        state.init<Closed>();
        return newNoopDeferredProxy();
      }

      KJ_IF_SOME(canceling, active->state.tryGet<Active::Canceling>()) {
        auto exception = kj::mv(canceling.exception);
        state = kj::cp(exception);
        return kj::Promise<DeferredProxy<void>>(kj::mv(exception));
      }

      // Notice that we are wrapping the promise returned by pumpToImpl()
      // with the canceler. This means that if the adapter is canceled while
      // pumping, or the adapter is dropped, the pump will be aborted.
      // After wrapping the promise, we add continuations to transition the
      // adapter to the closed or errored state as appropriate. It is important
      // to do this after wrapping since changing the state will cause the
      // Active to be destroyed, triggering the canceler to cancel the wrapped
      // promise chain if we haven't already exited it.
      return addNoopDeferredProxy(active->canceler.wrap(pumpToImpl(output, end))
                                      .then([self = selfRef.addRef()]() -> kj::Promise<void> {
        self->runIfAlive([](ReadableStreamSourceKjAdapter& self) {
          // At this point, pumping should have completed successfully.
          self.state.init<Closed>();
        });
        return kj::READY_NOW;
      }, [self = selfRef.addRef()](kj::Exception exception) -> kj::Promise<void> {
        self->runIfAlive([&](ReadableStreamSourceKjAdapter& self) {
          KJ_IF_SOME(active, self.state.tryGet<kj::Own<Active>>()) {
            active->cancel(kj::cp(exception));
          }
          self.state = kj::cp(exception);
        });
        return kj::mv(exception);
      }));
    }
    KJ_CASE_ONEOF(_, Closed) {
      // Already closed, nothing to do.
      return newNoopDeferredProxy();
    }
    KJ_CASE_ONEOF(exception, kj::Exception) {
      return kj::Promise<DeferredProxy<void>>(kj::cp(exception));
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api::streams
