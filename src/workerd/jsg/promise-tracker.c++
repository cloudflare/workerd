#include "promise-tracker.h"
#include "setup.h"
#include <kj/vector.h>
#include <v8.h>

namespace workerd::jsg {

UnsettledPromiseTracker::UnsettledPromiseTracker(v8::Isolate* isolate) : isolate(isolate) {
  isolate->SetPromiseHook([](v8::PromiseHookType type,
                             v8::Local<v8::Promise> promise,
                             v8::Local<v8::Value> parent) {
    auto& js = Lock::from(promise->GetIsolate());
    auto& isolate = IsolateBase::from(promise->GetIsolate());
    UnsettledPromiseTracker& self = KJ_ASSERT_NONNULL(isolate.getUnsettledPromiseTracker());
    switch (type) {
      case v8::PromiseHookType::kInit:
        {
          auto obj = js.obj();
          obj.set(js, "name"_kj, js.str(kj::str("Promise ", promise->GetIdentityHash())));

          auto message = ([&] {
            if (parent->IsPromise()) {
              return kj::str("follows ", parent.As<v8::Promise>()->GetIdentityHash());
            } else {
              return kj::str("created");
            }
          })();

          obj.set(js, "message"_kj, js.str(message));
          v8::Exception::CaptureStackTrace(js.v8Context(), obj);
          auto stack = obj.get(js, "stack"_kj);
          self.promises_.upsert(promise->GetIdentityHash(), stack.toString(js));
        }
        break;
      case v8::PromiseHookType::kResolve: {
          self.promises_.erase(promise->GetIdentityHash());
        }
        break;
      case v8::PromiseHookType::kBefore: {
        // Nothing to do in this case
        break;
      }
      case v8::PromiseHookType::kAfter: {
        // Nothing to do in this case
        break;
      }
    }
  });
}

UnsettledPromiseTracker::~UnsettledPromiseTracker() noexcept(false) {
  isolate->SetPromiseHook(nullptr);
}

kj::String UnsettledPromiseTracker::report() {
  kj::Vector<kj::String> lines;
  for (auto& [id, stack] : promises_) {
    lines.add(kj::str("Unresolved ", stack));
  }
  return kj::str(kj::delimited(lines.releaseAsArray(), "\n"_kj));
}

}  // namespace workerd::jsg
