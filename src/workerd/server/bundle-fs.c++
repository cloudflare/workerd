#include "bundle-fs.h"

#include <capnp/message.h>

namespace workerd::server {
kj::Rc<Directory> getBundleDirectory(const WorkerSource& conf) {
  // Note that we are using a lazy directory here. That means we won't actually
  // build the directory structure out until it is actually accessed in order
  // to avoid unnecessary operations in the case a worker never actually uses
  // this part of the filesystem.

  // Importantly, the WorkerSource we get here won't be sticking around.
  // We need to copy the details we need out of it now...Critically, however,
  // the caller needs to arrange to keep the original source alive for the
  // lifetime of the directory since the directory only contains pointers.
  struct Entry {
    kj::StringPtr name;
    kj::ArrayPtr<const kj::byte> data;
  };
  kj::Vector<Entry> entries;
  KJ_SWITCH_ONEOF(conf.variant) {
    KJ_CASE_ONEOF(script, WorkerSource::ScriptSource) {
      entries.add(Entry{
        .name = script.mainScriptName,
        .data = script.mainScript.asBytes(),
      });
    }
    KJ_CASE_ONEOF(modules, WorkerSource::ModulesSource) {
      for (auto& module: modules.modules) {
        KJ_SWITCH_ONEOF(module.content) {
          KJ_CASE_ONEOF(esModule, WorkerSource::EsModule) {
            entries.add(Entry{
              .name = module.name,
              .data = esModule.body.asBytes(),
            });
          }
          KJ_CASE_ONEOF(commonJsModule, WorkerSource::CommonJsModule) {
            entries.add(Entry{
              .name = module.name,
              .data = commonJsModule.body.asBytes(),
            });
          }
          KJ_CASE_ONEOF(textModule, WorkerSource::TextModule) {
            entries.add(Entry{
              .name = module.name,
              .data = textModule.body.asBytes(),
            });
          }
          KJ_CASE_ONEOF(dataModule, WorkerSource::DataModule) {
            entries.add(Entry{
              .name = module.name,
              .data = dataModule.body,
            });
          }
          KJ_CASE_ONEOF(wasmModule, WorkerSource::WasmModule) {
            entries.add(Entry{
              .name = module.name,
              .data = wasmModule.body,
            });
          }
          KJ_CASE_ONEOF(jsonModule, WorkerSource::JsonModule) {
            entries.add(Entry{
              .name = module.name,
              .data = jsonModule.body.asBytes(),
            });
          }
          KJ_CASE_ONEOF(pythonModule, WorkerSource::PythonModule) {
            entries.add(Entry{
              .name = module.name,
              .data = pythonModule.body.asBytes(),
            });
          }
          KJ_CASE_ONEOF(pythonRequirement, WorkerSource::PythonRequirement) {
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

  return getLazyDirectoryImpl([entries = entries.releaseAsArray()] {
    Directory::Builder builder;
    kj::Path kRoot{};
    for (auto& entry: entries) {
      auto url = KJ_ASSERT_NONNULL(jsg::Url::tryParse(entry.name, "file:///"_kj));
      auto pathStr = kj::str(url.getPathname().slice(1));
      auto path = kRoot.eval(pathStr);
      builder.addPath(path, File::newReadable(entry.data));
    }
    return builder.finish();
  });
}

}  // namespace workerd::server
