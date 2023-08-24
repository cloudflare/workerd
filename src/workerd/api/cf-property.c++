// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cf-property.h"
#include <workerd/io/features.h>

namespace workerd::api {

static constexpr auto kDefaultBotManagementValue = R"DATA({
  "corporateProxy": false,
  "verifiedBot": false,
  "jsDetection": { "passed": false },
  "staticResource": false,
  "detectionIds": {},
  "score": 99
})DATA";

static void handleDefaultBotManagement(jsg::Lock& js, v8::Local<v8::Object> handle) {
  // When the cfBotManagementNoOp compatibility flag is set, we'll check the
  // request cf blob to see if it contains a botManagement field. If it does
  // *not* we will add it using the following default fields.
  // Note that if the botManagement team changes any of the fields they provide,
  // this default value may need to be changed also.
  auto context = js.v8Context();
  if (!js.v8Has(handle, "botManagement"_kj)) {
    auto sym = v8::Private::ForApi(js.v8Isolate,
        jsg::v8StrIntern(js.v8Isolate, "botManagement"_kj));
    // For performance reasons, we only want to construct the default values
    // once per isolate so we cache the constructed value using an internal
    // private field on the global scope. Whenever we need to use it again we
    // pull the exact same value.
    auto defaultBm = jsg::check(context->Global()->GetPrivate(context, sym));
    if (defaultBm->IsUndefined()) {
      auto bm = js.parseJson(kj::StringPtr(kDefaultBotManagementValue));
      KJ_DASSERT(bm.getHandle(js)->IsObject());
      js.recursivelyFreeze(bm);
      defaultBm = bm.getHandle(js);
      jsg::check(context->Global()->SetPrivate(context, sym, defaultBm));
    }
    js.v8Set(handle, "botManagement"_kj, defaultBm);
  }
}

jsg::Optional<v8::Local<v8::Object>> CfProperty::get(jsg::Lock& js) {
  return getRef(js).map([&js](jsg::V8Ref<v8::Object>&& ref) mutable {
    return ref.getHandle(js);
  });
}

jsg::Optional<jsg::V8Ref<v8::Object>> CfProperty::getRef(jsg::Lock& js) {
  KJ_IF_MAYBE(cf, value) {
    KJ_SWITCH_ONEOF(*cf) {
      KJ_CASE_ONEOF(parsed, jsg::V8Ref<v8::Object>) {
        return parsed.addRef(js);
      }
      KJ_CASE_ONEOF(unparsed, kj::String) {
        auto parsed = js.parseJson(unparsed);
        auto handle = parsed.getHandle(js);
        KJ_ASSERT(handle->IsObject());

        auto objectHandle = handle.As<v8::Object>();
        if (!FeatureFlags::get(js).getNoCfBotManagementDefault()) {
          handleDefaultBotManagement(js, objectHandle);
        }

        // For the inbound request, we make the `cf` blob immutable.
        js.recursivelyFreeze(parsed);

        // replace unparsed string with a parsed v8 object
        auto parsedObject = parsed.cast<v8::Object>(js);
        this->value = parsedObject.addRef(js);
        return kj::mv(parsedObject);
      }
    }
  }

  return nullptr;
}


kj::Maybe<kj::String> CfProperty::serialize(jsg::Lock& js) {
  KJ_IF_MAYBE(cf, value) {
    KJ_SWITCH_ONEOF(*cf) {
      KJ_CASE_ONEOF(parsed, jsg::V8Ref<v8::Object>) {
        return js.serializeJson(parsed);
      }
      KJ_CASE_ONEOF(unparsed, kj::String) {
        if (!FeatureFlags::get(js).getNoCfBotManagementDefault()) {
          // we mess up with the value on this code path,
          // need to parse it, fix it and serialize back
          return js.serializeJson(KJ_ASSERT_NONNULL(getRef(js)));
        }

        return kj::str(unparsed);
      }
    }
  }

  return nullptr;
}

CfProperty CfProperty::deepClone(jsg::Lock& js) {
  KJ_IF_MAYBE(cf, value) {
    KJ_SWITCH_ONEOF(*cf) {
      KJ_CASE_ONEOF(parsed, jsg::V8Ref<v8::Object>) {
        auto ref = parsed.deepClone(js);
        return CfProperty(kj::mv(ref));
      }
      KJ_CASE_ONEOF(unparsed, kj::String) {
        return CfProperty(unparsed.asPtr());
      }
    }
  }

  return nullptr;
}

void CfProperty::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_MAYBE(cf, value) {
    KJ_SWITCH_ONEOF(*cf) {
      KJ_CASE_ONEOF(parsed, jsg::V8Ref<v8::Object>) {
        visitor.visit(parsed);
      }
      KJ_CASE_ONEOF_DEFAULT {}
    }
  }
}

} // namespace workerd::api
