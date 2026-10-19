// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>

namespace workerd::api::node {

// Dependencies are V8 handles, so circular module graphs do not create C++ Ref cycles.
struct VmModuleRecord {
  jsg::V8Ref<v8::Module> module;
  jsg::V8Ref<v8::Object> callbacks;
  kj::Array<jsg::V8Ref<v8::Module>> dependencies;
  void visitForGc(jsg::GcVisitor& visitor);
};

class VmModule;

class VmContext final: public jsg::Object, public jsg::ContextModuleCallbacks {
 public:
  VmContext(jsg::Lock& js, v8::Local<v8::Context> context);
  // Installs the realm's callbacks and a traced edge back to this resource.
  void attach(jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<VmContext>>& handler);
  jsg::JsObject getGlobal(jsg::Lock& js);
  v8::Local<v8::Context> getContext(jsg::Lock& js);
  // The compiled module's host-options token retains its wrapper and callbacks through
  // an ephemeron; the context's lookup index alone does not keep discarded modules alive.
  void addModule(jsg::Lock& js,
      jsg::Ref<VmModule>& module,
      v8::Local<v8::Symbol> token,
      v8::Local<v8::Object> callbacks,
      const jsg::TypeHandler<jsg::Ref<VmModule>>& handler);
  // Promotes a live matching wrapper; a collected or foreign module returns none.
  kj::Maybe<jsg::Ref<VmModule>> findModule(jsg::Lock& js, v8::Local<v8::Module> module);
  // The script/function must retain token in its host-defined options for callback lifetime.
  void addImportCallback(
      jsg::Lock& js, v8::Local<v8::Symbol> token, v8::Local<v8::Object> callbacks);
  bool initializeImportMeta(v8::Local<v8::Context> context,
      v8::Local<v8::Module> module,
      v8::Local<v8::Object> meta) override;
  kj::Maybe<v8::MaybeLocal<v8::Promise>> importDynamically(v8::Local<v8::Context> context,
      v8::Local<v8::Data> options,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes) override;

  JSG_RESOURCE_TYPE(VmContext) {
    JSG_METHOD(getGlobal);
  }

 private:
  jsg::V8Ref<v8::Context> context;
  kj::Vector<jsg::WeakRef<VmModule>> modules;
  // The token in a script's host-defined options is an ephemeron key. Merely retaining
  // a context must not retain every script, function, or module ever compiled in it.
  jsg::V8Ref<v8::Object> importCallbacks;
  jsg::V8Ref<v8::Function> weakMapGet;
  jsg::V8Ref<v8::Function> weakMapSet;
  void setCallbackRecord(jsg::Lock& js,
      v8::Local<v8::Symbol> token,
      v8::Local<v8::Value> callbacks,
      v8::Local<v8::Value> resource);
  void visitForGc(jsg::GcVisitor& visitor);
};

class VmScript final: public jsg::Object {
 public:
  VmScript(jsg::V8Ref<v8::UnboundScript> script,
      jsg::V8Ref<v8::Symbol> token,
      jsg::V8Ref<v8::Object> callbacks)
      : script(kj::mv(script)),
        token(kj::mv(token)),
        callbacks(kj::mv(callbacks)) {}
  // Binds this unbound script to the supplied realm, or the current realm when omitted.
  jsg::JsValue run(jsg::Lock& js,
      jsg::Optional<jsg::Ref<VmContext>> context,
      const jsg::TypeHandler<jsg::Ref<VmContext>>& handler);
  kj::Array<kj::byte> createCachedData(jsg::Lock& js);
  JSG_RESOURCE_TYPE(VmScript) {
    JSG_METHOD(run);
    JSG_METHOD(createCachedData);
  }

 private:
  jsg::V8Ref<v8::UnboundScript> script;
  jsg::V8Ref<v8::Symbol> token;
  jsg::V8Ref<v8::Object> callbacks;
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(script, token, callbacks);
  }
};

class VmModule final: public jsg::Object {
 public:
  VmModule(jsg::Ref<VmContext> context, VmModuleRecord record)
      : context(kj::mv(context)),
        record(kj::mv(record)) {}
  v8::Local<v8::Module> getModule(jsg::Lock& js);
  // Request order is V8's order and is the index used by setDependencies and resolve.
  jsg::JsArray getRequests(jsg::Lock& js);
  // Must be supplied before instantiation, with every dependency in the same realm.
  void setDependencies(jsg::Lock& js, kj::Array<jsg::Ref<VmModule>> dependencies);
  // Instantiates an already-resolved graph without invoking an asynchronous linker.
  void instantiate(jsg::Lock& js);
  // Returns V8's evaluation promise without draining microtasks or changing request ownership.
  jsg::JsValue evaluate(jsg::Lock& js);
  // Returns V8's live namespace, preserving identity across reads and export updates.
  jsg::JsValue getNamespace(jsg::Lock& js);
  jsg::JsValue getError(jsg::Lock& js);
  int getStatus(jsg::Lock& js);
  void setExport(jsg::Lock& js, jsg::JsString name, jsg::JsValue value);
  static v8::MaybeLocal<v8::Module> resolve(
      v8::Local<v8::Context> context, size_t index, v8::Local<v8::Module> referrer);
  static v8::MaybeLocal<v8::Promise> syntheticEvaluation(
      v8::Local<v8::Context> context, v8::Local<v8::Module> module);

  JSG_RESOURCE_TYPE(VmModule) {
    JSG_METHOD(getRequests);
    JSG_METHOD(setDependencies);
    JSG_METHOD(instantiate);
    JSG_METHOD(evaluate);
    JSG_METHOD(getNamespace);
    JSG_METHOD(getError);
    JSG_METHOD(getStatus);
    JSG_METHOD(setExport);
  }

 private:
  jsg::Ref<VmContext> context;
  VmModuleRecord record;
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(context);
    record.visitForGc(visitor);
  }
  friend class VmContext;
};

class VmUtil final: public jsg::Object {
 public:
  VmUtil() = default;
  VmUtil(jsg::Lock&, const jsg::Url&) {}
  // Without a sandbox this returns an ordinary global suitable for DONT_CONTEXTIFY.
  // The JSG boolean arguments only restrict guest eval/Function and Wasm; they never grant
  // compilation permission beyond the worker isolate's policy.
  jsg::Ref<VmContext> createContext(jsg::Lock& js,
      jsg::Optional<jsg::V8Ref<v8::Object>> sandbox,
      bool strings,
      bool wasm,
      const jsg::TypeHandler<jsg::Ref<VmContext>>& handler);
  // Reuses the current realm's resource so modules without an explicit context can link.
  jsg::Ref<VmContext> mainContext(
      jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<VmContext>>& handler);
  // Compiles without binding to a realm. Dynamic-import callbacks are registered when run.
  jsg::Ref<VmScript> compileScript(jsg::Lock& js,
      jsg::JsString code,
      jsg::JsString filename,
      int lineOffset,
      int columnOffset,
      jsg::V8Ref<v8::Object> callbacks);
  // Compiles in the requested realm with direct references to the supplied scope extensions.
  jsg::JsValue compileFunction(jsg::Lock& js,
      jsg::JsString code,
      kj::Array<jsg::V8Ref<v8::String>> params,
      jsg::JsString filename,
      int lineOffset,
      int columnOffset,
      jsg::Optional<jsg::Ref<VmContext>> context,
      kj::Array<jsg::V8Ref<v8::Object>> extensions,
      jsg::V8Ref<v8::Object> callbacks,
      const jsg::TypeHandler<jsg::Ref<VmContext>>& handler);
  // Creates an unlinked source module; its explicit callbacks never use the worker loader.
  jsg::Ref<VmModule> sourceTextModule(jsg::Lock& js,
      jsg::Ref<VmContext> context,
      jsg::JsString code,
      jsg::JsString identifier,
      int lineOffset,
      int columnOffset,
      jsg::V8Ref<v8::Object> callbacks,
      const jsg::TypeHandler<jsg::Ref<VmModule>>& handler);
  // The evaluate callback publishes initial exports through VmModule::setExport; later
  // setExport calls update every importer through V8's live bindings.
  jsg::Ref<VmModule> syntheticModule(jsg::Lock& js,
      jsg::Ref<VmContext> context,
      kj::Array<jsg::V8Ref<v8::String>> exports,
      jsg::JsString identifier,
      jsg::V8Ref<v8::Object> callbacks,
      const jsg::TypeHandler<jsg::Ref<VmModule>>& handler);

  JSG_RESOURCE_TYPE(VmUtil) {
    JSG_METHOD(createContext);
    JSG_METHOD(mainContext);
    JSG_METHOD(compileScript);
    JSG_METHOD(compileFunction);
    JSG_METHOD(sourceTextModule);
    JSG_METHOD(syntheticModule);
  }
};

#define EW_NODE_VM_ISOLATE_TYPES                                                                   \
  api::node::VmContext, api::node::VmScript, api::node::VmModule, api::node::VmUtil

}  // namespace workerd::api::node
