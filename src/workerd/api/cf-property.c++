// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cf-property.h"

#include <workerd/io/features.h>

namespace workerd::api {

static constexpr kj::StringPtr kDefaultBotManagementValue = R"DATA({
  "corporateProxy": false,
  "verifiedBot": false,
  "jsDetection": { "passed": false },
  "staticResource": false,
  "detectionIds": {},
  "score": 99
})DATA"_kjc;

// When the cfBotManagementNoOp compatibility flag is set, we'll check the
// request cf blob to see if it contains a botManagement field. If it does
// *not* we will add it using the following default fields.
// Note that if the botManagement team changes any of the fields they provide,
// this default value may need to be changed also.
static void handleDefaultBotManagement(jsg::Lock& js, jsg::JsObject handle) {
  auto name = "botManagement"_kjc;
  if (!handle.has(js, name)) {
    // For performance reasons, we only want to construct the default values
    // once per isolate so we cache the constructed value using an internal
    // private field on the global scope. Whenever we need to use it again we
    // pull the exact same value.
    auto bm = js.global().getPrivate(js, name);
    if (bm.isUndefined()) {
      bm = jsg::JsValue::fromJson(js, kDefaultBotManagementValue);
      KJ_DASSERT(bm.isObject());
      js.global().setPrivate(js, name, bm);
    }
    handle.set(js, name, bm);
  }
}

CfProperty::CfProperty(kj::Maybe<kj::StringPtr> unparsed) {
  KJ_IF_SOME(str, unparsed) {
    value = kj::str(str);
  }
}

CfProperty::CfProperty(jsg::Lock& js, const jsg::JsObject& object)
    : CfProperty(kj::Maybe(jsg::JsRef(js, object))) {}

CfProperty::CfProperty(kj::Maybe<jsg::JsRef<jsg::JsObject>>&& parsed) {
  KJ_IF_SOME(v, parsed) {
    value = kj::mv(v);
  }
}

jsg::Optional<jsg::JsObject> CfProperty::get(jsg::Lock& js) {
  return getRef(js).map(
      [&js](jsg::JsRef<jsg::JsObject>&& ref) mutable { return ref.getHandle(js); });
}

jsg::Optional<jsg::JsRef<jsg::JsObject>> CfProperty::getRef(jsg::Lock& js) {
  KJ_IF_SOME(cf, value) {
    KJ_SWITCH_ONEOF(cf) {
      KJ_CASE_ONEOF(parsed, jsg::JsRef<jsg::JsObject>) {
        return parsed.addRef(js);
      }
      KJ_CASE_ONEOF(unparsed, kj::String) {
        auto parsed = jsg::JsValue::fromJson(js, unparsed);
        auto object = KJ_ASSERT_NONNULL(parsed.tryCast<jsg::JsObject>());

        if (!FeatureFlags::get(js).getNoCfBotManagementDefault()) {
          handleDefaultBotManagement(js, object);
        }

        object.recursivelyFreeze(js);

        // replace unparsed string with a parsed v8 object
        this->value = object.addRef(js);
        return jsg::JsRef(js, object);
      }
    }
  }

  return kj::none;
}

kj::Maybe<kj::String> CfProperty::serialize(jsg::Lock& js) {
  KJ_IF_SOME(cf, value) {
    KJ_SWITCH_ONEOF(cf) {
      KJ_CASE_ONEOF(parsed, jsg::JsRef<jsg::JsObject>) {
        return jsg::JsValue(parsed.getHandle(js)).toJson(js);
      }
      KJ_CASE_ONEOF(unparsed, kj::String) {
        if (!FeatureFlags::get(js).getNoCfBotManagementDefault()) {
          // we mess up with the value on this code path,
          // need to parse it, fix it and serialize back
          jsg::JsValue handle = KJ_ASSERT_NONNULL(getRef(js)).getHandle(js);
          return handle.toJson(js);
        }

        return kj::str(unparsed);
      }
    }
  }

  return kj::none;
}

CfProperty CfProperty::deepClone(jsg::Lock& js) {
  // By default, when CfProperty is lazily parsed, the resulting JS object
  // will be recursively frozen, preventing edits. However, when the CfProperty
  // is cloned and the clone is lazily parsed, the resulting JS object must not
  // be frozen! So, to ensure that, we'll force the parse to occur here if it
  // hasn't been parsed already, this will ensure that the clone receives the
  // parsed object via json cloning below rather than the raw string.
  // TODO(cleanup): With a bit of refactoring we can preserve the lazy parsing
  // optimization through the clone. But for now, let's just do the easy thing.
  getRef(js);
  KJ_IF_SOME(cf, value) {
    KJ_SWITCH_ONEOF(cf) {
      KJ_CASE_ONEOF(parsed, jsg::JsRef<jsg::JsObject>) {
        return CfProperty(jsg::JsRef(js, parsed.getHandle(js).jsonClone(js)));
      }
      KJ_CASE_ONEOF(unparsed, kj::String) {
        KJ_FAIL_REQUIRE("The cf property should have been lazily parsed!");
      }
    }
  }

  return nullptr;
}

void CfProperty::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(cf, value) {
    KJ_SWITCH_ONEOF(cf) {
      KJ_CASE_ONEOF(parsed, jsg::JsRef<jsg::JsObject>) {
        visitor.visit(parsed);
      }
      KJ_CASE_ONEOF_DEFAULT {}
    }
  }
}

}  // namespace workerd::api
