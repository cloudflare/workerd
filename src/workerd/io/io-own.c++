#include "io-own.h"

#include "io-context.h"

#include <workerd/jsg/util.h>

namespace workerd {

void DeleteQueue::scheduleDeletion(OwnedObject* object) const {
  if (IoContext::hasCurrent() && IoContext::current().deleteQueue.queue.get() == this) {
    // Deletion from same thread. No need to enqueue.
    kj::AllowAsyncDestructorsScope scope;
    OwnedObjectList::unlink(*object);
  } else {
    auto lock = crossThreadDeleteQueue.lockExclusive();
    KJ_IF_SOME(state, *lock) {
      state.queue.add(object);

      // Wake the owning IoContext so it actually drains the queue, the same way
      // scheduleAction() does. Historically deletions did not wake the context and were only
      // drained at its next runInContextScope() entry; on a genuinely idle context (e.g. an
      // open RPC session receiving drops from other threads) that pins the object -- and any
      // peer waiting on its destructor's side effects -- until context teardown.
      //
      // Batching is preserved: fulfill() is idempotent until the signal task drains the queue
      // and resets the fulfiller, so a burst of deletions costs one wakeup. See
      // startDeleteQueueSignalTask in io-context.c++ for why the drain is deliberately lazy
      // and lock-free.
      //
      // The fulfiller can only be missing during IoContext construction (the signal task that
      // arms it starts after the queue member is initialized), before the queue could have
      // been shared; tolerate that instead of throwing since we are called from destructors.
      KJ_IF_SOME(fulfiller, state.crossThreadFulfiller) {
        // We are often called during GC of a JS heap object holding an IoOwn, with jsg's
        // DISALLOW_KJ_IO_DESTRUCTORS_SCOPE active. fulfill() can destroy an async object in
        // one edge case -- if the owning context is concurrently tearing down, the waiting
        // side has canceled the cross-thread promise, and fulfill() then deletes the orphaned
        // promise node. That destruction is safe (nothing else references the node), so allow
        // it explicitly, just like the same-thread branch above allows the deletion itself.
        kj::AllowAsyncDestructorsScope scope;
        fulfiller->fulfill();
      }
    }
  }
}

void DeleteQueue::scheduleAction(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& action) const {
  {
    auto lock = crossThreadDeleteQueue.lockExclusive();
    KJ_IF_SOME(state, *lock) {
      state.actions.add(kj::mv(action));
      KJ_REQUIRE_NONNULL(state.crossThreadFulfiller)->fulfill();
      return;
    }
  }

  // The queue was deleted, likely because the IoContext was destroyed and the
  // DeleteQueuePtr was invalidated. We are going to emit a warning and drop the
  // actions on the floor without scheduling them.
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    // We are creating an error here just so we can include the JavaScript stack
    // with the warning if it exists. We are not going to throw this error.
    auto err = v8::Exception::Error(
        js.str("A promise was resolved or rejected from a different request context than "
               "the one it was created in. However, the creating request has already been "
               "completed or canceled. Continuations for that request are unlikely to "
               "run safely and have been canceled. If this behavior breaks your worker, "
               "consider setting the `no_handle_cross_request_promise_resolution` "
               "compatibility flag for your worker."_kj))
                   .As<v8::Object>();
    // TODO(soon): Add documentation link to this warning.
    // Changing the name property to "Warning" will make the serialize stack start with
    // "Warning: " rather that "Error: "
    jsg::check(err->Set(js.v8Context(), js.str("name"_kj), js.str("Warning"_kj)));
    auto stack = jsg::check(err->Get(js.v8Context(), js.str("stack"_kj)));

    // Safe to mutate here since we have the exclusive lock on the queue above.
    ioContext.logWarning(kj::str(stack));
  }
}

void DeleteQueue::checkFarGet(const DeleteQueue& deleteQueue, const std::type_info& type) {
  IoContext::current().checkFarGet(deleteQueue, type);
}

void ReverseIoOwnValidity::checkValid() const {
  JSG_REQUIRE(
      isValid(), Error, "Couldn't complete operation because the execution context has ended.");
}

kj::Promise<void> DeleteQueue::resetCrossThreadSignal(bool refireForDeletions) const {
  auto lock = crossThreadDeleteQueue.lockExclusive();
  KJ_IF_SOME(state, *lock) {
    // Note: the previous fulfiller may still be waiting here. The signal task usually consumes
    // the signal before resetting, but when it has deferred queued deletions to a pending
    // hang-detector abort it awaits the signal joined with onAbort() (see
    // startDeleteQueueSignalTask), and an abort wake abandons the armed signal: its promise
    // side is canceled, so the old fulfiller can never be observed again and must be replaced
    // for future wakeups to work.
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    state.crossThreadFulfiller = kj::mv(paf.fulfiller);
    if (state.actions.size() > 0 || (refireForDeletions && state.queue.size() > 0)) {
      // Work arrived between the last drain and this reset. Its scheduleDeletion()/
      // scheduleAction() fulfilled the *previous* fulfiller (a no-op, it had already fired), so
      // nothing would ever fulfill the fresh one on that work's behalf; fire it now so the
      // signal task processes the work instead of sleeping on it.
      //
      // The caller passes refireForDeletions = false when it just deliberately declined to
      // drain the queue in favor of a pending hang-detector abort (see
      // startDeleteQueueSignalTask); re-firing for those same deletions would spin.
      KJ_ASSERT_NONNULL(state.crossThreadFulfiller)->fulfill();
    }
    return kj::mv(paf.promise);
  } else {
    return kj::NEVER_DONE;
  }
}

OwnedObjectList::~OwnedObjectList() noexcept(false) {
  while (head != kj::none) {
    // We want to have the same order of operations as the recursive destructor here. Without this
    // optimization, `~SpecificOwnedObject<T>` is invoked first, then `~OwnedObject()` which
    // destructs the next node which continues the process. The key takeaway is that we destroy each
    // node's `T` before we move onto the next node. This duplicates that behavior by unlinking
    // forward through the list, which hopefully should keep our stack size low no matter how many
    // `OwnedObject` instances we have.
    unlink(*KJ_ASSERT_NONNULL(head));
  }
}

void OwnedObjectList::unlink(OwnedObject& object) {
  KJ_IF_SOME(next, object.next) {
    next.get()->prev = object.prev;
  }
  *object.prev = kj::mv(object.next);
}

void OwnedObjectList::link(kj::Own<OwnedObject> object) {
  object->next = kj::mv(head);
  KJ_IF_SOME(next, object->next) {
    next.get()->prev = &object->next;
  }
  object->prev = &head;
  head = kj::mv(object);
}

void IoCrossContextExecutor::execute(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& func) {
  deleteQueue->scheduleAction(js, kj::mv(func));
}

}  // namespace workerd
