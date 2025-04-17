#include "commonjs.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/resource.h>

namespace workerd::api {

CommonJsModuleContext::CommonJsModuleContext(jsg::Lock& js, kj::Path path)
    : module(js.alloc<CommonJsModuleObject>(js, path.toString(true))),
      path(kj::mv(path)),
      exports(js.v8Isolate, module->getExports(js)) {}

v8::Local<v8::Value> CommonJsModuleContext::require(jsg::Lock& js, kj::String specifier) {
  auto modulesForResolveCallback = jsg::getModulesForResolveCallback(js.v8Isolate);
  KJ_REQUIRE(modulesForResolveCallback != nullptr, "didn't expect resolveCallback() now");

  if (isNodeJsCompatEnabled(js)) {
    KJ_IF_SOME(nodeSpec, jsg::checkNodeSpecifier(specifier)) {
      specifier = kj::mv(nodeSpec);
    }
  }

  kj::Path targetPath = ([&] {
    // If the specifier begins with one of our known prefixes, let's not resolve
    // it against the referrer.
    if (specifier.startsWith("node:") || specifier.startsWith("cloudflare:") ||
        specifier.startsWith("workerd:")) {
      return kj::Path::parse(specifier);
    }
    return path.parent().eval(specifier);
  })();

  // require() is only exposed to worker bundle modules so the resolve here is only
  // permitted to require worker bundle or built-in modules. Internal modules are
  // excluded.
  auto& info =
      JSG_REQUIRE_NONNULL(modulesForResolveCallback->resolve(js, targetPath, path,
                              jsg::ModuleRegistry::ResolveOption::DEFAULT,
                              jsg::ModuleRegistry::ResolveMethod::REQUIRE, specifier.asPtr()),
          Error, "No such module \"", targetPath.toString(), "\".");
  // Adding imported from suffix here not necessary like it is for resolveCallback, since we have a
  // js stack that will include the parent module's name and location of the failed require().

  auto options = jsg::ModuleRegistry::RequireImplOptions::DEFAULT;
  if (FeatureFlags::get(js).getExportCommonJsDefaultNamespace()) {
    options = jsg::ModuleRegistry::RequireImplOptions::EXPORT_DEFAULT;
  }

  return jsg::ModuleRegistry::requireImpl(js, info, options);
}

void CommonJsModuleContext::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("exports", exports);
  tracker.trackFieldWithSize("path", path.size());
}

kj::String CommonJsModuleContext::getFilename() const {
  return path.toString(true);
}

kj::String CommonJsModuleContext::getDirname() const {
  return path.parent().toString(true);
}

jsg::Ref<CommonJsModuleObject> CommonJsModuleContext::getModule(jsg::Lock& js) {
  return module.addRef();
}

v8::Local<v8::Value> CommonJsModuleContext::getExports(jsg::Lock& js) const {
  return exports.getHandle(js);
}
void CommonJsModuleContext::setExports(jsg::Value value) {
  exports = kj::mv(value);
}

CommonJsModuleObject::CommonJsModuleObject(jsg::Lock& js, kj::String path)
    : exports(js.v8Isolate, v8::Object::New(js.v8Isolate)),
      path(kj::mv(path)) {}

v8::Local<v8::Value> CommonJsModuleObject::getExports(jsg::Lock& js) const {
  return exports.getHandle(js);
}
void CommonJsModuleObject::setExports(jsg::Value value) {
  exports = kj::mv(value);
}

kj::StringPtr CommonJsModuleObject::getPath() const {
  return path;
}

void CommonJsModuleObject::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("exports", exports);
  tracker.trackField("path", path);
}
}  // namespace workerd::api
