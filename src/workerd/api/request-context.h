#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>

namespace workerd::api {

class RequestContextModule: public jsg::Object {
public:
  kj::Maybe<jsg::Value> getRequestId(jsg::Lock& js);

  JSG_RESOURCE_TYPE(RequestContextModule) {
    JSG_METHOD(getRequestId);
  }
};

template <typename TypeWrapper>
void registerRequestContextModule(
    workerd::jsg::ModuleRegistryImpl<TypeWrapper>& registry, auto featureFlags) {
  registry.template addBuiltinModule<RequestContextModule>("cloudflare-internal:request-context",
    workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

#define EW_REQUEST_CONTEXT_ISOLATE_TYPES     \
  api::RequestContextModule

}  // namespace workerd::api
