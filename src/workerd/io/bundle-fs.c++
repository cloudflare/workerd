#include "bundle-fs.h"

namespace workerd {
kj::Rc<Directory> getBundleDirectory(const WorkerSource& conf) {
  // Note that we are using a lazy directory here. That means we won't actually
  // build the directory structure out until it is actually accessed in order
  // to avoid unnecessary operations in the case a worker never actually uses
  // this part of the filesystem.

  // Importantly, the WorkerSource we get here won't be sticking around. Copy each module body into
  // a reference-counted buffer so that ownership follows the bytes through the lazy directory and
  // into the File that ultimately exposes them.
  struct Entry {
    kj::StringPtr name;
    kj::Rc<kj::Array<const kj::byte>> data;
  };
  kj::Vector<Entry> entries;
  auto addEntry = [&entries](kj::StringPtr name, kj::ArrayPtr<const kj::byte> data) {
    entries.add(Entry{
      .name = name,
      .data = kj::rc<kj::Array<const kj::byte>>(kj::heapArray<const kj::byte>(data)),
    });
  };
  KJ_SWITCH_ONEOF(conf.variant) {
    KJ_CASE_ONEOF(script, WorkerSource::ScriptSource) {
      addEntry(script.mainScriptName, script.mainScript.asBytes());
    }
    KJ_CASE_ONEOF(modules, WorkerSource::ModulesSource) {
      for (auto& module: modules.modules) {
        KJ_SWITCH_ONEOF(module.content) {
          KJ_CASE_ONEOF(esModule, WorkerSource::EsModule) {
            addEntry(module.name, esModule.body.asBytes());
          }
          KJ_CASE_ONEOF(commonJsModule, WorkerSource::CommonJsModule) {
            addEntry(module.name, commonJsModule.body.asBytes());
          }
          KJ_CASE_ONEOF(textModule, WorkerSource::TextModule) {
            addEntry(module.name, textModule.body.asBytes());
          }
          KJ_CASE_ONEOF(dataModule, WorkerSource::DataModule) {
            addEntry(module.name, dataModule.body);
          }
          KJ_CASE_ONEOF(wasmModule, WorkerSource::WasmModule) {
            addEntry(module.name, wasmModule.body);
          }
          KJ_CASE_ONEOF(jsonModule, WorkerSource::JsonModule) {
            addEntry(module.name, jsonModule.body.asBytes());
          }
          KJ_CASE_ONEOF(pythonModule, WorkerSource::PythonModule) {
            addEntry(module.name, pythonModule.body.asBytes());
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

  // `mutable` so we can move owned module bytes out of the captured entries into their Files.
  // The closure is invoked at most once: getLazyDirectoryImpl memoizes the produced Directory and
  // discards the closure after the first call.
  return getLazyDirectoryImpl([entries = entries.releaseAsArray()]() mutable {
    Directory::Builder builder;
    kj::Path kRoot{};
    // Defense-in-depth: reject module names whose parsed path exceeds a sane
    // segment count. Legitimate module paths are short (e.g. "src/util/helpers.js");
    // pathologically deep names can never be addressed by node:fs anyway.
    static constexpr size_t kMaxBundlePathDepth = 1024;
    for (auto& entry: entries) {
      auto url = KJ_ASSERT_NONNULL(jsg::Url::tryParse(entry.name, "file:///"_kj));
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
      builder.addPath(path, File::newReadable(kj::mv(entry.data)));
    }
    return builder.finish();
  });
}

}  // namespace workerd
