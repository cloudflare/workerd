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

namespace {

static thread_local bool inCppgcShimDestructor = false;

};

bool HeapTracer::isInCppgcDestructor() { return inCppgcShimDestructor; }

void HeapTracer::clearWrappers() {
  // When clearing wrappers (at isolate shutdown), we may be destroying objects that were recenly
  // determined to be unreachable, but the CppgcShim destructors haven't been run yet. We need to
  // treat this case as if we are running CppgcShim destructors, that is, assume any
  // TracedReferences we destroy have already been collected so cannot be touched.
  // TODO(cleanup): Rename `inCppgcShimDestructor` to `possiblyCollectingUnreachableObject`?
  KJ_ASSERT(!inCppgcShimDestructor);
  inCppgcShimDestructor = true;
  KJ_DEFER(inCppgcShimDestructor = false);

  while (!wrappers.empty()) {
    // Don't freelist the shim because we're shutting down anyway.
    wrappers.front().detachWrapper(false);
  }
  clearFreelistedShims();
}

class Wrappable::CppgcShim final: public cppgc::GarbageCollected<CppgcShim> {
  // V8's GC integrates with cppgc, aka "oilpan", a garbage collector for C++ objects. We want to
  // integrate with the GC in order to receive GC visitation callbacks, so that the GC is able to
  // trace through our C++ objects to find what is reachable through them. The only way for us to
  // supprot this is by integrating with cppgc.
  //
  // However, workerd was written using KJ idioms long before cppgc existed. Rewriting all our code
  // to use cppgc allocation instead would be a highly invasive change. Maybe we'll do it someday,
  // but today is not the day. So, our API objects continue to be allocated on the regular (non-GC)
  // C++ heap.
  //
  // CppgcShim provides a compromise. For each API object that has been wrapped for use from JS,
  // we create a CppgcShim object on the cppgc heap. This basically just contains a pointer to the
  // regular old C++ object. This lets us get our GC visitation without fully integrating with
  // cppgc.
  //
  // There is an additional trick here: As of this writing, cppgc objects cannot be collected
  // during V8's minor GC passes ("scavenge" passes). Only full GCs ("trace" passes) can collect
  // them. But we do want our API objects to be collectable during minor GC. We integrate with V8's
  // EmbedderRootsHandler to get notification when these objects can be collected. But when they
  // are, what happens to the CppgcShim object we allocated? We can't force it to be collected
  // early. We could just discard it and let it be collected during the next major GC, but that
  // would mean accumulating a lot of garbage shims. Instead, we freelist the objects: when a
  // wrapper is collected during minor GC, the CppgcShim is placed in a freelist and can be
  // reused for a future allocation, if that allocation occurs before the next major GC. When a
  // major GC occurs, the freelist is cleared, since any unreachable CppgcShim objects are likely
  // condemned after that point and will be deleted shortly thereafter.

public:
  CppgcShim(Wrappable& wrappable): state(Active { kj::addRef(wrappable) }) {
    KJ_DASSERT(wrappable.cppgcShim == nullptr);
    wrappable.cppgcShim = *this;
  }

  ~CppgcShim() {
    // (Unlike most KJ destructors, we don't mark this noexcept(false) because it's called from
    // V8 which doesn't support exceptions.)

    KJ_DASSERT(!inCppgcShimDestructor);
    inCppgcShimDestructor = true;
    KJ_DEFER(inCppgcShimDestructor = false);

    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(active, Active) {
        KJ_DASSERT(&KJ_ASSERT_NONNULL(active.wrappable->cppgcShim) == this);
        KJ_DASSERT(active.wrappable->strongWrapper.IsEmpty());
        active.wrappable->detachWrapper(false);
      }
      KJ_CASE_ONEOF(freelisted, Freelisted) {
        KJ_DASSERT(&KJ_ASSERT_NONNULL(*freelisted.prev) == this);
        *freelisted.prev = freelisted.next;
        KJ_IF_MAYBE(next, freelisted.next) {
          KJ_DASSERT(next->state.get<Freelisted>().prev == &freelisted.next);
          next->state.get<Freelisted>().prev = freelisted.prev;
        }
      }
      KJ_CASE_ONEOF(d, Dead) {}
    }
  }

  void Trace(cppgc::Visitor* visitor) const {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(active, Active) {
        active.wrappable->traceFromV8(*visitor);
      }
      KJ_CASE_ONEOF(freelisted, Freelisted) {
        // We're tracing a shim for an object that was collected in minor GC. This could happen
        // due to conservative GC or due to incremental marking. Unfortunately the shim won't be
        // collected on this pass but hopefully it can be on the next pass.
      }
      KJ_CASE_ONEOF(d, Dead) {}
    }
  }

  struct Active {
    kj::Own<Wrappable> wrappable;
  };
  struct Freelisted {
    // The JavaScript wrapper using this shim was collected in a minor GC. cppgc objects can only
    // be collected in full GC, so we freelist the shim object in the meantime.

    kj::Maybe<Wrappable::CppgcShim&> next;
    kj::Maybe<Wrappable::CppgcShim&>* prev;
    // kj::List doesn't quite work here because the list link is inside a OneOf. Also we want a
    // LIFO list anyway so we don't need a tail pointer, which makes things easier. So we do it
    // manually.
  };
  struct Dead {};

  mutable kj::OneOf<Active, Freelisted, Dead> state;
  // This is `mutable` because `Trace()` is const. We configure V8 to perform traces atomically in
  // the main thread so concurrency is not a concern.
};

void HeapTracer::addToFreelist(Wrappable::CppgcShim& shim) {
  auto& freelisted = shim.state.init<Wrappable::CppgcShim::Freelisted>();
  freelisted.next = freelistedShims;
  KJ_IF_MAYBE(next, freelisted.next) {
    next->state.get<Wrappable::CppgcShim::Freelisted>().prev = &freelisted.next;
  }
  freelisted.prev = &freelistedShims;
  freelistedShims = shim;
}

Wrappable::CppgcShim* HeapTracer::allocateShim(Wrappable& wrappable) {
  KJ_IF_MAYBE(shim, freelistedShims) {
    freelistedShims = shim->state.get<Wrappable::CppgcShim::Freelisted>().next;
    KJ_IF_MAYBE(next, freelistedShims) {
      next->state.get<Wrappable::CppgcShim::Freelisted>().prev = &freelistedShims;
    }
    shim->state = Wrappable::CppgcShim::Active { kj::addRef(wrappable) };
    KJ_DASSERT(wrappable.cppgcShim == nullptr);
    wrappable.cppgcShim = *shim;
    return shim;
  } else {
    auto& cppgcAllocHandle = isolate->GetCppHeap()->GetAllocationHandle();
    return cppgc::MakeGarbageCollected<Wrappable::CppgcShim>(cppgcAllocHandle, wrappable);
  }
}

void HeapTracer::clearFreelistedShims() {
  for (;;) {
    KJ_IF_MAYBE(shim, freelistedShims) {
      freelistedShims = shim->state.get<Wrappable::CppgcShim::Freelisted>().next;
      shim->state = Wrappable::CppgcShim::Dead {};
    } else {
      break;
    }
  }
}

kj::Own<Wrappable> Wrappable::detachWrapper(bool shouldFreelistShim) {
  KJ_IF_MAYBE(shim, cppgcShim) {
    auto& tracer = HeapTracer::getTracer(isolate);
    auto result = kj::mv(KJ_ASSERT_NONNULL(shim->state.tryGet<CppgcShim::Active>()).wrappable);
    if (shouldFreelistShim) {
      tracer.addToFreelist(*shim);
    } else {
      shim->state = CppgcShim::Dead {};
    }
    wrapper = nullptr;
    cppgcShim = nullptr;
    strongWrapper.Reset();
    tracer.removeWrapper({}, *this);
    if (strongRefcount > 0) {
      // Need to visit child references in order to convert them to strong references, since we
      // no longer have an intervening wrapper.
      GcVisitor visitor(*this, nullptr);
      jsgVisitForGc(visitor);
    }
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
  cppgcVisitor.Trace(KJ_ASSERT_NONNULL(wrapper));
  GcVisitor visitor(*this, cppgcVisitor);
  jsgVisitForGc(visitor);
}

void Wrappable::attachWrapper(v8::Isolate* isolate,
                              v8::Local<v8::Object> object, bool needsGcTracing) {
  auto& tracer = HeapTracer::getTracer(isolate);

  KJ_REQUIRE(wrapper == nullptr);
  KJ_REQUIRE(strongWrapper.IsEmpty());

  auto& wrapperRef = wrapper.emplace(isolate, object);
  this->isolate = isolate;

  // Set a class ID so we can recognize this in HeapTracer::IsRoot(). We reuse WRAPPABLE_TAG for
  // this for lack of a reason not to, though technically we could be using a different identifier
  // here vs. in WRAPPABLE_TAG_FIELD_INDEX below.
  wrapperRef.SetWrapperClassId(WRAPPABLE_TAG);

  // Add to list of objects to force-clean at isolate shutdown.
  tracer.addWrapper({}, *this);

  // Set up internal fields for a newly-allocated object.
  KJ_REQUIRE(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
  object->SetAlignedPointerInInternalField(WRAPPED_OBJECT_FIELD_INDEX, this);

  // Allocate the cppgc shim.
  auto cppgcShim = tracer.allocateShim(*this);

  object->SetAlignedPointerInInternalField(CPPGC_SHIM_FIELD_INDEX, cppgcShim);
  object->SetAlignedPointerInInternalField(WRAPPABLE_TAG_FIELD_INDEX,
      // const_cast because V8 expects non-const `void*` pointers, but it won't actually modify
      // the tag.
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
      GcVisitor subVisitor(*this, visitor.cppgcVisitor);
      jsgVisitForGc(subVisitor);
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
        // Create the TracedReference.
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
