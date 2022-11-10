// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE
//
// This file defines basic helpers involved in wrapping C++ objects for JavaScript consumption,
// including garbage-collecting those objects.

#include <kj/debug.h>

#include <kj/common.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/vector.h>
#include <kj/string.h>
#include <kj/list.h>
#include <v8.h>
#include <cppgc/member.h>
#include <cppgc/persistent.h>

namespace workerd::jsg {

using kj::uint;

class GcVisitor;
class Wrappable;
class V8Handle;

class Wrappable: public kj::Refcounted {
  // Base class for C++ objects which can be "wrapped" for JavaScript consumption.
  // Wrappable is refcounted via kj::Refcounted. A single instance may have multiple
  // handles keeping it alive. When the last handle is dropped, the Wrappable instance
  // will be destroyed.
  //
  // The "wrapper" is a garbage collected handle for the Wrappable that has an associated
  // JavaScript object that serves as a proxy for the C++ object in JavaScript. The wrapper
  // is "weak". That is, when V8 determines that the wrapper is no longer reachable through
  // any path, the wrapper will be collected and the ref it holds on the Wrappable will be
  // released.
  //
  // The wrapper is represented in the implementation by three distinct components:
  //   * The WrapperHandle (used internally by the Wrapper class to interact with
  //     the other two components below...)
  //   * The WrapperHandle::Shim object, an opaque cppgc garbage collected object
  //     that is connected to the actual v8 JavaScript object.
  //   * The JavaScript object that represents (and proxies) the Wrappable instance
  //     in JavaScript.
  //
  // When we refer to "wrapper" generically, we're generally referring to all of these.
  //
  // TODO(oilpan): Currently, the wrapper Shim can only be collected in a full gc. Oilpan
  // garbage collected objects do not currently support being collected on scavenge without
  // a hack using EmbedderRootsHandler, which we might be able to implement. Still investigating
  // that.
  //
  // A Wrappable instance does not necessarily have a wrapper Shim attached. E.g. for JSG_RESOURCE
  // types, wrappers are allocated lazily when the object first gets passed into JavaScript.
  //
  // The wrapper Shim implements a Trace method. The purpose of Trace is to determine what
  // garbage collected objects are reachable from the shim. It is part of the mechanism
  // that v8 uses to determine if gc managed objects can be cleaned up or not. The trace
  // algorithm is simple: The shim will ask the Wrappable to iterate through all of the
  // handles to potentially garbage collected objects it owns and "visit" each. Here,
  // "visit" effectively means to mark the object as being reachable from the wrapper.
  // There are two kinds of "handles to potentially garbage collected objects" that a
  // Wrappable may own: (a) A Ref (described below) or (b) A V8Handle (a handle that
  // holds a reference to a V8 JavaScript value).
  //
  // Like a wrapper, a Ref is a handle to a Wrappable. A Ref can be either Strong or Traced.
  // When a Ref is first created, it is always Strong -- that is, it holds a strong ref to
  // the Wrappable that keeps the Wrappable alive. If the Wrappable has a wrapper, a Strong
  // ref *also* holds a persistent ref to the wrapper's JavaScript object. This ensures that
  // the wrapper cannot be garbage collected as long as the Strong Ref exists.
  //
  // However, when a Ref (whose own Wrappable has a wrapper) is owned by another Wrappable
  // as a child, and the owning Wrappable is traced, the Ref will be transitioned into a Traced
  // Ref, which holds a traced reference to the wrapper. Tracing will then visit that
  // traced reference to mark it reachable. A traced Ref does not maintain its own strong
  // kj::Own reference to its Wrappable, and the reference it has to the JavaScript wrapper
  // will only remain valid so long as it is reachable.

public:
  enum EmbedderMetadataIndex {
    kEmbedderId,
    kEmbedderSlot,
    kEmbedderFieldCount,
  };
  static uint16_t kWorkerdEmbedderId;

  Wrappable() = default;
  KJ_DISALLOW_COPY(Wrappable);
  virtual ~Wrappable() noexcept(false);

  virtual const char* jsgTypeName() const { return "Wrappable"; }
  // A simple diagnostic label useful for logging. Should be overridden by subclasses.

  v8::Local<v8::Object> getHandle(v8::Isolate* isolate) const;
  // Gets the handle associated with the wrapper (if there is one). Returns an
  // empty v8::Local<v8::Object>() if there is no wrapper.

  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) const;
  // Gets the handle associated with the wrapper (if there is one). Returns
  // nullptr if there is no wrapper.

  void attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object);
  // Attach to a JavaScript object.
  //
  // The object MUST have exactly 2 internal field slots, which will be initialized by this
  // call as follows:
  // - Internal field 0 is special and is used by the GC tracing implementation.
  // - Internal field 1 is set to a pointer to the Wrappable. It can be used to unwrap the
  //   object.

  v8::Local<v8::Object> attachOpaqueWrapper(v8::Local<v8::Context> context);
  // Attach an empty object as the wrapper.

  static kj::Maybe<Wrappable&> tryUnwrapOpaque(v8::Isolate* isolate, v8::Local<v8::Value> handle);
  // If `handle` was originally returned by attachOpaqueWrapper(), return the Wrappable it wraps.
  // Otherwise, return nullptr.

  virtual void jsgVisitForGc(GcVisitor& visitor) const {};

  template <typename T>
  static kj::Maybe<T&> tryUnwrap(const v8::Local<v8::Context>& context,
                                 const v8::Local<v8::Object>& object);

private:
  class WrapperHandle final {
    // The WrapperHandle is used internally by Wrappable to manage the V8
    // wrapper object (if one exists).
  public:
    class Shim;
    // The Shim is an opaque object associated with the JS wrapper that holds
    // a kj::Own reference to the Wrappable. It will stay alive as long as the
    // wrapper stays alive.

    KJ_DISALLOW_COPY(WrapperHandle);

    v8::Local<v8::Object> getHandle(v8::Isolate* isolate) const;
    kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) const;

    void attach(kj::Own<Wrappable> wrappable, v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    // Attaches the given wrapper to the shim handle. Note that this must be done while
    // we are under the isolate lock.

    bool isEmpty() const;
    // True if the Shim has not been created or has been destroyed.

    static kj::Maybe<Wrappable&> tryUnwrap(const v8::Local<v8::Context>& context,
                                           const v8::Local<v8::Object>& object);
    // If the given object is a proxy for a Wrappable, returns a reference to the
    // instance. Otherwise returns nullptr.

    template <typename T>
    static kj::Maybe<T&> tryUnwrap(const v8::Local<v8::Context>& context,
                                   const v8::Local<v8::Object>& object) {
      KJ_IF_MAYBE(wrappable, tryUnwrap(context, object)) {
        return reinterpret_cast<T&>(*wrappable);
      }
      return nullptr;
    }

  private:
    v8::Isolate* isolate = nullptr;
    cppgc::WeakPersistent<Shim> wrapper;

    bool wasAttached = false;
    // A Wrappable can only ever be attached to a JS object at most once.

    WrapperHandle() = default;

    friend class Wrappable;
  };

  class RefBase {
    // RefBase is the type-erased base for a jsg::Ref. It holds a reference handle
    // to a Wrappable. The type of reference it holds depends on whether the ref
    // is traced or not.
    //
    // Tracing is merely an indication that the Ref is reachable as a member of some
    // other Wrappable.
    //
    // When a RefBase is first created, it always holds a StrongRefHandle.
    // A StrongRefHandle wraps its own kj::Own over the Wrappable.
    // When the RefBase is traced, demonstrating that it is reachable from a
    // traced object, and the Wrappable has a JS wrapper associated with it, the
    // RefBase will be transitioned into a TracedRefHandle, which holds a
    // v8::TracedReference to the JS wrapper and a weak reference to the Wrappable.
  public:
    RefBase(decltype(nullptr)) {}
    RefBase(RefBase&& other);
    // When a RefBase is created by moving an existing RefBase, even one currently
    // holding a TracedRefHandle, the new RefBase is always created holding a
    // StrongRefHandle. Specifically, the traced state does not propagate with
    // the moved ref.

    template <typename T>
    RefBase(kj::Own<T> innerParam) : refHandle(innerParam->getStrongRefHandle()) {}

    KJ_DISALLOW_COPY(RefBase);

    RefBase& operator=(RefBase&& other);
    // When a RefBase is assigned by moving an existing RefBase, even one currently
    // holding a TracedRefHandle, the assigned RefBase is always transitioned to a
    // StrongRefHandle.

    template <typename T>
    inline T* get() {
      KJ_IF_MAYBE(wrappable, tryGetWrappable()) {
        return static_cast<T*>(wrappable);
      }
      return nullptr;
    }

    template <typename T>
    inline const T* get() const {
      KJ_IF_MAYBE(wrappable, tryGetWrappable()) {
        return static_cast<const T*>(wrappable);
      }
      return nullptr;
    }

    kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) const;
    void attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object);

    bool isEmpty() const;
    // Returns true if either refHandle is nullptr or refHandle is a TracedRefHandle
    // and its weak reference to the Wrappable has been cleared.

  private:
    struct StrongRefHandle final: public kj::Refcounted {
      // A Wrappable only ever has at most one refcounted StrongRefHandle at
      // a time. All strong Ref<T>s hold a reference to the same StrongRefHandle,
      // which holds its own kj::Own to the Wrappable. If there is a wrapper,
      // the StrongRefHandle will hold a strong reference to it, keeping it alive
      // as a GC root.

      kj::Own<Wrappable> inner;
      cppgc::Persistent<WrapperHandle::Shim> wrapper;

      StrongRefHandle(kj::Own<Wrappable> ptr);
      ~StrongRefHandle() noexcept(false);

      StrongRefHandle(StrongRefHandle&&) = delete;
      StrongRefHandle& operator=(StrongRefHandle&&) = delete;
      KJ_DISALLOW_COPY(StrongRefHandle);
    };

    struct TracedRefHandle final: public kj::Refcounted {
      // A Wrappable only ever has at most one refcounted TracedRefHandle at
      // a time, and only when a Ref has been traced.

      kj::Maybe<Wrappable&> maybeWrappable;
      cppgc::Member<WrapperHandle::Shim> wrapper;

      TracedRefHandle(cppgc::Member<WrapperHandle::Shim> wrapper,
                      Wrappable& wrappable);
      ~TracedRefHandle() noexcept(false);

      TracedRefHandle(TracedRefHandle&&) = delete;
      TracedRefHandle& operator=(TracedRefHandle&&) = delete;
      KJ_DISALLOW_COPY(TracedRefHandle);

      void visit(GcVisitor& visitor);
    };

    using RefHandle = kj::OneOf<kj::Own<StrongRefHandle>, kj::Own<TracedRefHandle>>;

    kj::Maybe<RefHandle> refHandle;
    uint lastTraceId = 0;
    kj::Maybe<const Wrappable&> maybeParent;

    RefBase(kj::Maybe<RefHandle> handle);
    RefBase(kj::Own<StrongRefHandle> handle);

    kj::Maybe<RefHandle> addStrongRef();
    // Acquire a new StrongRefHandle from this RefBase if it is not empty.

    kj::Maybe<Wrappable&> tryGetWrappable();
    kj::Maybe<const Wrappable&> tryGetWrappable() const;

    kj::Maybe<RefHandle&> maybeRefHandle();
    kj::Maybe<const RefHandle&> maybeRefHandle() const;

    bool isTraced() const;
    // Returns true if refHandle is not nullptr and is a TracedRefHandle.

    void visit(GcVisitor& visitor);

    template <typename T> friend class Ref;
    friend class GcVisitor;
    friend class Object;
    friend class Wrappable;
    friend class V8Handle;
    friend struct StrongRefHandle;
    friend struct TracedRefHandle;
  };

  WrapperHandle wrapper;
  // Handle to the JS wrapper object. The wrapper is created lazily when the object is first
  // exported to JavaScript; until then, the wrapper is empty.

  kj::Maybe<RefBase::StrongRefHandle&> maybeStrongRefHandle;
  // There is only ever at most one refcounted StrongRefHandle created for a single
  // Wrappable, with each "strong" RefBase holding a kj::Own reference to it. If
  // the StrongRefHandle does not exist, one will be created on demand.

  kj::Maybe<RefBase::TracedRefHandle&> maybeTracedRefHandle;
  // There is only ever at most one refcounted TracedRefHandle created for a single
  // Wrappable, with each "traced" RefBase holding a kj::Own reference to it. If
  // the TracedRefHandle does not exist, one will be created on demand.

  kj::Own<RefBase::StrongRefHandle> getStrongRefHandle();
  kj::Maybe<kj::Own<RefBase::TracedRefHandle>> getTracedRefHandle();
  // We can only get a TracedRefHandle if the Wrappable has a wrapper.

  inline bool hasStrongRef() const { return maybeStrongRefHandle != nullptr; }
  inline bool hasTracedRef() const { return maybeTracedRefHandle != nullptr; }
  inline bool hasWrapper() const { return !wrapper.isEmpty(); }

  void trace(GcVisitor& visitor) const;

  template <typename T>
  friend class Ref;
  friend class Object;
  friend class WrapperHandle;
  friend class WrapperHandle::Shim;
  friend class V8Handle;
  template <typename TypeWrapper, typename T>
  friend class ResourceWrapper;
  friend class GcVisitor;

  template <typename T, bool isContext>
  friend T& extractInternalPointer(const v8::Local<v8::Context>& context,
                                   const v8::Local<v8::Object>& object);
};

template <typename T>
kj::Maybe<T&> Wrappable::tryUnwrap(const v8::Local<v8::Context>& context,
                                   const v8::Local<v8::Object>& object) {
  return WrapperHandle::tryUnwrap<T>(context, object);
}

template <typename T>
class TracedGlobal final {
  // Holds a reference to a V8 JavaScript value.
  // Initially, the reference is strong (using a strong v8::Global), ensuring
  // that the reference is held as a GC root. Once visited during tracing,
  // the v8::Global is marked weak and the v8::TracedReference is visited.
  // From that point on, the TracedGlobal instance will remain valid only
  // as long as V8 determines that it is reachable.
public:
  TracedGlobal(v8::Isolate* isolate, v8::Local<T> handle)
      : isolate(isolate),
        handle(isolate, kj::mv(handle)) {}

  TracedGlobal(TracedGlobal&& other)
      : isolate(other.isolate),
        handle(kj::mv(other.handle)) {
    this->handle.ClearWeak();
    other.reset();
    KJ_ASSERT(other.isEmpty());
  }

  KJ_DISALLOW_COPY(TracedGlobal);

  TracedGlobal& operator=(TracedGlobal&& other) {
    isolate = other.isolate;
    handle.Reset(isolate, other.handle.Get(isolate));
    lastTraceId = 0;
    kj::ctor(traced, isolate, handle.Get(isolate));
    other.reset();
    KJ_ASSERT(other.isEmpty());
    return *this;
  }

  inline void reset() {
    // Importantly, we don't touch the v8::TracedReference here.
    // We don't actually need to and it's poison for us to touch
    // if we don't know for absolutely certain that V8 hasn't
    // marked it unreachable.
    handle.Reset();
    lastTraceId = 0;
  }

  inline bool isEmpty() const { return handle.IsEmpty(); }

  kj::Maybe<v8::Local<T>> tryGetHandle(v8::Isolate* isolate) const;

  void visit(GcVisitor& visitor);
private:
  v8::Isolate* isolate;
  v8::Global<T> handle;
  v8::TracedReference<T> traced;
  // v8::TracedReference is a bit special. It is poison to touch once V8
  // determines it is unreachable. We use it here only to allow us to
  // trace the reference. We always use the v8::Global to access the actual
  // object.
  uint lastTraceId = 0;
  kj::Maybe<const Wrappable&> maybeParent;

  friend class V8Handle;
};

class V8Handle final {
  // Holds a reference to v8::Data. If the v8 object is a wrapper object (it has an underlying
  // Wrappable), then this will hold a Ref to the Wrappable, rather than holding a reference to the
  // wrapper itself (using TracedGlobal).
public:
  V8Handle() = default;
  V8Handle(v8::Isolate* isolate, v8::Local<v8::Data> data);
  V8Handle(V8Handle&&) = default;
  V8Handle& operator=(V8Handle&&) = default;

  KJ_DISALLOW_COPY(V8Handle);

  kj::Maybe<v8::Local<v8::Data>> tryGetHandle(v8::Isolate* isolate) const;
  v8::Local<v8::Data> getHandle(v8::Isolate* isolate) const;

  void reset();
  bool isEmpty() const;
  bool isTraced() const;

  bool operator==(const V8Handle& other) const;

private:
  using RefType = kj::OneOf<Wrappable::RefBase, TracedGlobal<v8::Data>>;
  kj::Maybe<RefType> ref;

  static kj::Maybe<RefType> getV8Ref(v8::Isolate* isolate, v8::Local<v8::Data> data);

  void visit(GcVisitor& visitor);

  friend class Data;
  friend class GcVisitor;
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
