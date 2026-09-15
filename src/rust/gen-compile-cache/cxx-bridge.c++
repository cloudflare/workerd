#include "cxx-bridge.h"

#include <workerd/jsg/compile-cache.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/type-wrapper.h>

#include <kj-rs/kj-rs.h>

#include <capnp/serialize.h>

using namespace kj_rs;
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

struct State {
  // Must outlive V8System: V8 may keep pointers into argv.
  kj::Array<kj::String> flagStorage;
  kj::Array<const kj::StringPtr> flags;
  jsg::V8System system;
  v8::Isolate::CreateParams params;
  CompileCacheIsolate isolate;

  explicit State(::rust::Slice<const ::rust::String> v8Flags)
      : flagStorage(copyFlags(v8Flags)),
        flags(KJ_MAP(f, flagStorage) -> kj::StringPtr { return f; }),
        system(flags),
        isolate(system, kj::heap<jsg::IsolateObserver>(), params) {}

 private:
  static kj::Array<kj::String> copyFlags(::rust::Slice<const ::rust::String> v8Flags) {
    auto builder = kj::heapArrayBuilder<kj::String>(v8Flags.size());
    for (const auto& f: v8Flags) {
      builder.add(kj::str(f));
    }
    return builder.finish();
  }
};

// V8 can only be initialized once per process; created on first use with that call's flags.
State& getState(::rust::Slice<const ::rust::String> v8Flags) {
  static State state(v8Flags);
  return state;
}

// For scripts the runtime loads via CompileFunction with one context extension (see
// per-isolate-bootstrap.c++). The extension count must match the consumer.
//
// TODO(cormick): Only the top-level wrapper is compiled here (everything, with `eager`), so inner
// functions are still lazily compiled per isolate. Executing the scripts first would need the
// runtime's natives; alternatively measure whether `eager` is worth the extra bytecode memory.
v8::ScriptCompiler::CachedData* compileAsFunction(jsg::Lock& js,
    v8::Local<v8::String> resourceName,
    v8::Local<v8::String> contentStr,
    v8::ScriptCompiler::CompileOptions compileOptions) {
  v8::ScriptOrigin origin(resourceName);
  auto source = v8::ScriptCompiler::Source(contentStr, origin, nullptr);
  v8::Local<v8::Object> ext = v8::Object::New(js.v8Isolate);
  auto fn = jsg::check(v8::ScriptCompiler::CompileFunction(
      js.v8Context(), &source, 0, nullptr, 1, &ext, compileOptions));
  return v8::ScriptCompiler::CreateCodeCacheForFunction(fn);
}

// For ES modules the runtime loads via CompileModule (see jsg/modules.c++).
v8::ScriptCompiler::CachedData* compileAsModule(jsg::Lock& js,
    v8::Local<v8::String> resourceName,
    v8::Local<v8::String> contentStr,
    v8::ScriptCompiler::CompileOptions compileOptions) {
  constexpr bool isModule = true;
  v8::ScriptOrigin origin(resourceName, resourceLineOffset, resourceColumnOffset,
      resourceIsSharedCrossOrigin, scriptId, {}, resourceIsOpaque, isWasm, isModule);
  auto source = v8::ScriptCompiler::Source(contentStr, origin, nullptr);
  auto module =
      jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, compileOptions));
  return v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript());
}

}  // namespace

::rust::Vec<uint8_t> compile(::rust::Str path,
    ::rust::Str source,
    ::rust::Slice<const ::rust::String> v8Flags,
    bool asFunction,
    bool eager) {
  auto& ccIsolate = getState(v8Flags).isolate;
  auto compileOptions =
      eager ? v8::ScriptCompiler::kEagerCompile : v8::ScriptCompiler::kNoCompileOptions;

  auto data = ccIsolate.runInLockScope([&](CompileCacheIsolate::Lock& isolateLock) {
    return JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<CompilerCacheContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      return js.tryCatch([&]() {
        auto resourceName = jsg::newExternalOneByteString(js, kj::from<Rust>(path));
        auto contentStr = jsg::newExternalOneByteString(js, kj::from<Rust>(source));

        auto codeCache = asFunction
            ? compileAsFunction(js, resourceName, contentStr, compileOptions)
            : compileAsModule(js, resourceName, contentStr, compileOptions);
        KJ_REQUIRE(codeCache != nullptr, "V8 failed to create a code cache", path);
        auto data = kj::arrayPtr(codeCache->data, codeCache->length).as<RustCopy>();
        delete codeCache;
        return data;
      }, [&](jsg::Value exception) -> ::rust::Vec<uint8_t> {
        auto kjException = js.exceptionToKj(kj::mv(exception));
        KJ_FAIL_REQUIRE("JavaScript compilation error", path, kjException.getDescription());
      });
    });
  });

  return kj::mv(data);
}

}  // namespace workerd::rust::gen_compile_cache
