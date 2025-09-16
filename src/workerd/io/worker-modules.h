#pragma once

#include <workerd/api/modules.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/util/strong-bool.h>

#include <pyodide/python-entrypoint.embed.h>

#include <capnp/schema-loader.h>
#include <capnp/schema.h>

// This header provides utilities for setting up the ModuleRegistry for a worker.
// It is meant to be included in only two places; workerd-api.c++ and the equivalent
// file in the internal repo. It is templated on the TypeWrapper and JsgIsolate types.
namespace workerd {
namespace api {
class ServiceWorkerGlobalScope;
class CommonJsModuleContext;
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
    jsg::modules::ModuleRegistry::Builder::Options options) {
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
          bundleBuilder.addSyntheticModule(
              def.name, jsg::modules::Module::newWasmModuleHandler(content.body));
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
          KJ_REQUIRE(featureFlags.getPythonWorkers(),
              "The python_workers compatibility flag is required to use Python.");
          firstEsm = false;
          hasPythonModules = true;
          kj::StringPtr entry = PYTHON_ENTRYPOINT;
          bundleBuilder.addEsmModule(def.name, entry);
          break;
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

}  // namespace workerd
