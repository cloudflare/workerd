#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

#include <kj/filesystem.h>

namespace workerd::api {

class CommonJsModuleObject final: public jsg::Object {
 public:
  CommonJsModuleObject(jsg::Lock& js, kj::String path);

  jsg::JsValue getExports(jsg::Lock& js) const;
  void setExports(jsg::Lock& js, jsg::JsValue value);
  kj::StringPtr getPath() const;

  JSG_RESOURCE_TYPE(CommonJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(path, getPath);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  jsg::JsRef<jsg::JsValue> exports;
  kj::String path;
};

class CommonJsModuleContext final: public jsg::Object {
 public:
  CommonJsModuleContext(jsg::Lock& js, kj::Path path);
  CommonJsModuleContext(jsg::Lock& js, const jsg::Url& url);

  jsg::JsValue require(jsg::Lock& js, kj::String specifier);

  jsg::Ref<CommonJsModuleObject> getModule(jsg::Lock& js);

  jsg::JsValue getExports(jsg::Lock& js) const;
  void setExports(jsg::Lock& js, jsg::JsValue value);

  kj::String getFilename() const;
  kj::String getDirname() const;

  jsg::JsValue getModuleExports(jsg::Lock& js) {
    return getModule(js)->getExports(js);
  }

  JSG_RESOURCE_TYPE(CommonJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_INSTANCE_PROPERTY(__filename, getFilename);
    JSG_LAZY_INSTANCE_PROPERTY(__dirname, getDirname);
  }

  jsg::Ref<CommonJsModuleObject> module;

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // If pathOrSpecifier is a path, then we're using the old module registry
  // implementation. If it is a jsg::Url, then we are using the new module
  // registry implementation.
  kj::OneOf<kj::Path, jsg::Url> pathOrSpecifier;
  jsg::JsRef<jsg::JsValue> exports;
};

// Used with the original module registry implementation.
template <typename LockType>
struct CommonJsImpl: public jsg::ModuleRegistry::CommonJsModuleInfo::CommonJsModuleProvider {
  jsg::Ref<api::CommonJsModuleContext> context;
  CommonJsImpl(jsg::Lock& js, kj::Path path)
      : context(js.alloc<api::CommonJsModuleContext>(js, kj::mv(path))) {}
  KJ_DISALLOW_COPY_AND_MOVE(CommonJsImpl);
  jsg::JsObject getContext(jsg::Lock& js) override {
    auto& lock = kj::downcast<LockType>(js);
    return jsg::JsObject(lock.wrap(js.v8Context(), context.addRef()));
  }
  jsg::JsValue getExports(jsg::Lock& js) override {
    return jsg::JsValue(context->getModule(js)->getExports(js));
  }
};

#define EW_CJS_ISOLATE_TYPES api::CommonJsModuleObject, api::CommonJsModuleContext

}  // namespace workerd::api
