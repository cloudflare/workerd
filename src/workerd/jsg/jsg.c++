// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "setup.h"
#include "workerd/jsg/util.h"
#include <workerd/util/thread-scopes.h>

namespace workerd::jsg {

kj::String stringifyHandle(v8::Local<v8::Value> value) {
  // This is the only place in the entire codebase where we use v8::Isolate::GetCurrent(). It's
  // hard to avoid since we want `kj::str(handle)` to work, which doesn't give us a chance to
  // pass in a `Lock` or whatever.
  // TODO(cleanup): Perhaps we should require you to call `js.toString(handle)`?
  auto& js = jsg::Lock::from(v8::Isolate::GetCurrent());
  return js.withinHandleScope([&] {
    v8::Local<v8::String> str = workerd::jsg::check(value->ToDetailString(js.v8Context()));
    v8::String::Utf8Value utf8(js.v8Isolate, str);
    if (*utf8 == nullptr) {
      return kj::str("(couldn't stringify)");
    } else {
      return kj::str(*utf8);
    }
  });
}

JsExceptionThrown::JsExceptionThrown() {
  tracePtr = kj::getStackTrace(trace, 0);
}

const char* JsExceptionThrown::what() const noexcept {
  whatBuffer = kj::str(
      "Uncaught JsExceptionThrown\nstack: ",
      kj::stringifyStackTraceAddresses(tracePtr));
  return whatBuffer.cStr();
}

void Data::destroy() {
  assertInvariant();
  if (isolate != nullptr) {
    if (v8::Locker::IsLocked(isolate)) {
      handle.Reset();

      // If we have a TracedReference, Reset() it too, to let V8 know that this value is no longer
      // used. Note that merely destroying the TracedReference does nothing -- only explicitly
      // calling Reset() has an effect.
      //
      // In particular, this permits `Data` values to be collected by minor (non-tracing) GC, as
      // long as there are no cycles.
      //
      // HOWEVER, this is not safe if the TracedReference is being destroyed as a result of a
      // major (traced) GC. In that case, the TracedReference itself may point to a reference slot
      // that was already collected, and trying to reset it would be UB.
      //
      // In all other cases, resetting the handle is safe:
      // - During minor GC, TracedReferences aren't collected by the GC itself, so must still be
      //   valid.
      // - If the `Data` is being destroyed _not_ as part of GC, e.g. it's being destroyed because
      //   the data structure holding it is being modified in a way that drops the reference, then
      //   that implies that the reference is still reachable, so must still be valid.
      KJ_IF_SOME(t, tracedHandle) {
        if (!HeapTracer::isInCppgcDestructor()) {
          t.Reset();
        }
      }
    } else {
      // This thread doesn't have the isolate locked right now. To minimize lock contention, we'll
      // defer these handles' destruction to the next time the isolate is locked.
      //
      // Note that only the v8::Global part of `handle` needs to be destroyed under isolate lock.
      // The `tracedRef` part has a trivial destructor so can be destroyed on any thread.
      auto& jsgIsolate = *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
      jsgIsolate.deferDestruction(v8::Global<v8::Data>(kj::mv(handle)));
    }
    isolate = nullptr;
  }
}

void Data::moveFromTraced(Data& other, v8::TracedReference<v8::Data>& otherTracedRef) noexcept {
  // Implement move constructor when the source of the move has previously been visited for
  // garbage collection.
  //
  // This method is `noexcept` because if an exception is thrown below we're probably going to
  // segfault later.

  // We must hold a lock to move from a GC-reachable reference. (But we don't generally need a lock
  // for moving from non-GC-reachable refs.)
  KJ_ASSERT(v8::Locker::IsLocked(isolate));

  // Verify the handle was not garbage-collected by trying to read it. The intention is for this
  // to crash if the handle was GC'd before being moved away.
  {
    auto& js = jsg::Lock::from(isolate);
    js.withinHandleScope([&] {
      auto local = handle.Get(js.v8Isolate);
      if (local->IsValue()) {
        local.As<v8::Value>()->IsArrayBufferView();
      }
    });
  }

  // `other` is a traced `Data`, but once moved, we don't assume the new location is traced.
  // So, we need to make the handle strong.
  handle.ClearWeak();

  // Presumably, `other` is about to be destroyed. The destructor of `TracedReference`, though,
  // does nothing, because it doesn't know if the reference is even still valid, since it
  // could be called during GC sweep time. But here, we know that `other` is definitely still
  // valid, because we wouldn't be moving from an unreachable object. So we should Reset() the
  // `TracedReference` so that V8 knows it's gone, which might make minor GCs more effective.
  otherTracedRef.Reset();

  other.tracedHandle = kj::none;
}

Lock::Lock(v8::Isolate* v8Isolate)
    : v8Isolate(v8Isolate), locker(v8Isolate),
      previousData(v8Isolate->GetData(2)),
      warningsLogged(IsolateBase::from(v8Isolate).areWarningsLogged()) {
  if (previousData != nullptr) {
    // Hmm, there's already a current lock. It must be a recursive lock (i.e. a second lock taken
    // on the same isolate in the same thread), otherwise `locker`'s constructor would have blocked
    // waiting for the other thread to release the lock. We don't want to support this, but
    // historically we have.
#ifdef KJ_DEBUG
    // In debug mode, abort immediately. This makes it a little easier to debug than if we threw
    // an exception.
    ([]() noexcept { KJ_FAIL_REQUIRE("attempt to take recursive isolate lock"); })();
#else
    // In release mode, log the error.
    // TODO(soon): This shouldn't happen but we know it does in at least one case. Once things
    // are cleaned up and we know this no longer happens in production, change this to throw.
    // Then we can stop storing `previousData`.
    KJ_LOG(ERROR, "took recursive isolate lock", kj::getStackTrace());
#endif
  }
  v8Isolate->SetData(2, this);
}
Lock::~Lock() noexcept(false) {
  v8Isolate->SetData(2, previousData);
}

Value Lock::parseJson(kj::ArrayPtr<const char> data) {
  return withinHandleScope([&] {
    return v8Ref(jsg::check(v8::JSON::Parse(v8Context(), v8Str(v8Isolate, data))));
  });
}

Value Lock::parseJson(v8::Local<v8::String> text) {
  return withinHandleScope([&] {
    return v8Ref(jsg::check(v8::JSON::Parse(v8Context(), text)));
  });
}

kj::String Lock::serializeJson(v8::Local<v8::Value> value) {
  return withinHandleScope([&] {
    return toString(jsg::check(v8::JSON::Stringify(v8Context(), value)));
  });
}

void Lock::recursivelyFreeze(Value& value) {
  jsg::recursivelyFreeze(v8Context(), value.getHandle(*this));
}

v8::Local<v8::String> Lock::wrapString(kj::StringPtr text) {
  return v8Str(v8Isolate, text);
}

bool Lock::toBool(v8::Local<v8::Value> value) {
  return value->BooleanValue(v8Isolate);
}

v8::Local<v8::Value> Lock::v8Error(kj::StringPtr message) {
  return v8::Exception::Error(v8Str(v8Isolate, message));
}

v8::Local<v8::Value> Lock::v8TypeError(kj::StringPtr message) {
  return v8::Exception::TypeError(v8Str(v8Isolate, message));
}

void Lock::logWarning(kj::StringPtr message) {
  IsolateBase::from(v8Isolate).logWarning(*this, message);
}

void Lock::setAllowEval(bool allow) {
  IsolateBase::from(v8Isolate).setAllowEval({}, allow);
}

void Lock::setCaptureThrowsAsRejections(bool capture) {
  IsolateBase::from(v8Isolate).setCaptureThrowsAsRejections({}, capture);
}

void Lock::setCommonJsExportDefault(bool exportDefault) {
  IsolateBase::from(v8Isolate).setCommonJsExportDefault({}, exportDefault);
}

void Lock::setLoggerCallback(kj::Function<Logger>&& logger) {
  IsolateBase::from(v8Isolate).setLoggerCallback({}, kj::mv(logger));
}

void Lock::requestGcForTesting() const {
  if (!isPredictableModeForTest()) {
    KJ_LOG(ERROR, "Test GC used while not in a test");
    return;
  }
  v8Isolate->RequestGarbageCollectionForTesting(
    v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
}

void Lock::v8Set(v8::Local<v8::Object> obj,
                 kj::StringPtr name,
                 v8::Local<v8::Value> value) {
  KJ_ASSERT(check(obj->Set(v8Context(), v8StrIntern(v8Isolate, name), value)));
}

void Lock::v8Set(v8::Local<v8::Object> obj, kj::StringPtr name, Value& value) {
  v8Set(obj, name, value.getHandle(*this));
}

void Lock::v8Set(v8::Local<v8::Object> obj, V8Ref<v8::String>& name, Value& value) {
  KJ_ASSERT(check(obj->Set(v8Context(), name.getHandle(*this), value.getHandle(*this))));
}

v8::Local<v8::Value> Lock::v8Get(v8::Local<v8::Object> obj, kj::StringPtr name) {
  return check(obj->Get(v8Context(), v8StrIntern(v8Isolate, name)));
}

v8::Local<v8::Value> Lock::v8Get(v8::Local<v8::Array> obj, uint idx) {
  return check(obj->Get(v8Context(), idx));
}

bool Lock::v8Has(v8::Local<v8::Object> obj, kj::StringPtr name) {
  return check(obj->Has(v8Context(), v8StrIntern(v8Isolate, name)));
}

bool Lock::v8HasOwn(v8::Local<v8::Object> obj, kj::StringPtr name) {
  return check(obj->HasOwnProperty(v8Context(), v8StrIntern(v8Isolate, name)));
}

kj::StringPtr Lock::getUuid() const {
  return IsolateBase::from(v8Isolate).getUuid();
}

void Lock::runMicrotasks() {
  v8Isolate->PerformMicrotaskCheckpoint();
}

void Lock::terminateExecution() {
  v8Isolate->TerminateExecution();
}

Name Lock::newSymbol(kj::StringPtr symbol) {
  return Name(*this, v8::Symbol::New(v8Isolate, v8StrIntern(v8Isolate, symbol)));
}

Name Lock::newSharedSymbol(kj::StringPtr symbol) {
  return Name(*this, v8::Symbol::For(v8Isolate, v8StrIntern(v8Isolate, symbol)));
}

Name Lock::newApiSymbol(kj::StringPtr symbol) {
  return Name(*this, v8::Symbol::ForApi(v8Isolate, v8StrIntern(v8Isolate, symbol)));
}

Name::Name(kj::String string)
    : hash(kj::hashCode(string)),
      inner(kj::mv(string)) {}

Name::Name(kj::StringPtr string)
    : hash(kj::hashCode(string)),
      inner(kj::str(string)) {}

Name::Name(Lock& js, v8::Local<v8::Symbol> symbol)
    : hash(symbol->GetIdentityHash()),
      inner(js.v8Ref(symbol)) {}

kj::OneOf<kj::StringPtr, v8::Local<v8::Symbol>> Name::getUnwrapped(v8::Isolate* isolate) {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(str, kj::String) {
      return str.asPtr();
    }
    KJ_CASE_ONEOF(symbol, V8Ref<v8::Symbol>) {
      return symbol.getHandle(isolate);
    }
  }
  KJ_UNREACHABLE;
}

void Name::visitForGc(GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(string, kj::String) {}
    KJ_CASE_ONEOF(symbol, V8Ref<v8::Symbol>) {
      visitor.visit(symbol);
    }
  }
}

Name Name::clone(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(str, kj::String) {
      return Name(kj::str(str));
    }
    KJ_CASE_ONEOF(symbol, V8Ref<v8::Symbol>) {
      return Name(js, symbol.getHandle(js));
    }
  }
  KJ_UNREACHABLE;
}

kj::String Name::toString(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(str, kj::String) {
      return kj::str(str);
    }
    KJ_CASE_ONEOF(sym, V8Ref<v8::Symbol>) {
      return kj::str("Symbol(", sym.getHandle(js)->Description(js.v8Isolate), ")");
    }
  }
  KJ_UNREACHABLE;
}

bool isInGcDestructor() {
  return HeapTracer::isInCppgcDestructor();
}

}  // namespace workerd::jsg
