// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// This file defines basic helpers involved in wrapping C++ objects for JavaScript consumption,
// including garbage-collecting those objects.

#include <kj/common.h>
#include <kj/refcount.h>
#include <kj/vector.h>
#include <kj/string.h>
#include <kj/list.h>
#include <v8.h>

namespace cppgc { class Visitor; }

namespace workerd::jsg {

using kj::uint;

class GcVisitor;
class HeapTracer;

class Wrappable: public kj::Refcounted {
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

public:
  static constexpr uint INTERNAL_FIELD_COUNT = 3;
  // Number of internal fields in a wrapper object.
  //
  // V8's EmbedderHeapTracer API imposes the following seemingly-arbitrary requirements on objects'
  // internal fields:
  // - The object has at least two internal fields (otherwise, it is ignored).
  // - The first internal field is not null (otherwise, the object is ignored).
  // - The object has an even number of internal fields (otherwise, DCHECK-failure).
  // - Only the first two internal field values are reported to the tracing API.
  //
  // Right then, we'll allocate two fields. The first will point to the GC tracing callback
  // (null if no tracing needed), the second will point to the object itself.

  static constexpr uint WRAPPED_OBJECT_FIELD_INDEX = 2;
  // Index of the internal field that points back to the `Wrappable`.

  static constexpr uint CPPGC_SHIM_FIELD_INDEX = 1;
  // Index of the internal field that contains a pointer to the cppgc shim object, used to get
  // tracing callback from V8 and get notification when the wrapper is collected.

  static constexpr uint WRAPPABLE_TAG_FIELD_INDEX = 0;
  // Field must contain a pointer to `WRAPPABLE_TAG`. This is a requirement of v8::CppHeap. The
  // purpose is not clear.
  //
  // Note that although V8 lets us configure which slot this tag is in, in practice if we set it
  // to anything other than zero, we see crashes inside V8. It appears that V8 allocates some
  // objects of its own which have internal fields, and then GC doesn't check that the index is
  // in-bounds before reading it...

  static constexpr uint16_t WRAPPABLE_TAG = 0xeb04;
  // The value pointed to by the internal field field `WRAPPABLE_TAG_FIELD_INDEX`.
  //
  // This value was chosen randomly.

  void addStrongRef();
  void removeStrongRef();

  void maybeDeferDestruction(bool strong, kj::Own<void> ownSelf, Wrappable* self);
  // Called by jsg::Ref<T> to ensure that its Wrappable is destroyed under the isolate lock.
  // `ownSelf` keeps the raw `self` pointer valid -- they are passed separately because Wrappable is
  // a private base class of the object.

  v8::Local<v8::Object> getHandle(v8::Isolate* isolate);

  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) {
    return wrapper.map([&](v8::TracedReference<v8::Object>& ref) {
      return ref.Get(isolate);
    });
  }

  void visitRef(GcVisitor& visitor, kj::Maybe<Wrappable&>& refParent, bool& refStrong);
  // Visits a Ref<T> pointing at this Wrappable. `refParent` and `refStrong` are the members of
  // `Ref<T>`, and this method is invoked on the object the ref points at. (This avoids the need
  // to templatize the implementation of this method.)

  void attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object, bool needsGcTracing);
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

  v8::Local<v8::Object> attachOpaqueWrapper(v8::Local<v8::Context> context, bool needsGcTracing);
  // Attach an empty object as the wrapper.

  static kj::Maybe<Wrappable&> tryUnwrapOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle);
  // If `handle` was originally returned by attachOpaqueWrapper(), return the Wrappable it wraps.
  // Otherwise, return nullptr.

  virtual void jsgVisitForGc(GcVisitor& visitor);
  // Perform GC visitation. This is named with the `jsg` prefix because it pollutes the
  // namespace of JSG_RESOURCE types.

  kj::Own<Wrappable> detachWrapper();
  // Detaches the wrapper from V8 and returns the reference that V8 had previously held.
  // (Typically, the caller will ignore the return value, thus dropping the reference.)

  void traceFromV8(cppgc::Visitor& cppgcVisitor);
  // Called by HeapTracer when V8 tells us that it found a reference to this object.

private:
  class CppgcShim;

  kj::Maybe<CppgcShim&> cppgcShim;
  // If a JS wrapper is currently allocated, this point to the cppgc shim object.

  kj::Maybe<v8::TracedReference<v8::Object>> wrapper;
  // Handle to the JS wrapper object. The wrapper is created lazily when the object is first
  // exported to JavaScript; until then, the wrapper is empty.
  //
  // If the wrapper object is "unmodified" from its original creation state, then V8 may choose to
  // collect it even when it could still technically be reached via C++ objects. The idea here is
  // that if the object is returned to JavaScript again later, the wrapper can be reconstructed at
  // that time. However, if the wrapper is modified by the application (e.g. monkey-patched with
  // a new property), then collecting and recreating it won't work. The logic to decide if an
  // object has been "modified" is internal to V8 and baked into its use of EmbedderRootsHandler.

  v8::Global<v8::Object> strongWrapper;
  // Whenever there are non-GC-traced references to the object (i.e. from other C++ objects, i.e.
  // strongRefcount > 0), and `wrapper` is non-null, then `strongWrapper` contains a copy of
  // `wrapper`, to force it to stay alive. Otherwise, `strongWrapper` is empty.

  v8::Isolate* isolate = nullptr;
  // Will be non-null if `wrapper` has ever been non-null.

  uint strongRefcount = 0;
  // How many strong Ref<T>s point at this object, forcing the wrapper to stay alive even if GC
  // tracing doesn't find it?
  //
  // Whenever the value of the boolean expression (strongRefcount > 0 && wrapper.IsEmpty()) changes,
  // a GC visitation is needed to update all outgoing refs.

  kj::ListLink<Wrappable> link;
  // When `wrapperRef` is non-empty, the Wrappable is a member of the list `HeapTracer::wrappers`.

  friend class GcVisitor;
  friend class HeapTracer;
};

class HeapTracer: public v8::EmbedderRootsHandler {
  // For historical reasons, this is actually implemented in setup.c++.

public:
  explicit HeapTracer(v8::Isolate* isolate);

  ~HeapTracer() noexcept {
    // Destructor has to be noexcept because it inherits from a V8 type that has a noexcept
    // destructor.
    KJ_IREQUIRE(isolate == nullptr, "you must call HeapTracer.destroy()");
  }

  void destroy();
  // Call under isolate lock when shutting down isolate.

  static HeapTracer& getTracer(v8::Isolate* isolate);

  void addWrapper(kj::Badge<Wrappable>, Wrappable& wrappable) { wrappers.add(wrappable); }
  void removeWrapper(kj::Badge<Wrappable>, Wrappable& wrappable) { wrappers.remove(wrappable); }
  void clearWrappers();

  // implements EmbedderRootsHandler -------------------------------------------
  bool IsRoot(const v8::TracedReference<v8::Value>& handle) override;
  void ResetRoot(const v8::TracedReference<v8::Value>& handle) override;

private:
  v8::Isolate* isolate;
  kj::Vector<Wrappable*> wrappersToTrace;

  kj::Vector<Wrappable*> detachLater;
  // Wrappables on which detachWrapper() should be called at the end of this GC pass.

  kj::List<Wrappable, &Wrappable::link> wrappers;
  // List of all Wrappables for which a JavaScript wrapper exists.
};

#define DISALLOW_KJ_IO_DESTRUCTORS_SCOPE \
  kj::DisallowAsyncDestructorsScope disallow( \
      "JavaScript heap objects must not contain KJ I/O objects without a IoOwn")
// Try to use this in any scope where JavaScript wrapped objects are destroyed, to confirm that
// they don't hold disallowed references to KJ I/O objects. IoOwn's destructor will explicitly
// create AllowAsyncDestructorsScope to permit holding such objects via IoOwn. This is meant to
// help catch bugs.

// TODO(soon):
// - Track memory usage of native objects.

}  // namespace workerd::jsg
