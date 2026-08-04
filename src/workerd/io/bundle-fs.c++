#include "bundle-fs.h"

namespace workerd {
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
    // When the module body is owned by the (transient) WorkerSource -- as is the case for
    // TypeScript transpiled at load time, where the body lives in EsModule::ownBody -- we must copy
    // it here, because the WorkerSource (and thus ownBody) is destroyed once worker setup completes.
    // At materialization this owned copy is handed to the File itself (File::newReadable(kj::Array)),
    // so the bytes outlive both the source and the lazy closure -- note the closure that holds these
    // entries is itself destroyed after the directory is first materialized (see
    // LazyDirectory::getDirectory), so a non-owning File would still dangle. When kj::none, `data`
    // points into memory the caller guarantees outlives the directory (process-lifetime capnp
    // buffers or a retained source clone). See VULN-136997 and the matching copy in
    // worker-modules.h.
    kj::Maybe<kj::Array<const kj::byte>> ownedData;
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
            Entry entry{
              .name = module.name,
              .data = esModule.body.asBytes(),
            };
            // If the body was transpiled at load time it is owned by the transient WorkerSource;
            // copy it so the lazy directory does not read freed memory after the source is
            // destroyed (VULN-136997).
            if (esModule.ownBody != kj::none) {
              auto owned = kj::heapArray<const kj::byte>(esModule.body.asBytes());
              entry.data = owned.asPtr();
              entry.ownedData = kj::mv(owned);
            }
            entries.add(kj::mv(entry));
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

  // `mutable` so we can move the owned module bytes out of the captured entries into their Files
  // below. This is safe because the closure is invoked at most once: getLazyDirectoryImpl memoizes
  // the produced Directory and discards the closure after the first call.
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
      KJ_IF_SOME(owned, entry.ownedData) {
        // The bytes are owned by the transient WorkerSource; transfer ownership to the File so
        // they survive both the source's destruction and this closure's (VULN-136997).
        builder.addPath(path, File::newReadable(kj::mv(owned)));
      } else {
        builder.addPath(path, File::newReadable(entry.data));
      }
    }
    return builder.finish();
  });
}

}  // namespace workerd
