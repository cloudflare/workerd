#include "bundle-fs.h"

#include <capnp/message.h>

namespace workerd::server {
kj::Rc<Directory> getBundleDirectory(config::Worker::Reader conf) {
  // Note that we are using a lazy directory here. That means we won't actually
  // build the directory structure out until it is actually accessed in order
  // to avoid unnecessary operations in the case a worker never actually uses
  // this part of the filesystem.
  return getLazyDirectoryImpl([conf] {
    Directory::Builder builder;
    kj::Path kRoot{};

    auto addToBundle = [&](kj::StringPtr name, kj::ArrayPtr<const kj::byte> data) {
      auto url = KJ_ASSERT_NONNULL(jsg::Url::tryParse(name, "file:///"_kj));
      auto pathStr = kj::str(url.getPathname().slice(1));
      auto path = kRoot.eval(pathStr);
      builder.addPath(path, File::newReadable(data));
    };

    switch (conf.which()) {
      case config::Worker::Which::MODULES: {
        for (auto module: conf.getModules()) {
          switch (module.which()) {
            case config::Worker::Module::ES_MODULE: {
              addToBundle(module.getName(), module.getEsModule().asBytes());
              break;
            }
            case config::Worker::Module::COMMON_JS_MODULE: {
              addToBundle(module.getName(), module.getCommonJsModule().asBytes());
              break;
            }
            case config::Worker::Module::TEXT: {
              addToBundle(module.getName(), module.getText().asBytes());
              break;
            }
            case config::Worker::Module::DATA: {
              addToBundle(module.getName(), module.getData().asBytes());
              break;
            }
            case config::Worker::Module::WASM: {
              addToBundle(module.getName(), module.getWasm().asBytes());
              break;
            }
            case config::Worker::Module::JSON: {
              addToBundle(module.getName(), module.getJson().asBytes());
              break;
            }
            case config::Worker::Module::PYTHON_MODULE: {
              addToBundle(module.getName(), module.getPythonModule().asBytes());
              break;
            }
            case config::Worker::Module::PYTHON_REQUIREMENT: {
              // Just ignore it.
              break;
            }
            case config::Worker::Module::OBSOLETE: {
              // Just ignore it.
              break;
            }
          }
        }
        break;
      }
      case config::Worker::Which::SERVICE_WORKER_SCRIPT: {
        addToBundle("worker.js"_kj, conf.getServiceWorkerScript().asBytes());
        break;
      }
      case config::Worker::Which::INHERIT: {
        // Doing nothing here.
        break;
      }
    }

    return builder.finish();
  });
}

}  // namespace workerd::server
