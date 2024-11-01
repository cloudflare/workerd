#include "cxx-bridge.h"

#include <workerd/jsg/compile-cache.h>
#include <workerd/jsg/setup.h>

#include <rust/cxx-integration/lib.rs.h>

#include <capnp/serialize.h>

namespace workerd::rust::gen_compile_cache {

namespace {
struct CompilerCacheContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(CompilerCacheContext) {}
};

JSG_DECLARE_ISOLATE_TYPE(CompileCacheIsolate, CompilerCacheContext);

constexpr int resourceLineOffset = 0;
constexpr int resourceColumnOffset = 0;
constexpr bool resourceIsSharedCrossOrigin = false;
constexpr int scriptId = -1;
constexpr bool resourceIsOpaque = false;
constexpr bool isWasm = false;
constexpr bool isModule = true;
constexpr v8::ScriptCompiler::CompileOptions compileOptions = v8::ScriptCompiler::kNoCompileOptions;

}  // namespace

::rust::Vec<uint8_t> compile(::rust::Str path, ::rust::Str source) {
  static jsg::V8System system{};
  static v8::Isolate::CreateParams params{};
  static CompileCacheIsolate ccIsolate(system, kj::heap<jsg::IsolateObserver>(), params);

  auto data = ccIsolate.runInLockScope([&](CompileCacheIsolate::Lock& isolateLock) {
    return JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<CompilerCacheContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      auto resourceName = jsg::newExternalOneByteString(js, fromRust(path));
      v8::ScriptOrigin origin(resourceName, resourceLineOffset, resourceColumnOffset,
          resourceIsSharedCrossOrigin, scriptId, {}, resourceIsOpaque, isWasm, isModule);

      auto contentStr = jsg::newExternalOneByteString(js, fromRust(source));
      auto source = v8::ScriptCompiler::Source(contentStr, origin, nullptr);
      auto module =
          jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, compileOptions));

      auto codeCache = v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript());
      auto data = kj::arrayPtr(codeCache->data, codeCache->length).as<RustCopy>();
      delete codeCache;
      return data;
    });
  });

  return kj::mv(data);
}

}  // namespace workerd::rust::gen_compile_cache
