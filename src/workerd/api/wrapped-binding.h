// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/http.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

constexpr kj::StringPtr WRAPPED_BINDING_INNER_NAME = "fetcher"_kj;

// Native base for an application-level binding implemented by an internal JavaScript module around
// one service stub. Wrapper classes inherit this type, so their instances carry JSG's internal
// fields and use the normal JSG resource serialization path.
class WrappedBinding final: public jsg::Object {
 public:
  explicit WrappedBinding(jsg::LenientOptional<jsg::Ref<Fetcher>> maybeFetcher) {
    KJ_IF_SOME(fetcher, maybeFetcher) {
      this->fetcher = kj::mv(fetcher);
    }
  }

  static jsg::Ref<WrappedBinding> constructor(
      jsg::Lock& js, jsg::LenientOptional<jsg::Ref<Fetcher>> fetcher) {
    return js.alloc<WrappedBinding>(kj::mv(fetcher));
  }

  JSG_RESOURCE_TYPE(WrappedBinding) {}

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<WrappedBinding> deserialize(jsg::Lock& js,
      rpc::SerializationTag tag,
      jsg::Deserializer& deserializer,
      const jsg::TypeHandler<jsg::Ref<Fetcher>>& innerHandler,
      const jsg::TypeHandler<jsg::Ref<WrappedBinding>>& selfHandler);

  JSG_SERIALIZABLE(rpc::SerializationTag::WRAPPED_BINDING);

  void setWrapperModule(kj::String moduleName) {
    KJ_REQUIRE(fetcher != kj::none, "serializable wrapped binding has no fetcher");
    KJ_REQUIRE(wrapperModule == kj::none, "wrapped binding module was already set");
    wrapperModule = kj::mv(moduleName);
  }

 private:
  kj::Maybe<jsg::Ref<Fetcher>> fetcher;
  kj::Maybe<kj::String> wrapperModule;

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_SOME(f, fetcher) {
      visitor.visit(f);
    }
  }
};

class WrappedBindingModule final: public jsg::Object {
 public:
  WrappedBindingModule() = default;
  WrappedBindingModule(jsg::Lock&, const jsg::Url&) {}

  JSG_RESOURCE_TYPE(WrappedBindingModule) {
    JSG_NESTED_TYPE(WrappedBinding);
  }
};

template <class Registry>
void registerWrappedBindingModule(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<WrappedBindingModule>(
      "cloudflare-internal:wrapped-binding", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalWrappedBindingModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:wrapped-binding"_url;
  builder.addObject<WrappedBindingModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}

#define EW_WRAPPED_BINDING_ISOLATE_TYPES api::WrappedBinding, api::WrappedBindingModule

}  // namespace workerd::api
