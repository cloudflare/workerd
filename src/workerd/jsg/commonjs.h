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

// ======================================================================================

// TODO(cleanup): Ideally these would exist over with the rest of the Node.js
// compat related stuff in workerd/api/node but there's a dependency cycle issue
// to work through there. Specifically, these are needed in jsg but jsg cannot
// depend on workerd/api. We should revisit to see if we can get these moved over.

// The NodeJsModuleContext is used in support of the NodeJsCompatModule type.
// It adds additional extensions to the global context that would normally be
// expected within the global scope of a Node.js compatible module (such as
// Buffer and process).

// TODO(cleanup): There's a fair amount of duplicated code between the CommonJsModule
// and NodeJsModule types... should be deduplicated.
class NodeJsModuleObject: public jsg::Object {
 public:
  NodeJsModuleObject(jsg::Lock& js, kj::String path);

  v8::Local<v8::Value> getExports(jsg::Lock& js);
  void setExports(jsg::Value value);
  kj::StringPtr getPath();

  // TODO(soon): Additional properties... We can likely get by without implementing most
  // of these (if any).
  // * children https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulechildren
  // * filename https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulefilename
  // * id https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleid
  // * isPreloading https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleispreloading
  // * loaded https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleloaded
  // * parent https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleparent
  // * paths https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulepaths
  // * require https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulerequireid

  JSG_RESOURCE_TYPE(NodeJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_READONLY_INSTANCE_PROPERTY(path, getPath);
  }

  void visitForMemoryInfo(MemoryTracker& tracker) const {
    tracker.trackField("exports", exports);
    tracker.trackField("path", path);
  }

 private:
  jsg::Value exports;
  kj::String path;
};

// The NodeJsModuleContext is similar in structure to CommonJsModuleContext
// with the exception that:
// (a) Node.js-compat built-in modules can be required without the `node:` specifier-prefix
//     (meaning that worker-bundle modules whose names conflict with the Node.js built-ins
//     are ignored), and
// (b) The common Node.js globals that we implement are exposed. For instance, `process`
//     and `Buffer` will be found at the global scope.
class NodeJsModuleContext: public jsg::Object {
 public:
  NodeJsModuleContext(jsg::Lock& js, kj::Path path);

  v8::Local<v8::Value> require(jsg::Lock& js, kj::String specifier);

  // TODO(soon): Implement setImmediate/clearImmediate

  jsg::Ref<NodeJsModuleObject> getModule(jsg::Lock& js);

  v8::Local<v8::Value> getExports(jsg::Lock& js);
  void setExports(jsg::Value value);

  kj::String getFilename();
  kj::String getDirname();

  JSG_RESOURCE_TYPE(NodeJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_INSTANCE_PROPERTY(__filename, getFilename);
    JSG_LAZY_INSTANCE_PROPERTY(__dirname, getDirname);
  }

  jsg::Ref<NodeJsModuleObject> module;

  void visitForMemoryInfo(MemoryTracker& tracker) const {
    tracker.trackField("exports", exports);
    tracker.trackFieldWithSize("path", path.size());
  }

 private:
  kj::Path path;
  jsg::Value exports;
};

}  // namespace workerd::jsg
