// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/http.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

class ExtendedFetcherModule final: public jsg::Object {
 public:
  ExtendedFetcherModule() = default;
  ExtendedFetcherModule(jsg::Lock&, const jsg::Url&) {}

  JSG_RESOURCE_TYPE(ExtendedFetcherModule) {
    JSG_NESTED_TYPE(ExtendedFetcher);
  }
};

template <class Registry>
void registerExtendedFetcherModule(Registry& registry) {
  registry.template addBuiltinModule<ExtendedFetcherModule>(
      "cloudflare-internal:extended-fetcher", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalExtendedFetcherModuleBundle() {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  builder.addObject<ExtendedFetcherModule, TypeWrapper>("cloudflare-internal:extended-fetcher"_url);
  return builder.finish();
}

#define EW_EXTENDED_FETCHER_ISOLATE_TYPES api::ExtendedFetcherModule

}  // namespace workerd::api
