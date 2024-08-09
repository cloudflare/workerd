// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wrappable.h"
#include "jsg.h"
#include "setup.h"
#include <kj/debug.h>
#include <kj/async.h>
#include <v8-cppgc.h>
#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

namespace workerd::jsg {

namespace {

static thread_local bool inCppgcShimDestructor = false;

};

bool HeapTracer::isInCppgcDestructor() {
  return inCppgcShimDestructor;
}

void HeapTracer::clearWrappers() {
  // When clearing wrappers (at isolate shutdown), we may be destroying objects that were recently
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

// V8's GC integrates with cppgc, aka "oilpan", a garbage collector for C++ objects. We want to
// integrate with the GC in order to receive GC visitation callbacks, so that the GC is able to
// trace through our C++ objects to find what is reachable through them. The only way for us to
// support this is by integrating with cppgc.
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
class Wrappable::CppgcShim final: public cppgc::GarbageCollected<CppgcShim> {
public:
  CppgcShim(Wrappable& wrappable): state(Active{kj::addRef(wrappable)}) {
    KJ_DASSERT(wrappable.cppgcShim == kj::none);
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
        KJ_IF_SOME(next, freelisted.next) {
          KJ_DASSERT(next.state.get<Freelisted>().prev == &freelisted.next);
          next.state.get<Freelisted>().prev = freelisted.prev;
        }
      }
      KJ_CASE_ONEOF(d, Dead) {
      }
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
      KJ_CASE_ONEOF(d, Dead) {
      }
    }
  }

  struct Active {
    kj::Own<Wrappable> wrappable;
  };

  // The JavaScript wrapper using this shim was collected in a minor GC. cppgc objects can only
  // be collected in full GC, so we freelist the shim object in the meantime.
  struct Freelisted {
    kj::Maybe<Wrappable::CppgcShim&> next;
    kj::Maybe<Wrappable::CppgcShim&>* prev;
    // kj::List doesn't quite work here because the list link is inside a OneOf. Also we want a
    // LIFO list anyway so we don't need a tail pointer, which makes things easier. So we do it
    // manually.
  };
  struct Dead {};

  kj::StringPtr jsgGetMemoryName() const {
    return "CppgcShim"_kjc;
  }
  size_t jsgGetMemorySelfSize() const {
    return sizeof(CppgcShim);
  }
  void jsgGetMemoryInfo(MemoryTracker& tracker) const {
    KJ_IF_SOME(active, state.tryGet<Active>()) {
      tracker.trackField("wrappable", active.wrappable);
    }
  }
  bool jsgGetMemoryInfoIsRootNode() const {
    return false;
  }

  mutable kj::OneOf<Active, Freelisted, Dead> state;
  // This is `mutable` because `Trace()` is const. We configure V8 to perform traces atomically in
  // the main thread so concurrency is not a concern.
};

void HeapTracer::addToFreelist(Wrappable::CppgcShim& shim) {
  auto& freelisted = shim.state.init<Wrappable::CppgcShim::Freelisted>();
  freelisted.next = freelistedShims;
  KJ_IF_SOME(next, freelisted.next) {
    next.state.get<Wrappable::CppgcShim::Freelisted>().prev = &freelisted.next;
  }
  freelisted.prev = &freelistedShims;
  freelistedShims = shim;
}

Wrappable::CppgcShim* HeapTracer::allocateShim(Wrappable& wrappable) {
  KJ_IF_SOME(shim, freelistedShims) {
    freelistedShims = shim.state.get<Wrappable::CppgcShim::Freelisted>().next;
    KJ_IF_SOME(next, freelistedShims) {
      next.state.get<Wrappable::CppgcShim::Freelisted>().prev = &freelistedShims;
    }
    shim.state = Wrappable::CppgcShim::Active{kj::addRef(wrappable)};
    KJ_DASSERT(wrappable.cppgcShim == kj::none);
    wrappable.cppgcShim = shim;
    return &shim;
  } else {
    auto& cppgcAllocHandle = isolate->GetCppHeap()->GetAllocationHandle();
    return cppgc::MakeGarbageCollected<Wrappable::CppgcShim>(cppgcAllocHandle, wrappable);
  }
}

void HeapTracer::clearFreelistedShims() {
  for (;;) {
    KJ_IF_SOME(shim, freelistedShims) {
      freelistedShims = shim.state.get<Wrappable::CppgcShim::Freelisted>().next;
      shim.state = Wrappable::CppgcShim::Dead{};
    } else {
      break;
    }
  }
}

void HeapTracer::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& wrapper: wrappers) {
    tracker.trackField("wrapper", wrapper);
  }
  // TODO(soon): Track the other fields here?
}

kj::Own<Wrappable> Wrappable::detachWrapper(bool shouldFreelistShim) {
  KJ_IF_SOME(shim, cppgcShim) {
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
    // There's a possibility that the CppgcShim has already been found to be unreachable by a GC
    // pass, but has not actually been destroyed yet. For some reason, cppgc likes to delay the
    // calling of actual destructors. However, in ASAN builds, cppgc will poison the memory in the
    // meantime, because it figures that we "shouldn't" be accessing unreachable memory. This
    // assumption makes sense in the abstract, but not for our specific use case, where we are
    // essentially maintaining a weak pointer to the CppgcShim. If the destructor had been called,
    // then `cppgcShim` here would have been nulled out at that time. We're expecting that until
    // the destructor is called, we can still safely access the object to detach the wrapper.
    //
    // So to work around cppgc's incorrect assumption, we manually unpoison the memory.
    //
    // Note: An alternative strategy could have been for CppgcShim itself to allocate a separate
    // C++ heap object to store its own state in, so that that state could be modified even while
    // the CppgcShim object itself is poisoned. In this case `Wrappable::cppgcShim` would change to
    // point at this state object, not to the `CppgcShim` itself. However, this approach would
    // require extra heap allocation for everyone, just to satisfy ASAN, which seems undesirable.
    ASAN_UNPOISON_MEMORY_REGION(&shim, sizeof(shim));
#endif

    auto& tracer = HeapTracer::getTracer(isolate);
    auto result = kj::mv(KJ_ASSERT_NONNULL(shim.state.tryGet<CppgcShim::Active>()).wrappable);
    if (shouldFreelistShim) {
      tracer.addToFreelist(shim);
    } else {
      shim.state = CppgcShim::Dead{};
    }
    wrapper = kj::none;
    cppgcShim = kj::none;
    strongWrapper.Reset();
    tracer.removeWrapper({}, *this);
    if (strongRefcount > 0) {
      // Need to visit child references in order to convert them to strong references, since we
      // no longer have an intervening wrapper.
      GcVisitor visitor(*this, kj::none);
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
  // The `isolate == nullptr` check here ensures that `jsg::alloc<T>()` can be used with no
  // isolate, simply allocating the object as a normal C++ heap object.
  KJ_DREQUIRE(isolate == nullptr || v8::Isolate::TryGetCurrent() != nullptr,
      "referencing wrapper without isolate lock");
  if (strongRefcount++ == 0) {
    // This object previously had no strong references, but now it has one.
    KJ_IF_SOME(w, wrapper) {
      // Copy the traced reference into the strong reference.
      v8::HandleScope scope(isolate);
      strongWrapper.Reset(isolate, w.Get(isolate));
    } else {
      // Since we have no JS wrapper, we're forced to recursively mark all references reachable
      // through this wrapper as strong.
      GcVisitor visitor(*this, kj::none);
      jsgVisitForGc(visitor);
    }
  }
}
void Wrappable::removeStrongRef() {
  KJ_DREQUIRE(isolate == nullptr || v8::Isolate::TryGetCurrent() == isolate,
      "destroying wrapper without isolate lock");
  if (--strongRefcount == 0) {
    // This was the last strong reference.
    if (wrapper == kj::none) {
      // We have no wrapper. We need to mark all references held by this object as weak.
      if (isolate != nullptr) {
        // But only if the current isolate isn't null. If strong ref count is zero,
        // the wrapper is empty, and isolate is null, then the child handles it has will
        // be released anyway (since we're about to be destroyed), thus this visitation
        // isn't required (and may be buggy, since it may happen outside the isolate lock).
        GcVisitor visitor(*this, kj::none);
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

void Wrappable::attachWrapper(
    v8::Isolate* isolate, v8::Local<v8::Object> object, bool needsGcTracing) {
  auto& tracer = HeapTracer::getTracer(isolate);

  KJ_REQUIRE(wrapper == kj::none);
  KJ_REQUIRE(strongWrapper.IsEmpty());

  // The C++ Wrappable object must hold a TracedReference to its own JavaScript wrapper, while
  // such a wrapper exists. This way, if the object is reached through C++ again later, we can
  // return the same object to JavaScript.
  //
  // This reference is special: it is marked as "droppable". This tells V8 that we know how to
  // recreate this wrapper on-demand (from the C++ object). This is an optimization: If the
  // application drops all of its direct references to the wrapper, such that object is only
  // reachable implicitly through C++ objects, then V8 can drop the wrapper entirely and have us
  // recreate it later, when JS needs it again.
  //
  // For example, consider a Request object that contains a Headers object. Say the application
  // accesses the Headers briefly, like `request.headers.get("foo")` -- it doesn't keep around a
  // direct reference to the Headers. But it DOES keep around a reference to the Request, and the
  // C++ API object backing the Request keeps a `jsg::Ref<Headers>`. In this case, we do not really
  // need the JavaScript wrapper for `Headers` to stick around. We know we can create a new one if
  // and when it is needed. So we tell V8 that our internal reference is "droppable", so that it
  // will go ahead and drop it in this scenario. (Specifically, v8 calls
  // `EmbedderRootsHandler::ResetRoot()`, which is implemented by our `HeapTracer`, to tell us that
  // it is dropping the wrapper.)
  //
  // Note that there are things that the application might do which actually make it unsafe for us
  // to drop and recreate the wrapper. For example, the application could add a property to the
  // wrapper object itself, like `request.headers.foo = 123`. Later on, when the app accesses
  // `request.headers.foo` again, it expects the property will still be there. But if we dropped
  // our wrapper and recreated it, the property would be gone. Luckily, V8 already handles this
  // for us! V8 knows not to drop our wrapper if the application has done anything with it such
  // that a recreated wrapper would no longer be equivalent.
  wrapper.emplace(isolate, object, v8::TracedReference<v8::Object>::IsDroppable());
  this->isolate = isolate;

  // Add to list of objects to force-clean at isolate shutdown.
  tracer.addWrapper({}, *this);

  // Set up internal fields for a newly-allocated object.
  KJ_REQUIRE(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
  int indices[] = {WRAPPABLE_TAG_FIELD_INDEX, WRAPPED_OBJECT_FIELD_INDEX};
  void* values[] = {const_cast<uint16_t*>(&WORKERD_WRAPPABLE_TAG), this};
  object->SetAlignedPointerInInternalFields(2, indices, values);

  v8::Object::Wrap<WRAPPABLE_TAG>(isolate, object, tracer.allocateShim(*this));

  if (strongRefcount > 0) {
    strongWrapper.Reset(isolate, object);

    // This object has untraced references, but didn't have a wrapper. That means that any refs
    // transitively reachable through the reference are strong. Now that a wrapper exists, the
    // refs will be traced when the wrapper is traced, so they should be converted to traced
    // references. Performing a visitation pass will update them.
    GcVisitor visitor(*this, kj::none);
    jsgVisitForGc(visitor);
  }
}

void Wrappable::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("cppgcshim", cppgcShim);
}

v8::Local<v8::Object> Wrappable::attachOpaqueWrapper(
    v8::Local<v8::Context> context, bool needsGcTracing) {
  auto isolate = context->GetIsolate();
  auto object =
      jsg::check(IsolateBase::getOpaqueTemplate(isolate)->InstanceTemplate()->NewInstance(context));
  attachWrapper(isolate, object, needsGcTracing);
  return object;
}

kj::Maybe<Wrappable&> Wrappable::tryUnwrapOpaque(
    v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  if (handle->IsObject()) {
    v8::Local<v8::Object> instance =
        v8::Local<v8::Object>::Cast(handle)->FindInstanceInPrototypeChain(
            IsolateBase::getOpaqueTemplate(isolate));
    if (!instance.IsEmpty()) {
      return *reinterpret_cast<Wrappable*>(
          instance->GetAlignedPointerFromInternalField(WRAPPED_OBJECT_FIELD_INDEX));
    }
  }

  return kj::none;
}

void Wrappable::jsgVisitForGc(GcVisitor& visitor) {
  // Nothing; subclasses that need tracing will override.
}

void Wrappable::visitRef(GcVisitor& visitor, kj::Maybe<Wrappable&>& refParent, bool& refStrong) {
  KJ_IF_SOME(p, refParent) {
    KJ_ASSERT(&p == &visitor.parent);
  } else {
    refParent = visitor.parent;
  }

  if (isolate == nullptr) {
    isolate = visitor.parent.isolate;
  }

  // Make ref strength match the parent.
  if (visitor.parent.strongRefcount > 0 && visitor.parent.wrapper == kj::none) {
    // This reference should be strong, because the parent has strong refs and does not have its
    // own wrapper that will be traced.

    if (!refStrong) {
      // Ref transitions from weak to strong.
      //
      // This should never happen during a GC pass, since we should only be visiting traced
      // references then.
      KJ_ASSERT(visitor.cppgcVisitor == kj::none);
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

  KJ_IF_SOME(cgv, visitor.cppgcVisitor) {
    // We're visiting for the purpose of a GC trace.
    KJ_IF_SOME(w, wrapper) {
      cgv.Trace(w);
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
    if (parent.strongRefcount > 0 && parent.wrapper == kj::none) {
      // This is directly reachable by a strong ref, so mark the handle strong.
      if (value.tracedHandle != kj::none) {
        // Convert the handle back to strong and discard the traced reference.
        value.handle.ClearWeak();
        value.tracedHandle = kj::none;
      }
    } else {
      // This is only reachable via traced objects, so the handle should be weak, and we should
      // hold a TracedReference alongside it.
      if (value.tracedHandle == kj::none) {
        // Create the TracedReference.
        v8::HandleScope scope(parent.isolate);
        value.tracedHandle =
            v8::TracedReference<v8::Data>(parent.isolate, value.handle.Get(parent.isolate));

        // Set the handle weak.
        value.handle.SetWeak();
      }
    }

    KJ_IF_SOME(c, cppgcVisitor) {
      KJ_IF_SOME(t, value.tracedHandle) {
        c.Trace(t);
      }
    }
  }
}

}  // namespace workerd::jsg
