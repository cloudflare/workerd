#pragma once

#include <workerd/api/commonjs.h>
#include <workerd/api/modules.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/util/strong-bool.h>

#include <pyodide/python-entrypoint.embed.h>

#include <capnp/blob.h>
#include <capnp/schema-loader.h>
#include <capnp/schema.h>

// This header provides utilities for setting up the ModuleRegistry for a worker.
// It is meant to be included in only two places; workerd-api.c++ and the equivalent
// file in the internal repo. It is templated on the TypeWrapper and JsgIsolate types.

namespace workerd {
namespace api {
class ServiceWorkerGlobalScope;
}  // namespace api

WD_STRONG_BOOL(IsPythonWorker);

namespace modules::capnp {
// Helper to iterate over the nested nodes of a schema for capnp modules, filtering
// out the kinds we don't care about.
void filterNestedNodes(const auto& schemaLoader, const auto& schema, auto fn) {
  for (auto nested: schema.getProto().getNestedNodes()) {
    auto child = schemaLoader.get(nested.getId());
    switch (child.getProto().which()) {
      case ::capnp::schema::Node::FILE:
      case ::capnp::schema::Node::STRUCT:
      case ::capnp::schema::Node::INTERFACE: {
        fn(nested.getName(), child);
        break;
      }
      case ::capnp::schema::Node::ENUM:
      case ::capnp::schema::Node::CONST:
      case ::capnp::schema::Node::ANNOTATION:
        // These kinds are not implemented and cannot contain further nested scopes, so
        // don't generate anything at all for now.
        break;
    }
  }
}

// This is used only by the original module registry implementation in both workerd
// and the internal project. It collects the exports and instantiates the exports of
// a capnp module at the same time and returns a ModuleInfo for the original registry.
// The new module registry variation uses a different approach where the exports are
// collected up front by the exports are instantiated lazily when the module is actually
// resolved.
template <typename JsgIsolate>
jsg::ModuleRegistry::ModuleInfo addCapnpModule(
    typename JsgIsolate::Lock& lock, uint64_t typeId, kj::StringPtr name) {
  const auto& schemaLoader = lock.template getCapnpSchemaLoader<api::ServiceWorkerGlobalScope>();
  auto schema = schemaLoader.get(typeId);
  auto fileScope = lock.v8Ref(lock.wrap(lock.v8Context(), schema).template As<v8::Value>());
  kj::Vector<kj::StringPtr> exports;
  kj::HashMap<kj::StringPtr, jsg::Value> topLevelDecls;

  filterNestedNodes(schemaLoader, schema, [&](auto name, const auto& child) {
    // topLevelDecls are the actual exported values...
    topLevelDecls.insert(
        name, lock.v8Ref(lock.wrap(lock.v8Context(), child).template As<v8::Value>()));
    // ... while exports is just the list of names
    exports.add(name);
  });

  return jsg::ModuleRegistry::ModuleInfo(lock, name, exports.asPtr().asConst(),
      jsg::ModuleRegistry::CapnpModuleInfo(kj::mv(fileScope), kj::mv(topLevelDecls)));
}
}  // namespace modules::capnp

// ===========================================================================================
// Python module support

namespace modules::python {
kj::Own<api::pyodide::PyodideMetadataReader::State> createPyodideMetadataState(
    const Worker::Script::ModulesSource& source,
    api::pyodide::IsWorkerd isWorkerd,
    api::pyodide::IsTracing isTracing,
    api::pyodide::SnapshotToDisk snapshotToDisk,
    api::pyodide::CreateBaselineSnapshot createBaselineSnapshot,
    PythonSnapshotRelease::Reader pythonRelease,
    kj::Maybe<kj::Array<kj::byte>> maybeSnapshot,
    CompatibilityFlags::Reader featureFlags);
}  // namespace modules::python

// Creates an instance of the (new) ModuleRegistry. This method provides the
// initialization logic that is agnostic to the Worker::Api implementation,
// but accepts a callback parameter to handle the Worker::Api-specific details.
//
// Note: this is a big template but it will only be called from two places in
// the codebase, one for workerd and one for the internal project. It depends
// on the TypeWrapper specific to each project.
template <typename TypeWrapper>
static kj::Arc<jsg::modules::ModuleRegistry> newWorkerModuleRegistry(
    const jsg::ResolveObserver& resolveObserver,
    kj::Maybe<const Worker::Script::ModulesSource&> maybeSource,
    const CompatibilityFlags::Reader& featureFlags,
    const jsg::Url& bundleBase,
    auto setupForApi,
    jsg::modules::ModuleRegistry::Builder::Options options =
        jsg::modules::ModuleRegistry::Builder::Options::NONE) {
  jsg::modules::ModuleRegistry::Builder builder(resolveObserver, bundleBase, options);

  // This callback is used when a module is being loaded to arrange evaluating the
  // module outside of the current IoContext.
  builder.setEvalCallback([](jsg::Lock& js, const auto& module, auto v8Module,
                              const auto& observer) -> jsg::Promise<jsg::Value> {
    return js.tryOrReject<jsg::Value>([&] {
      // Creating the SuppressIoContextScope here ensures that the current IoContext,
      // if any, is moved out of the way while we are evaluating.
      SuppressIoContextScope suppressIoContextScope;
      KJ_DASSERT(!IoContext::hasCurrent(), "Module evaluation must not be in an IoContext");
      return jsg::check(v8Module->Evaluate(js.v8Context()));
    });
  });

  // Add the module bundles that are built into the runtime.
  api::registerBuiltinModules<TypeWrapper>(builder, featureFlags);

  bool hasPythonModules = false;

  // Add the module bundles that are configured by the worker (if any)
  // The only case where maybeSource is none is when the worker is using
  // the old service worker script format or "inherit", in which case
  // we will initialize a module registry with the built-ins, extensions,
  // etc but no worker bundle modules will be added.
  KJ_IF_SOME(source, maybeSource) {
    // Register any capnp schemas contained in the source bundle
    auto& schemaLoader = builder.getSchemaLoader();
    for (auto schema: source.capnpSchemas) {
      schemaLoader.load(schema);
    }

    jsg::modules::ModuleBundle::BundleBuilder bundleBuilder(bundleBase);
    bool firstEsm = true;
    using namespace workerd::api::pyodide;

    for (auto& def: source.modules) {
      KJ_SWITCH_ONEOF(def.content) {
        KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
          jsg::modules::Module::Flags flags = jsg::modules::Module::Flags::ESM;
          // Only the first ESM module we encounter is the main module.
          // This should also be the first module in the list but we're
          // not enforcing that here.
          if (firstEsm) {
            flags = flags | jsg::modules::Module::Flags::MAIN;
            firstEsm = false;
          }
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addEsmModule(def.name, content.body, flags);
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newTextModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newDataModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addWasmModule(def.name, content.body);
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
          // The content.body is memory-resident and is expected to outlive the
          // module registry. We can safely pass a reference to the module handler.
          // It will not be copied into a JS string until the module is actually
          // evaluated.
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newJsonModuleHandler(content.body));
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
          kj::ArrayPtr<const kj::StringPtr> named;
          KJ_IF_SOME(n, content.namedExports) {
            named = n;
          }
          bundleBuilder.addSyntheticModule(def.name,
              jsg::modules::Module::newCjsStyleModuleHandler<api::CommonJsModuleContext,
                  TypeWrapper>(content.body, def.name),
              KJ_MAP(name, named) { return kj::str(name); });
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
          KJ_FAIL_ASSERT("Python modules are not currently supported with the new module registry");
          // KJ_REQUIRE(featureFlags.getPythonWorkers(),
          //     "The python_workers compatibility flag is required to use Python.");
          // firstEsm = false;
          // hasPythonModules = true;
          // kj::StringPtr entry = PYTHON_ENTRYPOINT;
          // bundleBuilder.addEsmModule(def.name, entry);
          // break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
          // Handled separately
          break;
        }
        KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
          // For the new module registry, the implementation is a bit different than
          // the original. Up front we collect only the names of the exports since we
          // need to know those when we create the synthetic module. The actual exports
          // themselves, however, are instantiated lazily when the module is actually
          // resolved and evaluated.
          auto& schemaLoader = builder.getSchemaLoader();
          auto schema = schemaLoader.get(content.typeId);
          kj::Vector<kj::String> exports;
          modules::capnp::filterNestedNodes(schemaLoader, schema,
              [&](auto name, const capnp::Schema& child) { exports.add(kj::str(name)); });

          bundleBuilder.addSyntheticModule(def.name,
              [typeId = content.typeId, &schemaLoader](jsg::Lock& js, const jsg::Url&,
                  const jsg::modules::Module::ModuleNamespace& ns,
                  const jsg::CompilationObserver& observer) {
            auto& typeWrapper = TypeWrapper::from(js.v8Isolate);
            KJ_IF_SOME(schema, schemaLoader.tryGet(typeId)) {
              return js.tryCatch([&] {
                // Set the default export...
                ns.setDefault(js,
                    jsg::JsValue(typeWrapper.wrap(js, js.v8Context(), kj::none, schema)
                                     .template As<v8::Value>()));
                // Set each of the named exports...
                // The names must match what we collected when the bundle was built.
                modules::capnp::filterNestedNodes(
                    schemaLoader, schema, [&](auto name, const auto& child) {
                  ns.set(js, name,
                      jsg::JsValue(typeWrapper.wrap(js, js.v8Context(), kj::none, child)));
                });
                return true;
              }, [&](jsg::Value exception) {
                js.v8Isolate->ThrowException(exception.getHandle(js));
                return false;
              });
            } else {
              // The schema should have been loaded when the Worker::Script was created.
              // This likely indicates an internal error of some kind.
              js.v8Isolate->ThrowException(
                  js.typeError("Invalid or unknown capnp module type identifier"));
              return false;
            }
          },
              exports.releaseAsArray());
        }
      }
    }

    builder.add(bundleBuilder.finish());
  }

  // Now perform any Worker::Api-specific setup.
  setupForApi(builder, hasPythonModules ? IsPythonWorker::YES : IsPythonWorker::NO);

  // All done!
  return builder.finish();
}

// ======================================================================================
// Legacy module registry support

namespace modules::legacy {

template <typename JsgIsolate>
static v8::Local<v8::String> compileTextGlobal(
    typename JsgIsolate::Lock& lock, ::capnp::Text::Reader reader) {
  return lock.wrapNoContext(reader);
};

template <typename JsgIsolate>
static v8::Local<v8::ArrayBuffer> compileDataGlobal(
    typename JsgIsolate::Lock& lock, ::capnp::Data::Reader reader) {
  return lock.wrapNoContext(kj::heapArray(reader));
};

template <typename JsgIsolate>
static v8::Local<v8::WasmModuleObject> compileWasmGlobal(typename JsgIsolate::Lock& lock,
    ::capnp::Data::Reader reader,
    const jsg::CompilationObserver& observer) {
  lock.setAllowEval(true);
  KJ_DEFER(lock.setAllowEval(false));

  // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
  // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
  // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
  // compiles fast but runs slower.
  AllowV8BackgroundThreadsScope scope;

  return jsg::compileWasmModule(lock, reader, observer);
};

template <typename JsgIsolate>
static v8::Local<v8::Value> compileJsonGlobal(
    typename JsgIsolate::Lock& lock, ::capnp::Text::Reader reader) {
  return jsg::check(v8::JSON::Parse(lock.v8Context(), lock.wrapNoContext(reader)));
};

// Compiles a module for the legacy module registry, returning kj::none if the module
// is a Python module or Python requirement, which are handled elsewhere.
template <typename JsgIsolate>
kj::Maybe<jsg::ModuleRegistry::ModuleInfo> tryCompileLegacyModule(jsg::Lock& js,
    kj::StringPtr name,
    const Worker::Script::ModuleContent& moduleContent,
    const jsg::CompilationObserver& observer,
    CompatibilityFlags::Reader featureFlags) {
  auto& lock = kj::downcast<typename JsgIsolate::Lock>(js);
  KJ_SWITCH_ONEOF(moduleContent) {
    KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
      return jsg::ModuleRegistry::ModuleInfo(js, name, kj::none,
          jsg::ModuleRegistry::TextModuleInfo(
              js, modules::legacy::compileTextGlobal<JsgIsolate>(lock, content.body)));
    }
    KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
      return jsg::ModuleRegistry::ModuleInfo(js, name, kj::none,
          jsg::ModuleRegistry::DataModuleInfo(
              js, modules::legacy::compileDataGlobal<JsgIsolate>(lock, content.body)));
    }
    KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
      auto wasmModule =
          modules::legacy::compileWasmGlobal<JsgIsolate>(lock, content.body, observer);
      auto moduleInfo = jsg::ModuleRegistry::ModuleInfo(
          js, name, kj::none, jsg::ModuleRegistry::WasmModuleInfo(js, wasmModule));
      moduleInfo.setModuleSourceObject(lock, wasmModule.template As<v8::Object>());
      return moduleInfo;
    }
    KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
      return jsg::ModuleRegistry::ModuleInfo(js, name, kj::none,
          jsg::ModuleRegistry::JsonModuleInfo(
              js, modules::legacy::compileJsonGlobal<JsgIsolate>(lock, content.body)));
    }
    KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
      // TODO(soon): Make sure passing nullptr to compile cache is desired.
      return jsg::ModuleRegistry::ModuleInfo(js, name, content.body, nullptr /* compile cache */,
          jsg::ModuleInfoCompileOption::BUNDLE, observer);
    }
    KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
      return jsg::ModuleRegistry::ModuleInfo(js, name, content.namedExports,
          jsg::ModuleRegistry::CommonJsModuleInfo(lock, name, content.body,
              kj::heap<api::CommonJsImpl<typename JsgIsolate::Lock>>(js, kj::Path::parse(name))));
    }
    KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
      // Nothing to do. Handled elsewhere.
      return kj::none;
    }
    KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
      // Nothing to do. Handled elsewhere.
      return kj::none;
    }
    KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
      return workerd::modules::capnp::addCapnpModule<JsgIsolate>(lock, content.typeId, name);
    }
  }
  KJ_UNREACHABLE;
}

template <typename JsgIsolate>
kj::Array<Worker::Script::CompiledGlobal> compileServiceWorkerGlobals(jsg::Lock& js,
    const Worker::Script::ScriptSource& source,
    const Worker::Isolate& isolate,
    const jsg::CompilationObserver& observer) {
  auto& lock = kj::downcast<typename JsgIsolate::Lock>(js);

  auto globals = source.globals.asPtr();
  auto compiledGlobals = kj::heapArrayBuilder<Worker::Script::CompiledGlobal>(globals.size());

  for (auto& global: globals) {
    js.withinHandleScope([&] {
      // Don't use String's usual TypeHandler here because we want to intern the string.
      auto name = jsg::v8StrIntern(js.v8Isolate, global.name);

      v8::Local<v8::Value> value;

      KJ_SWITCH_ONEOF(global.content) {
        KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
          value =
              workerd::modules::legacy::template compileTextGlobal<JsgIsolate>(lock, content.body);
        }
        KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
          value =
              workerd::modules::legacy::template compileDataGlobal<JsgIsolate>(lock, content.body);
        }
        KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
          value = workerd::modules::legacy::template compileWasmGlobal<JsgIsolate>(
              lock, content.body, observer);
        }
        KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
          value =
              workerd::modules::legacy::template compileJsonGlobal<JsgIsolate>(lock, content.body);
        }
        KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
          KJ_FAIL_REQUIRE("modules not supported with mainScript");
        }
        KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
          KJ_FAIL_REQUIRE("modules not supported with mainScript");
        }
        KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
          KJ_FAIL_REQUIRE("modules not supported with mainScript");
        }
        KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
          KJ_FAIL_REQUIRE("modules not supported with mainScript");
        }
        KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
          KJ_FAIL_REQUIRE("modules not supported with mainScript");
        }
      }

      compiledGlobals.add(Worker::Script::CompiledGlobal{
        {lock.v8Isolate, name},
        {lock.v8Isolate, value},
      });
    });
  }

  return compiledGlobals.finish();
}

}  // namespace modules::legacy

}  // namespace workerd
