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

namespace workerd::jsg {

using kj::uint;

class GcVisitor;
class HeapTracer;

class TraceableHandle: public v8::Global<v8::Data> {
  // Internal type used anywhere where a v8::Global with support for GC tracing is desired. This
  // encapsulates the very weird behavior of TracedReference.

public:
  using v8::Global<v8::Data>::Global;

  TraceableHandle(TraceableHandle&& other)
      : v8::Global<v8::Data>(kj::mv(other)) {
    // Since we don't know if `other.lastMarked` is current, we have to assume `other.tracedRef` is
    // invalid and not touch it. Setting `lastMarked` to zero ensures it'll be recreated if needed.
    other.lastMarked = 0;
  }
  TraceableHandle& operator=(TraceableHandle&& other) {
    static_cast<v8::Global<v8::Data>&>(*this) = kj::mv(other);
    // Since we don't know if `other.lastMarked` is current, we have to assume `other.tracedRef` is
    // invalid and not touch it. Setting `lastMarked` to zero ensures it'll be recreated if needed.
    lastMarked = 0;
    other.lastMarked = 0;
    return *this;
  }

  KJ_DISALLOW_COPY(TraceableHandle);

  uint getLastMarked() { return lastMarked; }

private:
  v8::TracedReference<v8::Data> tracedRef;
  // Space for a TracedReference. Note that V8's TracedReference has very weird lifetime
  // properties. It becomes poison when V8 decides it is unreachable. Any attempt to use it after
  // that point will crash or worse. This includes calling `Reset()`! `Reset()` will blow up if
  // the reference has been collected. And attempting to assign `tracedRef` implicitly calls
  // `Reset()`! So if the ref has been deemed unreachable then YOU CANNOT ASSIGN OVER IT.
  //
  // However, the type has no destructor. So... you can safely placement-new over it. (Or use
  // kj::ctor(), which is a nice interface for placement-new.)

  uint lastMarked = 0;
  // Last trace ID at which `tracedRef` was marked. 0 indicates never marked. If `lastMarked` is
  // not equal to either the current or previous trace, then `tracedRef` must be assumed to be
  // poison which must not be touched!

  friend class HeapTracer;
};

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
  static constexpr uint INTERNAL_FIELD_COUNT = 2;
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

  static constexpr uint WRAPPED_OBJECT_FIELD_INDEX = 1;
  // Index of the internal field that points back to the `Wrappable`.

  static constexpr uint NEEDS_TRACING_FIELD_INDEX = 0;
  // Index of the internal field that must be non-null to convince EmbedderHeapTracer that the
  // object needs tracing. The actual value is not relevant aside from nullness.

  void addStrongRef();
  void removeStrongRef();

  void maybeDeferDestruction(bool strong, kj::Own<void> ownSelf, Wrappable* self);
  // Called by jsg::Ref<T> to ensure that its Wrappable is destroyed under the isolate lock.
  // `ownSelf` keeps the raw `self` pointer valid -- they are passed separately because Wrappable is
  // a private base class of the object.

  v8::Local<v8::Object> getHandle(v8::Isolate* isolate);

  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) {
    if (wrapper.IsEmpty()) {
      return nullptr;
    } else {
      // V8 doesn't let us cast directly from v8::Data to subtypes of v8::Value, so we're forced to
      // use this double cast... Ech.
      return wrapper.Get(isolate).As<v8::Value>().As<v8::Object>();
    }
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

  void traceFromV8(uint traceId);
  // Called by HeapTracer when V8 tells us that it found a reference to this object.

private:
  TraceableHandle wrapper;
  // Handle to the JS wrapper object. The wrapper is created lazily when the object is first
  // exported to JavaScript; until then, the wrapper is empty.
  //
  // This handle is marked strong whenever there are non-GC-traced references to the object (i.e.
  // from other C++ objects). Otherwise, it is marked weak, with a weak callback that potentially
  // deletes the object.
  //
  // This handle always holds a v8::Object.

  v8::Isolate* isolate = nullptr;
  // Will be non-null if `wrapper` is non-empty or `lastTraceId` is non-zero.

  uint lastTraceId = 0;
  // Last GC trace in which this object was reached. 0 = never reached.
  //
  // Whenever this changes, a GC visitation must be executed to update outgoing refs.

  uint strongRefcount = 0;
  // How many strong Ref<T>s point at this object, forcing the wrapper to stay alive even if GC
  // tracing doesn't find it?
  //
  // Whenever the value of the boolean expression (strongRefcount > 0 && wrapper.IsEmpty()) changes,
  // a GC visitation is needed to update all outgoing refs.

  enum {
    NOT_DETACHED,
    WHILE_SCAVENGING,
    WHILE_TRACING,
    OTHER
  } detached = NOT_DETACHED;
  // Has this had a wrapper in the past which was detached? For debugging.

  uint detachedTraceId = 0;
  // traceId when detachment occurred.

  kj::Own<Wrappable> wrapperRef;
  // A pointer to self, intended to represent V8's reference to this object. Must be reset at
  // the same time as `wrapper` is reset.

  kj::ListLink<Wrappable> link;
  // When `wrapperRef` is non-empty, the Wrappable is a member of the list `HeapTracer::wrappers`.

  void resetWrapperHandle();
  kj::Own<Wrappable> detachWrapperRef();
  static void deleterPass1(const v8::WeakCallbackInfo<Wrappable>& data);
  static void deleterPass2(const v8::WeakCallbackInfo<Wrappable>& data);

  void setWeak();

  friend class GcVisitor;
  friend class HeapTracer;
};

class HeapTracer final: public v8::EmbedderHeapTracer {
  // For historical reasons, this is actually implemented in setup.c++.

public:
  explicit HeapTracer(v8::Isolate* isolate): isolate(isolate) {
    isolate->SetEmbedderHeapTracer(this);
  }

  ~HeapTracer() noexcept {
    // Destructor has to be noexcept because it inherits from a V8 type that has a noexcept
    // destructor.
    KJ_IREQUIRE(isolate == nullptr, "you must call HeapTracer.destroy()");
  }

  void destroy();
  // Call under isolate lock when shutting down isolate.

  static HeapTracer& getTracer(v8::Isolate* isolate) {
    return *reinterpret_cast<HeapTracer*>(isolate->GetEmbedderHeapTracer());
  }

  uint currentTraceId() { return traceId; }

  void mark(TraceableHandle& handle);
  // If no trace is in progress, does nothing. If a trace is in progress, either calls
  // RegisterEmbedderReference() now or arranges for it to be called before the end of the trace.

  void addWrapper(kj::Badge<Wrappable>, Wrappable& wrappable) { wrappers.add(wrappable); }
  void removeWrapper(kj::Badge<Wrappable>, Wrappable& wrappable) { wrappers.remove(wrappable); }
  void clearWrappers();

  void startScavenge() { scavenging = true; }
  void endScavenge() { scavenging = false; }

  void RegisterV8References(const std::vector<std::pair<void*, void*>>& internalFields) override;
  void TracePrologue(TraceFlags flags) override;
  bool AdvanceTracing(double deadlineMs) override;
  bool IsTracingDone() override;
  void TraceEpilogue(TraceSummary* trace_summary) override;
  void EnterFinalPause(EmbedderStackState stackState) override;

  bool isTracing() { return inTrace; }
  bool isScavenging() { return scavenging; }

private:
  v8::Isolate* isolate;
  uint traceId = 1;
  bool inTrace = false;
  bool inAdvanceTracing = false;
  bool scavenging = false;
  kj::Vector<Wrappable*> wrappersToTrace;
  kj::Vector<v8::TracedReference<v8::Data>> referencesToMarkLater;

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
