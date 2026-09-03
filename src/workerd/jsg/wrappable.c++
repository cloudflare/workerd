// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wrappable.h"

#include "jsg.h"
#include "setup.h"

#include <workerd/util/thread-scopes.h>

#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#include <v8-cppgc.h>

#include <kj/async.h>
#include <kj/debug.h>

#include <cstdlib>

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
    auto& wrappable = wrappers.front();
    auto ownWrappable = wrappable.detachWrapper(false);
    // Clear the isolate pointer so that any code that later tries to use it (e.g.,
    // maybeDeferDestruction() or GcVisitor) will see that the isolate is gone and skip
    // V8 operations. Without this, objects that outlive their isolate (like WebSockets
    // stored in a HibernationManager) would have a dangling isolate pointer and crash
    // when trying to check v8::Locker::IsLocked() or create V8 handles.
    ownWrappable->isolate = nullptr;
  }
  clearFreelistedShims();
}

using JSGWrappable = workerd::jsg::Wrappable;

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
class Wrappable::CppgcShim final: public v8::Object::Wrappable {
 public:
  CppgcShim(JSGWrappable& wrappable, v8::CppHeapPointerTag tag)
      : tag(tag),
        state(Active{kj::addRef(wrappable)}) {
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
      KJ_CASE_ONEOF(d, Dead) {}
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
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

  const char* GetHumanReadableName() const override {
    return "CppgcShim";
  }

  struct Active {
    kj::Own<JSGWrappable> wrappable;
  };

  // The JavaScript wrapper using this shim was collected in a minor GC. cppgc objects can only
  // be collected in full GC, so we freelist the shim object in the meantime.
  struct Freelisted {
    kj::Maybe<JSGWrappable::CppgcShim&> next;
    kj::Maybe<JSGWrappable::CppgcShim&>* prev;
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

  JSGWrappable& resolve(v8::Local<v8::Object> object) {
    KJ_IF_SOME(active, state.tryGet<Active>()) {
      KJ_IF_SOME(wrapper, active.wrappable->wrapper) {
        if (wrapper == object) {
          return *active.wrappable;
        }
      } else {
        KJ_FAIL_ASSERT("active CppgcShim has no wrapper");
      }
    }
    reportWrapperIdentityMismatch();
  }

  // The per-type CppHeapPointerTag this shim was Wrapped with. A shim is only ever reused for the
  // same tag (see HeapTracer::freelistedShimsByTag), so this value is fixed for the shim's whole
  // lifetime and identifies which freelist bucket it belongs to.
  v8::CppHeapPointerTag tag;

  mutable kj::OneOf<Active, Freelisted, Dead> state;
  // This is `mutable` because `Trace()` is const. We configure V8 to perform traces atomically in
  // the main thread so concurrency is not a concern.
};

kj::Maybe<JSGWrappable::CppgcShim&>& HeapTracer::freelistHeadFor(v8::CppHeapPointerTag tag) {
  auto index = wrappableTagBucketIndex(static_cast<uint16_t>(tag));
  // Guaranteed in range by the static_assert in TypeWrapper::wrappableTag<T>() at every wrap site;
  // this is a defensive backstop for the non-resource catch-all and any future direct callers.
  KJ_ASSERT(index < kMaxWrappableTags);
  return freelistedShimsByTag[index];
}

void HeapTracer::addToFreelist(JSGWrappable::CppgcShim& shim) {
  auto& head = freelistHeadFor(shim.tag);
  auto& freelisted = shim.state.init<JSGWrappable::CppgcShim::Freelisted>();
  freelisted.next = head;
  KJ_IF_SOME(next, freelisted.next) {
    next.state.get<JSGWrappable::CppgcShim::Freelisted>().prev = &freelisted.next;
  }
  freelisted.prev = &head;
  head = shim;
}

JSGWrappable::CppgcShim* HeapTracer::allocateShim(
    JSGWrappable& wrappable, v8::CppHeapPointerTag tag) {
  // Under gc-stress (either turn-boundary or alloc mode), skip freelist reuse so each freed shim
  // keeps a unique address and ASAN poisoning sticks, giving cleaner use-after-free reports.
  if (!isGcStressModeForTest() && !isAllocGcStressModeForTest()) {
    // Only reuse a shim from the bucket for this exact tag, so a recycled shim's table-entry tag
    // never changes and a stale wrapper handle can't be confused across types.
    auto& head = freelistHeadFor(tag);
    KJ_IF_SOME(shim, head) {
      head = shim.state.get<JSGWrappable::CppgcShim::Freelisted>().next;
      KJ_IF_SOME(next, head) {
        next.state.get<JSGWrappable::CppgcShim::Freelisted>().prev = &head;
      }
      KJ_DASSERT(shim.tag == tag);
      shim.state = JSGWrappable::CppgcShim::Active{kj::addRef(wrappable)};
      KJ_DASSERT(wrappable.cppgcShim == kj::none);
      wrappable.cppgcShim = shim;
      return &shim;
    }
  }
  auto& cppgcAllocHandle = isolate->GetCppHeap()->GetAllocationHandle();
  auto* shim =
      cppgc::MakeGarbageCollected<JSGWrappable::CppgcShim>(cppgcAllocHandle, wrappable, tag);
  return shim;
}

void HeapTracer::clearFreelistedShims() {
  for (auto& head: freelistedShimsByTag) {
    for (;;) {
      KJ_IF_SOME(shim, head) {
        head = shim.state.get<JSGWrappable::CppgcShim::Freelisted>().next;
        shim.state = JSGWrappable::CppgcShim::Dead{};
      } else {
        break;
      }
    }
  }
}

void HeapTracer::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& wrapper: wrappers) {
    tracker.trackField("wrapper", wrapper);
  }
  // TODO(soon): Track the other fields here?
}

void HeapTracer::substituteCppgcShimForTest(
    v8::Isolate* isolate, v8::Local<v8::Object> target, v8::Local<v8::Object> source) {
  auto* shim = v8::Object::Unwrap<v8::Object::Wrappable>(isolate, source, kJsgWrappableTagRange);
  KJ_REQUIRE(shim != nullptr);
  v8::Object::Wrap(isolate, target, shim, static_cast<Wrappable::CppgcShim*>(shim)->tag);
}

void substituteCppgcShimForTest(
    v8::Isolate* isolate, v8::Local<v8::Object> target, v8::Local<v8::Object> source) {
  HeapTracer::substituteCppgcShimForTest(isolate, target, source);
}

void detachWrapperForTest(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  auto& wrappable = *Wrappable::unwrapFromShimAnyType(isolate, object);
  auto drop = wrappable.detachWrapper(true);
}

void resetRootForTest(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  v8::TracedReference<v8::Value> handle(isolate, object);
  HeapTracer::getTracer(isolate).ResetRoot(handle);
}

bool sharesCppgcShimForTest(
    v8::Isolate* isolate, v8::Local<v8::Object> first, v8::Local<v8::Object> second) {
  auto* firstShim =
      v8::Object::Unwrap<v8::Object::Wrappable>(isolate, first, kJsgWrappableTagRange);
  auto* secondShim =
      v8::Object::Unwrap<v8::Object::Wrappable>(isolate, second, kJsgWrappableTagRange);
  return firstShim != nullptr && secondShim != nullptr && firstShim == secondShim;
}

Wrappable* Wrappable::unwrapFromShim(
    v8::Isolate* isolate, v8::Local<v8::Object> object, v8::CppHeapPointerTagRange tagRange) {
  // Read the CppgcShim out of V8's CppHeap pointer table, requiring the stored tag to fall within
  // `tagRange`. If the object was never wrapped, or its tag is outside the range (a type-confused
  // or forged handle), Unwrap returns nullptr.
  auto* shim =
      static_cast<CppgcShim*>(v8::Object::Unwrap<v8::Object::Wrappable>(isolate, object, tagRange));
  if (shim == nullptr) {
    return nullptr;
  }

  return &shim->resolve(object);
}

Wrappable* Wrappable::unwrapFromShimAnyType(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  return unwrapFromShim(isolate, object, kJsgWrappableTagRange);
}

Wrappable* Wrappable::unwrapFromShimInRangeOrAbort(
    v8::Isolate* isolate, v8::Local<v8::Object> object, v8::CppHeapPointerTagRange tagRange) {
  // Unwrap with the full JSG range so Object::Unwrap never faults, then range-check the shim's own
  // tag ourselves. See the header for why the check can't be delegated to Object::Unwrap, and why
  // an out-of-range tag here is a memory-safety violation that must abort.
  auto* shim = static_cast<CppgcShim*>(
      v8::Object::Unwrap<v8::Object::Wrappable>(isolate, object, kJsgWrappableTagRange));
  if (shim != nullptr) {
    auto tag = static_cast<uint16_t>(shim->tag);
    if (tag >= static_cast<uint16_t>(tagRange.first) &&
        tag <= static_cast<uint16_t>(tagRange.last)) {
      return &shim->resolve(object);
    }
  }

  KJ_LOG(FATAL, "wrapper type mismatch: object's tag is outside the expected range");
  abort();
}

kj::Own<JSGWrappable> JSGWrappable::detachWrapper(bool shouldFreelistShim) {
  KJ_IF_SOME(shim, cppgcShim) {
    auto& tracer = HeapTracer::getTracer(isolate);
    auto result =
        kj::mv(KJ_ASSERT_NONNULL(shim.state.tryGet<JSGWrappable::CppgcShim::Active>()).wrappable);
    if (shouldFreelistShim) {
      tracer.addToFreelist(shim);
    } else {
      shim.state = JSGWrappable::CppgcShim::Dead{};
    }
    wrapper = kj::none;
    cppgcShim = kj::none;
    strongWrapper.Reset();
    // Note: weak refs are deliberately NOT invalidated here. Detaching the wrapper does not
    // imply the object is dying: this method also runs when V8 drops an unmodified droppable
    // wrapper via ResetRoot() (the object stays alive through C++ refs and the wrapper is
    // recreated on demand the next time it is passed to JS) and at isolate shutdown via
    // clearWrappers() (objects like hibernatable WebSockets outlive the isolate). A
    // jsg::WeakRef tracks the object's lifetime, not the wrapper's; invalidation happens in
    // ~Wrappable().
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
  // The `isolate == nullptr` check here ensures that `js.alloc<T>()` can be used with no
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
    auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(SET_DATA_ISOLATE_BASE));
    jsgIsolate.deferDestruction(kj::mv(item));
  }
}

void Wrappable::traceFromV8(cppgc::Visitor& cppgcVisitor) {
  cppgcVisitor.Trace(KJ_ASSERT_NONNULL(wrapper));
  GcVisitor visitor(*this, cppgcVisitor);
  jsgVisitForGc(visitor);
}

void Wrappable::attachWrapper(v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    bool needsGcTracing,
    v8::CppHeapPointerTag tag) {
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

  // Set up internal fields for a newly-allocated object.
  KJ_REQUIRE(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
  // Internal field 0 holds a marker identifying this as a workerd API object; see
  // isWorkerdApiObject(). The marker's address is a small integer type-tagged by slot index.
  auto tagAddress = const_cast<uint16_t*>(&WORKERD_WRAPPABLE_TAG);
  object->SetAlignedPointerInInternalField(WRAPPABLE_TAG_FIELD_INDEX, tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(WRAPPABLE_TAG_FIELD_INDEX));

  // The C++ object is reached only through the CppgcShim, stored in V8's CppHeap pointer table
  // with a per-type tag. There is deliberately no second, independently-corruptible pointer to
  // the object in an internal field: a single tagged handle means dispatch and GC lifetime can
  // never be driven by different pointers, closing the wrapper type-confusion / use-after-free
  // class of bugs. The shim carries the same tag so it lands in the matching freelist bucket.
  auto* shim = tracer.allocateShim(*this, tag);

  // Add to list of objects to force-clean at isolate shutdown. This pairs with `cppgcShim`, which
  // allocateShim() has just set and detachWrapper() clears as it unlinks us again, so the two are
  // never out of step. Membership without a shim would break clearWrappers(), which relies on
  // detachWrapper() returning the Wrappable it unlinks; a shim without membership would break
  // detachWrapper(), which unlinks unconditionally once it sees a shim.
  tracer.addWrapper({}, *this);

  v8::Object::Wrap(isolate, object, shim, tag);

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
  auto isolate = v8::Isolate::GetCurrent();
  auto object =
      jsg::check(IsolateBase::getOpaqueTemplate(isolate)->InstanceTemplate()->NewInstance(context));
  // Null prototype: opaque wrappers flow through v8::Promise::Resolver::Resolve(), whose thenable
  // check does Get(value, "then"). A null prototype keeps that lookup off Object.prototype.
  jsg::check(object->SetPrototype(context, v8::Null(isolate)));
  attachWrapper(isolate, object, needsGcTracing,
      static_cast<v8::CppHeapPointerTag>(kNonResourceWrappableTag));
  return object;
}

kj::Maybe<Wrappable&> Wrappable::tryUnwrapOpaque(
    v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  if (handle->IsObject()) {
    v8::Local<v8::Object> instance =
        v8::Local<v8::Object>::Cast(handle)->FindInstanceInPrototypeChain(
            IsolateBase::getOpaqueTemplate(isolate));
    if (!instance.IsEmpty()) {
      // The prototype-chain check above accepts exactly our opaque wrappers, which all carry
      // kNonResourceWrappableTag. A tag outside that range therefore means the object's CppHeap
      // handle was redirected to an unrelated type while its prototype chain was left intact -- an
      // in-sandbox memory-safety violation, which aborts (matching the resource unwrap sites)
      // rather than returning a mistyped pointer for a downstream dynamic_cast to catch.
      return *unwrapFromShimInRangeOrAbort(isolate, instance, kNonResourceWrappableTagRange);
    }
  }

  return kj::none;
}

void reportWrapperTypeMismatch(const std::type_info& expected, const std::type_info& actual) {
  // Only reachable if the wrapper's internal field has been made to point at an object of the
  // wrong type, which means memory outside this process's control has already been corrupted.
  // Abort: edgeworker's crash handler turns this into an abrupt shutdown of the isolate.
  KJ_LOG(FATAL, "JS wrapper's C++ object is not of the expected type", typeName(expected),
      typeName(actual));
  abort();
}

void reportWrapperIdentityMismatch() {
  KJ_LOG(FATAL, "JS wrapper's CppHeap shim does not own the dispatch receiver");
  abort();
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
        value.handle.ClearWeak<void>();
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

void GcVisitor::visit(v8::Global<v8::Value>& strong, v8::TracedReference<v8::Data>& traced) {
  if (strong.IsEmpty()) {
    return;
  }

  // Mirror visit(Data&): make handle strength match the parent.
  //
  // The `parent.wrapper == kj::none` check mirrors the same condition in
  // visit(Data&): even when strongRefcount > 0, if the JS wrapper already
  // exists we must keep the handle in traced mode so cppgc can follow edges
  // from it.  Only when there is no wrapper yet (object not yet exported to JS)
  // does a positive strongRefcount alone justify keeping the handle strong.
  if (parent.strongRefcount > 0 && parent.wrapper == kj::none) {
    // Parent has strong Rust refs and no JS wrapper — keep handle strong,
    // discard any traced ref.
    if (!traced.IsEmpty()) {
      strong.ClearWeak<void>();
      traced.Reset();
    }
  } else {
    // Parent is only reachable via GC tracing — downgrade to a TracedReference.
    if (traced.IsEmpty()) {
      v8::HandleScope scope(parent.isolate);
      traced.Reset(parent.isolate, strong.Get(parent.isolate));
      strong.SetWeak();
    }
  }

  KJ_IF_SOME(c, cppgcVisitor) {
    if (!traced.IsEmpty()) {
      c.Trace(traced);
    }
  }
}

}  // namespace workerd::jsg
