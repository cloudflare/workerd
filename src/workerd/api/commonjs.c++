#include "commonjs.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/resource.h>

namespace workerd::api {

CommonJsModuleContext::CommonJsModuleContext(jsg::Lock& js, kj::Path path)
    : module(js.alloc<CommonJsModuleObject>(js, path.toString(true))),
      pathOrSpecifier(kj::mv(path)),
      exports(js, module->getExports(js)) {}

CommonJsModuleContext::CommonJsModuleContext(jsg::Lock& js, const jsg::Url& specifier)
    : module(js.alloc<CommonJsModuleObject>(js, kj::str(specifier.getHref()))),
      pathOrSpecifier(specifier.clone()),
      exports(js, module->getExports(js)) {}

jsg::JsValue CommonJsModuleContext::require(jsg::Lock& js, kj::String specifier) {
  if (isNodeJsCompatEnabled(js)) {
    KJ_IF_SOME(nodeSpec, jsg::checkNodeSpecifier(specifier)) {
      specifier = kj::mv(nodeSpec);
    }
  }

  if (FeatureFlags::get(js).getNewModuleRegistry()) {
    return jsg::modules::ModuleRegistry::resolve(js, specifier, "default"_kj,
        jsg::modules::ResolveContext::Type::BUNDLE, jsg::modules::ResolveContext::Source::REQUIRE,
        KJ_ASSERT_NONNULL(pathOrSpecifier.tryGet<jsg::Url>()));
  }

  auto& path = KJ_ASSERT_NONNULL(pathOrSpecifier.tryGet<kj::Path>());

  auto modulesForResolveCallback = jsg::getModulesForResolveCallback(js.v8Isolate);
  KJ_REQUIRE(modulesForResolveCallback != nullptr, "didn't expect resolveCallback() now");

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
  KJ_SWITCH_ONEOF(pathOrSpecifier) {
    KJ_CASE_ONEOF(path, kj::Path) {
      tracker.trackFieldWithSize("path", path.size());
    }
    KJ_CASE_ONEOF(specifier, jsg::Url) {
      tracker.trackField("specifier", specifier);
    }
  }
}

kj::String CommonJsModuleContext::getFilename() const {
  KJ_SWITCH_ONEOF(pathOrSpecifier) {
    KJ_CASE_ONEOF(path, kj::Path) {
      return path.toString(true);
    }
    KJ_CASE_ONEOF(specifier, jsg::Url) {
      // The specifier is a URL. We want to parse it as a path and
      // return just the filename portion.
      // TODO(soon): kj::Path::parse() requires a kj::StringPtr but
      // the path name here is a kj::ArrayPtr<const char>. We can
      // avoid an extraneous copy here by updating kj::Path::parse
      // to also accept a kj::ArrayPtr<const char>.
      auto path = kj::str(specifier.getPathname().slice(1));
      auto filename = kj::Path::parse(path).basename();
      return filename.toString(false);
    }
  }
  KJ_UNREACHABLE;
}

kj::String CommonJsModuleContext::getDirname() const {
  KJ_SWITCH_ONEOF(pathOrSpecifier) {
    KJ_CASE_ONEOF(path, kj::Path) {
      return path.parent().toString(true);
    }
    KJ_CASE_ONEOF(specifier, jsg::Url) {
      // The specifier is a URL. We want to parse it as a path and
      // return just the directory portion.
      auto path = kj::str(specifier.getPathname().slice(1));
      auto pathObj = kj::Path::parse(path);
      return pathObj.parent().toString(true);
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<CommonJsModuleObject> CommonJsModuleContext::getModule(jsg::Lock& js) {
  return module.addRef();
}

jsg::JsValue CommonJsModuleContext::getExports(jsg::Lock& js) const {
  return exports.getHandle(js);
}
void CommonJsModuleContext::setExports(jsg::Lock& js, jsg::JsValue value) {
  exports = jsg::JsRef(js, value);
}

CommonJsModuleObject::CommonJsModuleObject(jsg::Lock& js, kj::String path)
    : exports(js, js.obj()),
      path(kj::mv(path)) {}

jsg::JsValue CommonJsModuleObject::getExports(jsg::Lock& js) const {
  return exports.getHandle(js);
}
void CommonJsModuleObject::setExports(jsg::Lock& js, jsg::JsValue value) {
  exports = jsg::JsRef(js, value);
}

kj::StringPtr CommonJsModuleObject::getPath() const {
  return path;
}

void CommonJsModuleObject::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("exports", exports);
  tracker.trackField("path", path);
}
}  // namespace workerd::api
