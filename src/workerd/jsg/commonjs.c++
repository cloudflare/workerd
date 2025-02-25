#include "commonjs.h"

#include "jsvalue.h"
#include "modules.h"
#include "resource.h"

namespace workerd::jsg {

CommonJsModuleContext::CommonJsModuleContext(jsg::Lock& js, kj::Path path)
    : module(jsg::alloc<CommonJsModuleObject>(js, path.toString(true))),
      path(kj::mv(path)),
      exports(js.v8Isolate, module->getExports(js)) {}

v8::Local<v8::Value> CommonJsModuleContext::require(jsg::Lock& js, kj::String specifier) {
  auto modulesForResolveCallback = getModulesForResolveCallback(js.v8Isolate);
  KJ_REQUIRE(modulesForResolveCallback != nullptr, "didn't expect resolveCallback() now");

  if (isNodeJsCompatEnabled(js)) {
    KJ_IF_SOME(nodeSpec, checkNodeSpecifier(specifier)) {
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
  auto& info = JSG_REQUIRE_NONNULL(modulesForResolveCallback->resolve(js, targetPath, path,
                                       ModuleRegistry::ResolveOption::DEFAULT,
                                       ModuleRegistry::ResolveMethod::REQUIRE, specifier.asPtr()),
      Error, "No such module \"", targetPath.toString(), "\".");
  // Adding imported from suffix here not necessary like it is for resolveCallback, since we have a
  // js stack that will include the parent module's name and location of the failed require().

  ModuleRegistry::RequireImplOptions options = ModuleRegistry::RequireImplOptions::DEFAULT;
  if (getCommonJsExportDefault(js.v8Isolate)) {
    options = ModuleRegistry::RequireImplOptions::EXPORT_DEFAULT;
  }

  return ModuleRegistry::requireImpl(js, info, options);
}

void CommonJsModuleContext::visitForMemoryInfo(MemoryTracker& tracker) const {
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

void CommonJsModuleObject::visitForMemoryInfo(MemoryTracker& tracker) const {
  tracker.trackField("exports", exports);
  tracker.trackField("path", path);
}

// ======================================================================================

NodeJsModuleContext::NodeJsModuleContext(jsg::Lock& js, kj::Path path)
    : module(jsg::alloc<NodeJsModuleObject>(js, path.toString(true))),
      path(kj::mv(path)),
      exports(js.v8Ref(module->getExports(js))) {}

v8::Local<v8::Value> NodeJsModuleContext::require(jsg::Lock& js, kj::String specifier) {
  // If it is a bare specifier known to be a Node.js built-in, then prefix the
  // specifier with node:
  bool isNodeBuiltin = false;
  auto resolveOption = jsg::ModuleRegistry::ResolveOption::DEFAULT;
  KJ_IF_SOME(spec, checkNodeSpecifier(specifier)) {
    specifier = kj::mv(spec);
    isNodeBuiltin = true;
    resolveOption = jsg::ModuleRegistry::ResolveOption::BUILTIN_ONLY;
  }

  // TODO(cleanup): This implementation from here on is identical to the
  // CommonJsModuleContext::require. We should consolidate these as the
  // next step.

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
      JSG_REQUIRE_NONNULL(modulesForResolveCallback->resolve(js, targetPath, path, resolveOption,
                              ModuleRegistry::ResolveMethod::REQUIRE, specifier.asPtr()),
          Error, "No such module \"", targetPath.toString(), "\".");
  // Adding imported from suffix here not necessary like it is for resolveCallback, since we have a
  // js stack that will include the parent module's name and location of the failed require().

  if (!isNodeBuiltin) {
    JSG_REQUIRE_NONNULL(
        info.maybeSynthetic, TypeError, "Cannot use require() to import an ES Module.");
  }

  return ModuleRegistry::requireImpl(js, info, ModuleRegistry::RequireImplOptions::EXPORT_DEFAULT);
}

v8::Local<v8::Value> NodeJsModuleContext::getBuffer(jsg::Lock& js) {
  auto value = require(js, kj::str("node:buffer"));
  JSG_REQUIRE(value->IsObject(), TypeError, "Invalid node:buffer implementation");
  auto module = value.As<v8::Object>();
  auto buffer = js.v8Get(module, "Buffer"_kj);
  JSG_REQUIRE(buffer->IsFunction(), TypeError, "Invalid node:buffer implementation");
  return buffer;
}

v8::Local<v8::Value> NodeJsModuleContext::getProcess(jsg::Lock& js) {
  auto value = require(js, kj::str("node:process"));
  JSG_REQUIRE(value->IsObject(), TypeError, "Invalid node:process implementation");
  return value;
}

kj::String NodeJsModuleContext::getFilename() {
  return path.toString(true);
}

kj::String NodeJsModuleContext::getDirname() {
  return path.parent().toString(true);
}

jsg::Ref<NodeJsModuleObject> NodeJsModuleContext::getModule(jsg::Lock& js) {
  return module.addRef();
}

v8::Local<v8::Value> NodeJsModuleContext::getExports(jsg::Lock& js) {
  return exports.getHandle(js);
}

void NodeJsModuleContext::setExports(jsg::Value value) {
  exports = kj::mv(value);
}

NodeJsModuleObject::NodeJsModuleObject(jsg::Lock& js, kj::String path)
    : exports(js.v8Isolate, v8::Object::New(js.v8Isolate)),
      path(kj::mv(path)) {}

v8::Local<v8::Value> NodeJsModuleObject::getExports(jsg::Lock& js) {
  return exports.getHandle(js);
}

void NodeJsModuleObject::setExports(jsg::Value value) {
  exports = kj::mv(value);
}

kj::StringPtr NodeJsModuleObject::getPath() {
  return path;
}

}  // namespace workerd::jsg
