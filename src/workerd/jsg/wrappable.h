// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// This file defines basic helpers involved in wrapping C++ objects for JavaScript consumption,
// including garbage-collecting those objects.

#include <workerd/jsg/wrappable-tag.h>

#include <cppgc/persistent.h>
#include <v8-context.h>
#include <v8-object.h>
#include <v8-version.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/list.h>
#include <kj/refcount.h>
#include <kj/vector.h>

#include <typeinfo>

// Niche value optimization for v8::TracedReference<T>. This teaches kj::Maybe to use
// TracedReference's built-in empty state (IsEmpty()) as the "none" representation, eliminating
// the extra bool + alignment padding that kj::Maybe normally adds. This saves 8 bytes per
// Maybe<TracedReference<T>> instance while preserving full type safety.
namespace kj {
template <typename T>
struct MaybeTraits<v8::TracedReference<T>> {
  static void initNone(v8::TracedReference<T>* ptr) noexcept {
    ctor(*ptr);
  }
  static bool isNone(const v8::TracedReference<T>& ref) noexcept {
    return ref.IsEmpty();
  }
  static constexpr bool noneIsMoveSafe = false;
};
}  // namespace kj

namespace cppgc {
class Visitor;
}

namespace workerd::jsg {

// The ContextPointerSlot enum defines the embedder slots we use in v8::Context for
// storing pointers to various important objects.
enum class ContextPointerSlot : int {
  // Pointer slot 0 is special and should never be used by us.
  RESERVED = 0,
  GLOBAL_WRAPPER = 1,
  MODULE_REGISTRY = 2,
  EXTENDED_CONTEXT_WRAPPER = 3,
  VIRTUAL_FILE_SYSTEM = 4,
  BOOTSTRAP_STATE = 5,
  // Keep the MAX_POINTER_SLOT as the last entry and always set to
  // to the highest value of the other entries. We use this to
  // ensure that the highest used index is always initialized in
  // every context we create without having to update the specific
  // callsites whenever we add a new slot. We can just make the
  // change here.
  MAX_POINTER_SLOT = BOOTSTRAP_STATE,
};

inline void setAlignedPointerInEmbedderData(
    v8::Local<v8::Context> context, ContextPointerSlot slot, void* ptr) {
  // The type tag is a small integer that should be different for every pointer
  // type to avoid type confusion attacks.  We just use the slot index for now,
  // since we have a different pointer type for each slot.
  KJ_DASSERT(slot != ContextPointerSlot::RESERVED, "Attempt to use reserved embedder data slot.");
  context->SetAlignedPointerInEmbedderData(
      static_cast<int>(slot), ptr, static_cast<v8::EmbedderDataTypeTag>(slot));
}

template <typename T>
kj::Maybe<T&> getAlignedPointerFromEmbedderData(
    v8::Local<v8::Context> context, ContextPointerSlot slot) {
  KJ_DASSERT(slot != ContextPointerSlot::RESERVED, "Attempt to use reserved embedder data slot.");
  void* ptr = context->GetAlignedPointerFromEmbedderData(
      static_cast<int>(slot), static_cast<v8::EmbedderDataTypeTag>(slot));
  if (ptr == nullptr) return kj::none;
  return *reinterpret_cast<T*>(ptr);
}

class MemoryTracker;

using kj::uint;

class GcVisitor;
class HeapTracer;
class Object;
class Wrappable;  // Forward declaration for WeakRefAnchor.

// Shared alive/dead flag for weak references to Wrappable objects. Allocated lazily in
// Wrappable when a weak reference is first requested via getOrCreateWeakRefAnchor().
// Automatically invalidated in Wrappable's destructor, so derived types never need to
// manage invalidation.
//
// The anchor itself does NOT store the target pointer — each jsg::WeakRef<T> stores its
// own typed T* alongside a reference to this anchor. This avoids downcasting from the
// privately-inherited Wrappable base class.
class WeakRefAnchor final: public kj::Refcounted {
 public:
  bool isAlive() const {
    return alive;
  }

 private:
  bool alive = true;

  void invalidate() {
    alive = false;
  }

  friend class Wrappable;
};

// Base class for C++ objects which can be "wrapped" for JavaScript consumption. A JavaScript
// "wrapper" object is created, and then the JS wrapper and C++ Wrappable are "attached" to each
// other via attachWrapper().
//
// A Wrappable instance does not necessarily have a wrapper attached. E.g. for JSG_RESOURCE
// types, wrappers are allocated lazily when the object first gets passed into JavaScript.
//
// Wrappable is refcounted via kj::Refcounted. When a JavaScript wrapper exists, it counts as
// a reference, keeping the object alive. When the JS object is garbage-collected, this
// reference is dropped, freeing the C++ object (unless other references exist).
//
// Wrappable also maintains a *second* reference count on the wrapper itself. While the second
// refcount is non-zero, the wrapper (the JavaScript object) will not be allowed to be
// garbage-collected, even if there are no references to it from other JS objects. This is
// important if the C++ object may be re-exported to JavaScript in the future and needs to have
// the same identity at that point (including maintaining any monkey-patches that the script
// may have applied to it previously).
//
// For resource types, this wrapper refcount counts the number of Ref<T>s that point to the
// Wrappable and are not visible to GC tracing.
class Wrappable: public kj::Refcounted {
 public:
  ~Wrappable() noexcept(false) {
    // Invalidate all outstanding jsg::WeakRef<T>s before any derived state is accessed again.
    // This is safe in single-threaded JSG context because no other code can call tryGet() during
    // the destructor call chain.
    KJ_IF_SOME(a, weakRefAnchor) {
      a->invalidate();
    }
  }

  enum InternalFields : int {
    // Field must contain a pointer to `WORKERD_WRAPPABLE_TAG`. This is a workerd-specific
    // tag that helps us to identify a v8 API object as one of our own.
    WRAPPABLE_TAG_FIELD_INDEX,

    // Number of internal fields in a wrapper object.
    //
    // The pointer back to the C++ Wrappable is NOT stored in an internal field: it lives only in
    // V8's CppHeap pointer table, reached via the CppgcShim (see attachWrapper / unwrapFromShim).
    // Keeping a single tagged handle -- rather than a second, independently-corruptible internal
    // field -- is what prevents wrapper type-confusion and use-after-free.
    INTERNAL_FIELD_COUNT,
  };

  // kFirstObjectWrappableTag is the first embedder-assignable wrappable tag. It is valid in both
  // sandbox configurations (workerd uses a single tag consistently for all of its objects).
  static constexpr v8::CppHeapPointerTag WRAPPABLE_TAG =
      v8::CppHeapPointerTag::kFirstObjectWrappableTag;

  // The value pointed to by the internal field field `WRAPPABLE_TAG_FIELD_INDEX`.
  //
  // This value was chosen randomly.
  static constexpr uint16_t WORKERD_WRAPPABLE_TAG = 0xeb04;
  static constexpr uint16_t WORKERD_RUST_WRAPPABLE_TAG = 0xeb05;

  static bool isWorkerdApiObject(v8::Local<v8::Object> object) {
    return object->GetAlignedPointerFromInternalField(WRAPPABLE_TAG_FIELD_INDEX,
               static_cast<v8::EmbedderDataTypeTag>(WRAPPABLE_TAG_FIELD_INDEX)) ==
        &WORKERD_WRAPPABLE_TAG;
  }

  // Does a live JS wrapper currently exist for this Wrappable?
  //
  // False both before the first attachWrapper() and after detachWrapper(), and also during the
  // window in which a major GC has collected the wrapper but the deferred ~CppgcShim has not run
  // yet -- see isCondemned().
  bool hasWrapper() const {
    return weakShim.Get() != nullptr;
  }

  // True when a completed major GC collected this Wrappable's wrapper and cleared `weakShim`,
  // but the ~CppgcShim that will release this Wrappable has not run yet. The Wrappable is still
  // alive and its weak-ref anchor still reports alive, yet it is doomed: the shim holds the last
  // reference to it, and the shim's destructor is only deferred, not cancelled.
  //
  // A Wrappable is a member of HeapTracer::wrappers exactly between attachWrapper() and
  // detachWrapper(), so being linked while having no wrapper isolates precisely the case where
  // cppgc nulled `weakShim` behind our back. That link test is load-bearing, because a null
  // `weakShim` has two producers: cppgc on major GC (this case), and detachWrapper() itself --
  // including the minor-GC path, where V8 drops a droppable wrapper while the Wrappable lives
  // on.
  bool isCondemned() const {
    return link.isLinked() && !hasWrapper();
  }

  // Invalidate all outstanding jsg::WeakRef<T>s pointing at this Wrappable. Called lazily from
  // WeakRef::tryAddRef() when a condemned Wrappable is detected; ~Wrappable() performs the same
  // invalidation for ordinary destruction. Deliberately NOT called from detachWrapper(): that
  // also runs when V8 drops an unmodified droppable wrapper via ResetRoot() and at isolate
  // shutdown, while the Wrappable remains alive and usable — a WeakRef tracks Wrappable
  // lifetime, not wrapper lifetime.
  void invalidateWeakRefs() {
    KJ_IF_SOME(a, weakRefAnchor) {
      a->invalidate();
    }
  }

  // Act on a condemned Wrappable (see isCondemned()): invalidate outstanding weak refs and bump
  // the isolate's condemned counter (see HeapTracer::getCondemnedWrapperCount()). Called from
  // WeakRef::tryAddRef(), which must refuse to promote such a Wrappable.
  inline void condemn();

  void addStrongRef();
  void removeStrongRef();
  uint getStrongRefcount() const {
    return strongRefcount;
  }

  // Called by jsg::Ref<T> to ensure that its Wrappable is destroyed under the isolate lock.
  // `ownSelf` keeps the raw `self` pointer valid -- they are passed separately because Wrappable is
  // a private base class of the object.
  void maybeDeferDestruction(bool strong, kj::Own<void> ownSelf, Wrappable* self);

  v8::Local<v8::Object> getHandle(v8::Isolate* isolate);

  // This Wrappable's JS wrapper, or null if none currently exists. Reaches the handle through
  // `weakShim`, so a wrapper the GC has already collected reads as absent rather than as a
  // zapped handle.
  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate);

  // Visits a Ref<T> pointing at this Wrappable. `refParent` and `refStrong` are the members of
  // `Ref<T>`, and this method is invoked on the object the ref points at. (This avoids the need
  // to templatize the implementation of this method.)
  void visitRef(GcVisitor& visitor, kj::Maybe<Wrappable&>& refParent, bool& refStrong);

  // Attach to a JavaScript object. This increments the Wrappable's refcount until `object`
  // is garbage-collected (or unlink() is called).
  //
  // The object MUST have exactly INTERNAL_FIELD_COUNT internal field slots. Internal field 0 holds
  // the workerd-API marker (see isWorkerdApiObject); the C++ object pointer itself is stored in
  // V8's CppHeap pointer table rather than an internal field.
  //
  // If `needsGcTracing` is true, then the virtual method jsgVisitForGc() will be called to
  // perform GC tracing. If false, the method is never called (may be more efficient, if the
  // method does nothing anyway).
  //
  // `tag` is the per-type CppHeapPointerTag identifying the wrapped object's type. It is stored
  // in V8's CppHeap pointer table (via Object::Wrap) and range-checked at unwrap time to reject
  // type-confused handles. Resource types pass their own tag (TypeWrapper::wrappableTag<T>());
  // non-resource wrappables (functions, opaque wrappers) pass kNonResourceWrappableTag.
  void attachWrapper(v8::Isolate* isolate,
      v8::Local<v8::Object> object,
      bool needsGcTracing,
      v8::CppHeapPointerTag tag);

  // Attach an empty, null-prototype object as the wrapper.
  v8::Local<v8::Object> attachOpaqueWrapper(v8::Local<v8::Context> context, bool needsGcTracing);

  // If `handle` was originally returned by attachOpaqueWrapper(), return the Wrappable it wraps.
  // Otherwise, return nullptr.
  static kj::Maybe<Wrappable&> tryUnwrapOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle);

  // If this is a `jsg::Object`, return it, otherwise null. `Object` inherits `Wrappable`
  // privately, which makes it impossible to `dynamic_cast` from a `Wrappable` to an `Object` (or
  // to any of its subclasses); this hands the caller a pointer it can cast from. Named with the
  // `jsg` prefix because it pollutes the namespace of JSG_RESOURCE types.
  virtual Object* jsgTryGetObject() {
    return nullptr;
  }

  // Perform GC visitation. This is named with the `jsg` prefix because it pollutes the
  // namespace of JSG_RESOURCE types.
  virtual void jsgVisitForGc(GcVisitor& visitor);

  virtual kj::StringPtr jsgGetMemoryName() const {
    KJ_UNIMPLEMENTED("jsgGetTypeName is not implemented. "
                     "It must be overridden by subclasses");
  }

  virtual size_t jsgGetMemorySelfSize() const {
    KJ_UNIMPLEMENTED("jsgGetMemorySelfSize is not implemented. "
                     "It must be overridden by subclasses");
  }

  virtual void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

  virtual bool jsgGetMemoryInfoIsRootNode() const {
    return strongRefcount > 0;
  }

  virtual v8::Local<v8::Object> jsgGetMemoryInfoWrapperObject(v8::Isolate* isolate) {
    KJ_IF_SOME(handle, tryGetHandle(isolate)) {
      return handle;
    }
    return v8::Local<v8::Object>();
  }

  // Detaches the wrapper from V8 and returns the reference that V8 had previously held.
  // (Typically, the caller will ignore the return value, thus dropping the reference.)
  kj::Own<Wrappable> detachWrapper(bool shouldFreelistShim);

  // Given a JS wrapper object, unwrap it back to its Wrappable, requiring that the object's tag
  // (stored in V8's CppHeap pointer table by attachWrapper) lies within `tagRange`. Returns
  // nullptr if the object was never wrapped or its tag is outside the range (i.e. a type-confused
  // or forged handle). Dispatch goes through the CppgcShim's owning reference, so the pointer
  // returned is always the lifetime-correct one.
  static Wrappable* unwrapFromShim(
      v8::Isolate* isolate, v8::Local<v8::Object> object, v8::CppHeapPointerTagRange tagRange);

  // Like unwrapFromShim(), but accepts any JSG wrappable tag. For call sites that have already
  // established the object's type by another means (a prototype-chain check, or an
  // isWorkerdApiObject() marker check plus a dynamic-type lookup) and only need the pointer.
  // Returns nullptr if the object was never wrapped.
  static Wrappable* unwrapFromShimAnyType(v8::Isolate* isolate, v8::Local<v8::Object> object);

  // Unwrap an object already known to be one of our wrappers (the caller has done a prototype-chain
  // check for `tagRange`'s type), requiring its stored tag to fall within `tagRange`, and abort if
  // it does not.
  //
  // The prototype-chain check the caller performed accepts exactly the objects whose own map was
  // built from this type's template -- i.e. genuine, JSG-wrapped instances of the type or a
  // subclass, all of whose tags fall in `tagRange`. The only way to reach here with an out-of-range
  // tag is therefore a wrapper whose CppHeap handle or shim tag was corrupted in-sandbox, which is a
  // memory-safety violation. We abort so it is surfaced rather than silently ignored; returning a
  // pointer would risk type confusion, and returning "not found" would hide the attack.
  //
  // The range check is done in C++ (after unwrapping with the full JSG tag range) rather than by
  // passing `tagRange` to Object::Unwrap, because a checked V8 build turns a narrow-range miss into
  // its own fatal DCHECK before returning, producing a different crash than the abort here.
  static Wrappable* unwrapFromShimInRangeOrAbort(
      v8::Isolate* isolate, v8::Local<v8::Object> object, v8::CppHeapPointerTagRange tagRange);

  // Called by HeapTracer when V8 tells us that it found a reference to this object.
  void traceFromV8(cppgc::Visitor& cppgcVisitor);

 private:
  class CppgcShim;

  // The shim owning this Wrappable's wrapper handle, or null when no live wrapper exists.
  // Downcasts `weakShim`; see its comment for why that is stored as the cppgc base type.
  kj::Maybe<CppgcShim&> getShim() const;

  // The body of detachWrapper(), taking the shim explicitly. ~CppgcShim must use this: on the
  // major-GC path cppgc has already cleared `weakShim`, so getShim() would find nothing.
  kj::Own<Wrappable> detachFromShim(CppgcShim& shim, bool shouldFreelistShim);

  // The cppgc shim owning this Wrappable's JS wrapper, or null when no live wrapper exists.
  //
  // The wrapper handle itself lives in the shim, not here. The wrapper is created lazily when the
  // Wrappable is first exported to JavaScript; until then this is null.
  //
  // If the wrapper is "unmodified" from its original creation state, then V8 may choose to
  // collect it even when the Wrappable could still technically be reached from C++. The idea here
  // is that if the Wrappable is returned to JavaScript again later, the wrapper can be
  // reconstructed at that time. However, if the wrapper is modified by the application (e.g.
  // monkey-patched with a new property), then collecting and recreating it won't work. The logic
  // to decide if a wrapper has been "modified" is internal to V8 and baked into its use of
  // EmbedderRootsHandler.
  //
  // The reference is weak so that the GC, not us, decides when it goes away. When a major GC
  // collects the wrapper, cppgc nulls this in MarkerBase::ProcessWeakness() -- during the same
  // atomic pause that zaps the wrapper's traced node, and long before the deferred ~CppgcShim
  // runs. Every reader therefore observes "no wrapper" as soon as the GC knows it, instead of
  // reaching a zapped handle through a bare pointer.
  //
  // Declared as the cppgc base type because CppgcShim is defined in wrappable.c++ while
  // cppgc::WeakPersistent needs a complete type. Only CppgcShim instances are ever stored here;
  // getShim() does the downcast.
  cppgc::WeakPersistent<v8::Object::Wrappable> weakShim;

  // Whenever there are non-GC-traced references to this Wrappable (i.e. from other C++ objects,
  // i.e. strongRefcount > 0) and a wrapper exists, `strongWrapper` contains a copy of the wrapper
  // handle, to force the wrapper to stay alive. Otherwise, `strongWrapper` is empty.
  v8::Global<v8::Object> strongWrapper;

  // Will be non-null if a wrapper has ever been attached.
  v8::Isolate* isolate = nullptr;

  // How many strong Ref<T>s point at this Wrappable, forcing its wrapper to stay alive even if
  // GC tracing doesn't find it?
  //
  // Whenever the value of the boolean expression (strongRefcount > 0 && !hasWrapper()) changes, a
  // GC visitation is needed to update all outgoing refs. The four places that can change it --
  // attachWrapper(), detachWrapper(), addStrongRef() and removeStrongRef() -- each run one.
  //
  // cppgc clearing `weakShim` behind our back does not count, and needs no visitation: clearing
  // requires the wrapper to have been collected, which requires `strongWrapper` to be empty (see
  // the assert in ~CppgcShim), which means strongRefcount is 0 -- so the expression is false both
  // before and after. In other words, a wrapper can only be condemned while strongRefcount == 0.
  uint strongRefcount = 0;

  // While a wrapper is attached, the Wrappable is a member of the list `HeapTracer::wrappers`.
  kj::ListLink<Wrappable> link;

  // Lazy-allocated shared state for jsg::WeakRef<T>. Zero overhead for Wrappables that never
  // have weak references taken. Created on first call to getOrCreateWeakRefAnchor().
  kj::Maybe<kj::Rc<WeakRefAnchor>> weakRefAnchor;

  // Returns (or creates) the shared WeakRefAnchor for this Wrappable. Used by
  // Ref<T>::getWeakRef().
  kj::Rc<WeakRefAnchor> getOrCreateWeakRefAnchor() {
    KJ_IF_SOME(a, weakRefAnchor) {
      return a.addRef();
    }
    auto a = kj::rc<WeakRefAnchor>();
    weakRefAnchor = a.addRef();
    return a;
  }

  friend class Object;
  friend class GcVisitor;
  friend class HeapTracer;
  friend class MemoryTracker;
  template <typename>
  friend class Ref;
};

// For historical reasons, this is actually implemented in setup.c++.
class HeapTracer: public v8::EmbedderRootsHandler {
 public:
  explicit HeapTracer(v8::Isolate* isolate);

  ~HeapTracer() noexcept {
    // Destructor has to be noexcept because it inherits from a V8 type that has a noexcept
    // destructor.
    KJ_IREQUIRE(isolate == nullptr, "you must call HeapTracer.destroy()");
  }

  // Call under isolate lock when shutting down isolate.
  void destroy();

  static HeapTracer& getTracer(v8::Isolate* isolate);

  // Returns true if the current thread is currently executing the destructor of a CppgcShim
  // object, which implies that we are collecting unreachable objects.
  static bool isInCppgcDestructor();

  void addWrapper(kj::Badge<Wrappable>, Wrappable& wrappable) {
    wrappers.add(wrappable);
  }
  void removeWrapper(kj::Badge<Wrappable>, Wrappable& wrappable) {
    wrappers.remove(wrappable);
  }
  void clearWrappers();

  void addToFreelist(Wrappable::CppgcShim& shim);
  Wrappable::CppgcShim* allocateShim(Wrappable& wrappable, v8::CppHeapPointerTag tag);
  void clearFreelistedShims();
  static void substituteCppgcShimForTest(
      v8::Isolate* isolate, v8::Local<v8::Object> target, v8::Local<v8::Object> source);

 private:
  // Returns the head slot of the intrusive freelist for `tag`. The slot has a stable address (it
  // lives in the fixed-size freelistedShimsByTag array), so freelisted shims may point back to it.
  kj::Maybe<Wrappable::CppgcShim&>& freelistHeadFor(v8::CppHeapPointerTag tag);

 public:
  // Number of times WeakRef::tryAddRef() has detected a condemned target in this isolate, i.e.
  // the number of times the dangling-wrapper hazard has actually been caught rather
  // than merely guarded against.
  //
  // This exists so that the regression test can assert it reached the hazard. The window is
  // only reachable under a *natural* major GC: a forced GC (v8::Isolate::
  // RequestGarbageCollectionForTesting, and hence --gc-stress) sweeps atomically, so cppgc
  // runs ~CppgcShim inside the GC and no deferred window exists to observe. That makes the
  // test inherently probabilistic, and without this counter a pass would be
  // indistinguishable from never having exercised the code path at all.
  //
  // Not test-only: the increment sits on an already-cold path (a WeakRef promotion that is
  // about to fail), so it costs nothing in production, and the count is useful for
  // diagnosing how often this occurs in the wild.
  uint64_t getCondemnedWrapperCount() const {
    return condemnedWrapperCount;
  }

  // implements EmbedderRootsHandler -------------------------------------------
  void ResetRoot(const v8::TracedReference<v8::Value>& handle) override;
  bool TryResetRoot(const v8::TracedReference<v8::Value>& handle) override;

  kj::StringPtr jsgGetMemoryName() const {
    return "HeapTracer"_kjc;
  }
  size_t jsgGetMemorySelfSize() const {
    return sizeof(*this);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  bool jsgGetMemoryInfoIsRootNode() const {
    return false;
  }

 private:
  v8::Isolate* isolate;
  kj::Vector<Wrappable*> wrappersToTrace;

  // Wrappables on which detachWrapper() should be called at the end of this GC pass.
  kj::Vector<Wrappable*> detachLater;

  // List of all Wrappables for which a JavaScript wrapper exists.
  kj::List<Wrappable, &Wrappable::link> wrappers;

  // Freelists of shim objects for wrappers that were collected during a minor GC. The shim
  // objects can be reused for future allocations before the next major GC.
  //
  // A shim's single CppHeap-pointer-table entry carries its object's per-type tag, written once at
  // Wrap time. When a shim is recycled it is re-Wrapped, which would rewrite that tag -- so a shim
  // must only ever be reused for the same type across its whole lifetime, or a stale wrapper
  // handle for an old occupant could pass the tag check against a new occupant of a different
  // type. We therefore partition the freelist by tag: each bucket only ever holds shims that last
  // served (and will next serve) that tag.
  //
  // A fixed-size array indexed by dense bucket index (wrappableTagBucketIndex(tag)). Each element
  // is the head of an intrusive LIFO list; freelisted shims store a raw pointer back to their head
  // slot, so the array elements must have stable addresses -- hence a plain array rather than a
  // grown-on-demand vector. Sized by kMaxWrappableTags, which is enforced at wrap time by a
  // static_assert in TypeWrapper::wrappableTag<T>().
  kj::Maybe<Wrappable::CppgcShim&> freelistedShimsByTag[kMaxWrappableTags] = {};

  // See getCondemnedWrapperCount(). Plain (non-atomic) because it is only ever touched from
  // Wrappable::condemn(), which runs under the isolate lock.
  uint64_t condemnedWrapperCount = 0;

  friend class Wrappable;
};

inline void Wrappable::condemn() {
  // Only reachable from isCondemned() returning true, which implies this Wrappable is still
  // linked into HeapTracer::wrappers, which implies attachWrapper() set `isolate`.
  KJ_DASSERT(isolate != nullptr);
  ++HeapTracer::getTracer(isolate).condemnedWrapperCount;
  invalidateWeakRefs();
}

void substituteCppgcShimForTest(
    v8::Isolate* isolate, v8::Local<v8::Object> target, v8::Local<v8::Object> source);
void detachWrapperForTest(v8::Isolate* isolate, v8::Local<v8::Object> object);
void resetRootForTest(v8::Isolate* isolate, v8::Local<v8::Object> object);
bool sharesCppgcShimForTest(
    v8::Isolate* isolate, v8::Local<v8::Object> first, v8::Local<v8::Object> second);

// Try to use this in any scope where JavaScript wrapped objects are destroyed, to confirm that
// they don't hold disallowed references to KJ I/O objects. IoOwn's destructor will explicitly
// create AllowAsyncDestructorsScope to permit holding such objects via IoOwn. This is meant to
// help catch bugs.
#define DISALLOW_KJ_IO_DESTRUCTORS_SCOPE                                                           \
  kj::DisallowAsyncDestructorsScope disallow(                                                      \
      "JavaScript heap objects must not contain KJ I/O objects without a IoOwn")
// TODO(soon):
// - Track memory usage of native objects.

// Log the types involved in a failed downcast and abort.  Out-of-line and not
// templated, to keep the code that unwrappers inline as small as possible.
[[noreturn]] void reportWrapperTypeMismatch(
    const std::type_info& expected, const std::type_info& actual);
[[noreturn]] void reportWrapperIdentityMismatch();

// Cast an `Object` that came from a wrapper to the type the callsite expects,
// aborting if the object is not of that type.
//
// In-sandbox corruption may allow an attacker to confuse types.  An object's
// RTTI lives outside the V8 sandbox, out of reach of such a substitution, so
// use that to confirm the type instead.
//
// Defined in jsg.h, where `Object` is complete.
template <typename T>
T& downcastObject(Object& object);

// Cast a `Wrappable` taken from a wrapper's internal field to the type the
// callsite expects, aborting if the object is not of that type.
template <typename T>
T& downcastWrappable(Wrappable& wrappable) {
  if constexpr (kj::canConvert<T&, Object&>()) {
    // A `dynamic_cast` cannot start from `Wrappable`: the runtime check is
    // specified to succeed only through a public base, so a
    // privately-inherited one fails regardless of what access the callsite
    // has. Every resource type reaches `T` through the public `Object` base,
    // so start from there. This also lands on the correct address for a `T`
    // that inherits `Object` at a non-zero offset, which a `reinterpret_cast`
    // would not.
    Object* object = wrappable.jsgTryGetObject();
    if (object == nullptr) {
      reportWrapperTypeMismatch(typeid(T), typeid(wrappable));
    }
    return downcastObject<T>(*object);
  } else {
    // Wrappables that are not `Object`s -- `WrappableFunction` and
    // `OpaqueWrappable` -- inherit `Wrappable` publicly, so they can be cast
    // directly.
    const auto& actualType = typeid(wrappable);
    if (&actualType == &typeid(T) || actualType == typeid(T)) {
      return static_cast<T&>(wrappable);
    }
    T* result = dynamic_cast<T*>(&wrappable);
    if (result == nullptr) {
      reportWrapperTypeMismatch(typeid(T), actualType);
    }
    return *result;
  }
}

// Given a handle to a resource type, extract the raw C++ object pointer.
//
// `tagRange` is the set of CppHeapPointerTags that the receiver's static type accepts: the
// receiver type's own tag through the tags of all its subclasses. If the wrapper's tag is outside
// this range -- i.e. the handle names an object of an unrelated type -- unwrapping fails,
// signalling a type-confusion attempt. Callers compute the range via
// TypeWrapper::wrappableTagRange<T>() (resource types) or kNonResourceWrappableTag (functions and
// opaque wrappers).
template <typename T, bool isContext>
T& extractInternalPointer(v8::Isolate* isolate,
    const v8::Local<v8::Context>& context,
    const v8::Local<v8::Object>& object,
    v8::CppHeapPointerTagRange tagRange) {
  // Due to bugs in V8, we can't use internal fields on the global object:
  //   https://groups.google.com/d/msg/v8-users/RET5b3KOa5E/3EvpRBzwAQAJ
  //
  // So, when wrapping a global object, we store the pointer in the "embedder data" of the context
  // instead of the internal fields of the object. The global is a per-context singleton and is
  // not reached through the tagged CppHeap-pointer path, so the tag range does not apply to it.

  if constexpr (isContext) {
    // V8 docs say EmbedderData slot 0 is special, so we use slot 1. (See comments in newContext().)
    return KJ_ASSERT_NONNULL(
        getAlignedPointerFromEmbedderData<T>(context, ContextPointerSlot::GLOBAL_WRAPPER));
  } else {
    // A tag outside the range for the type this dispatch site expects is a memory-safety violation
    // (an object whose CppHeap handle has been redirected to an unrelated type), so this aborts
    // rather than throwing, which JSG would catch and turn into a recoverable JS exception.
    Wrappable* ptr = Wrappable::unwrapFromShimInRangeOrAbort(isolate, object, tagRange);
    return downcastWrappable<T>(*ptr);
  }
}

// Convenience wrapper around extractInternalPointer for resource types, filling in the accepted
// tag range from TypeWrapper::wrappableTagRange<T>() so dispatch sites don't each repeat it. The
// range is an isolate-wide value that only TypeWrapper can compute, so it can't be a default
// argument on extractInternalPointer itself (which lives here, where TypeWrapper is not visible).
template <typename TypeWrapper, typename T, bool isContext>
T& extractInternalPointerFor(v8::Isolate* isolate,
    const v8::Local<v8::Context>& context,
    const v8::Local<v8::Object>& object) {
  return extractInternalPointer<T, isContext>(
      isolate, context, object, TypeWrapper::template wrappableTagRange<T>());
}

}  // namespace workerd::jsg
