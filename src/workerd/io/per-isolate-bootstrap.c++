// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "per-isolate-bootstrap.h"

#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/util.h>

#include <capnp/dynamic.h>
#include <capnp/schema.h>

namespace workerd {

namespace {

// The script lookup table is the same for every context since PER_ISOLATE_BUNDLE
// is a compile-time constant. We cache it as a process-wide static to avoid
// rebuilding the HashMap on every context creation. This is safe because:
//   - The table is immutable after construction (read-only concurrent access)
//   - Keys (kj::StringPtr) and values (Module::Reader) are non-owning views
//     into the static capnp bundle data which lives for the process lifetime
//   - C++ guarantees thread-safe initialization of function-local statics
const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& getScriptTable(jsg::Bundle::Reader bundle) {
  static auto table = [&]() {
    kj::HashMap<kj::StringPtr, jsg::Module::Reader> t;
    for (auto module: bundle.getModules()) {
      t.insert(module.getName(), module);
    }
    return t;
  }();
  return table;
}

// Per-context state for the bootstrap require() mechanism.
// Heap-allocated and stored in the context's BOOTSTRAP_STATE embedder data slot
// so that require() remains callable for lazy evaluation after the initial
// bootstrap completes.
struct BootstrapState {
  // Scripts is a const reference to the process-wide script lookup table built
  // from the compiled-in bundle. It is guaranteed to outlive this BootstrapState
  const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& scripts;
  kj::HashMap<kj::String, jsg::JsRef<jsg::JsValue>> cache;
  kj::HashSet<kj::String> loading;
  jsg::JsRef<jsg::JsObject> compatFlagsObj;
  jsg::JsRef<jsg::JsFunction> requireFn;
  // The primordials object, loaded before main and injected as a pseudo-global
  // into every script. Provides prototype-pollution-safe references to built-in
  // methods and constructors.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> primordials;
};

// Retrieve the BootstrapState from the current context's embedder data.
BootstrapState& getBootstrapState(jsg::Lock& js) {
  return KJ_REQUIRE_NONNULL(jsg::getAlignedPointerFromEmbedderData<BootstrapState>(
                                js.v8Context(), jsg::ContextPointerSlot::BOOTSTRAP_STATE),
      "per-isolate bootstrap state not initialized");
}

// Normalize a require() specifier by stripping a leading "./" prefix.
kj::StringPtr normalizeSpecifier(kj::StringPtr specifier) {
  if (specifier.startsWith("./"_kj)) {
    return specifier.slice(2);
  }
  return specifier;
}

// The V8 callback backing the require() function injected into bootstrap scripts.
void requireCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto& js = jsg::Lock::from(info.GetIsolate());
  auto& state = getBootstrapState(js);

  js.withinHandleScope([&] {
    KJ_REQUIRE(info.Length() >= 1 && info[0]->IsString(), "require() expects a string argument");

    auto specifierUtf8 = jsg::JsValue(info[0]).toString(js);
    auto normalized = normalizeSpecifier(specifierUtf8);

    // Check cache.
    KJ_IF_SOME(cached, state.cache.find(normalized)) {
      info.GetReturnValue().Set(v8::Local<v8::Value>(cached.getHandle(js)));
      return;
    }

    // Cycle detection.
    KJ_REQUIRE(!state.loading.contains(normalized), "circular dependency in per-isolate bootstrap",
        normalized);

    // Look up script in bundle.
    auto& script = KJ_REQUIRE_NONNULL(
        state.scripts.find(normalized), "per-isolate bootstrap: unknown script", normalized);

    // Mark as loading.
    state.loading.insert(kj::str(normalized));
    KJ_DEFER(state.loading.eraseMatch(normalized));

    // Create the module and exports objects for this script.
    auto moduleObj = js.obj();
    auto exportsObj = js.obj();
    moduleObj.set(js, "exports"_kj, exportsObj);

    // Build the context extension object containing all pseudo-globals:
    //   require, compatFlags, primordials, module, exports
    auto extObj = js.obj();
    if (normalized != "primordials") {
      extObj.set(js, "require"_kj, state.requireFn.getHandle(js));
    }
    extObj.set(js, "compatFlags"_kj, state.compatFlagsObj.getHandle(js));
    extObj.set(js, "module"_kj, moduleObj);
    extObj.set(js, "exports"_kj, exportsObj);
    KJ_IF_SOME(primordials, state.primordials) {
      extObj.set(js, "primordials"_kj, primordials.getHandle(js));
    }

    // Compile the script as a function with the context extension.
    // CompileFunction with context_extensions makes the extension object's
    // properties available as variables in the function scope without
    // putting them on globalThis.
    auto source = script.getSrc();
    auto sourceStr = js.str(source);
    v8::ScriptOrigin origin(js.strIntern(normalized));

    // Use the compile cache from the bundle if available and compatible.
    v8::ScriptCompiler::CachedData* cachedData = nullptr;
    auto options = v8::ScriptCompiler::kNoCompileOptions;
    auto compileCache = script.getCompileCache();
    if (compileCache.size() > 0 && compileCache.begin() != nullptr) {
      // V8 takes ownership of the CachedData instance but not the underlying
      // buffer (which lives in the static capnp bundle data).
      cachedData = new v8::ScriptCompiler::CachedData(compileCache.begin(), compileCache.size(),
          v8::ScriptCompiler::CachedData::BufferNotOwned);
      if (cachedData->CompatibilityCheck(js.v8Isolate) !=
          v8::ScriptCompiler::CachedData::kSuccess) {
        delete cachedData;
        cachedData = nullptr;
      }
    }

    // Source takes ownership of cachedData. Do not use cachedData after this.
    v8::ScriptCompiler::Source compilerSource(sourceStr, origin, cachedData);

    auto maybeCached = compilerSource.GetCachedData();
    if (maybeCached != nullptr && !maybeCached->rejected) {
      options = v8::ScriptCompiler::kConsumeCodeCache;
    }

    v8::Local<v8::Object> ext = extObj;
    auto fn = jsg::JsFunction(jsg::check(v8::ScriptCompiler::CompileFunction(
        js.v8Context(), &compilerSource, 0, nullptr, 1, &ext, options)));

    // Execute the script.
    fn.call(js, js.undefined());

    // Read back module.exports (script may have reassigned it).
    auto result = moduleObj.get(js, "exports"_kj);

    // Cache the result.
    state.cache.insert(kj::str(normalized), result.addRef(js));

    info.GetReturnValue().Set(v8::Local<v8::Value>(result));
  });
}

// Build a JS object with { flagName: true/false } entries from compat flags.
jsg::JsRef<jsg::JsObject> buildCompatFlagsObject(jsg::Lock& js, CompatibilityFlags::Reader flags) {
  auto obj = js.obj();

  auto schema = capnp::Schema::from<CompatibilityFlags>();
  auto dynamicFlags = capnp::toDynamic(flags);

  for (auto field: schema.getFields()) {
    // Find the enableFlag annotation to get the JS-visible flag name.
    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        auto flagName = annotation.getValue().getText();
        auto value = dynamicFlags.get(field).as<bool>();
        obj.set(js, flagName, js.boolean(value));
        break;
      }
    }
  }

  return obj.addRef(js);
}

}  // namespace

void runPerIsolateBootstrap(
    jsg::Lock& js, jsg::Bundle::Reader bundle, CompatibilityFlags::Reader flags) {
  auto context = js.v8Context();

  // Get (or lazily build) the thread-local script lookup table.
  auto& scripts = getScriptTable(bundle);

  // Verify required scripts exist.
  KJ_REQUIRE(scripts.find("primordials"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'primordials' script");
  KJ_REQUIRE(scripts.find("main"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'main' entry point");

  // Heap-allocate the per-context bootstrap state and store it in the context's
  // embedder data. This allows the require() function to remain usable beyond
  // this call, supporting lazy evaluation patterns in bootstrap scripts.
  // We use new instead of the idiomatic kj::heap<...>, etc because the
  // pointer is being stored in a raw form in the embedder data slot.
  auto* state = new BootstrapState{.scripts = scripts};
  jsg::setAlignedPointerInEmbedderData(context, jsg::ContextPointerSlot::BOOTSTRAP_STATE, state);

  // Build the compat flags object.
  state->compatFlagsObj = buildCompatFlagsObject(js, flags);

  // Create the require() function. No v8::External needed — the callback
  // reads state from the context embedder slot.
  state->requireFn =
      jsg::JsFunction(jsg::check(v8::Function::New(context, requireCallback))).addRef(js);

  // Load primordials first — this must happen before any other script so that
  // built-in prototype methods are captured before anything could pollute them.
  // The result is cached in state and injected as a pseudo-global into every
  // subsequent script via the context extension object.
  JSG_TRY(js) {
    auto result = state->requireFn.getHandle(js).call(js, js.undefined(), js.str("primordials"_kj));
    state->primordials = result.addRef(js);

    // Run the entry point. This synchronously executes main.js, which may
    // require() other scripts. All execution is synchronous.
    state->requireFn.getHandle(js).call(js, js.undefined(), js.str("main"_kj));
  }
  JSG_CATCH(exception) {
    kj::throwFatalException(js.exceptionToKj(kj::mv(exception)));
  }
}

void cleanupPerIsolateBootstrap(jsg::Lock& js, v8::Local<v8::Context> context) {
  KJ_IF_SOME(state,
      jsg::getAlignedPointerFromEmbedderData<BootstrapState>(
          context, jsg::ContextPointerSlot::BOOTSTRAP_STATE)) {
    delete &state;
    jsg::setAlignedPointerInEmbedderData(
        context, jsg::ContextPointerSlot::BOOTSTRAP_STATE, nullptr);
  }
}

}  // namespace workerd
