#include "bundle-fs.h"

#include <workerd/util/arc-view.h>

namespace workerd {
kj::Rc<Directory> getBundleDirectory(const WorkerSource& conf) {
  // Note that we are using a lazy directory here. That means we won't actually
  // build the directory structure out until it is actually accessed in order
  // to avoid unnecessary operations in the case a worker never actually uses
  // this part of the filesystem.

  // Each entry shares ownership of its bytes with the WorkerSource, so the directory may be
  // materialized long after the source itself is gone.
  struct Entry {
    kj::Arc<kj::StringPtr> name;
    kj::Arc<kj::ArrayPtr<const kj::byte>> data;
  };
  kj::Vector<Entry> entries;
  auto add = [&](const kj::Arc<kj::StringPtr>& name, kj::Arc<kj::ArrayPtr<const kj::byte>> data) {
    entries.add(Entry{.name = name.addRef(), .data = kj::mv(data)});
  };
  KJ_SWITCH_ONEOF(conf.variant) {
    KJ_CASE_ONEOF(script, WorkerSource::ScriptSource) {
      add(script.mainScriptName, asByteView(script.mainScript.addRef()));
    }
    KJ_CASE_ONEOF(modules, WorkerSource::ModulesSource) {
      for (auto& module: *modules.modules) {
        KJ_SWITCH_ONEOF(module.content) {
          KJ_CASE_ONEOF(esModule, WorkerSource::EsModule) {
            add(module.name, asByteView(esModule.body.addRef()));
          }
          KJ_CASE_ONEOF(commonJsModule, WorkerSource::CommonJsModule) {
            add(module.name, asByteView(commonJsModule.body.addRef()));
          }
          KJ_CASE_ONEOF(textModule, WorkerSource::TextModule) {
            add(module.name, asByteView(textModule.body.addRef()));
          }
          KJ_CASE_ONEOF(dataModule, WorkerSource::DataModule) {
            add(module.name, dataModule.body.addRef());
          }
          KJ_CASE_ONEOF(wasmModule, WorkerSource::WasmModule) {
            add(module.name, wasmModule.body.addRef());
          }
          KJ_CASE_ONEOF(jsonModule, WorkerSource::JsonModule) {
            add(module.name, asByteView(jsonModule.body.addRef()));
          }
          KJ_CASE_ONEOF(pythonModule, WorkerSource::PythonModule) {
            add(module.name, asByteView(pythonModule.body.addRef()));
          }
          KJ_CASE_ONEOF(pythonRequirement, WorkerSource::ObsoletePythonRequirement) {
            // Just ignore it.
          }
          KJ_CASE_ONEOF(capnpModule, WorkerSource::CapnpModule) {
            // Capnp modules are not supported in the bundle.
            // Just ignore it.
          }
        }
      }
    }
  }

  return getLazyDirectoryImpl([entries = entries.releaseAsArray()]() {
    Directory::Builder builder;
    kj::Path kRoot{};
    // Defense-in-depth: reject module names whose parsed path exceeds a sane
    // segment count. Legitimate module paths are short (e.g. "src/util/helpers.js");
    // pathologically deep names can never be addressed by node:fs anyway.
    static constexpr size_t kMaxBundlePathDepth = 1024;
    for (auto& entry: entries) {
      auto url = KJ_ASSERT_NONNULL(jsg::Url::tryParse(*entry.name, "file:///"_kj));
      // If the name is not a valid file URL path, ignore it.
      if (url.getProtocol() != "file:"_kj) {
        continue;
      }
      auto pathStr = kj::str(url.getPathname().slice(1));
      auto path = kRoot.eval(pathStr);
      if (path.size() > kMaxBundlePathDepth) {
        KJ_LOG(WARNING, "Skipping overly deep module path", path.size());
        continue;
      }
      builder.addPath(path, File::newReadable(entry.data.addRef()));
    }
    return builder.finish();
  });
}

}  // namespace workerd
