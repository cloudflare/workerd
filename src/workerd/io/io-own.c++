#include "io-own.h"

#include "io-context.h"

#include <workerd/jsg/util.h>

namespace workerd {

#ifdef KJ_DEBUG
Finalizeable::Finalizeable(): context(IoContext::current()) {
  if (context.actor != kj::none) {
    // Disable the destructor check because finalization doesn't apply to actors.
    finalized = true;
  }
}

Finalizeable::~Finalizeable() noexcept(false) {
  KJ_ASSERT(finalized || !context.isFinalized(),
      "Finalizeable object survived request finalization without being finalized. This usually "
      "means it was not passed to IoContext::addObject<T>() as a Finalizeable T.");
}

#endif

void DeleteQueue::scheduleDeletion(OwnedObject* object) const {
  if (IoContext::hasCurrent() && IoContext::current().deleteQueue.queue.get() == this) {
    // Deletion from same thread. No need to enqueue.
    kj::AllowAsyncDestructorsScope scope;
    OwnedObjectList::unlink(*object);
  } else {
    auto lock = crossThreadDeleteQueue.lockExclusive();
    KJ_IF_SOME(state, *lock) {
      state.queue.add(object);
    }
  }
}

void DeleteQueue::scheduleAction(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& action) const {
  KJ_IF_SOME(state, *crossThreadDeleteQueue.lockExclusive()) {
    state.actions.add(kj::mv(action));
    KJ_REQUIRE_NONNULL(state.crossThreadFulfiller)->fulfill();
    return;
  }

  // The queue was deleted, likely because the IoContext was destroyed and the
  // DeleteQueuePtr was invalidated. We are going to emit a warning and drop the
  // actions on the floor without scheduling them.
  if (IoContext::hasCurrent()) {
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
    IoContext::current().logWarning(kj::str(stack));
  }
}

void DeleteQueue::checkFarGet(const DeleteQueue& deleteQueue, const std::type_info& type) {
  IoContext::current().checkFarGet(deleteQueue, type);
}

void DeleteQueue::checkWeakGet(workerd::WeakRef<IoContext>& weak) {
  if (!weak.isValid()) {
    JSG_FAIL_REQUIRE(
        Error, kj::str("Couldn't complete operation because the execution context has ended."));
  }
}

kj::Promise<void> DeleteQueue::resetCrossThreadSignal() const {
  auto lock = crossThreadDeleteQueue.lockExclusive();
  KJ_IF_SOME(state, *lock) {
    KJ_IF_SOME(fulfiller, state.crossThreadFulfiller) {
      // We should only reset the signal if it has been fulfilled.
      KJ_ASSERT(!fulfiller->isWaiting());
    }
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    state.crossThreadFulfiller = kj::mv(paf.fulfiller);
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
  KJ_IF_SOME(f, object->finalizer) {
    if (finalizersRan) {
      KJ_LOG(ERROR, "somehow new objects are being added after finalizers already ran",
          kj::getStackTrace());
      f.finalize();
    }
  }

  object->next = kj::mv(head);
  KJ_IF_SOME(next, object->next) {
    next.get()->prev = &object->next;
  }
  object->prev = &head;
  head = kj::mv(object);
}

kj::Vector<kj::StringPtr> OwnedObjectList::finalize() {
  KJ_ASSERT(!finalizersRan);
  finalizersRan = true;

  kj::Vector<kj::StringPtr> warnings;
  auto* link = &head;
  while (*link != kj::none) {
    auto& l = KJ_ASSERT_NONNULL(*link);
    KJ_IF_SOME(f, l->finalizer) {
      KJ_IF_SOME(w, f.finalize()) {
        warnings.add(w);
      }
#ifdef KJ_DEBUG
      f.finalized = true;
#endif
    }
    link = &l->next;
  }

  return warnings;
}

void IoCrossContextExecutor::execute(jsg::Lock& js, kj::Function<void(jsg::Lock&)>&& func) {
  deleteQueue->scheduleAction(js, kj::mv(func));
}

}  // namespace workerd
