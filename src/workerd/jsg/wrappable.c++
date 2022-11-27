// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wrappable.h"
#include "jsg.h"
#include "setup.h"
#include <kj/debug.h>
#include <v8-cppgc.h>
#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>

namespace workerd::jsg {

class Wrappable::CppgcShim final: public cppgc::GarbageCollected<CppgcShim> {
public:
  CppgcShim(Wrappable& wrappable): wrappable(kj::addRef(wrappable)) {
    KJ_DASSERT(wrappable.cppgcShim == nullptr);
    wrappable.cppgcShim = *this;
  }

  ~CppgcShim() {
    KJ_IF_MAYBE(w, wrappable) {
      KJ_DASSERT(&KJ_ASSERT_NONNULL(w->get()->cppgcShim) == this);
      KJ_DASSERT(w->get()->strongWrapper.IsEmpty());
      w->get()->detachWrapper();
    }
  }

  void Trace(cppgc::Visitor* visitor) const {
    KJ_IF_MAYBE(w, wrappable) {
      w->get()->traceFromV8(*visitor);
    }
  }

  mutable kj::Maybe<kj::Own<Wrappable>> wrappable;
  // This can become null if the wrappable is force-collected without waiting for GC.
};

kj::Own<Wrappable> Wrappable::detachWrapper() {
  KJ_IF_MAYBE(s, cppgcShim) {
    auto result = kj::mv(KJ_ASSERT_NONNULL(s->wrappable));
    s->wrappable = nullptr;
    wrapper = nullptr;
    cppgcShim = nullptr;
    strongWrapper.Reset();
    HeapTracer::getTracer(isolate).removeWrapper({}, *this);
    return result;
  } else {
    return {};
  }
}

v8::Local<v8::Object> Wrappable::getHandle(v8::Isolate* isolate) {
  return KJ_REQUIRE_NONNULL(tryGetHandle(isolate));
}

void Wrappable::addStrongRef() {
  KJ_DREQUIRE(v8::Isolate::TryGetCurrent() != nullptr, "referencing wrapper without isolate lock");
  if (strongRefcount++ == 0) {
    // This object previously had no strong references, but now it has one.
    KJ_IF_MAYBE(w, wrapper) {
      // Copy the traced reference into the strong reference.
      v8::HandleScope scope(isolate);
      strongWrapper.Reset(isolate, w->Get(isolate));
    } else {
      // Since we have no JS wrapper, we're forced to recursively mark all references reachable
      // through this wrapper as strong.
      GcVisitor visitor(*this, nullptr);
      jsgVisitForGc(visitor);
    }
  }
}
void Wrappable::removeStrongRef() {
  KJ_DREQUIRE(isolate == nullptr || v8::Isolate::TryGetCurrent() == isolate,
              "destroying wrapper without isolate lock");
  if (--strongRefcount == 0) {
    // This was the last strong reference.
    if (wrapper == nullptr) {
      // We have no wrapper. We need to mark all references held by this object as weak.
      if (isolate != nullptr) {
        // But only if the current isolate isn't null. If strong ref count is zero,
        // the wrapper is empty, and isolate is null, then the child handles it has will
        // be released anyway (since we're about to be destroyed), thus this visitation
        // isn't required (and may be buggy, since it may happen outside the isolate lock).
        GcVisitor visitor(*this, nullptr);
        jsgVisitForGc(visitor);
      }
    } else {
      // Just clear the strong ref.
      strongWrapper.Reset();
    }
  }
}

void Wrappable::maybeDeferDestruction(bool strong, kj::Own<void> ownSelf, Wrappable* self) {
  DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;

  auto item = IsolateBase::RefToDelete(strong, kj::mv(ownSelf), self);

  if (isolate == nullptr || v8::Locker::IsLocked(isolate)) {
    // If we never attached a wrapper and were never traced, or the isolate is already locked, then
    // we can just destroy the Wrappable immediately.
    auto drop = kj::mv(item);
  } else {
    // Otherwise, we have a wrapper and we don't have the isolate locked.
    auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
    jsgIsolate.deferDestruction(kj::mv(item));
  }
}

void Wrappable::traceFromV8(cppgc::Visitor& cppgcVisitor) {
  uint traceId = HeapTracer::getTracer(isolate).currentTraceId();

  if (lastTraceId == traceId) {
    // Duplicate trace, ignore.
    //
    // This can happen in particular if V8 choses to allocate an object unmarked but we determine
    // that the object is already reachable. In that case we mark the object *and* run our own
    // trace (because we can't be sure V8 didn't allocate the object already-marked), so we might
    // get duplicate traces.
  } else {
    lastTraceId = traceId;
    cppgcVisitor.Trace(KJ_ASSERT_NONNULL(wrapper));
    GcVisitor visitor(*this, cppgcVisitor);
    jsgVisitForGc(visitor);
  }
}

void Wrappable::attachWrapper(v8::Isolate* isolate,
                              v8::Local<v8::Object> object, bool needsGcTracing) {
  auto& tracer = HeapTracer::getTracer(isolate);

  KJ_REQUIRE(wrapper == nullptr);
  KJ_REQUIRE(strongWrapper.IsEmpty());

  wrapper = v8::TracedReference<v8::Object>(isolate, object);
  this->isolate = isolate;

  // Add to list of objects to force-clean at isolate shutdown.
  tracer.addWrapper({}, *this);

  // Set up internal fields for a newly-allocated object.
  KJ_REQUIRE(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
  object->SetAlignedPointerInInternalField(WRAPPED_OBJECT_FIELD_INDEX, this);

  // Allocate the cppgc shim.
  auto& cppgcAllocHandle = isolate->GetCppHeap()->GetAllocationHandle();
  auto cppgcShim = cppgc::MakeGarbageCollected<CppgcShim>(cppgcAllocHandle, *this);
  this->cppgcShim = *cppgcShim;

  object->SetAlignedPointerInInternalField(CPPGC_SHIM_FIELD_INDEX, cppgcShim);
  object->SetAlignedPointerInInternalField(WRAPPABLE_TAG_FIELD_INDEX,
      const_cast<uint16_t*>(&WRAPPABLE_TAG));

  if (strongRefcount > 0) {
    strongWrapper.Reset(isolate, object);

    // This object has untraced references, but didn't have a wrapper. That means that any refs
    // transitively reachable through the reference are strong. Now that a wrapper exists, the
    // refs will be traced when the wrapper is traced, so they should be converted to traced
    // references. Performing a visitation pass will update them.
    GcVisitor visitor(*this, nullptr);
    jsgVisitForGc(visitor);
  }
}

v8::Local<v8::Object> Wrappable::attachOpaqueWrapper(
    v8::Local<v8::Context> context, bool needsGcTracing) {
  auto isolate = context->GetIsolate();
  auto object = jsg::check(IsolateBase::getOpaqueTemplate(isolate)
      ->InstanceTemplate()->NewInstance(context));
  attachWrapper(isolate, object, needsGcTracing);
  return object;
}

kj::Maybe<Wrappable&> Wrappable::tryUnwrapOpaque(
    v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  if (handle->IsObject()) {
    v8::Local<v8::Object> instance = v8::Local<v8::Object>::Cast(handle)
        ->FindInstanceInPrototypeChain(IsolateBase::getOpaqueTemplate(isolate));
    if (!instance.IsEmpty()) {
      return *reinterpret_cast<Wrappable*>(
          instance->GetAlignedPointerFromInternalField(WRAPPED_OBJECT_FIELD_INDEX));
    }
  }

  return nullptr;
}

void Wrappable::jsgVisitForGc(GcVisitor& visitor) {
  // Nothing; subclasses that need tracing will override.
}

void Wrappable::visitRef(GcVisitor& visitor, kj::Maybe<Wrappable&>& refParent, bool& refStrong) {
  KJ_IF_MAYBE(p, refParent) {
    KJ_ASSERT(p == &visitor.parent);
  } else {
    refParent = visitor.parent;
  }

  if (isolate == nullptr) {
    isolate = visitor.parent.isolate;
  }

  // Make ref strength match the parent.
  if (visitor.parent.strongRefcount > 0 && visitor.parent.wrapper == nullptr) {
    // This reference should be strong, because the parent has strong refs and does not have its
    // own wrapper that will be traced.

    if (!refStrong) {
      // Ref transitions from weak to strong.
      //
      // This should never happen during a GC pass, since we should only be visiting traced
      // references then.
      KJ_ASSERT(visitor.cppgcVisitor == nullptr);
      addStrongRef();
      refStrong = true;
    }
  } else {
    if (refStrong) {
      // Ref transitions from strong to weak.
      //
      // Note that a Ref can become weak here as part of a GC pass. Specifically, the Ref might
      // have previously been added to an object that already had a JS wrapper before the Ref was
      // added. In this case, we won't detect that the Ref is traced until the next GC pass reaches
      // it.
      refStrong = false;
      removeStrongRef();
    }
  }

  KJ_IF_MAYBE(cgv, visitor.cppgcVisitor) {
    // We're visiting for the purpose of a GC trace.
    KJ_IF_MAYBE(w, wrapper) {
      cgv->Trace(*w);
    } else {
      // This object doesn't currently have a wrapper, so traces must transitively trace through
      // it. However, as an optimization, we can skip the trace if we've already been traced in
      // this trace pass.
      auto& tracer = HeapTracer::getTracer(isolate);
      if (lastTraceId != tracer.currentTraceId()) {
        lastTraceId = tracer.currentTraceId();
        GcVisitor subVisitor(*this, visitor.cppgcVisitor);
        jsgVisitForGc(subVisitor);
      }
    }
  }
}

void GcVisitor::visit(Data& value) {
  if (!value.handle.IsEmpty()) {
    // Make ref strength match the parent.
    if (parent.strongRefcount > 0 && parent.wrapper == nullptr) {
      // This is directly reachable by a strong ref, so mark the handle strong.
      if (value.tracedHandle != nullptr) {
        // Convert the handle back to strong and discard the traced reference.
        value.handle.ClearWeak();
        value.tracedHandle = nullptr;
      }
    } else {
      // This is only reachable via traced objects, so the handle should be weak, and we should
      // hold a TracedReference alongside it.
      if (value.tracedHandle == nullptr) {
        // Create the TrancedReference.
        v8::HandleScope scope(parent.isolate);
        value.tracedHandle = v8::TracedReference<v8::Data>(
            parent.isolate, value.handle.Get(parent.isolate));

        // Set the handle weak.
        value.handle.SetWeak();
      }
    }

    KJ_IF_MAYBE(c, cppgcVisitor) {
      KJ_IF_MAYBE(t, value.tracedHandle) {
        c->Trace(*t);
      }
    }
  }
}

}  // namespace workerd::jsg
