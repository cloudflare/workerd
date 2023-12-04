#pragma once

#include <kj/common.h>
#include <kj/mutex.h>
#include <kj/refcount.h>
#include <kj/string.h>
#include <kj/vector.h>

namespace workerd {

class IoContext;

template <typename T> class IoOwn;
template <typename T> class IoPtr;

template <typename T>
struct RemoveIoOwn_ { typedef T Type; static constexpr bool is = false; };
template <typename T>
struct RemoveIoOwn_<IoOwn<T>> { typedef T Type; static constexpr bool is = true; };

template <typename T>
constexpr bool isIoOwn() { return RemoveIoOwn_<T>::is; }
template <typename T>
using RemoveIoOwn = typename RemoveIoOwn_<T>::Type;

// If an object passed to addObject(Own<T>) implements Finalizeable, then once it is known to
// be the case that no code will ever run in the context of this IoContext again,
// finalize() will be called.
//
// This is primarily used to proactively fail out hanging promises once we know they can never
// be fulfilled, so that requests fail fast rather than hang forever.
//
// Finalizers should NOT call into JavaScript or really do much of anything except for calling
// reject() on some Fulfiller object. It can optionally return a warning which should be
// logged if the inspector is attached.
class Finalizeable {
public:
  KJ_DISALLOW_COPY_AND_MOVE(Finalizeable);

#ifdef KJ_DEBUG
  Finalizeable();
  ~Finalizeable() noexcept(false);
  // In debug mode, we assert that this object was actually finalized. A Finalizeable object that
  // doesn't get finalized typically arises when a derived class multiply-inherits from
  // Finalizeable and some other non-Finalizeable class T, then gets passed to
  // `IoContext::addObject()` as a T. This can be a source of baffling bugs.
#else
  Finalizeable() = default;
#endif

private:
  virtual kj::Maybe<kj::StringPtr> finalize() = 0;
  friend class IoContext;

#ifdef KJ_DEBUG
  IoContext& context;

  // Set true by IoContext::runFinalizers();
  bool finalized = false;
#endif

  friend class OwnedObjectList;
};

struct OwnedObject {
  kj::Maybe<kj::Own<OwnedObject>> next;
  kj::Maybe<kj::Own<OwnedObject>>* prev;
  kj::Maybe<Finalizeable&> finalizer;
};

template <typename T>
struct SpecificOwnedObject: public OwnedObject {
  SpecificOwnedObject(kj::Own<T> ptr): ptr(kj::mv(ptr)) {}
  kj::Own<T> ptr;
};

class OwnedObjectList {
public:
  OwnedObjectList() = default;
  KJ_DISALLOW_COPY_AND_MOVE(OwnedObjectList);
  ~OwnedObjectList() noexcept(false);

  void link(kj::Own<OwnedObject> object);
  static void unlink(OwnedObject& object);

  // Runs the finalizer for each object in forward order and returns a vector of any warnings
  // returned from those finalizers.
  kj::Vector<kj::StringPtr> finalize();

  bool isFinalized() {
    return finalizersRan;
  }

private:
  kj::Maybe<kj::Own<OwnedObject>> head;

  bool finalizersRan = false;
};

// Object which receives possibly-cross-thread deletions of owned objects.
class DeleteQueue: public kj::AtomicRefcounted {
public:
  DeleteQueue()
      : crossThreadDeleteQueue(State { kj::Vector<OwnedObject*>() }) {}

  void scheduleDeletion(OwnedObject* object) const;

  struct State {
    kj::Vector<OwnedObject*> queue;
  };

  // Pointers from IoOwns that were dropped in other threads, and therefore should be deleted
  // whenever the IoContext gets around to it. The maybe is changed to kj::none when the
  // IoContext goes away, at which point all OwnedObjects have already been deleted so
  // cross-thread deletions can just be ignored.
  kj::MutexGuarded<kj::Maybe<State>> crossThreadDeleteQueue;

  // Implements the corresponding methods of IoContext and ActorContext.
  template <typename T> IoOwn<T> addObject(kj::Own<T> obj, OwnedObjectList& ownedObjects);

  static void checkFarGet(const DeleteQueue* deleteQueue);
};

template <typename T>
inline IoOwn<T> DeleteQueue::addObject(
    kj::Own<T> obj, OwnedObjectList& ownedObjects) {
  auto& ref = *obj;

  // HACK: We need an Own<OwnedObject>, but we actually need to allocate it as the subclass
  //   SpecificOwnedObject<T>. OwnedObject is not polymorphic, which means kj::Own will refuse
  //   to upcast kj::Own<SpecificOwnedObject<T>> to kj::Own<OwnedObject> since it can't guarantee
  //   the disposers are compatible. However, since we're only using single inheritance here, the
  //   disposers *are* compatible (the numeric value of pointers to SpecificOwnedObject<T> and
  //   its parent OwnedObject are equal). So, instead of forcing OwnedObject to be polymorphic
  //   (which would have forced a bunch of useless vtables and vtable pointers)... I'm manually
  //   constructing the kj::Own<> using a disposer that I know is compatible.
  // TODO(cleanup): Can KJ be made to support this use case?
  kj::Own<OwnedObject> ownedObject(
      new SpecificOwnedObject<T>(kj::mv(obj)),
      kj::_::HeapDisposer<SpecificOwnedObject<T>>::instance);

  if constexpr (kj::canConvert<T&, Finalizeable&>()) {
    ownedObject->finalizer = ref;
  }

  IoOwn<T> result(kj::atomicAddRef(*this),
      static_cast<SpecificOwnedObject<T>*>(ownedObject.get()));
  ownedObjects.link(kj::mv(ownedObject));
  return result;
}

// When the IoContext is destroyed, we need to null out the DeleteQueue. Complicating
// matters a bit, we need to cancel all tasks (destroy the TaskSet) before this happens, so
// we can't just do it in IoContext's destrucrtor. As a hack, we customize our pointer
// to the delete queue to get the tear-down order right.
class DeleteQueuePtr: public kj::Own<DeleteQueue> {
public:
  DeleteQueuePtr(kj::Own<DeleteQueue> value)
      : kj::Own<DeleteQueue>(kj::mv(value)) {}
  KJ_DISALLOW_COPY_AND_MOVE(DeleteQueuePtr);
  ~DeleteQueuePtr() noexcept(false) {
    auto ptr = get();
    if (ptr != nullptr) {
      *ptr->crossThreadDeleteQueue.lockExclusive() = kj::none;
    }
  }
};

// Owned pointer held by a V8 heap object, pointing to a KJ event loop object. Cannot be
// dereferenced unless the isolate is executing on the appropriate event loop thread.
template <typename T>
class IoOwn {

public:
  IoOwn(IoOwn&& other);
  IoOwn(decltype(nullptr)): item(nullptr) {}
  ~IoOwn() noexcept(false);
  KJ_DISALLOW_COPY(IoOwn);

  T* operator->();
  T& operator*() { return *operator->(); }
  operator kj::Own<T>() &&;
  IoOwn& operator=(IoOwn&& other);
  IoOwn& operator=(decltype(nullptr));

  // Releases this object from the IoOwn, but instead of deleting it, attaches it to the
  // IoContext (or ActorContext) such that it won't be destroyed until that context is torn
  // down.
  //
  // This may need to be used in cases where an application could directly observe the destruction
  // of this object. If that's the case, then the object cannot be destroyed during GC, as this
  // would let the application observe GC, which might enable side channels. So, the destructor
  // of the owning object must manually call `deferGcToContext()` to pass all such objects away
  // to their respective contexts.
  //
  // Since this is expected to be called during GC, it is safe to call from a thread other than
  // the one that owns the IoContext.
  void deferGcToContext() &&;

private:
  friend class IoContext;
  friend class DeleteQueue;

  kj::Own<const DeleteQueue> deleteQueue;
  SpecificOwnedObject<T>* item;

  IoOwn(kj::Own<const DeleteQueue> deleteQueue,
        SpecificOwnedObject<T>* item)
      : deleteQueue(kj::mv(deleteQueue)), item(item) {}
};

// Reference held by a V8 heap object, pointing to a KJ event loop object. Cannot be
// dereferenced unless the isolate is executing on the appropriate event loop thread.
template <typename T>
class IoPtr {
public:
  IoPtr(const IoPtr& other)
      : deleteQueue(kj::atomicAddRef(*other.deleteQueue)), ptr(other.ptr) {}
  IoPtr(IoPtr&& other) = default;

  T* operator->();
  T& operator*() { return *operator->(); }
  IoPtr& operator=(decltype(nullptr));

private:
  friend class IoContext;
  friend class DeleteQueue;

  kj::Own<const DeleteQueue> deleteQueue;
  T* ptr;

  IoPtr(kj::Own<const DeleteQueue> deleteQueue,
         T* ptr)
      : deleteQueue(kj::mv(deleteQueue)), ptr(ptr) {}
};

template <typename T>
IoOwn<T>::IoOwn(IoOwn&& other)
    : deleteQueue(kj::mv(other.deleteQueue)),
      item(other.item) {
  other.item = nullptr;
}

template <typename T>
IoOwn<T>::~IoOwn() noexcept(false) {
  if (item != nullptr) {
    deleteQueue->scheduleDeletion(item);
  }
}

template <typename T>
IoOwn<T>& IoOwn<T>::operator=(IoOwn<T>&& other) {
  if (item != nullptr) {
    deleteQueue->scheduleDeletion(item);
  }
  deleteQueue = kj::mv(other.deleteQueue);
  item = other.item;
  other.item = nullptr;
  return *this;
}

template <typename T>
IoOwn<T>& IoOwn<T>::operator=(decltype(nullptr)) {
  if (item != nullptr) {
    deleteQueue->scheduleDeletion(item);
  }
  deleteQueue = nullptr;
  item = nullptr;
  return *this;
}

template <typename T>
void IoOwn<T>::deferGcToContext() && {
  // Turns out, if we simply *don't* enqueue the item for deletion, we get the behavior we want.
  // So we can just null out the pointers here...
  item = nullptr;
  deleteQueue = nullptr;
}

template <typename T>
IoPtr<T>& IoPtr<T>::operator=(decltype(nullptr)) {
  deleteQueue = nullptr;
  ptr = nullptr;
  return *this;
}

template <typename T>
inline T* IoOwn<T>::operator->() {
  DeleteQueue::checkFarGet(deleteQueue);
  return item->ptr;
}

template <typename T>
inline IoOwn<T>::operator kj::Own<T>() && {
  DeleteQueue::checkFarGet(deleteQueue);
  auto result = kj::mv(item->ptr);
  OwnedObjectList::unlink(*item);
  item = nullptr;
  deleteQueue = nullptr;  // not needed anymore, might as well drop the refcount
  return result;
}

template <typename T>
inline T* IoPtr<T>::operator->() {
  DeleteQueue::checkFarGet(deleteQueue);
  return ptr;
}

}  // namespace workerd
