// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "per-isolate-bootstrap.h"

#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/util.h>
#include <workerd/util/autogate.h>

#include <per_isolate/per_isolate.capnp.h>

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
const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& getScriptTable() {
  static auto table = []() {
    jsg::Bundle::Reader bundle = PER_ISOLATE_BUNDLE;
    kj::HashMap<kj::StringPtr, jsg::Module::Reader> t;
    for (auto module: bundle.getModules()) {
      t.insert(module.getName(), module);
    }
    return t;
  }();
  return table;
}

// Parsed compat flag field: maps a JS-visible flag name to the capnp schema field.
// The schema is compiled into the binary, so these are cached per-process.
struct CompatFlagField {
  kj::StringPtr enableFlag;
  capnp::StructSchema::Field field;
};

const kj::ArrayPtr<const CompatFlagField> getCompatFlagFields() {
  static auto table = []() {
    auto schema = capnp::Schema::from<CompatibilityFlags>();
    kj::Vector<CompatFlagField> fields;
    for (auto field: schema.getFields()) {
      for (auto annotation: field.getProto().getAnnotations()) {
        if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
          fields.add(CompatFlagField{
            .enableFlag = annotation.getValue().getText(),
            .field = field,
          });
          break;
        }
      }
    }
    return fields.releaseAsArray();
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
  v8::Global<v8::DictionaryTemplate> contextExtensionTemplate;
  kj::HashMap<kj::String, jsg::JsRef<jsg::JsValue>> cache;
  kj::HashSet<kj::String> loading;
  jsg::JsRef<jsg::JsObject> compatFlagsObj;
  jsg::JsRef<jsg::JsObject> autogatesObj;
  jsg::JsRef<jsg::JsFunction> requireFn;
  // The primordials object, loaded before main and injected as a pseudo-global
  // into every script. Provides prototype-pollution-safe references to built-in
  // methods and constructors.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> primordials;
};

// Build the context extension object for a script. This provides the pseudo-global
// variables that scripts access directly (e.g. `require`, `compatFlags`, `primordials`).
// We use a DictionaryTemplate to create a fresh object for each script with the same
// shape. This is faster than creating a new object and defining properties on it for
// each script using set().
jsg::JsObject createContextExtension(jsg::Lock& js,
    BootstrapState& state,
    const jsg::JsObject& moduleObj,
    const jsg::JsObject& exportsObj,
    bool includeRequire) {

  auto tmpl = state.contextExtensionTemplate.Get(js.v8Isolate);
  if (tmpl.IsEmpty()) {
    static constexpr std::string_view names[] = {
      "require",
      "compatFlags",
      "autogates",
      "module",
      "exports",
      "primordials",
    };
    tmpl = v8::DictionaryTemplate::New(js.v8Isolate, names);
    state.contextExtensionTemplate.Reset(js.v8Isolate, tmpl);
  }

  v8::MaybeLocal<v8::Value> values[6] = {
    js.v8Undefined(),  // require
    v8::Local<v8::Value>(state.compatFlagsObj.getHandle(js)),
    v8::Local<v8::Value>(state.autogatesObj.getHandle(js)), v8::Local<v8::Value>(moduleObj),
    v8::Local<v8::Value>(exportsObj),
    js.v8Undefined(),  // primordials
  };

  if (includeRequire) {
    values[0] = v8::Local<v8::Value>(state.requireFn.getHandle(js));
  }
  KJ_IF_SOME(primordials, state.primordials) {
    values[5] = v8::Local<v8::Value>(primordials.getHandle(js));
  }
  return jsg::JsObject(tmpl->NewInstance(js.v8Context(), values));
}

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
    auto extObj =
        createContextExtension(js, state, moduleObj, exportsObj, normalized != "primordials");

    // Compile the script as a function with the context extension.
    // CompileFunction with context_extensions makes the extension object's
    // properties available as variables in the function scope without
    // putting them on globalThis.
    auto source = script.getSrc();
    // Use strExtern to avoid copying — the source data lives in the static
    // capnp bundle for the lifetime of the process.
    auto sourceStr = js.strExtern(source.asChars());
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

// Build a JS object with { flagName: true } entries for enabled compat flags.
// Disabled flags are omitted — scripts check with `if ('flag_name' in compatFlags)`
// which yields false for unset flags.
jsg::JsRef<jsg::JsObject> buildCompatFlagsObject(jsg::Lock& js, CompatibilityFlags::Reader flags) {
  auto obj = js.obj();
  auto dynamicFlags = capnp::toDynamic(flags);

  for (auto& entry: getCompatFlagFields()) {
    if (dynamicFlags.get(entry.field).as<bool>()) {
      obj.set(js, entry.enableFlag, js.boolean(true));
    }
  }

  return obj.addRef(js);
}

// Build a JS object with { gateName: true } entries for enabled autogates.
// Disabled autogates are omitted.
jsg::JsRef<jsg::JsObject> buildAutogatesObject(jsg::Lock& js) {
  auto obj = js.obj();

  for (auto i = util::AutogateKey(0); i < util::AutogateKey::NumOfKeys;
       i = util::AutogateKey(static_cast<int>(i) + 1)) {
    if (util::Autogate::isEnabled(i)) {
      auto name = kj::str(i);
      obj.set(js, name, js.boolean(true));
    }
  }

  return obj.addRef(js);
}

}  // namespace

void runPerIsolateBootstrap(jsg::Lock& js, CompatibilityFlags::Reader flags) {
  auto context = js.v8Context();

  // Get (or lazily build) the process-wide script lookup table.
  auto& scripts = getScriptTable();

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

  // Build the compat flags and autogates objects.
  state->compatFlagsObj = buildCompatFlagsObject(js, flags);
  state->autogatesObj = buildAutogatesObject(js);

  // Create the require() function. No v8::External needed — the callback
  // reads state from the context embedder slot.
  state->requireFn =
      jsg::JsFunction(jsg::check(v8::Function::New(context, requireCallback))).addRef(js);

  // Load primordials first — this must happen before any other script so that
  // built-in prototype methods are captured before anything could pollute them.
  // The result is cached in state and injected as a pseudo-global into every
  // subsequent script via the context extension object.
  JSG_TRY(js) {
    auto result =
        state->requireFn.getHandle(js).call(js, js.undefined(), js.strIntern("primordials"_kj));
    state->primordials = result.addRef(js);

    // Run the entry point. This synchronously executes main.js, which may
    // require() other scripts. All execution is synchronous.
    state->requireFn.getHandle(js).call(js, js.undefined(), js.strIntern("main"_kj));
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
