// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "setup.h"
#include "type-wrapper.h"

#include <cacheTest/compile-cache-fixture.capnp.h>

#include <kj/test.h>

namespace workerd::jsg::test {
namespace {

struct CacheContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(CacheContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CacheIsolate, CacheContext);

KJ_TEST("embedded ESM caches accept ASCII, Latin-1 and UTF-16 source strings") {
  V8System v8System;
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

}  // namespace
}  // namespace workerd::jsg::test
