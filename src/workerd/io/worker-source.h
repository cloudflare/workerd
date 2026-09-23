#pragma once

#include <v8-wasm.h>

#include <capnp/schema.capnp.h>
#include <kj/debug.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {

using kj::byte;

class DynamicEnvBuilder;

// Represents the source code for a Worker.
//
// Typically the Worker's source is delivered in a capnp message structure. However, workerd vs.
// the edge runtime use different capnp schemas. This is mostly because the edge runtime is much
// older and its definition is... ugly, so workerd replaced it with something cleaner for public
// consumption.
//
// WorkerSource is a data structure that can be constructed from either representation -- as well
// as from non-capnp-based sources, like the dynamic worker loader API.
//
// Names and module bodies are shared read-only views (`kj::Arc` over `kj::StringPtr` /
// `kj::ArrayPtr`). The source may be very large, so a view is projected directly from whatever
// owns the bytes -- typically the capnp config message -- rather than copied; the Arc keeps that
// owner alive for as long as any view of it exists, so clone() is a refcount bump and a
// WorkerSource may outlive the object it was extracted from. Use `arcView()` (util/arc-view.h)
// to wrap bytes that are not already shared.
struct WorkerSource {
  // These structs are the variants of the `ModuleContent` `OneOf`, defining all the different
  // module types.
  struct EsModule {
    // UTF-8 source. Not necessarily NUL-terminated (e.g. transpiler output).
    kj::Arc<kj::ArrayPtr<const char>> body;
  };
  struct CommonJsModule {
    kj::Arc<kj::StringPtr> body;
    kj::Maybe<kj::Arc<kj::Array<kj::String>>> namedExports;
  };
  struct TextModule {
    kj::Arc<kj::StringPtr> body;
  };
  struct DataModule {
    kj::Arc<kj::ArrayPtr<const byte>> body;
  };
  struct WasmModule {
    // Compiled .wasm file content.
    kj::Arc<kj::ArrayPtr<const byte>> body;

    // If the module was provided as an already-compiled `WebAssembly.Module` (e.g. passed to the
    // dynamic worker loader from another isolate), the shared compiled code, allowing the target
    // isolate to avoid recompiling.
    kj::Maybe<v8::CompiledWasmModule> compiledModule;
  };
  struct JsonModule {
    // JSON-encoded content; will be parsed automatically when imported.
    kj::Arc<kj::StringPtr> body;
  };
  struct PythonModule {
    kj::Arc<kj::StringPtr> body;
  };

  // This is no longer supported by Python, but it used to define built-in packages.
  struct ObsoletePythonRequirement {};

  // CapnpModule is a .capnp Cap'n Proto schema file. The original text of the file isn't provided;
  // instead, `ModulesSource::capnpSchemas` contains all the capnp schemas needed by the Worker,
  // and the `CapnpModule` only specifies the type ID of a particular file found in there.
  //
  // TODO(someday): Support CapnpSchema in workerd. Today, it's only supported in the internal
  //   codebase.
  struct CapnpModule {
    uint64_t typeId;
  };

  using ModuleContent = kj::OneOf<EsModule,
      CommonJsModule,
      TextModule,
      DataModule,
      WasmModule,
      JsonModule,
      PythonModule,
      ObsoletePythonRequirement,
      CapnpModule>;

  struct Module {
    kj::Arc<kj::StringPtr> name;
    ModuleContent content;

    // Hack for tests: register this as an internal module. Not allowed in production.
    bool treatAsInternalForTest = false;

    Module clone() {
      Module result{.name = name.addRef()};

      // TODO(cleanup): kj::OneOf should have a clone() method.
      KJ_SWITCH_ONEOF(content) {
        KJ_CASE_ONEOF(content, EsModule) {
          result.content = EsModule{.body = content.body.addRef()};
        }
        KJ_CASE_ONEOF(content, TextModule) {
          result.content = TextModule{.body = content.body.addRef()};
        }
        KJ_CASE_ONEOF(content, DataModule) {
          result.content = DataModule{.body = content.body.addRef()};
        }
        KJ_CASE_ONEOF(content, WasmModule) {
          result.content =
              WasmModule{.body = content.body.addRef(), .compiledModule = content.compiledModule};
        }
        KJ_CASE_ONEOF(content, JsonModule) {
          result.content = JsonModule{.body = content.body.addRef()};
        }
        KJ_CASE_ONEOF(content, CommonJsModule) {
          result.content = CommonJsModule{.body = content.body.addRef(),
            .namedExports = content.namedExports.map(
                [](const kj::Arc<kj::Array<kj::String>>& other) { return other.addRef(); })};
        }
        KJ_CASE_ONEOF(content, PythonModule) {
          result.content = PythonModule{.body = content.body.addRef()};
        }
        KJ_CASE_ONEOF(content, ObsoletePythonRequirement) {
          result.content = content;
        }
        KJ_CASE_ONEOF(content, CapnpModule) {
          result.content = content;
        }
      }

      return result;
    }
  };

  // Representation of source code for a worker using Service Workers syntax (deprecated, but will
  // be supported forever).
  struct ScriptSource {
    // Content of the script (JavaScript).
    kj::Arc<kj::StringPtr> mainScript;

    // Name of the script, used as the script origin for stack traces.
    kj::Arc<kj::StringPtr> mainScriptName;

    // Global variables to inject at startup.
    //
    // This is sort of weird and historical. Under the old Service Workers syntax, the entire
    // Worker is one JavaScript file, so there are no "modules" in the normal sense. However,
    // there were various extra blobs of data we wanted to distribute with the code: Wasm modules,
    // as well as large text and data blobs (e.g. embedded asset files). We decided at the time
    // that these made sense as types of bindings. But in fact they don't fit well in the bindings
    // abstraction: most bindings are used as configuration, but these are whole files, too big
    // to be treated like configuration. We ended up creating a mechanism to separate out these
    // binding types and distribute them with the code rather than the config. We also need them
    // to be delivered to the `Worker::Script` constructor rather than the `Worker` constructor
    // (long story).
    //
    // When ES modules arrived, it suddenly made sense to just say that these are modules, not
    // bindings. But of course, we have to keep supporting Service Workers syntax forever.
    //
    // Recall that in Service Workers syntax, bindings show up as global variables.
    //
    // So, this array contains the set of Service Worker bindings that are module-like (text, data,
    // or Wasm blobs), which should be injected into the global scope. We reuse the `Module` type
    // for this because it is convenient, but note that only a subset of types are actually
    // supported as globals. In this array, the `name` of each `Module` is the global variable
    // name.
    kj::Arc<kj::Array<Module>> globals = kj::arc<kj::Array<Module>>();

    // The worker may have a bundle of capnp schemas attached. (In Service Workers syntax, these
    // can't be referenced directly by the app, but they may be used by bindings.)
    kj::Arc<capnp::List<capnp::schema::Node>::Reader> capnpSchemas;

    ScriptSource clone() {
      return {
        .mainScript = mainScript.addRef(),
        .mainScriptName = mainScriptName.addRef(),
        .globals = globals.addRef(),
        .capnpSchemas = capnpSchemas.addRef(),
      };
    }
  };

  // Representation of source code for a worker using ES Modules syntax.
  struct ModulesSource {
    // Path to the main module, which can be looked up in the module registry.
    kj::Arc<kj::StringPtr> mainModule;

    // All the Worker's modules.
    kj::Arc<kj::Array<Module>> modules = kj::arc<kj::Array<Module>>();

    // The worker may have a bundle of capnp schemas attached.
    kj::Arc<capnp::List<capnp::schema::Node>::Reader> capnpSchemas;

    bool isPython = false;

    // Optional Python memory snapshot. The actual capnp type is declared in the internal codebase,
    // so we use AnyStruct here. This is deprecated anyway.
    kj::Maybe<kj::Arc<capnp::AnyStruct::Reader>> pythonMemorySnapshot;

    ModulesSource clone() {
      return {
        .mainModule = mainModule.addRef(),
        .modules = modules.addRef(),
        .capnpSchemas = capnpSchemas.addRef(),
        .isPython = isPython,
        .pythonMemorySnapshot = pythonMemorySnapshot.map(
            [](const kj::Arc<capnp::AnyStruct::Reader>& snapshot) { return snapshot.addRef(); }),
      };
    }
  };

  // The overall value is either ScriptSource or ModulesSource.
  kj::OneOf<ScriptSource, ModulesSource> variant;

  // See DynamicEnvBuilder, below. Not commonly used.
  kj::Maybe<kj::Arc<DynamicEnvBuilder>> dynamicEnvBuilder;

  WorkerSource(ScriptSource source): variant(kj::mv(source)) {}
  WorkerSource(ModulesSource source): variant(kj::mv(source)) {}

  // Clones the source by adding references to its immutable contents.
  WorkerSource clone() {
    KJ_SWITCH_ONEOF(variant) {
      KJ_CASE_ONEOF(script, ScriptSource) {
        return WorkerSource(script.clone());
      }
      KJ_CASE_ONEOF(modules, ModulesSource) {
        return WorkerSource(modules.clone());
      }
    }
    KJ_UNREACHABLE;
  }
};

// Bit of a hack: a `WorkerSource` can contain a `DynamicEnvBuilder`, which is an object that
// has something to do with constructing the `env` object and the `IoChannelFactory`. This
// mechanism is only used in the edge runtime when using dynamic worker loading, to work around a
// historical mess that exists there: the script code and `env` (bindings) are loaded from
// different places and can be mixed and matched, but the (much newer) dynamic worker loader API
// has both of these coming from the same invocation of the loader callback. To get the correct
// `env` through the windy passages and to the right place, we encode it in this "attachment" to
// `WorkerSource`.
//
// In `workerd`, this is not needed at all, due to the design being much newer and cleaner.
// Hopefully, the edge runtime can eventually be refactored to eliminate this!
class DynamicEnvBuilder: public kj::AtomicRefcounted {
  // No methods here: This type exists strictly to be downcast to the appropriate subclass in the
  // internal codebase.
};

}  // namespace workerd
