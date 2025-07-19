#pragma once

#include <capnp/schema.capnp.h>
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
// Note that this structure contains StringPtrs and ArrayPtrs pointing to external data which must
// remain alive while the WorkerSource is alive. This is done because the source may be very large,
// and we don't want to have to copy it all out of the original capnp structure.
struct WorkerSource {
  // These structs are the variants of the `ModuleContent` `OneOf`, defining all the different
  // module types.
  struct EsModule {
    kj::StringPtr body;
  };
  struct CommonJsModule {
    kj::StringPtr body;
    kj::Maybe<kj::Array<kj::StringPtr>> namedExports;
  };
  struct TextModule {
    kj::StringPtr body;
  };
  struct DataModule {
    kj::ArrayPtr<const byte> body;
  };
  struct WasmModule {
    // Compiled .wasm file content.
    kj::ArrayPtr<const byte> body;
  };
  struct JsonModule {
    // JSON-encoded content; will be parsed automatically when imported.
    kj::StringPtr body;
  };
  struct PythonModule {
    kj::StringPtr body;
  };

  // PythonRequirement is a variant of ModuleContent, but has no body. The module name specifies
  // a Python package to be provided by the system.
  struct PythonRequirement {};

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
      PythonRequirement,
      CapnpModule>;

  struct Module {
    kj::StringPtr name;
    ModuleContent content;

    // Hack for tests: register this as an internal module. Not allowed in production.
    bool treatAsInternalForTest = false;
  };

  // Representation of source code for a worker using Service Workers syntax (deprecated, but will
  // be supported forever).
  struct ScriptSource {
    // Content of the script (JavaScript). Pointer is valid only until the Script constructor
    // returns.
    kj::StringPtr mainScript;

    // Name of the script, used as the script origin for stack traces. Pointer is valid only until
    // the Script constructor returns.
    kj::StringPtr mainScriptName;

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
    kj::Array<Module> globals;

    // The worker may have a bundle of capnp schemas attached. (In Service Workers syntax, these
    // can't be referenced directly by the app, but they may be used by bindings.)
    capnp::List<capnp::schema::Node>::Reader capnpSchemas;
  };

  // Representation of source code for a worker using ES Modules syntax.
  struct ModulesSource {
    // Path to the main module, which can be looked up in the module registry. Pointer is valid
    // only until the Script constructor returns.
    kj::StringPtr mainModule;

    // All the Worker's modules.
    kj::Array<Module> modules;

    // The worker may have a bundle of capnp schemas attached.
    capnp::List<capnp::schema::Node>::Reader capnpSchemas;

    bool isPython;

    // Optional Python memory snapshot. The actual capnp type is declared in the internal codebase,
    // so we use AnyStruct here. This is deprecated anyway.
    kj::Maybe<capnp::AnyStruct::Reader> pythonMemorySnapshot;
  };

  // The overall value is either ScriptSource or ModulesSource.
  kj::OneOf<ScriptSource, ModulesSource> variant;

  // See DynamicEnvBuilder, below. Not commonly used.
  kj::Maybe<kj::Arc<DynamicEnvBuilder>> dynamicEnvBuilder;

  WorkerSource(ScriptSource source): variant(kj::mv(source)) {}
  WorkerSource(ModulesSource source): variant(kj::mv(source)) {}
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
