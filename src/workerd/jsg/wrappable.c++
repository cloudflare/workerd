// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wrappable.h"
#include "jsg.h"
#include "setup.h"
#include <kj/debug.h>

namespace workerd::jsg {

kj::Own<Wrappable> Wrappable::detachWrapper() {
  resetWrapperHandle();
  return detachWrapperRef();
}

void Wrappable::resetWrapperHandle() {
  if (!wrapper.IsEmpty()) {
    auto& tracer = HeapTracer::getTracer(isolate);
    detachedTraceId = tracer.currentTraceId();
    detached = tracer.isScavenging() ? WHILE_SCAVENGING
             : tracer.isTracing() ? WHILE_TRACING
             : OTHER;
    tracer.removeWrapper({}, *this);
  }
  wrapper.Reset();
}

kj::Own<Wrappable> Wrappable::detachWrapperRef() {
  return kj::mv(wrapperRef);
}

v8::Local<v8::Object> Wrappable::getHandle(v8::Isolate* isolate) {
  return KJ_REQUIRE_NONNULL(tryGetHandle(isolate));
}

void Wrappable::addStrongRef() {
  KJ_DREQUIRE(v8::Isolate::TryGetCurrent() != nullptr, "referencing wrapper without isolate lock");
  if (strongRefcount++ == 0) {
    // This object previously had no strong references, but now it has one.
    if (wrapper.IsEmpty()) {
      // Since we have no JS wrapper, we're forced to recursively mark all references reachable
      // through this wrapper as strong.
      GcVisitor visitor(*this);
      jsgVisitForGc(visitor);
    } else {
      // Mark the handle strong. V8 will find it and trace it.
      //
      // If a trace is already in-progress, V8 won't have registered this handle as a root at the
      // start of the trace, because it wasn't strong then. That's OK: as long as the handle still
      // exists and is strong when the trace cycle later enters its final pause, it'll be
      // discovered and traced then. OTOH if the handle becomes weak again before that (and
      // short-lived strong handles are common), then we can get away without tracing it.
      wrapper.ClearWeak<Wrappable>();
    }
  }
}
void Wrappable::removeStrongRef() {
  KJ_DREQUIRE(isolate == nullptr || v8::Isolate::TryGetCurrent() == isolate,
              "destroying wrapper without isolate lock");
  if (--strongRefcount == 0) {
    // This was the last strong reference.
    if (wrapper.IsEmpty()) {
      // We have no wrapper. We need to mark all references held by this object as weak.
      if (isolate != nullptr) {
        // But only if the current isolate isn't null. If strong ref count is zero,
        // the wrapper is empty, and isolate is null, then the child handles it has will
        // be released anyway (since we're about to be destroyed), thus this visitation
        // isn't required (and may be buggy, since it may happen outside the isolate lock).
        GcVisitor visitor(*this);
        jsgVisitForGc(visitor);
      }
    } else {
      // Mark the handle weak, so that it only stays alive if reached via tracing or if JavaScript
      // objects reference it.
      setWeak();
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

void Wrappable::traceFromV8(uint traceId) {
  if (lastTraceId == traceId) {
    // Duplicate trace, ignore.
    //
    // This can happen in particular if V8 choses to allocate an object unmarked but we determine
    // that the object is already reachable. In that case we mark the object *and* run our own
    // trace (because we can't be sure V8 didn't allocate the object already-marked), so we might
    // get duplicate traces.
  } else {
    lastTraceId = traceId;
    GcVisitor visitor(*this);
    jsgVisitForGc(visitor);
  }
}

void Wrappable::attachWrapper(v8::Isolate* isolate,
                              v8::Local<v8::Object> object, bool needsGcTracing) {
  auto& tracer = HeapTracer::getTracer(isolate);

  if (detached != NOT_DETACHED) {
    // It appears that this Wrappable once had a wrapper attached, and then that wrapper was GC'd,
    // but later on a wrapper was added again. This suggests a serious problem with our GC, in that
    // it is collecting objects that are still reachable from JavaScript. However, we can usually
    // continue operating even in the presence of such a bug: it'll only cause a real problem if
    // a script has attached additional properites to the object in JavaScript and expects them
    // to still be there later. This is relatively uncommon for scripts to do, though it does
    // happen.
#ifdef KJ_DEBUG
    KJ_FAIL_ASSERT("Wrappable had wrapper collected and then re-added later");
#else
    // Don't crash in production. Also avoid spamming logs.
    static bool alreadyWarned = false;
    if (!alreadyWarned) {
      kj::StringPtr collected;
      switch (detached) {
        case NOT_DETACHED:     collected = "NOT_DETACHED";     break;
        case WHILE_SCAVENGING: collected = "WHILE_SCAVENGING"; break;
        case WHILE_TRACING:    collected = "WHILE_TRACING";    break;
        case OTHER:            collected = "OTHER";            break;
      }
      KJ_LOG(ERROR, "Wrappable had wrapper collected and then re-added later", collected,
                    kj::getStackTrace(), lastTraceId, wrapper.getLastMarked(),
                    detachedTraceId, tracer.currentTraceId());
      alreadyWarned = true;
    }
#endif
  }

  KJ_REQUIRE(wrapper.IsEmpty());
  wrapperRef = kj::addRef(*this);
  wrapper.Reset(isolate, object);
  this->isolate = isolate;

  tracer.addWrapper({}, *this);

  // Set up internal fields for a newly-allocated object.
  KJ_REQUIRE(object->InternalFieldCount() == Wrappable::INTERNAL_FIELD_COUNT);
  object->SetAlignedPointerInInternalField(
      NEEDS_TRACING_FIELD_INDEX, needsGcTracing ? this : nullptr);
  object->SetAlignedPointerInInternalField(WRAPPED_OBJECT_FIELD_INDEX, this);

  if (lastTraceId == tracer.currentTraceId() || strongRefcount == 0) {
    // Either:
    // a) This object was reached during the most-recent trace cycle, but the wrapper wasn't
    //    allocated yet.
    // b) This object is currently only reachable from other JavaScript objects that themselves
    //    have wrappers reachable only from JavaScript. (Note: As of this writing, this never
    //    happens in practice since attachWrapper() is always called in cases where there is a
    //    strong ref, typically on the stack.)
    //
    // In either case, it's important that we inform V8 that the wrapper cannot be scavenged, since
    // it may be reachable via tracing. So, we must call tracer.mark(), which has the effect of
    // initializing the TracedReference.
    tracer.mark(wrapper);
  } else {
    // This object is not currently reachable via GC tracing from other C++ objects (it was not
    // reached during the most-recent cycle), therefore it does not need a v8::TracedReference
    // reference. It's best that we do not create such a reference unless it is needed, because the
    // presence of a TracedReference reference will make the object ineligible to be collected
    // during scavenges, because embedder heap tracing does not occur during those. Most wrappers
    // are only ever referenced from the JS heap, *not* from other C++ objects, therefore would
    // never be reached by tracing anyway -- we would like for those objects to remain eligible for
    // collection during scavenges.
    //
    // So, we will avoid initializing `tracedWrapper` until an object is first discovered to be
    // reachable via tracing from another C++ object.
  }

  if (strongRefcount == 0) {
    // This object has no untraced references, so we should make it weak. Note that any refs it
    // transitively holds are already weak, so we don't need to visit.
    setWeak();
  } else {
    // This object has untraced references, but didn't have a wrapper. That means that any refs
    // transitively reachable through the reference are strong. Now that a wrapper exists, the
    // refs will be traced when the wrapper is traced, so they need to be marked weak.
    GcVisitor visitor(*this);
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

void Wrappable::deleterPass1(const v8::WeakCallbackInfo<Wrappable>& data) {
  // We are required to clear the handle immediately.
  data.GetParameter()->resetWrapperHandle();

  // But we cannot do anything else right now. In particular, deleting the object could lead to
  // other V8 APIs being invoked, which is illegal right now. We must register a second-pass
  // callback to do that.
  data.SetSecondPassCallback(&deleterPass2);
}

void Wrappable::deleterPass2(const v8::WeakCallbackInfo<Wrappable>& data) {
  // Detach the wrapper ref and let it be deleted. This possibly deletes the Wrappable, if it has
  // no jsg::Refs left pointing at it from C++ objects.
  data.GetParameter()->detachWrapperRef();
}

void Wrappable::setWeak() {
  wrapper.SetWeak(this, &deleterPass1, v8::WeakCallbackType::kParameter);
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
  bool becameWeak = false;
  if (visitor.parent.strongRefcount > 0) {
    // This reference should be strong, because the parent has strong refs.
    //
    // TODO(soon): This is not quite right. If the parent has a wrapper object, then we only need
    //   a strong ref to that wrapper object itself. Children can be weak, because they'll be
    //   traced. But it's not just the parent -- if any ancestor has a wrapper, and no intermediate
    //   parents have strong refs, then we should be weak. Ugh. Not going to fix this in this
    //   commit.

    if (!refStrong) {
      // Ref transitions from weak to strong.
      addStrongRef();
      refStrong = true;
    }
  } else {
    if (refStrong) {
      // Ref transitions from strong to weak.
      refStrong = false;
      removeStrongRef();
      becameWeak = true;
    }
  }

  if (wrapper.IsEmpty()) {
    if (lastTraceId != visitor.parent.lastTraceId) {
      // Our wrapper hasn't been allocated yet, i.e. this object has never been directly visible to
      // JavaScript. However, we might transitively hold reference to objects that do have wrappers,
      // so we need to transitively trace to our children.
      lastTraceId = visitor.parent.lastTraceId;
      GcVisitor subVisitor(*this);
      jsgVisitForGc(subVisitor);
    }
  } else {
    // Wrapper is non-empty, so `isolate` can't be null.
    auto& tracer = HeapTracer::getTracer(isolate);

    if (becameWeak || visitor.parent.lastTraceId == tracer.currentTraceId()) {
      // Either:
      // a) This reference newly became a weak reference. However, it is clearly reachable from
      //    another object. Therefore, we must ensure that the TracedReference is initialized so
      //    that V8 knows that this object cannot be collected during scavenging and must instead
      //    wait for tracing. Marking will do this for us.
      // b) The parent has already been traced during this cycle. Probably, this call to visitRef()
      //    is actually a result of the parent being traced. So this is the usual case where we
      //    need to mark.
      tracer.mark(wrapper);
    }
  }
}

void GcVisitor::visit(Data& value) {
  if (!value.handle.IsEmpty()) {
    // Make ref strength match the parent.
    bool becameWeak = false;
    if (parent.strongRefcount > 0) {
      if (value.handle.IsWeak()) {
        value.handle.ClearWeak();
      }
    } else {
      if (!value.handle.IsWeak()) {
        value.handle.SetWeak();
        becameWeak = true;
      }
    }

    // Check if we need to mark.
    // TODO(soon): Why parent.lastTraceId != 0 vs. parent.lastTraceId == tracer.currentTraceId()?
    //   Just because we don't have a `tracer` object yet to check against? Does this actually
    //   make any difference in practice? Leaving it for now because the worst case is we mark
    //   too often, which is better than marking not often enough.
    if (becameWeak || parent.lastTraceId != 0) {
      // If `becameWeak`, then we must have an ancestor that has a wrapper and therefore a non-null
      // isolate. All children would inherit that isolate.
      //
      // If `parent.lastTraceId != 0`, then the parent has been traced directly before so would
      // certainly have an isolate.
      //
      // So either way, `parent.isolate` is non-null.
      HeapTracer::getTracer(parent.isolate).mark(value.handle);
    }
  }
}

}  // namespace workerd::jsg
