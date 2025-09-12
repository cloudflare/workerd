#pragma once

#include <workerd/api/commonjs.h>
#include <workerd/api/modules.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/util/strong-bool.h>

#include <pyodide/python-entrypoint.embed.h>

namespace workerd {

WD_STRONG_BOOL(IsPythonWorker);

// Creates an instance of the (new) ModuleRegistry. This method provides the
// initialization logic that is agnostic to the Worker::Api implementation,
// but accepts a callback parameter to handle the Worker::Api-specific details.
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
          auto& schemaLoader = builder.getSchemaLoader();
          auto schema = schemaLoader.get(content.typeId);
          kj::Vector<kj::String> exports;
          for (auto nested: schema.getProto().getNestedNodes()) {
            auto child = schemaLoader.get(nested.getId());
            switch (child.getProto().which()) {
              case capnp::schema::Node::FILE:
              case capnp::schema::Node::STRUCT:
              case capnp::schema::Node::INTERFACE: {
                exports.add(kj::str(nested.getName()));
                break;
              }
              case capnp::schema::Node::ENUM:
              case capnp::schema::Node::CONST:
              case capnp::schema::Node::ANNOTATION:
                // These kinds are not implemented and cannot contain further nested scopes, so
                // don't generate anything at all for now.
                break;
            }
          }

          bundleBuilder.addSyntheticModule(def.name,
              [typeId = content.typeId](jsg::Lock& js, const jsg::Url&,
                  const jsg::modules::Module::ModuleNamespace& ns,
                  const jsg::CompilationObserver& observer) {
            const capnp::SchemaLoader& schemaLoader =
                js.getCapnpSchemaLoader<api::ServiceWorkerGlobalScope>();
            KJ_IF_SOME(schema, schemaLoader.tryGet(typeId)) {
              return js.tryCatch([&] {
                auto& typeWrapper = TypeWrapper::from(js.v8Isolate);
                ns.setDefault(js,
                    jsg::JsValue(typeWrapper.wrap(js, js.v8Context(), kj::none, schema)
                                     .template As<v8::Value>()));
                for (auto nested: schema.getProto().getNestedNodes()) {
                  KJ_IF_SOME(child, schemaLoader.tryGet(nested.getId())) {
                    switch (child.getProto().which()) {
                      case capnp::schema::Node::FILE:
                      case capnp::schema::Node::STRUCT:
                      case capnp::schema::Node::INTERFACE: {
                        ns.set(js, nested.getName(),
                            jsg::JsValue(typeWrapper.wrap(js, js.v8Context(), kj::none, child)
                                             .template As<v8::Value>()));
                        break;
                      }
                      case capnp::schema::Node::ENUM:
                      case capnp::schema::Node::CONST:
                      case capnp::schema::Node::ANNOTATION:
                        // These kinds are not implemented and cannot contain further nested scopes, so
                        // don't generate anything at all for now.
                        break;
                    }
                  } else {
                    js.v8Isolate->ThrowException(
                        js.typeError("Invalid or unknown capnp module type identifier"));
                    return false;
                  }
                }
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
