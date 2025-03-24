#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/filesystem.h>

namespace workerd::jsg {

class CommonJsModuleObject final: public jsg::Object {
 public:
  CommonJsModuleObject(jsg::Lock& js, kj::String path);

  v8::Local<v8::Value> getExports(jsg::Lock& js) const;
  void setExports(jsg::Value value);
  kj::StringPtr getPath() const;

  JSG_RESOURCE_TYPE(CommonJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(path, getPath);
  }

  void visitForMemoryInfo(MemoryTracker& tracker) const;

 private:
  jsg::Value exports;
  kj::String path;
};

class CommonJsModuleContext final: public jsg::Object {
 public:
  CommonJsModuleContext(jsg::Lock& js, kj::Path path);

  v8::Local<v8::Value> require(jsg::Lock& js, kj::String specifier);

  jsg::Ref<CommonJsModuleObject> getModule(jsg::Lock& js);

  v8::Local<v8::Value> getExports(jsg::Lock& js) const;
  void setExports(jsg::Value value);

  kj::String getFilename() const;
  kj::String getDirname() const;

  JSG_RESOURCE_TYPE(CommonJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_INSTANCE_PROPERTY(__filename, getFilename);
    JSG_LAZY_INSTANCE_PROPERTY(__dirname, getDirname);
  }

  jsg::Ref<CommonJsModuleObject> module;

  void visitForMemoryInfo(MemoryTracker& tracker) const;

 private:
  kj::Path path;
  jsg::Value exports;
};

}  // namespace workerd::jsg
