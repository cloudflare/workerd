// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// This file defines basic helpers involved in wrapping C++ objects for JavaScript consumption,
// including garbage-collecting those objects.

#include <v8-context.h>
#include <v8-object.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/list.h>
#include <kj/refcount.h>
#include <kj/vector.h>

namespace cppgc {
class Visitor;
}

namespace workerd::jsg {

class MemoryTracker;

using kj::uint;

class GcVisitor;
class HeapTracer;

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
  enum InternalFields : int {
    // Field must contain a pointer to `WORKERD_WRAPPABLE_TAG`. This is a workerd-specific
    // tag that helps us to identify a v8 API object as one of our own.
    WRAPPABLE_TAG_FIELD_INDEX,

    // Index of the internal field that points back to the `Wrappable`.
    WRAPPED_OBJECT_FIELD_INDEX,

    // Number of internal fields in a wrapper object.
    INTERNAL_FIELD_COUNT,
  };

  static constexpr v8::CppHeapPointerTag WRAPPABLE_TAG = v8::CppHeapPointerTag::kDefaultTag;

  // The value pointed to by the internal field field `WRAPPABLE_TAG_FIELD_INDEX`.
  //
  // This value was chosen randomly.
  static constexpr uint16_t WORKERD_WRAPPABLE_TAG = 0xeb04;

  static bool isWorkerdApiObject(v8::Local<v8::Object> object) {
    return object->GetAlignedPointerFromInternalField(WRAPPABLE_TAG_FIELD_INDEX) ==
        &WORKERD_WRAPPABLE_TAG;
  }

  void addStrongRef();
  void removeStrongRef();

  // Called by jsg::Ref<T> to ensure that its Wrappable is destroyed under the isolate lock.
  // `ownSelf` keeps the raw `self` pointer valid -- they are passed separately because Wrappable is
  // a private base class of the object.
  void maybeDeferDestruction(bool strong, kj::Own<void> ownSelf, Wrappable* self);

  v8::Local<v8::Object> getHandle(v8::Isolate* isolate);

  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) {
    return wrapper.map([&](v8::TracedReference<v8::Object>& ref) { return ref.Get(isolate); });
  }

  // Visits a Ref<T> pointing at this Wrappable. `refParent` and `refStrong` are the members of
  // `Ref<T>`, and this method is invoked on the object the ref points at. (This avoids the need
  // to templatize the implementation of this method.)
  void visitRef(GcVisitor& visitor, kj::Maybe<Wrappable&>& refParent, bool& refStrong);

  // Attach to a JavaScript object. This increments the Wrappable's refcount until `object`
  // is garbage-collected (or unlink() is called).
  //
  // The object MUST have exactly 2 internal field slots, which will be initialized by this
  // call as follows:
  // - Internal field 0 is special and is used by the GC tracing implementation.
  // - Internal field 1 is set to a pointer to the Wrappable. It can be used to unwrap the
  //   object.
  //
  // If `needsGcTracing` is true, then the virtual method jsgVisitForGc() will be called to
  // perform GC tracing. If false, the method is never called (may be more efficient, if the
  // method does nothing anyway).
  void attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object, bool needsGcTracing);

  // Attach an empty object as the wrapper.
  v8::Local<v8::Object> attachOpaqueWrapper(v8::Local<v8::Context> context, bool needsGcTracing);

  // If `handle` was originally returned by attachOpaqueWrapper(), return the Wrappable it wraps.
  // Otherwise, return nullptr.
  static kj::Maybe<Wrappable&> tryUnwrapOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle);

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

  // Called by HeapTracer when V8 tells us that it found a reference to this object.
  void traceFromV8(cppgc::Visitor& cppgcVisitor);

 private:
  class CppgcShim;

  // If a JS wrapper is currently allocated, this point to the cppgc shim object.
  kj::Maybe<CppgcShim&> cppgcShim;

  // Handle to the JS wrapper object. The wrapper is created lazily when the object is first
  // exported to JavaScript; until then, the wrapper is empty.
  //
  // If the wrapper object is "unmodified" from its original creation state, then V8 may choose to
  // collect it even when it could still technically be reached via C++ objects. The idea here is
  // that if the object is returned to JavaScript again later, the wrapper can be reconstructed at
  // that time. However, if the wrapper is modified by the application (e.g. monkey-patched with
  // a new property), then collecting and recreating it won't work. The logic to decide if an
  // object has been "modified" is internal to V8 and baked into its use of EmbedderRootsHandler.
  kj::Maybe<v8::TracedReference<v8::Object>> wrapper;

  // Whenever there are non-GC-traced references to the object (i.e. from other C++ objects, i.e.
  // strongRefcount > 0), and `wrapper` is non-null, then `strongWrapper` contains a copy of
  // `wrapper`, to force it to stay alive. Otherwise, `strongWrapper` is empty.
  v8::Global<v8::Object> strongWrapper;

  // Will be non-null if `wrapper` has ever been non-null.
  v8::Isolate* isolate = nullptr;

  // How many strong Ref<T>s point at this object, forcing the wrapper to stay alive even if GC
  // tracing doesn't find it?
  //
  // Whenever the value of the boolean expression (strongRefcount > 0 && wrapper.IsEmpty()) changes,
  // a GC visitation is needed to update all outgoing refs.
  uint strongRefcount = 0;

  // When `wrapperRef` is non-empty, the Wrappable is a member of the list `HeapTracer::wrappers`.
  kj::ListLink<Wrappable> link;

  friend class GcVisitor;
  friend class HeapTracer;
  friend class MemoryTracker;
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
  Wrappable::CppgcShim* allocateShim(Wrappable& wrappable);
  void clearFreelistedShims();

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

  // List of shim objects for wrappers that were collected during a minor GC. The shim objects
  // can be reused for future allocations.
  kj::Maybe<Wrappable::CppgcShim&> freelistedShims;
};

// Try to use this in any scope where JavaScript wrapped objects are destroyed, to confirm that
// they don't hold disallowed references to KJ I/O objects. IoOwn's destructor will explicitly
// create AllowAsyncDestructorsScope to permit holding such objects via IoOwn. This is meant to
// help catch bugs.
#define DISALLOW_KJ_IO_DESTRUCTORS_SCOPE                                                           \
  kj::DisallowAsyncDestructorsScope disallow(                                                      \
      "JavaScript heap objects must not contain KJ I/O objects without a IoOwn")
// TODO(soon):
// - Track memory usage of native objects.

// Given a handle to a resource type, extract the raw C++ object pointer.
template <typename T, bool isContext>
T& extractInternalPointer(
    const v8::Local<v8::Context>& context, const v8::Local<v8::Object>& object) {
  // Due to bugs in V8, we can't use internal fields on the global object:
  //   https://groups.google.com/d/msg/v8-users/RET5b3KOa5E/3EvpRBzwAQAJ
  //
  // So, when wrapping a global object, we store the pointer in the "embedder data" of the context
  // instead of the internal fields of the object.

  if constexpr (isContext) {
    // V8 docs say EmbedderData slot 0 is special, so we use slot 1. (See comments in newContext().)
    return *reinterpret_cast<T*>(context->GetAlignedPointerFromEmbedderData(1));
  } else {
    KJ_ASSERT(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
    return *reinterpret_cast<T*>(
        object->GetAlignedPointerFromInternalField(Wrappable::WRAPPED_OBJECT_FIELD_INDEX));
  }
}

}  // namespace workerd::jsg
