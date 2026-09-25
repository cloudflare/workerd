// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Exclude unrelated Fetcher methods so this test only needs the resource types under test.
#define WORKERD_API_EXTENDED_FETCHER_TEST 1

#include "extended-fetcher.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/modules-new.h>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct ExtendedFetcherTestContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(ExtendedFetcherTestContext) {}
};

JSG_DECLARE_ISOLATE_TYPE(ExtendedFetcherTestIsolate,
    ExtendedFetcherTestContext,
    Fetcher,
    ExtendedFetcherInitializer,
    ExtendedFetcher,
    EW_EXTENDED_FETCHER_ISOLATE_TYPES);

KJ_TEST("ExtendedFetcher can be subclassed by a JavaScript binding") {
  jsg::test::Evaluator<ExtendedFetcherTestContext, ExtendedFetcherTestIsolate,
      CompatibilityFlags::Reader>
      evaluator(v8System);

  evaluator.run([](ExtendedFetcherTestIsolate::Lock& lock) {
    auto& js = lock;
    jsg::IsolateBase::from(js.v8Isolate).setUsingNewModuleRegistry();
    jsg::CompilationObserver observer;

    jsg::modules::ModuleBundle::BuiltinBuilder builtinBuilder;
    builtinBuilder.addEsm("test:extended-fetcher-binding"_url,
        R"js(
          import module from 'cloudflare-internal:extended-fetcher';

          export default function createBinding(initializer) {
            class TestBinding extends module.ExtendedFetcher {}

            const binding = new TestBinding(initializer);
            let reuseError;
            try {
              new TestBinding(initializer);
            } catch (error) {
              reuseError = `${error.name}: ${error.message}`;
            }

            return [
              binding instanceof TestBinding,
              binding instanceof module.ExtendedFetcher,
              binding.port,
              reuseError,
            ].join('|');
          }
        )js"_kjc);

    auto registry =
        jsg::modules::ModuleRegistry::Builder("file:///"_url)
            .add(getInternalExtendedFetcherModuleBundle<ExtendedFetcherTestIsolate_TypeWrapper>())
            .add(builtinBuilder.finish())
            .finish();
    auto attached = registry->attachToIsolate(js, observer);

    JSG_TRY(js) {
      auto factory = KJ_ASSERT_NONNULL(
          jsg::modules::ModuleRegistry::resolve(js, "test:extended-fetcher-binding", "default"_kjc,
              jsg::modules::ResolveContext::Type::BUILTIN)
              .tryCast<jsg::JsFunction>());
      auto initializer = js.alloc<ExtendedFetcherInitializer>(ExtendedFetcherInitializer::State{
        .channel = 123,
        .port = 5432,
        .isHyperdrive = IsHyperdrive::YES,
      });
      auto result = factory.call(
          js, js.undefined(), jsg::JsValue(lock.wrap(js.v8Context(), kj::mv(initializer))));

      KJ_ASSERT(kj::str(result) ==
          "true|true|5432|TypeError: ExtendedFetcher initializer has already been consumed.");
    }
    JSG_CATCH(exception) {
      js.throwException(kj::mv(exception));
    }
  });
}

}  // namespace
}  // namespace workerd::api
