#include "io-own.h"
#include "io-context.h"

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
  if (IoContext::hasCurrent() && IoContext::current().deleteQueue.get() == this) {
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

void DeleteQueue::checkFarGet(const DeleteQueue* deleteQueue) {
  IoContext::current().checkFarGet(deleteQueue);
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

}  // namespace workerd
