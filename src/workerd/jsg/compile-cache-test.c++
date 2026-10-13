// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "modules.h"
#include "setup.h"
#include "type-wrapper.h"

#include <cacheTest/compile-cache-fixture.capnp.h>

#include <kj/test.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct CacheContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(CacheContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CacheIsolate, CacheContext);

KJ_TEST("embedded ESM caches accept ASCII, Latin-1 and UTF-16 source strings") {
  CacheIsolate isolate(v8System, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](CacheIsolate::Lock& js) {
    JSG_WITHIN_CONTEXT_SCOPE(js, js.newContext<CacheContext>().getHandle(js), [&](jsg::Lock& js) {
      Bundle::Reader bundle = CACHE_TEST_BUNDLE;
      size_t oneByteCount = 0;
      size_t utf16Count = 0;
      KJ_ASSERT(bundle.getModules().size() == 3);
      for (auto module: bundle.getModules()) {
        js.withinHandleScope([&] {
          // wd_js_bundle generates the caches in module mode. Runtime ESM loading decodes UTF-8.
          auto content = js.str(module.getSrc().asChars());
          if (content.isOneByte(js)) {
            ++oneByteCount;
          } else {
            ++utf16Count;
          }
          auto cache = module.getCompileCache();
          KJ_ASSERT(cache.size() > 0, module.getName());
          v8::ScriptOrigin origin(
              js.str(module.getName()), 0, 0, false, -1, {}, false, false, true);
          v8::ScriptCompiler::Source source(content, origin,
              new v8::ScriptCompiler::CachedData(
                  cache.begin(), cache.size(), v8::ScriptCompiler::CachedData::BufferNotOwned));
          auto compiled = v8::ScriptCompiler::CompileModule(
              js.v8Isolate, &source, v8::ScriptCompiler::kConsumeCodeCache);
          KJ_EXPECT(!compiled.IsEmpty(), module.getName());
          KJ_EXPECT(!source.GetCachedData()->rejected, module.getName(), content.length(js));
        });
      }
      KJ_EXPECT(oneByteCount == 2);
      KJ_EXPECT(utf16Count == 1);
    });
  });
}

KJ_TEST("runtime UTF-8 module loading consumes embedded ESM caches") {
  const kj::StringPtr expectedValues[] = {"plain ASCII", "café", "π🚀"};
  CacheIsolate isolate(v8System, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](CacheIsolate::Lock& js) {
    JSG_WITHIN_CONTEXT_SCOPE(js, js.newContext<CacheContext>().getHandle(js), [&](jsg::Lock& js) {
      CompilationObserver observer;
      auto registry = ModuleRegistryImpl<CacheIsolate_TypeWrapper>::install(
          js.v8Isolate, js.v8Context(), observer);
      Bundle::Reader bundle = CACHE_TEST_BUNDLE;
      KJ_ASSERT(bundle.getModules().size() == kj::size(expectedValues));
      for (auto i: kj::indices(expectedValues)) {
        auto fixture = bundle.getModules()[i];
        auto source = kj::str(fixture.getSrc().asChars());
        // V8 checks source length, not content. Invalid syntax at the same UTF-16 length makes
        // successful loading prove that the runtime consumed the cache instead of recompiling.
        KJ_ASSERT(source[0] == '/');
        source[0] = ')';
        auto path = kj::Path::parse(fixture.getName());
        registry->add(path,
            ModuleRegistry::ModuleInfo(
                js, fixture.getName(), source.asPtr(), fixture.getCompileCache(), observer));
        auto module = KJ_REQUIRE_NONNULL(registry->resolve(js, path)).module.getHandle(js);
        instantiateModule(js, module);
        auto exports = check(module->GetModuleNamespace()->ToObject(js.v8Context()));
        auto value = check(exports->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "value"_kj)));
        v8::String::Utf8Value text(js.v8Isolate, value);
        KJ_EXPECT(*text == expectedValues[i], fixture.getName(), *text);
      }
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
