#include "readable-source-adapter.h"

#include "writable-sink.h"

#include <workerd/util/checked-queue.h>

#include <bit>

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

  kj::Own<ReadableSource> source;
  kj::Canceler canceler;
  TaskQueue queue;
  bool canceled = false;
  bool running = false;
  bool closePending = false;
  kj::Maybe<kj::Exception> pendingCancel;

  Active(kj::Own<ReadableSource> source): source(kj::mv(source)) {}
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
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableSource> source)
    : selfRef(kj::rc<WeakRef<ReadableStreamSourceJsAdapter>>(
          kj::Badge<ReadableStreamSourceJsAdapter>{}, *this)) {
  state.transitionTo<Open>(ioContext.addObject(kj::heap<Active>(kj::mv(source))));
}

ReadableStreamSourceJsAdapter::~ReadableStreamSourceJsAdapter() noexcept(false) {
  selfRef->invalidate();
}

void ReadableStreamSourceJsAdapter::cancel(kj::Exception exception) {
  KJ_IF_SOME(open, state.tryGetActive()) {
    open.active->cancel(kj::cp(exception));
  }
  state.forceTransitionTo<kj::Exception>(kj::mv(exception));
}

void ReadableStreamSourceJsAdapter::cancel(jsg::Lock& js, const jsg::JsValue& reason) {
  cancel(js.exceptionToKj(reason));
}

void ReadableStreamSourceJsAdapter::shutdown(jsg::Lock& js) {
  KJ_IF_SOME(open, state.tryGetActive()) {
    open.active->cancel(KJ_EXCEPTION(DISCONNECTED, "Stream was shut down."));
    state.transitionTo<Closed>();
  }
  // If we are are already closed or canceled, this is a no-op.
}

bool ReadableStreamSourceJsAdapter::isClosed() {
  return state.is<Closed>();
}

kj::Maybe<const kj::Exception&> ReadableStreamSourceJsAdapter::isCanceled() {
  return state.tryGetError();
}

jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> ReadableStreamSourceJsAdapter::read(
    jsg::Lock& js, ReadOptions options) {
  KJ_IF_SOME(exception, state.tryGetError()) {
    // Really should not have been called if errored but just in case,
    // return a rejected promise.
    return js.rejectedPromise<ReadResult>(js.exceptionToJs(kj::cp(exception)));
  }

  if (state.is<Closed>()) {
    // We are already in a closed state. This is a no-op, just return
    // an empty buffer.
    return js.resolvedPromise(ReadResult{
      .buffer = transferToEmptyBuffer(js, kj::mv(options.buffer)),
      .done = true,
    });
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    // Deference the IoOwn once to get the active state.
    Active& active = *open.active;

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
      co_return co_await active.source->read(buffer, minBytes);
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
          KJ_IF_SOME(open, self.state.tryGetActive()) {
            open.active->closePending = true;
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
            [self = selfRef.addRef()](
                jsg::Lock& js, jsg::Value exception) -> ReadableStreamSourceJsAdapter::ReadResult {
      // If an error occurred while reading, we need to transition the adapter
      // to the canceled state, but only if the adapter is still alive.
      auto error = jsg::JsValue(exception.getHandle(js));
      self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
      js.throwException(kj::mv(exception));
    });
  }

  KJ_UNREACHABLE;
}

// Transitions the adapter into the closing state. Once the read queue
// is empty, we will close the source and transition to the closed state.
jsg::Promise<void> ReadableStreamSourceJsAdapter::close(jsg::Lock& js) {
  KJ_IF_SOME(exception, state.tryGetError()) {
    // Really should not have been called if errored but just in case,
    // return a rejected promise.
    return js.rejectedPromise<void>(js.exceptionToJs(kj::cp(exception)));
  }

  if (state.is<Closed>()) {
    // We are already in a closed state. This is a no-op. This really
    // should not have been called if closed but just in case, return
    // a resolved promise.
    return js.resolvedPromise();
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& ioContext = IoContext::current();
    auto& active = *open.active;

    if (active.closePending) {
      return js.rejectedPromise<void>(js.typeError("Close already pending, cannot close again."));
    }

    active.closePending = true;
    auto promise = active.enqueue([]() -> kj::Promise<size_t> { co_return 0; });

    return ioContext
        .awaitIo(js, kj::mv(promise), [self = selfRef.addRef()](jsg::Lock&, size_t) {
      self->runIfAlive(
          [](ReadableStreamSourceJsAdapter& self) { self.state.transitionTo<Closed>(); });
    }).catch_(js, [self = selfRef.addRef()](jsg::Lock& js, jsg::Value&& exception) {
      // Likewise, while nothing should be waiting on the ready promise, we
      // should still reject it just in case.
      auto error = jsg::JsValue(exception.getHandle(js));
      self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
      js.throwException(kj::mv(exception));
    });
  }

  KJ_UNREACHABLE;
}

jsg::Promise<jsg::JsRef<jsg::JsString>> ReadableStreamSourceJsAdapter::readAllText(
    jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(exception, state.tryGetError()) {
    // Really should not have been called if errored but just in case,
    // return a rejected promise.
    return js.rejectedPromise<jsg::JsRef<jsg::JsString>>(js.exceptionToJs(kj::cp(exception)));
  }

  if (state.is<Closed>()) {
    // We are already in a closed state. This is a no-op. This really
    // should not have been called if closed but just in case, return
    // a resolved promise.
    return js.resolvedPromise(jsg::JsRef(js, js.str()));
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& ioContext = IoContext::current();
    auto& active = *open.active;

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
      self->runIfAlive(
          [&](ReadableStreamSourceJsAdapter& self) { self.state.transitionTo<Closed>(); });
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

  KJ_UNREACHABLE;
}

jsg::Promise<jsg::BufferSource> ReadableStreamSourceJsAdapter::readAllBytes(
    jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(exception, state.tryGetError()) {
    // Really should not have been called if errored but just in case,
    // return a rejected promise.
    return js.rejectedPromise<jsg::BufferSource>(js.exceptionToJs(kj::cp(exception)));
  }

  if (state.is<Closed>()) {
    // We are already in a closed state. This is a no-op. This really
    // should not have been called if closed but just in case, return
    // a resolved promise.
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
    return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& ioContext = IoContext::current();
    auto& active = *open.active;

    if (active.closePending) {
      return js.rejectedPromise<jsg::BufferSource>(
          js.typeError("Close already pending, cannot read."));
    }
    active.closePending = true;

    struct Holder {
      kj::Maybe<kj::Array<const kj::byte>> result;
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
      self->runIfAlive(
          [&](ReadableStreamSourceJsAdapter& self) { self.state.transitionTo<Closed>(); });
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
            [self = selfRef.addRef()](jsg::Lock& js, jsg::Value&& exception) -> jsg::BufferSource {
      // Likewise, while nothing should be waiting on the ready promise, we
      // should still reject it just in case.
      auto error = jsg::JsValue(exception.getHandle(js));
      self->runIfAlive([&](ReadableStreamSourceJsAdapter& self) { self.cancel(js, error); });
      js.throwException(kj::mv(exception));
    });
  }

  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> ReadableStreamSourceJsAdapter::tryGetLength(StreamEncoding encoding) {
  KJ_IF_SOME(open, state.tryGetActive()) {
    return open.active->source->tryGetLength(encoding);
  }
  return kj::none;
}

kj::Maybe<ReadableStreamSourceJsAdapter::Tee> ReadableStreamSourceJsAdapter::tryTee(
    jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(exception, state.tryGetError()) {
    js.throwException(js.exceptionToJs(kj::cp(exception)));
  }

  if (state.is<Closed>()) {
    // We are already closed, cannot tee.
    return kj::none;
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& active = *open.active;
    // If we are closing, or have pending tasks, we cannot tee.
    JSG_REQUIRE(!active.closePending && !active.running && active.queue.empty(), Error,
        "Cannot tee a stream that is closing or has pending reads.");
    auto tee = active.source->tee(limit);
    auto& ioContext = IoContext::current();
    state.transitionTo<Closed>();
    return Tee{
      .branch1 = kj::heap<ReadableStreamSourceJsAdapter>(js, ioContext, kj::mv(tee.branch1)),
      .branch2 = kj::heap<ReadableStreamSourceJsAdapter>(js, ioContext, kj::mv(tee.branch2)),
    };
  }

  KJ_UNREACHABLE;
}

// ===============================================================================================

struct ReadableSourceKjAdapter::Active {
  IoContext& ioContext;
  jsg::Ref<ReadableStream> stream;
  jsg::Ref<ReadableStreamDefaultReader> reader;
  kj::Canceler canceler;

  struct Idle {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "idle"_kj;
  };
  struct Readable {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "readable"_kj;
    // Previously read but unconsumed bytes. We keep these around for the next read call.
    kj::Array<const kj::byte> data;
    kj::ArrayPtr<const kj::byte> view;

    Readable(kj::Array<const kj::byte>&& data): data(kj::mv(data)), view(this->data) {}
  };
  struct Reading {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "reading"_kj;
    // The contract for ReadableStreamSource is that there can be only one read() in-flight
    // against the underlying stream at a time.
  };
  struct Done {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "done"_kj;
    // If a read returns fewer than the requested minBytes, that indicates the stream is done. We
    // make note of that here to prevent any further reads. We cannot transition to the closed
    // state in the promise chain of the read because the adapter will cancel the read promise
    // itself once Active is destroyed, and that would be a bad thing.
  };
  struct Canceling {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "canceling"_kj;
    kj::Exception exception;
  };
  struct Canceled {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "canceled"_kj;
    kj::Exception exception;
  };

  // Inner state machine for tracking read operation state:
  //   Idle -> Reading (start read)
  //   Reading -> Idle (read complete, no leftover)
  //   Reading -> Readable (read complete, has leftover)
  //   Reading -> Done (read returned less than minBytes)
  //   Any -> Canceling (error during read)
  //   Any -> Canceled (explicit cancel)
  // Done, Canceling, and Canceled are terminal states.
  using InnerState = ComposableStateMachine<TerminalStates<Done, Canceling, Canceled>,
      Idle,
      Readable,
      Reading,
      Done,
      Canceling,
      Canceled>;
  InnerState state;

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
struct ReadableSourceKjAdapter::ReadContext {
  jsg::Ref<ReadableStream> stream;
  jsg::Ref<ReadableStreamDefaultReader> reader;
  kj::ArrayPtr<kj::byte> buffer;
  // Only set to back the buffer if we need to keep it alive.
  kj::Maybe<kj::Array<kj::byte>> backingBuffer;
  size_t totalRead = 0;
  size_t minBytes = 0;
  kj::Maybe<Active::Readable> maybeLeftOver;
  // We keep a weak reference to the adapter itself so we can track
  // whether it is still alive while we are in a JS promise chain.
  // If the adapter is gone, or transitions to a closed or canceled
  // state we will abandon the read. If the ref is not set, then we
  // are in a pump operation and do not need to check for liveness.
  kj::Maybe<kj::Rc<WeakRef<ReadableSourceKjAdapter>>> adapterRef;

  void reset() {
    // Resetting is only allowed if we have the backing buffer.
    buffer = KJ_ASSERT_NONNULL(backingBuffer);
    totalRead = 0;
    minBytes = 0;
    maybeLeftOver = kj::none;
  }
};

namespace {
constexpr size_t kMinRemainingForAdditionalRead = 512;

jsg::Ref<ReadableStreamDefaultReader> initReader(jsg::Lock& js, jsg::Ref<ReadableStream>& stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError, "ReadableStream is locked.");
  JSG_REQUIRE(!stream->isDisturbed(), TypeError, "ReadableStream is disturbed.");
  auto reader = stream->getReader(js, kj::none);
  return kj::mv(KJ_ASSERT_NONNULL(reader.tryGet<jsg::Ref<ReadableStreamDefaultReader>>()));
}

using JsByteSource = kj::OneOf<jsg::JsRef<jsg::JsString>,
    jsg::JsRef<jsg::JsArrayBuffer>,
    jsg::JsRef<jsg::JsArrayBufferView>>;

kj::Maybe<JsByteSource> tryExtractJsByteSource(jsg::Lock& js, const jsg::JsValue& jsval) {
  KJ_IF_SOME(abView, jsval.tryCast<jsg::JsArrayBuffer>()) {
    return kj::Maybe(jsg::JsRef(js, abView));
  } else KJ_IF_SOME(ab, jsval.tryCast<jsg::JsArrayBufferView>()) {
    return kj::Maybe(jsg::JsRef(js, ab));
  } else KJ_IF_SOME(str, jsval.tryCast<jsg::JsString>()) {
    return kj::Maybe(jsg::JsRef(js, str));
  }
  return kj::none;
}

// Copies as much data from source into the context as possible, returning
// the number of bytes copied.
kj::Maybe<kj::Array<const kj::byte>> copyFromSource(
    jsg::Lock& js, ReadableSourceKjAdapter::ReadContext& context, const JsByteSource& source) {
  KJ_SWITCH_ONEOF(source) {
    KJ_CASE_ONEOF(str, jsg::JsRef<jsg::JsString>) {
      auto view = str.getHandle(js);
      size_t len = view.length(js);
      size_t toCopy = kj::min(len, context.buffer.size());

      if (toCopy == 0) {
        return kj::none;
      }

      if (toCopy < len) {
        // We are going to have left-over data. Unfortunately in this case
        // we have to copy the data twice... once into a kj::String and
        // again into our buffer. This is because the V8 string UTF-8
        // write API does not support partial writes with an offset.
        auto data = view.toUSVString(js);
        context.buffer.first(toCopy).copyFrom(data.asBytes().first(toCopy));
        context.totalRead += toCopy;
        context.buffer = context.buffer.slice(toCopy);
        KJ_DASSERT(context.buffer.size() == 0);
        return kj::Maybe(data.asBytes().slice(toCopy).attach(kj::mv(data)));
      }

      // We can copy everything in one go. Yay! This is great because we
      // can avoid a double copy here.
      auto ret KJ_UNUSED = view.writeInto(js, context.buffer.asChars().first(toCopy),
          jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8);
      KJ_DASSERT(ret.written == toCopy);
      context.totalRead += toCopy;
      context.buffer = context.buffer.slice(toCopy);
      return kj::none;
    }
    KJ_CASE_ONEOF(ab, jsg::JsRef<jsg::JsArrayBuffer>) {
      auto src = ab.getHandle(js).asArrayPtr();
      size_t toCopy = kj::min(src.size(), context.buffer.size());
      if (toCopy == 0) {
        return kj::none;
      }

      context.buffer.first(toCopy).copyFrom(src.first(toCopy));
      context.totalRead += toCopy;
      context.buffer = context.buffer.slice(toCopy);

      if (toCopy < src.size()) {
        KJ_DASSERT(context.buffer.size() == 0);
        // TODO(mpk): For now, we have to copy the left-over data into a new array.
        // Why? I'm happy you asked! Because the src is backed by a
        // v8::BackingStore protected by the v8 sandboxing rules and we
        // don't yet have the memory protection key logic in place to safely
        // share that memory outside of the v8 heap. For now, copy. Later
        // we can revisit this to hopefully avoid the additinal copy.
        return kj::Maybe(kj::heapArray(src.slice(toCopy)));
      }

      return kj::none;
    }
    KJ_CASE_ONEOF(view, jsg::JsRef<jsg::JsArrayBufferView>) {
      auto src = view.getHandle(js).asArrayPtr();
      size_t toCopy = kj::min(src.size(), context.buffer.size());
      if (toCopy == 0) {
        // Copy nothing. Return 0.
        return kj::none;
      }

      context.buffer.first(toCopy).copyFrom(src.first(toCopy));
      context.totalRead += toCopy;
      context.buffer = context.buffer.slice(toCopy);

      if (toCopy < src.size()) {
        KJ_DASSERT(context.buffer.size() == 0);
        return kj::Maybe(kj::heapArray(src.slice(toCopy)));
      }

      return kj::none;
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

ReadableSourceKjAdapter::Active::Active(
    jsg::Lock& js, IoContext& ioContext, jsg::Ref<ReadableStream> stream)
    : ioContext(ioContext),
      stream(kj::mv(stream)),
      reader(initReader(js, this->stream)) {
  state.transitionTo<Idle>();
}

ReadableSourceKjAdapter::Active::~Active() noexcept(false) {
  cancel(KJ_EXCEPTION(DISCONNECTED, "ReadableSourceKjAdapter is canceled."));
}

void ReadableSourceKjAdapter::Active::cancel(kj::Exception reason) {
  if (state.is<Canceled>()) {
    return;
  }
  bool wasDone = state.is<Done>();
  state.forceTransitionTo<Canceled>(kj::cp(reason));
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

ReadableSourceKjAdapter::ReadableSourceKjAdapter(
    jsg::Lock& js, IoContext& ioContext, jsg::Ref<ReadableStream> stream, Options options)
    : options(options),
      selfRef(
          kj::rc<WeakRef<ReadableSourceKjAdapter>>(kj::Badge<ReadableSourceKjAdapter>{}, *this)) {
  state.transitionTo<KjOpen>(kj::heap<Active>(js, ioContext, kj::mv(stream)));
}

ReadableSourceKjAdapter::~ReadableSourceKjAdapter() noexcept(false) {
  selfRef->invalidate();
}

jsg::Promise<kj::Own<ReadableSourceKjAdapter::ReadContext>> ReadableSourceKjAdapter::readInternal(
    jsg::Lock& js, kj::Own<ReadContext> context, MinReadPolicy minReadPolicy) {
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
  return context->reader->read(js).then(js,
      ioContext.addFunctor([context = kj::mv(context), minReadPolicy](jsg::Lock& js,
                               ReadResult result) mutable -> jsg::Promise<kj::Own<ReadContext>> {
    if (result.done || result.value == kj::none) {
      // Stream is ended.
      return js.resolvedPromise(kj::mv(context));
    }

    auto& value = KJ_ASSERT_NONNULL(result.value);

    // Ok, we have some data. Let's make sure it is bytes.
    // We accept either an ArrayBuffer, ArrayBufferView, or string.
    auto jsval = jsg::JsValue(value.getHandle(js));
    KJ_IF_SOME(result, tryExtractJsByteSource(js, jsval)) {
      // Process the resulting data.
      KJ_IF_SOME(leftOver, copyFromSource(js, *context, result)) {
        KJ_ASSERT(context->buffer.size() == 0);
        if (leftOver.size() > 0) {
          context->maybeLeftOver = Active::Readable(kj::mv(leftOver));
        } else {
          context->maybeLeftOver = kj::none;
        }
        return js.resolvedPromise(kj::mv(context));
      }

      // At this point, we should have no left over data.
      KJ_DASSERT(context->maybeLeftOver == kj::none);

      // We should also have some space left in our destination buffer.
      KJ_DASSERT(context->buffer.size() > 0);

      // We might continue reading only if the adapter is still alive and
      // in an active state...
      bool continueReading = true;
      KJ_IF_SOME(adapterRef, context->adapterRef) {
        continueReading = adapterRef->isValid();
        adapterRef->runIfAlive(
            [&](ReadableSourceKjAdapter& adapter) { continueReading = adapter.state.isActive(); });
      }

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
      bool minReadSatisfied = context->totalRead >= context->minBytes &&
          (minReadPolicy == MinReadPolicy::IMMEDIATE ||
              context->buffer.size() < kMinRemainingForAdditionalRead);

      if (!continueReading || minReadSatisfied) {
        return js.resolvedPromise(kj::mv(context));
      }

      // We still have not satisfied the minimum read requirement or we are
      // trying to fill up a larger buffer. We will need to read more. Let's
      // call readInternal again to get the next chunk of data. Keep in mind
      // that this is not a true recursive call because readInternal returns
      // a jsg::Promise. We're just chaining the promises together here.
      return readInternal(js, kj::mv(context), minReadPolicy);
    }

    // Oooo, invalid type. We cannot handle this and must treat this as a fatal error.
    // We will cancel the stream and return an error.
    auto error = js.typeError("ReadableStream provided a non-bytes value. Only ArrayBuffer, "
                              "ArrayBufferView, or string are supported.");
    context->reader->cancel(js, error);
    return js.rejectedPromise<kj::Own<ReadContext>>(error);
  }),
      ioContext.addFunctor([](jsg::Lock& js, jsg::Value exception) {
    // In this case, the reader should already be in an errored state
    // since it it the read that failed. Just propagate the error.
    return js.rejectedPromise<kj::Own<ReadContext>>(kj::mv(exception));
  }));
}

// We separate out the actual read implementation so that it can be used by
// both read and the pumpToImpl implementation.
kj::Promise<size_t> ReadableSourceKjAdapter::readImpl(
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
        active.state.transitionTo<Active::Idle>();
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

    active.state.transitionTo<Active::Idle>();

    // Did we at least satisfy the minimum bytes?
    if (size >= minBytes) {
      // Awesome, we are technically done with this read.
      // While we might actually have more room in our buffer, and the
      // minReadyPolicy might be OPPORTUNISTIC, we will not try to
      // read more from the stream right now so that we can avoid having
      // to grab the isolate lock for this read. Instead, let's return
      // what we have and let the caller call read again if/when they want.
      // This risks leaving a fair amount of unused space in the buffer
      // and requiring more read calls but it avoids the overhead of
      // an additional isolate lock grab when we know we can at least
      // provide some data right now.
      return size;
    }
  }

  // If we got here, we still have not satisfied the minimum bytes,
  // so we will continue on to read more from the stream. But, we
  // also should not have any more data left over. Let's verify.
  KJ_ASSERT(active.state.is<Active::Idle>());
  active.state.transitionTo<Active::Reading>();

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

      self->runIfAlive([&](ReadableSourceKjAdapter& self) {
        // Ok, we're still alive! Yay! But, let's check to make sure we didn't
        // change state while we were reading.
        KJ_IF_SOME(open, self.state.tryGetActive()) {
          auto& active = *open.active;
          // Ok, we're still active. Let's see if we have any left over data
          // that we need to stash away for the next read.
          KJ_IF_SOME(leftOver, context->maybeLeftOver) {
            // We have some left over data. Stash it away for the next read.
            active.state.transitionTo<Active::Readable>(kj::mv(leftOver));
            // In this branch, we must have filled the entire destination
            // buffer and satisfied the minimum read requirement or else
            // we wouldn't have any left over data. Let's just assert that
            // invariant just in case.
            KJ_DASSERT(context->totalRead >= context->minBytes);
          } else if (context->totalRead < context->minBytes) {
            // We returned fewer than the minimum bytes requested. This is our
            // signal that we're done.
            active.state.transitionTo<Active::Done>();
            // We cannot change the state to Closed here because we are still
            // inside the kj::Promise chain wrapped by the canceler. If we
            // change the state to Closed, the Active would be destroyed, causing
            // this promise chain to be canceled.
            auto droppedReader KJ_UNUSED = kj::mv(active.reader);
            auto droppedStream KJ_UNUSED = kj::mv(active.stream);
            // In this branch, we should not have any left over data.
            // Let's assert that invariant just in case.
            KJ_DASSERT(context->maybeLeftOver == kj::none);
          } else {
            // Our read is complete. Return to the idle state and we're done.
            active.state.transitionTo<Active::Idle>();

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
    self->runIfAlive([&](ReadableSourceKjAdapter& self) {
      KJ_IF_SOME(open, self.state.tryGetActive()) {
        open.active->state.forceTransitionTo<Active::Canceling>(Active::Canceling{
          .exception = kj::cp(exception),
        });
      }
    });
    return kj::mv(exception);
  });
}

kj::Promise<size_t> ReadableSourceKjAdapter::read(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) {

  if (buffer.size() == 0) {
    // Nothing to read. This is a no-op.
    return static_cast<size_t>(0);
  }

  // Clamp the minBytes to [1, buffer.size()].
  minBytes = kj::min(buffer.size(), kj::max(minBytes, 1UL));
  KJ_DASSERT(minBytes >= 1 && minBytes <= buffer.size(),
      "minBytes must be less than or equal to the buffer size.");

  KJ_IF_SOME(exception, state.tryGetError()) {
    return kj::cp(exception);
  }

  if (state.is<KjClosed>()) {
    return static_cast<size_t>(0);
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& active = *open.active;
    KJ_SWITCH_ONEOF(active.state) {
      KJ_CASE_ONEOF(_, Active::Reading) {
        KJ_FAIL_REQUIRE("Cannot have multiple concurrent reads.");
      }
      KJ_CASE_ONEOF(_, Active::Done) {
        // The previous read indicated that it was the last read by returning
        // less than the minimum bytes requested. We have to treat this as
        // the stream being closed.
        state.transitionTo<KjClosed>();
        return static_cast<size_t>(0);
      }
      KJ_CASE_ONEOF(canceling, Active::Canceling) {
        // The stream is being canceled. Propagate the exception and complete
        // the state transition.
        return KJ_ASSERT_NONNULL(checkCancelingOrCanceled(active));
      }
      KJ_CASE_ONEOF(canceled, Active::Canceled) {
        // The stream was canceled. Propagate the exception and complete
        // the state transition.
        return KJ_ASSERT_NONNULL(checkCancelingOrCanceled(active));
      }
      KJ_CASE_ONEOF(r, Active::Readable) {
        // There is some data left over from a previous read.
        return readImpl(active, buffer, minBytes);
      }
      KJ_CASE_ONEOF(_, Active::Idle) {
        // There are no pending reads and no left over data.
        return readImpl(active, buffer, minBytes);
      }
    }
  }

  KJ_UNREACHABLE;
}

kj::Maybe<size_t> ReadableSourceKjAdapter::tryGetLength(StreamEncoding encoding) {
  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& active = *open.active;
    if (active.state.is<Active::Done>() || active.state.is<Active::Canceled>()) {
      // If the previous read indicated that it was the last, then
      // let's just transition to the closed state now and return kj::none.
      state.transitionTo<KjClosed>();
      return kj::none;
    }
    if (checkCancelingOrCanceled(active) != kj::none) {
      return kj::none;
    }
    return active.stream->tryGetLength(encoding).map(
        [](uint64_t len) { return static_cast<size_t>(len); });
  }

  // The stream is either closed or errored.
  return kj::none;
}

void ReadableSourceKjAdapter::cancel(kj::Exception reason) {
  KJ_IF_SOME(open, state.tryGetActive()) {
    open.active->cancel(kj::cp(reason));
  }
  state.forceTransitionTo<kj::Exception>(kj::mv(reason));
}

kj::Maybe<kj::Exception> ReadableSourceKjAdapter::checkCancelingOrCanceled(Active& active) {
  KJ_IF_SOME(canceling, active.state.tryGet<Active::Canceling>()) {
    auto exception = kj::mv(canceling.exception);
    state.forceTransitionTo<kj::Exception>(kj::cp(exception));
    return kj::mv(exception);
  }
  KJ_IF_SOME(canceled, active.state.tryGet<Active::Canceled>()) {
    auto exception = kj::mv(canceled.exception);
    state.forceTransitionTo<kj::Exception>(kj::cp(exception));
    return kj::mv(exception);
  }
  return kj::none;
}

void ReadableSourceKjAdapter::throwIfCancelingOrCanceled(Active& active) {
  KJ_IF_SOME(exception, checkCancelingOrCanceled(active)) {
    kj::throwFatalException(kj::mv(exception));
  }
}

kj::Promise<void> ReadableSourceKjAdapter::pumpToImpl(
    kj::Own<Active> active, WritableSink& output, EndAfterPump end) {
  // Note: this intentionally contains code that is similar to the ReadableSourceImpl::pumpImpl
  // impl in readable-source.c++. The optimizations are generally the same but the targets are
  // a bit different (ReadableStream vs. kj::AsyncInputStream).
  KJ_DASSERT(active->state.is<Active::Idle>() || active->state.is<Active::Readable>(),
      "pumpToImpl called when stream is not in an active state.");

  static constexpr size_t DEFAULT_BUFFER_SIZE = 16384;
  static constexpr size_t MIN_BUFFER_SIZE = 1024;
  static constexpr size_t MED_BUFFER_SIZE = MIN_BUFFER_SIZE << 6;
  static constexpr size_t MAX_BUFFER_SIZE = MIN_BUFFER_SIZE << 7;
  static constexpr size_t MEDIUM_THRESHOLD = 1048576;
  static_assert(MIN_BUFFER_SIZE < DEFAULT_BUFFER_SIZE);
  static_assert(DEFAULT_BUFFER_SIZE < MED_BUFFER_SIZE);
  static_assert(MED_BUFFER_SIZE < MAX_BUFFER_SIZE);
  static_assert(MAX_BUFFER_SIZE < MEDIUM_THRESHOLD);

  // The minimum read policy to use during the pump. Starts as OPPORTUNISTIC
  // but will be adjusted based on observed stream behavior.
  MinReadPolicy minReadPolicy = MinReadPolicy::OPPORTUNISTIC;

  // Our stream may or may not have a known length.
  size_t bufferSize = DEFAULT_BUFFER_SIZE;
  kj::Maybe<uint64_t> maybeRemaining = active->stream->tryGetLength(StreamEncoding::IDENTITY);
  KJ_IF_SOME(length, maybeRemaining) {
    // Streams that advertise their length SHOULD always tell the truth.
    // But... on the off chance they don't, we'll still try to behave
    // reasonably. At worst we will allocate a backing buffer and
    // perform a single read. If this proves to be a performance issue,
    // we can fall back to strictly enforcing the advertised length.
    if (length <= MEDIUM_THRESHOLD) {
      // When `length` is below the medium threshold, use
      // the nearest power of 2 >= length within the range
      // [MIN_BUFFER_SIZE, MED_BUFFER_SIZE].
      bufferSize = kj::max(MIN_BUFFER_SIZE, std::bit_ceil(length));
      bufferSize = kj::min(MED_BUFFER_SIZE, bufferSize);
    } else {
      // Otherwise, use the biggest buffer.
      bufferSize = MAX_BUFFER_SIZE;
    }
  }

  bool writeFailed = false;
  bool readFailed = false;

  // First, if the active state is in the Readable state, we need to drain the
  // left over data before starting the main read loop.
  KJ_IF_SOME(readable, active->state.tryGet<Active::Readable>()) {
    co_await output.write(readable.view);
    active->state.transitionTo<Active::Idle>();
  }

  static const auto pumpReadImpl = [](Active& active, kj::ArrayPtr<kj::byte> dest, size_t minBytes,
                                       MinReadPolicy minReadPolicy) -> kj::Promise<size_t> {
    // Keep in mind that every call to pumpReadImpl requires acquiring the isolate lock!
    auto context = kj::heap<ReadContext>({
      .stream = active.stream.addRef(),
      .reader = active.reader.addRef(),
      .buffer = dest,
      .totalRead = 0,
      .minBytes = minBytes,
      .adapterRef = kj::none,  // no need to track adapter liveness during pump
    });

    return active.ioContext.run([context = kj::mv(context), minReadPolicy](jsg::Lock& js) mutable {
      auto& ioContext = IoContext::current();
      // The readInternal method (and the underlying read on the stream) should optimize
      // itself based on the bytes available in the stream itself and the minBytes requested.
      return ioContext
          .awaitJs(js, ReadableSourceKjAdapter::readInternal(js, kj::mv(context), minReadPolicy))
          .then([](kj::Own<ReadContext> context) mutable -> kj::Promise<size_t> {
        return context->totalRead;
      });
    });
  };

  static const auto cancelReaderImpl = [](Active& active,
                                           kj::Exception reason) -> kj::Promise<void> {
    // Canceling the reader requires acquiring the isolate lock, unfortunately.
    return active.ioContext.run(
        [reader = active.reader.addRef(), exception = kj::mv(reason)](jsg::Lock& js) mutable {
      auto& ioContext = IoContext::current();
      auto error = js.exceptionToJsValue(kj::mv(exception));
      auto promise = reader->cancel(js, error.getHandle(js));
      return ioContext.awaitJs(js, kj::mv(promise));
    });
  };

  int currentReadBuf = 0;
  kj::SmallArray<kj::byte, 4 * MIN_BUFFER_SIZE> backing(bufferSize * 2);
  kj::ArrayPtr<kj::byte> buffers[2] = {
    backing.first(bufferSize),
    backing.slice(bufferSize),
  };

  // We will use an adaptive minBytes value to try to optimize read sizes based on
  // observed stream behavior. We start with a minBytes set to half the buffer size.
  // As the stream is read, we will adjust minBytes up or down depending on whether
  // the stream is consistently filling the buffer or not.
  size_t minBytes = bufferSize >> 1;
  kj::Maybe<kj::Exception> pendingException;

  // Initiate our first read.
  auto readPromise = pumpReadImpl(*active, buffers[currentReadBuf], minBytes, minReadPolicy);
  size_t iterationCount = 0;
  size_t consecutiveFastReads = 0;

  try {
    while (true) {
      size_t amount = 0;
      {
        KJ_ON_SCOPE_FAILURE(readFailed = true);
        amount = co_await readPromise;
      }
      iterationCount++;

      // If the read returned < minBytes, that indicates the stream is done.
      // Let's write the bytes we got, end the output if needed, and exit.
      if (amount < minBytes) {
        KJ_ON_SCOPE_FAILURE(writeFailed = true);
        if (amount > 0) {
          co_await output.write(buffers[currentReadBuf].slice(0, amount));
        }
        if (end) {
          co_await output.end();
        }
        co_return;
      }

      auto writeBuf = buffers[currentReadBuf].slice(0, amount);
      currentReadBuf = 1 - currentReadBuf;  // switch buffers

      // Before we perform the next read, let's adapt minBytes based on stream behavior
      // we have observed on the previous read.
      if (iterationCount <= 3 || iterationCount % 10 == 0) {
        if (amount == bufferSize) {
          // Stream is filling buffer completely... Use smaller minBytes to
          // increase responsiveness, should produce more reads with less data.
          if (bufferSize >= 4 * DEFAULT_BUFFER_SIZE) {
            // For large buffers (≥64KB), be more aggressive about responsiveness.
            // 25% of a large buffer is still a substantial chunk (e.g., 32KB for 128KB).
            minBytes = bufferSize >> 2;  // 25%
          } else {
            // For smaller buffers, 50% provides better balance, avoiding chunks
            // that are too small for efficient processing (e.g., keeps 16KB → 8KB).
            minBytes = bufferSize >> 1;  // 50%
          }
        } else {
          // Stream didn't fill buffer - likely slower or at natural boundary.
          // Use higher minBytes to accumulate larger chunks and reduce iteration overhead.
          minBytes = (bufferSize >> 2) + (bufferSize >> 1);  // 75%
        }
      }

      KJ_IF_SOME(remaining, maybeRemaining) {
        if (amount > remaining) {
          // The stream lied about its length. Ignore further length tracking.
          maybeRemaining = kj::none;
        } else {
          // Otherwise, set minBytes to whatever is expected to remain.
          remaining -= amount;
          maybeRemaining = remaining;
          if (remaining < minBytes && remaining > 0) {
            minBytes = remaining;
          }
        }
      }

      // If we're in IMMEDIATE mode, check if the stream has recovered and is consistently
      // providing good amounts of data. If so, switch back to OPPORTUNISTIC to reduce
      // the number of isolate lock acquisitions.
      if (minReadPolicy == MinReadPolicy::IMMEDIATE) {
        if (amount >= (bufferSize >> 1)) {
          consecutiveFastReads++;
          if (consecutiveFastReads >= 10) {
            minReadPolicy = MinReadPolicy::OPPORTUNISTIC;
            consecutiveFastReads = 0;
          }
        } else {
          consecutiveFastReads = 0;
        }
      }

      // Switch to IMMEDIATE after 3 iterations if we're seeing consistently small reads
      // (< 25% of buffer).
      if (minReadPolicy == MinReadPolicy::OPPORTUNISTIC && iterationCount > 3 &&
          amount < (bufferSize >> 2)) {
        minReadPolicy = MinReadPolicy::IMMEDIATE;
        consecutiveFastReads = 0;
      }

      // Start working on the next read.
      readPromise = pumpReadImpl(*active, buffers[currentReadBuf], minBytes, minReadPolicy);

      {
        KJ_ON_SCOPE_FAILURE(writeFailed = true);
        co_await output.write(writeBuf);
      }
    }
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    if (!writeFailed) {
      // If we got an error and it wasn't the write that failed, arrange to abort
      // the output...
      output.abort(kj::cp(exception));
    }
    if (readFailed) {
      // If the read failed, the reader should already be in an errored state
      // so we can skip canceling it. Just propagate the exception directly.
      kj::throwFatalException(kj::mv(exception));
    }
    // Otherwise, we need to cancel the reader. Let's not do that within the catch block...
    pendingException = kj::mv(exception);
  }

  KJ_IF_SOME(exception, pendingException) {
    co_await cancelReaderImpl(*active, kj::cp(exception));
    kj::throwFatalException(kj::mv(exception));
  }
}

kj::Promise<DeferredProxy<void>> ReadableSourceKjAdapter::pumpTo(
    WritableSink& output, EndAfterPump end) {
  // The pumpTo operation continually reads from the stream and writes
  // to the output until the stream is closed or an error occurs. Once
  // the pump starts, the adapter transitions to the closed state and
  // ownership of the underlying stream is transferred to the pump
  // operation.

  KJ_IF_SOME(exception, state.tryGetError()) {
    return kj::Promise<DeferredProxy<void>>(DeferredProxy<void>{kj::cp(exception)});
  }

  if (state.is<KjClosed>()) {
    // Already closed, nothing to do.
    return newNoopDeferredProxy();
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& active = *open.active;
    // Per the contract for ReadableStreamSource::pumpTo, the pump operation
    // will take over ownership of the underlying stream until it is complete,
    // leaving the adapter itself in a closed state once the pump starts.
    // Dropping the returned promise will cancel the pump operation.
    // We do, however, need to first make sure that our active state is
    // not already pending a read or terminal state change.
    KJ_REQUIRE(!active.state.is<Active::Reading>(), "Cannot have multiple concurrent reads.");

    if (active.state.is<Active::Done>()) {
      // The previous read indicated that it was the last read by returning
      // less than the minimum bytes requested, or the stream was fully
      // canceled. We have to treat this as the stream being closed.
      state.transitionTo<KjClosed>();
      return newNoopDeferredProxy();
    }

    KJ_IF_SOME(exception, checkCancelingOrCanceled(active)) {
      return kj::Promise<DeferredProxy<void>>(kj::mv(exception));
    }

    // The active state should be Readable of Idle here. Let's verify.
    KJ_DASSERT(active.state.is<Active::Readable>() || active.state.is<Active::Idle>());

    // The Active state will be transferred into the pumpImpl operation.
    auto activeState = kj::mv(open.active);
    state.transitionTo<KjClosed>();  // transition to closed immediately

    // Because pumpToImpl is wrapping a JavaScript stream, it is not eligible
    // for deferred proxying. We will return a noopDeferredProxy that wraps the
    // promise from pumpToImpl();
    return addNoopDeferredProxy(pumpToImpl(kj::mv(activeState), output, end));
  }

  KJ_UNREACHABLE;
}

ReadableSource::Tee ReadableSourceKjAdapter::tee(size_t) {
  KJ_UNIMPLEMENTED("Teeing a ReadableSourceKjAdapter is not supported.");
  // Explanation: Teeing a ReadableStream must be done under the isolate lock,
  // as does creating a new ReadableSourceKjAdapter. However, when tee()
  // is called we are not guaranteed to be under the isolate lock, nor can
  // we acquire the lock here because this is a synchronous operation and
  // acquiring the isolate lock requires waiting for a promise to resolve.
  //
  // Teeing here is unlikely to be necessary. If you do need a tee, it's
  // necessary to tee the underlying ReadableStream directly and create
  // two separate ReadableSourceKjAdapters, one for each branch of
  // that tee while the lock is held.
}

kj::Promise<kj::Array<const kj::byte>> ReadableSourceKjAdapter::readAllBytes(size_t limit) {
  co_return co_await readAllImpl<kj::byte>(limit);
}

kj::Promise<kj::String> ReadableSourceKjAdapter::readAllText(size_t limit) {
  auto array = co_await readAllImpl<char>(limit);
  co_return kj::String(kj::mv(array));
}

template <typename T>
kj::Promise<kj::Array<T>> ReadableSourceKjAdapter::readAllImpl(size_t limit) {
  KJ_IF_SOME(exception, state.tryGetError()) {
    kj::throwFatalException(kj::cp(exception));
  }

  if (state.is<KjClosed>()) {
    co_return kj::Array<T>();
  }

  KJ_IF_SOME(open, state.tryGetActive()) {
    auto& active = *open.active;
    KJ_REQUIRE(!active.state.is<Active::Reading>(), "Cannot have multiple concurrent reads.");

    if (active.state.is<Active::Done>()) {
      // The previous read indicated that it was the last read by returning
      // less than the minimum bytes requested. We have to treat this as
      // the stream being closed.
      state.transitionTo<KjClosed>();
      co_return kj::Array<T>();
    }

    throwIfCancelingOrCanceled(active);

    // Our readAll operation will accumulate data into a buffer up to the
    // specified limit. If the limit is exceeded, the returned promise will
    // be rejected. Once the readAll operation starts, the adapter is moved
    // into a closed state and ownership of the underlying stream is transferred
    // to the readAll promise.
    auto activeState = kj::mv(open.active);
    state.transitionTo<KjClosed>();  // transition to closed immediately

    KJ_DASSERT(activeState->state.is<Active::Readable>() || activeState->state.is<Active::Idle>());

    // We do not use the canceler here. The adapter is closed and can be safely dropped.
    // This promise, however, will keep the stream alive until the read is completed.
    // If the returned promise is dropped, the readAll operation will be canceled.
    CancelationToken cancelationToken;
    co_return co_await IoContext::current().run(
        [limit, active = kj::mv(activeState), cancelationToken = cancelationToken.getWeakRef()](
            jsg::Lock& js) mutable -> kj::Promise<kj::Array<T>> {
      kj::Vector<T> accumulated;
      // If we know the length of the stream ahead of time, and it is within the limit,
      // we can reserve that much space in the accumulator to avoid multiple allocations.
      KJ_IF_SOME(length, active->stream->tryGetLength(StreamEncoding::IDENTITY)) {
        if (length <= limit) {
          accumulated.reserve(length);  // Pre-allocate
        }
      }

      auto& ioContext = IoContext::current();
      return ioContext.awaitJs(js,
          readAllReadImpl(js, ioContext.addObject(kj::mv(active)), kj::mv(accumulated), limit,
              kj::mv(cancelationToken)));
    });
  }

  KJ_UNREACHABLE;
}

template <typename T>
jsg::Promise<kj::Array<T>> ReadableSourceKjAdapter::readAllReadImpl(jsg::Lock& js,
    IoOwn<Active> active,
    kj::Vector<T> accumulated,
    size_t limit,
    kj::Rc<WeakRef<CancelationToken>> cancelationToken) {

  // Check for cancelation. The cancelation token is a weak ref. If the promise
  // that represents the readAll operation is dropped, the token will be invalidated.
  // Since there is no way to directly cancel a JavaScript promise, this is the best
  // we can do to interrupt the loop.
  if (!cancelationToken->isValid()) {
    return js.rejectedPromise<kj::Array<T>>(js.error("readAll operation was canceled."));
  }

  // First, drain any leftover data if the active state is in Readable mode.
  KJ_IF_SOME(readable, active->state.tryGet<Active::Readable>()) {
    auto leftover = readable.view.asBytes();
    if (leftover.size() > limit) {
      auto error = js.rangeError("Memory limit would be exceeded before EOF.");
      return active->reader->cancel(js, error).then(
          js, [ex = jsg::JsRef(js, error)](jsg::Lock& js) {
        return js.rejectedPromise<kj::Array<T>>(ex.getHandle(js));
      });
    }
    if constexpr (kj::isSameType<T, char>()) {
      accumulated.addAll(leftover.asChars());
    } else {
      accumulated.addAll(leftover);
    }
    active->state.transitionTo<Active::Idle>();
  }

  return active->reader->read(js).then(js,
      [active = kj::mv(active), accumulated = kj::mv(accumulated), limit,
          cancelationToken = kj::mv(cancelationToken)](
          jsg::Lock& js, ReadResult result) mutable -> jsg::Promise<kj::Array<T>> {
    // Check for cancelation.
    if (!cancelationToken->isValid()) {
      return js.rejectedPromise<kj::Array<T>>(js.error("readAll operation was canceled."));
    }

    if (result.done || result.value == kj::none) {
      // Stream ended. Return accumulated data.
      // If we're reading text, add NUL terminator.
      if constexpr (kj::isSameType<T, char>()) {
        accumulated.add('\0');
      }
      return js.resolvedPromise(accumulated.releaseAsArray());
    }

    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto jsval = jsg::JsValue(value.getHandle(js));

    kj::ArrayPtr<const kj::byte> bytes;
    kj::Maybe<kj::String> maybeOwnedString;

    KJ_IF_SOME(str, jsval.tryCast<jsg::JsString>()) {
      auto data = str.toUSVString(js);
      bytes = data.asBytes();
      maybeOwnedString = kj::mv(data);
    } else KJ_IF_SOME(ab, jsval.tryCast<jsg::JsArrayBuffer>()) {
      bytes = ab.asArrayPtr();
    } else KJ_IF_SOME(view, jsval.tryCast<jsg::JsArrayBufferView>()) {
      bytes = view.asArrayPtr();
    } else {
      auto error = js.typeError("ReadableStream provided a non-bytes value. Only ArrayBuffer, "
                                "ArrayBufferView, or string are supported.");
      return active->reader->cancel(js, error).then(
          js, [err = jsg::JsRef(js, error)](jsg::Lock& js) {
        return js.rejectedPromise<kj::Array<T>>(err.getHandle(js));
      });
    }

    if (accumulated.size() + bytes.size() > limit) {
      auto error = js.rangeError("Memory limit would be exceeded before EOF.");
      return active->reader->cancel(js, error).then(
          js, [err = jsg::JsRef(js, error)](jsg::Lock& js) {
        return js.rejectedPromise<kj::Array<T>>(err.getHandle(js));
      });
    }

    // Accumulate the bytes.
    if constexpr (kj::isSameType<T, char>()) {
      accumulated.addAll(bytes.asChars());
    } else {
      accumulated.addAll(bytes);
    }

    // Continue reading.
    return readAllReadImpl(
        js, kj::mv(active), kj::mv(accumulated), limit, kj::mv(cancelationToken));
  });
}

}  // namespace workerd::api::streams
