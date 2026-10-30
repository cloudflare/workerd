// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-event-target.h>
#include <workerd/api/js-events-bridge.h>
#include <workerd/util/autogate.h>

namespace workerd::api {

JsEvent::JsEvent(jsg::Lock&, jsg::JsRef<jsg::JsObject> obj): impl(kj::mv(obj)) {}

jsg::JsValue JsEvent::getHandle(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(event, jsg::Ref<Event>) {
      auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<Event>>());
      return jsg::JsValue(handler.wrap(js, event.addRef()));
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      return obj.getHandle(js);
    }
  }
  KJ_UNREACHABLE;
}

JsEventTarget JsEventTarget::from(jsg::Lock& js, EventTarget& target) {
  auto ref = jsg::_jsgThis(&target);
  if (util::Autogate::isEnabled(util::AutogateKey::TYPESCRIPT_EVENTTARGET)) {
    // A target without a wrapper has never been seen by JavaScript, so it has no TypeScript
    // state, nor any listeners.
    KJ_IF_SOME(handle, ref.tryGetHandle(js)) {
      KJ_IF_SOME(isEventTarget, events::tryGetCppExport(js, "isEventTarget"_kj)) {
        auto obj = jsg::JsObject(handle);
        if (isEventTarget.call(js, js.undefined(), obj).isTruthy(js)) {
          return JsEventTarget(js, obj.addRef(js));
        }
      }
    }
  }
  return JsEventTarget(Impl(kj::mv(ref)));
}

JsEventTarget::JsEventTarget(jsg::Lock&, jsg::JsRef<jsg::JsObject> obj): impl(kj::mv(obj)) {}

JsEvent JsEventTarget::createEvent(jsg::Lock& js, kj::StringPtr type) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(_, jsg::Ref<EventTarget>) {
      return js.alloc<Event>(type, Event::Init{}, Trusted::YES);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      auto event = events::dispatchCall(js, "createTrustedEvent"_kj, js.str(type));
      return JsEvent(js, KJ_ASSERT_NONNULL(event.tryCast<jsg::JsObject>()).addRef(js));
    }
  }
  KJ_UNREACHABLE;
}

JsEventTarget::DispatchResult JsEventTarget::dispatch(
    jsg::Lock& js, JsEvent event, DispatchExceptionPolicy policy) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(target, jsg::Ref<EventTarget>) {
      KJ_SWITCH_ONEOF(event.impl) {
        KJ_CASE_ONEOF(e, jsg::Ref<Event>) {
          return target->dispatchEventImpl(js, kj::mv(e), policy);
        }
        KJ_CASE_ONEOF(_, jsg::JsRef<jsg::JsObject>) {
          KJ_FAIL_REQUIRE("a TypeScript event cannot be dispatched at a C++ event target");
        }
      }
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      auto outcomeValue = events::dispatchCall(js, "dispatchEvent"_kj, obj.getHandle(js),
          event.getHandle(js), js.boolean(policy == DispatchExceptionPolicy::PROPAGATE));
      auto outcome = KJ_ASSERT_NONNULL(outcomeValue.tryCast<jsg::JsObject>());
      DispatchResult result{.result = outcome.get(js, "notCanceled"_kj).isTruthy(js)};
      if (outcome.get(js, "threw"_kj).isTruthy(js)) {
        result.firstException = jsg::JsRef(js, outcome.get(js, "exception"_kj));
      }
      return result;
    }
  }
  KJ_UNREACHABLE;
}

size_t JsEventTarget::getListenerCount(jsg::Lock& js, kj::StringPtr type) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(target, jsg::Ref<EventTarget>) {
      return target->getHandlerCount(type);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      auto count = events::dispatchCall(js, "getListenerCount"_kj, obj.getHandle(js), js.str(type));
      return KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(count.tryCast<jsg::JsNumber>()).value(js));
    }
  }
  KJ_UNREACHABLE;
}

kj::Array<kj::String> JsEventTarget::getListenerTypes(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(target, jsg::Ref<EventTarget>) {
      return KJ_MAP(name, target->getHandlerNames()) { return kj::str(name); };
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      auto types =
          KJ_ASSERT_NONNULL(events::dispatchCall(js, "getListenerTypes"_kj, obj.getHandle(js))
                                .tryCast<jsg::JsArray>());
      auto result = kj::heapArrayBuilder<kj::String>(types.size());
      for (uint32_t i = 0; i < types.size(); i++) {
        result.add(types.get(js, i).toString(js));
      }
      return result.finish();
    }
  }
  KJ_UNREACHABLE;
}

void JsEventTarget::removeAllListeners(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(impl) {
    KJ_CASE_ONEOF(target, jsg::Ref<EventTarget>) {
      target->removeAllHandlers();
      return;
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      events::dispatchCall(js, "removeAllListeners"_kj, obj.getHandle(js));
      return;
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api
