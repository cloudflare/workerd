// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wrappable.h"
#include "jsg.h"
#include "setup.h"
#include <kj/debug.h>
#include <kj/memory.h>

#include <cppgc/garbage-collected.h>
#include <cppgc/allocation.h>
#include <cppgc/visitor.h>

namespace workerd::jsg {

namespace {
void verifyTraceParent(const Wrappable& current, kj::Maybe<const Wrappable&> maybeParent) {
  // Provides a quick double check that a traceable object is being traced by its
  // actual parent.
  KJ_IF_MAYBE(parent, maybeParent) {
    KJ_ASSERT(parent == &current);
  } else {
    maybeParent = current;
  }
}
}  // namespace

// ======================================================================================
// V8Handle

kj::Maybe<kj::OneOf<Wrappable::RefBase, TracedGlobal<v8::Data>>> V8Handle::getV8Ref(
    v8::Isolate* isolate,
    v8::Local<v8::Data> data) {
  // If the given data is a wrapper object for a Wrappable, then we will grab
  // a ref to the Wrappable, otherwise we'll grab a v8::Global holding
  // onto the data.
  const auto maybeGetRef = [&](v8::Local<v8::Object> object, bool isContext)
      -> kj::Maybe<Wrappable::RefBase> {
    auto context = isolate->GetCurrentContext();
    KJ_IF_MAYBE(wrappable, Wrappable::WrapperHandle::tryUnwrap(context, object)) {
      return Wrappable::RefBase(wrappable->getStrongRefHandle());
    }
    return nullptr;
  };

  if (!data.IsEmpty()) {
    if (data->IsValue()) {
      auto value = data.As<v8::Value>();
      if (value->IsObject()) {
        KJ_IF_MAYBE(ref, maybeGetRef(value.As<v8::Object>(), value->IsContext())) {
          return kj::Maybe(kj::mv(*ref));
        }
      }
    }
    return kj::Maybe(TracedGlobal(isolate, data));
  }
  return nullptr;
}

V8Handle::V8Handle(v8::Isolate* isolate, v8::Local<v8::Data> data)
    : ref(getV8Ref(isolate, data)) {}

kj::Maybe<v8::Local<v8::Data>> V8Handle::tryGetHandle(v8::Isolate* isolate) const {
  KJ_IF_MAYBE(r, ref) {
    KJ_SWITCH_ONEOF(*r) {
      KJ_CASE_ONEOF(ref, TracedGlobal<v8::Data>) {
        return ref.tryGetHandle(isolate);
      }
      KJ_CASE_ONEOF(wrappable, Wrappable::RefBase) {
        return const_cast<Wrappable::RefBase&>(wrappable).tryGetHandle(isolate);
      }
    }
  }
  return nullptr;
}

v8::Local<v8::Data> V8Handle::getHandle(v8::Isolate* isolate) const {
  return tryGetHandle(isolate).orDefault(v8::Local<v8::Data>());
}

void V8Handle::reset() {
  KJ_IF_MAYBE(r, ref) {
    KJ_SWITCH_ONEOF(*r) {
      KJ_CASE_ONEOF(ref, TracedGlobal<v8::Data>) {
        ref.reset();
      }
      KJ_CASE_ONEOF(wrappable, Wrappable::RefBase) {
        auto dropMe = kj::mv(wrappable);
      }
    }
    ref = nullptr;
  }
}

bool V8Handle::isEmpty() const {
  KJ_IF_MAYBE(r, ref) {
    KJ_SWITCH_ONEOF(*r) {
      KJ_CASE_ONEOF(ref, TracedGlobal<v8::Data>) {
        return ref.isEmpty();
      }
      KJ_CASE_ONEOF(wrappable, Wrappable::RefBase) {
        return wrappable.isEmpty();
      }
    }
  }
  return true;
}

bool V8Handle::isTraced() const {
  KJ_IF_MAYBE(r, ref) {
    KJ_SWITCH_ONEOF(*r) {
      KJ_CASE_ONEOF(ref, TracedGlobal<v8::Data>) {
        return !ref.handle.IsEmpty() && ref.handle.IsWeak();
      }
      KJ_CASE_ONEOF(ref, Wrappable::RefBase) {
        return ref.isTraced();
      }
    }
  }
  return false;
}

void V8Handle::visit(GcVisitor& visitor) {
  KJ_IF_MAYBE(r, ref) {
    KJ_SWITCH_ONEOF(*r) {
      KJ_CASE_ONEOF(ref, Wrappable::RefBase) {
        visitor.visit(ref);
      }
      KJ_CASE_ONEOF(traced, TracedGlobal<v8::Data>) {
        traced.visit(visitor);
      }
    }
  }
}

bool V8Handle::operator==(const V8Handle& other) const {
  if (&other == this) return true;
  if (isEmpty()) {
    return other.isEmpty();
  }
  KJ_IF_MAYBE(otherRef, other.ref) {
    KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(ref)) {
      KJ_CASE_ONEOF(ref, Wrappable::RefBase) {
        // This is a wrappable. It should point to the same wrappable...
        KJ_IF_MAYBE(otherRefBase, otherRef->tryGet<Wrappable::RefBase>()) {
          return ref.get<Wrappable>() == otherRefBase->get<Wrappable>();
        }
      }
      KJ_CASE_ONEOF(ref, TracedGlobal<v8::Data>) {
        // If it's a TracedGlobal, we can only do a comparison under the isolate lock...
        KJ_ASSERT(v8::Locker::IsLocked(ref.isolate));
        KJ_IF_MAYBE(otherTracedGlobal, otherRef->tryGet<TracedGlobal<v8::Data>>()) {
          // ... and under a HandleScope...
          v8::HandleScope scope(ref.isolate);
          // ... and only if the isolates for both also match.
          return ref.isolate == otherTracedGlobal->isolate &&
                 ref.handle.Get(ref.isolate) == otherTracedGlobal->handle.Get(ref.isolate);
        }
      }
    }
  }
  return false;
}

// ======================================================================================
// Wrappable

uint16_t Wrappable::kWorkerdEmbedderId = 1;

Wrappable::~Wrappable() noexcept(false) {
  KJ_IF_MAYBE(tracedRefHandle, maybeTracedRefHandle) {
    tracedRefHandle->maybeWrappable = nullptr;
  }
}

v8::Local<v8::Object> Wrappable::getHandle(v8::Isolate* isolate) const {
  return tryGetHandle(isolate).orDefault(v8::Local<v8::Object>());
}

kj::Maybe<v8::Local<v8::Object>> Wrappable::tryGetHandle(v8::Isolate* isolate) const {
  return wrapper.tryGetHandle(isolate);
}

void Wrappable::attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  KJ_ASSERT(!object.IsEmpty());
  KJ_ASSERT(wrapper.isEmpty());
  wrapper.attach(kj::addRef(*this), isolate, object);
  KJ_IF_MAYBE(strong, maybeStrongRefHandle) {
    strong->wrapper = cppgc::Persistent<WrapperHandle::Shim>(wrapper.wrapper.Get());
  }
}

v8::Local<v8::Object> Wrappable::attachOpaqueWrapper(v8::Local<v8::Context> context) {
  auto isolate = context->GetIsolate();
  auto object = jsg::check(IsolateBase::getOpaqueTemplate(isolate)
      ->InstanceTemplate()->NewInstance(context));
  attachWrapper(isolate, object);
  return object;
}

kj::Maybe<Wrappable&> Wrappable::tryUnwrapOpaque(
    v8::Isolate* isolate,
    v8::Local<v8::Value> handle) {
  if (handle->IsObject()) {
    v8::Local<v8::Object> instance = v8::Local<v8::Object>::Cast(handle)
        ->FindInstanceInPrototypeChain(IsolateBase::getOpaqueTemplate(isolate));
    if (!instance.IsEmpty()) {
      return WrapperHandle::tryUnwrap<Wrappable>(isolate->GetCurrentContext(), instance);
    }
  }

  return nullptr;
}

kj::Own<Wrappable::RefBase::StrongRefHandle> Wrappable::getStrongRefHandle() {
  KJ_IF_MAYBE(strong, maybeStrongRefHandle) {
    return kj::addRef(*strong);
  }
  auto handle = kj::refcounted<RefBase::StrongRefHandle>(kj::addRef(*this));
  if (hasWrapper()) {
    handle->wrapper = cppgc::Persistent<WrapperHandle::Shim>(wrapper.wrapper.Get());
  }
  return kj::mv(handle);
}

kj::Maybe<kj::Own<Wrappable::RefBase::TracedRefHandle>> Wrappable::getTracedRefHandle() {
  if (hasWrapper()) {
    KJ_IF_MAYBE(traced, maybeTracedRefHandle) {
      return kj::addRef(*traced);
    }
    return kj::refcounted<RefBase::TracedRefHandle>(
        cppgc::Member<WrapperHandle::Shim>(wrapper.wrapper.Get()),
        *this);
  }
  return nullptr;
}

// ======================================================================================
// Wrappable::WrapperHandle::Shim

class Wrappable::WrapperHandle::Shim final:
    public cppgc::GarbageCollected<Wrappable::WrapperHandle::Shim> {
public:
  Shim(kj::Own<Wrappable> wrappable,
       v8::Isolate* isolate,
       v8::Local<v8::Object> object)
      : wrappable(kj::mv(wrappable)),
        wrapper(isolate, object),
        diagnosticTypeName(this->wrappable->jsgTypeName()) {
    object->SetAlignedPointerInInternalField(Wrappable::kEmbedderId,
                                             &Wrappable::kWorkerdEmbedderId);
    object->SetAlignedPointerInInternalField(Wrappable::kEmbedderSlot, this);
  }

  Shim(Shim&&) = delete;
  Shim&& operator=(Shim&&) = delete;
  KJ_DISALLOW_COPY(Shim);

  Wrappable* get() { return wrappable.get(); }
  const Wrappable* get() const { return wrappable.get(); }

  void Trace(cppgc::Visitor* visitor) const {
    GcVisitor gcVisitor(visitor, ++traceCounter);
    wrappable->trace(gcVisitor);
  }

  kj::Maybe<v8::Local<v8::Object>> tryGetHandle(v8::Isolate* isolate) const {
    return wrapper.tryGetHandle(isolate);
  }

  const char* getDiagnosticTypeName() const { return diagnosticTypeName; }

  void visit(GcVisitor& gcVisitor) {
    wrapper.visit(gcVisitor);
  }

private:
  kj::Own<Wrappable> wrappable;
  TracedGlobal<v8::Object> wrapper;

  mutable uint traceCounter = 0;
  const char* diagnosticTypeName;

  friend class WrapperHandle;
};

void Wrappable::trace(GcVisitor& gcVisitor) const {
  gcVisitor.push(*this);
  if (wrapper.wrapper.Get() != nullptr) {
    wrapper.wrapper->visit(gcVisitor);
  }
  jsgVisitForGc(gcVisitor);
  gcVisitor.pop();
}

// ======================================================================================
// Wrappable::WrapperHandle

v8::Local<v8::Object> Wrappable::WrapperHandle::getHandle(v8::Isolate* isolate) const {
  return tryGetHandle(isolate).orDefault(v8::Local<v8::Object>());
}

kj::Maybe<v8::Local<v8::Object>> Wrappable::WrapperHandle::tryGetHandle(
    v8::Isolate* isolate) const {
  auto shim = wrapper.Get();
  if (shim != nullptr) {
    return shim->tryGetHandle(isolate);
  }
  return nullptr;
}

kj::Maybe<Wrappable&> Wrappable::WrapperHandle::tryUnwrap(
    const v8::Local<v8::Context>& context,
    const v8::Local<v8::Object>& object) {
  // Can't be a wrapper object unless there are at least two internal fields.
  if (object->InternalFieldCount() >= 2) {
    void* ptr = object->GetAlignedPointerFromInternalField(Wrappable::kEmbedderSlot);
    if (ptr != nullptr) {
      return *reinterpret_cast<Shim*>(ptr)->get();
    }
  }
  return nullptr;
}

void Wrappable::WrapperHandle::attach(kj::Own<Wrappable> ownSelf,
                             v8::Isolate* isolate,
                             v8::Local<v8::Object> obj) {
  KJ_ASSERT(v8::Locker::IsLocked(isolate));
  KJ_ASSERT(isEmpty());
  KJ_ASSERT(obj->InternalFieldCount() == kEmbedderFieldCount);

  if (wasAttached) {
    // It appears that this Wrappable once had a wrapper attached, and then that wrapper was GC'd,
    // but later on a wrapper was added again. This suggests a serious problem with our GC, in that
    // it is collecting objects that are still reachable from JavaScript. However, we can usually
    // continue operating even in the presence of such a bug: it'll only cause a real problem if
    // a script has attached additional properties to the object in JavaScript and expects them
    // to still be there later. We know that scripts do so but we don't really know how common
    // it is.
#ifdef KJ_DEBUG
    KJ_FAIL_ASSERT("Wrappable had wrapper collected and then re-added later");
#else
    // Don't crash in production. Also avoid spamming logs.
    static bool alreadyWarned = false;
    if (!alreadyWarned) {
      KJ_LOG(ERROR, "Wrappable had wrapper collected and then re-added later", kj::getStackTrace());
      alreadyWarned = true;
    }
#endif
  }

  wasAttached = true;
  this->isolate = isolate;

  // We don't typically use the new keyword for things. In this case, we want to. When the object
  // is because it is garbage collected, it will be destroyed.
  wrapper = IsolateBase::from(isolate).getHeap().alloc<Shim>(kj::mv(ownSelf), isolate, obj);
  wrapper->Trace(nullptr);
}

bool Wrappable::WrapperHandle::isEmpty() const {
  return wrapper.Get() == nullptr;
}

// ======================================================================================
// Wrappable::RefBase::StrongRefHandle

Wrappable::RefBase::StrongRefHandle::StrongRefHandle(kj::Own<Wrappable> ptr) : inner(kj::mv(ptr)) {
  KJ_ASSERT(inner->maybeStrongRefHandle == nullptr);
  inner->maybeStrongRefHandle = this;
}

Wrappable::RefBase::StrongRefHandle::~StrongRefHandle() noexcept(false) {
  inner->maybeStrongRefHandle = nullptr;
}

// ======================================================================================
// Wrappable::RefBase::TracedRefHandle

Wrappable::RefBase::TracedRefHandle::TracedRefHandle(
    cppgc::Member<WrapperHandle::Shim> wrapper,
    Wrappable& wrappable)
    : maybeWrappable(wrappable),
      wrapper(kj::mv(wrapper)) {
  KJ_ASSERT(wrappable.maybeTracedRefHandle == nullptr);
  wrappable.maybeTracedRefHandle = this;
}

Wrappable::RefBase::TracedRefHandle::~TracedRefHandle() noexcept(false) {
  KJ_IF_MAYBE(wrappable, maybeWrappable) {
    wrappable->maybeTracedRefHandle = nullptr;
  }
}

void Wrappable::RefBase::TracedRefHandle::visit(GcVisitor& visitor) {
  visitor.visit(wrapper);
}

// ======================================================================================
// Wrappable::RefBase

Wrappable::RefBase::RefBase(kj::Maybe<RefHandle> handle)
    : refHandle(kj::mv(handle)) {}

Wrappable::RefBase::RefBase(RefBase&& other) : refHandle(other.addStrongRef()) {
  auto dropMe = kj::mv(other.refHandle);
  other.lastTraceId = 0;
}

Wrappable::RefBase::RefBase(kj::Own<Wrappable::RefBase::StrongRefHandle> strong)
    : refHandle(kj::mv(strong)) {}

Wrappable::RefBase& Wrappable::RefBase::operator=(RefBase&& other) {
  refHandle = other.addStrongRef();
  lastTraceId = 0;
  auto dropMe = kj::mv(other.refHandle);
  other.lastTraceId = 0;
  return *this;
}

kj::Maybe<Wrappable&> Wrappable::RefBase::tryGetWrappable() {
  KJ_IF_MAYBE(handle, refHandle) {
    KJ_SWITCH_ONEOF(*handle) {
      KJ_CASE_ONEOF(strong, kj::Own<Wrappable::RefBase::StrongRefHandle>) {
        return *strong->inner;
      }
      KJ_CASE_ONEOF(traced, kj::Own<Wrappable::RefBase::TracedRefHandle>) {
        KJ_IF_MAYBE(inner, traced->maybeWrappable) {
          return inner;
        }
      }
    }
  }
  return nullptr;
}

kj::Maybe<const Wrappable&> Wrappable::RefBase::tryGetWrappable() const {
  KJ_IF_MAYBE(handle, refHandle) {
    KJ_SWITCH_ONEOF(*handle) {
      KJ_CASE_ONEOF(strong, kj::Own<Wrappable::RefBase::StrongRefHandle>) {
        return *strong->inner;
      }
      KJ_CASE_ONEOF(traced, kj::Own<Wrappable::RefBase::TracedRefHandle>) {
        KJ_IF_MAYBE(inner, traced->maybeWrappable) {
          return inner;
        }
      }
    }
  }
  return nullptr;
}

kj::Maybe<Wrappable::RefBase::RefHandle> Wrappable::RefBase::addStrongRef() {
  KJ_IF_MAYBE(wrappable, tryGetWrappable()) {
    return kj::Maybe<Wrappable::RefBase::RefHandle>(wrappable->getStrongRefHandle());
  }
  return nullptr;
}

kj::Maybe<v8::Local<v8::Object>> Wrappable::RefBase::tryGetHandle(v8::Isolate* isolate) const {
  // If the object has a JS wrapper, return it. Note that the JS wrapper is initialized lazily
  // when the object is first passed to JS, so you can't be sure that it exists. To reliably
  // get a handle (creating it on-demand if necessary), use a TypeHandler<Ref<T>>.
  KJ_IF_MAYBE(wrappable, tryGetWrappable()) {
    return wrappable->tryGetHandle(isolate);
  }
  return nullptr;
}

void Wrappable::RefBase::attachWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  // Attach a JavaScript object which implements the JS interface for this C++ object. Normally,
  // this happens automatically the first time the Ref is passed across the FFI barrier into JS.
  // This method may be useful in order to use a different wrapper type than the one that would
  // be used automatically. This method is also useful when implementing TypeWrapperExtensions.
  //
  // It is an error to attach a wrapper when another wrapper is already attached. Hence,
  // typically this should only be called on a newly-allocated object.
  KJ_IF_MAYBE(wrappable, tryGetWrappable()) {
    wrappable->attachWrapper(isolate, object);
  }
}

kj::Maybe<Wrappable::RefBase::RefHandle&> Wrappable::RefBase::maybeRefHandle() {
  KJ_IF_MAYBE(handle, refHandle) {
    return *handle;
  }
  return nullptr;
}

kj::Maybe<const Wrappable::RefBase::RefHandle&> Wrappable::RefBase::maybeRefHandle() const {
  KJ_IF_MAYBE(handle, refHandle) {
    return *handle;
  }
  return nullptr;
}

bool Wrappable::RefBase::isEmpty() const {
  KJ_IF_MAYBE(handle, refHandle) {
    KJ_SWITCH_ONEOF(*handle) {
      KJ_CASE_ONEOF(strong, kj::Own<StrongRefHandle>) {
        return false;
      }
      KJ_CASE_ONEOF(weak, kj::Own<TracedRefHandle>) {
        return weak->maybeWrappable == nullptr;
      }
    }
  }
  return true;
}

bool Wrappable::RefBase::isTraced() const {
  KJ_IF_MAYBE(handle, refHandle) {
    return handle->is<kj::Own<Wrappable::RefBase::TracedRefHandle>>();
  }
  return false;
}

void Wrappable::RefBase::visit(GcVisitor& visitor) {
  verifyTraceParent(visitor.getCurrent(), maybeParent);
  if (lastTraceId == visitor.getTraceId()) {
    // This was already traced in this pass. Do not trace again.
    return;
  }
  lastTraceId = visitor.getTraceId();

  KJ_IF_MAYBE(handle, refHandle) {
    KJ_SWITCH_ONEOF(*handle) {
      KJ_CASE_ONEOF(strong, kj::Own<Wrappable::RefBase::StrongRefHandle>) {
        // If the Wrappable has a wrapper, then we are going to transition this into
        // a TracedRefHandle and visit it. Otherwise, we skip visiting it and continue
        // on to visiting its children.
        if (strong->inner->hasWrapper()) {
          bool hasTracedRef = strong->inner->hasTracedRef();
          KJ_IF_MAYBE(tracedRefHandle, strong->inner->getTracedRefHandle()) {
            refHandle = kj::mv((*tracedRefHandle));
            if (hasTracedRef) {
              return visit(visitor);
            }
          }
        }
      }
      KJ_CASE_ONEOF(traced, kj::Own<Wrappable::RefBase::TracedRefHandle>) {
        KJ_IF_MAYBE(wrappable, traced->maybeWrappable) {
          traced->visit(visitor);
        }
      }
    }

    KJ_IF_MAYBE(wrappable, tryGetWrappable()) {
      wrappable->trace(visitor);
    }
  }
}

template <typename T>
kj::Maybe<v8::Local<T>> TracedGlobal<T>::tryGetHandle(v8::Isolate* isolate) const {
  KJ_ASSERT(isolate == this->isolate);
  KJ_ASSERT(v8::Locker::IsLocked(isolate));
  if (!handle.IsEmpty()) {
    KJ_ASSERT(isolate == this->isolate);
    KJ_ASSERT(v8::Locker::IsLocked(isolate));
    v8::EscapableHandleScope scope(isolate);
    return scope.Escape(handle.Get(isolate));
  }
  return nullptr;
}

template <typename T>
void TracedGlobal<T>::visit(GcVisitor& visitor) {
  verifyTraceParent(visitor.getCurrent(), maybeParent);
  if (!isEmpty() && lastTraceId != visitor.getTraceId()) {
    lastTraceId = visitor.getTraceId();
    if (!handle.IsWeak()) {
      handle.SetWeak();
    }
    if (traced.IsEmpty()) {
      traced.Reset(isolate, handle.Get(isolate));
    }
    visitor.visit(traced);
  }
}

}  // namespace workerd::jsg
