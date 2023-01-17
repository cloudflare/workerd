// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "setup.h"
#include <execinfo.h>
#include <workerd/util/thread-scopes.h>

namespace workerd::jsg {

kj::String stringifyHandle(v8::Local<v8::Value> value) {
  // This is the only place in the entire codebase where we use v8::Isolate::GetCurrent(). It's
  // hard to avoid since we want `kj::str(handle)` to work, which doesn't give us a chance to
  // pass in a `Lock` or whatever.
  // TODO(cleanup): Perhaps we should require you to call `js.toString(handle)`?
  auto isolate = v8::Isolate::GetCurrent();
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> str =
      workerd::jsg::check(value->ToDetailString(isolate->GetCurrentContext()));
  v8::String::Utf8Value utf8(isolate, str);
  if (*utf8 == nullptr) {
    return kj::str("(couldn't stringify)");
  } else {
    return kj::str(*utf8);
  }
}

JsExceptionThrown::JsExceptionThrown() {
  traceSize = backtrace(trace, kj::size(trace));
  for (auto& addr: kj::arrayPtr(trace, traceSize)) {
    // Report call address, not return address.
    addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) - 1);
  }
}

const char* JsExceptionThrown::what() const noexcept {
  whatBuffer = kj::str(
      "Uncaught JsExceptionThrown\nstack: ",
      kj::stringifyStackTraceAddresses(kj::arrayPtr(trace, traceSize)));
  return whatBuffer.cStr();
}

void Data::destroy() {
  assertInvariant();
  if (isolate != nullptr) {
    if (v8::Locker::IsLocked(isolate)) {
      handle.Reset();

      // If we have a TracedReference, Reset() it too, to let V8 know that this value is no longer
      // used. Note that merely detroying the TracedReference does nothing -- only explicitly
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
      KJ_IF_MAYBE(t, tracedHandle) {
        if (!HeapTracer::isInCppgcDestructor()) {
          t->Reset();
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

#ifdef KJ_DEBUG
void Data::assertInvariantImpl() {
    // Assert that only empty values are associated with null isolates.
  KJ_DASSERT(isolate != nullptr || handle.IsEmpty());
}
#endif

Lock::Lock(v8::Isolate* v8Isolate)
    : v8Isolate(v8Isolate), locker(v8Isolate), scope(v8Isolate),
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

Value Lock::parseJson(kj::StringPtr text) {
  v8::HandleScope scope(v8Isolate);
  return jsg::Value(v8Isolate,
      jsg::check(v8::JSON::Parse(v8Isolate->GetCurrentContext(), v8Str(v8Isolate, text))));
}

kj::String Lock::serializeJson(v8::Local<v8::Value> value) {
  v8::HandleScope scope(v8Isolate);
  return toString(jsg::check(
      v8::JSON::Stringify(v8Isolate->GetCurrentContext(), value)));
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

kj::StringPtr Lock::getUuid() const {
  return IsolateBase::from(v8Isolate).getUuid();
}

v8::Local<v8::Private> Lock::getPrivateSymbolFor(Lock::PrivateSymbols symbol) {
  return IsolateBase::from(v8Isolate).getPrivateSymbolFor(symbol);
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
    : inner(kj::mv(string)),
      hash(kj::hashCode(string)) {}

Name::Name(kj::StringPtr string)
    : inner(kj::str(string)),
      hash(kj::hashCode(string)) {}

Name::Name(Lock& js, v8::Local<v8::Symbol> symbol)
    : inner(js.v8Ref(symbol)),
      hash(symbol->GetIdentityHash()) {}

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

}  // namespace workerd::jsg
