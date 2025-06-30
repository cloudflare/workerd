// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "process.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>

#include <kj/vector.h>

namespace workerd::api::node {

jsg::JsValue ProcessModule::getBuiltinModule(jsg::Lock& js, kj::String specifier) {
  auto rawSpecifier = kj::str(specifier);
  bool isNode = false;
  KJ_IF_SOME(spec, jsg::checkNodeSpecifier(specifier)) {
    isNode = true;
    specifier = kj::mv(spec);
  }

  if (FeatureFlags::get(js).getNewModuleRegistry()) {
    KJ_IF_SOME(mod, js.resolveInternalModule(specifier)) {
      return mod;
    }
    return js.undefined();
  }

  auto registry = jsg::ModuleRegistry::from(js);
  if (registry == nullptr) return js.undefined();
  auto path = kj::Path::parse(specifier);

  KJ_IF_SOME(info,
      registry->resolve(js, path, kj::none, jsg::ModuleRegistry::ResolveOption::BUILTIN_ONLY,
          jsg::ModuleRegistry::ResolveMethod::IMPORT, rawSpecifier.asPtr())) {
    auto module = info.module.getHandle(js);
    jsg::instantiateModule(js, module);

    // For Node.js modules, we want to grab the default export and return that.
    // For other built-ins, we'll return the module namespace instead. Can be
    // a bit confusing but it's a side effect of Node.js modules originally
    // being commonjs and the official getBuiltinModule returning what is
    // expected to be the default export, while the behavior of other built-ins
    // is not really defined by Node.js' implementation.
    if (isNode) {
      return jsg::JsValue(js.v8Get(module->GetModuleNamespace().As<v8::Object>(), "default"_kj));
    } else {
      return jsg::JsValue(module->GetModuleNamespace());
    }
  }

  return js.undefined();
}

jsg::JsObject ProcessModule::getEnvObject(jsg::Lock& js) {
  if (FeatureFlags::get(js).getPopulateProcessEnv()) {
    KJ_IF_SOME(env, js.getWorkerEnv()) {
      return jsg::JsObject(env.getHandle(js));
    }
  }

  // Default to empty object.
  return js.obj();
}

namespace {
[[noreturn]] void handleProcessExit(jsg::Lock& js, int code) {
  // There are a few things happening here. First, we abort the current IoContext
  // in order to shut down this specific request....
  auto message =
      kj::str("The Node.js process.exit(", code, ") API was called. Canceling the request.");
  auto& ioContext = IoContext::current();
  // If we have a tail worker, let's report the error.
  KJ_IF_SOME(tracer, ioContext.getWorkerTracer()) {
    // Why create the error like this in tracing? Because we're adding the exception
    // to the trace and ideally we'd have the JS stack attached to it. Just using
    // JSG_KJ_EXCEPTION would not give us that, and we only want to incur the cost
    // of creating and capturing the stack when we actually need it.
    auto ex = KJ_ASSERT_NONNULL(js.error(message).tryCast<jsg::JsObject>());
    tracer.addException(ioContext.getInvocationSpanContext(), ioContext.now(),
        ex.get(js, "name"_kj).toString(js), ex.get(js, "message"_kj).toString(js),
        ex.get(js, "stack"_kj).toString(js));
    ioContext.abort(js.exceptionToKj(ex));
  } else {
    ioContext.abort(JSG_KJ_EXCEPTION(FAILED, Error, kj::mv(message)));
  }
  // ...then we tell the isolate to terminate the current JavaScript execution.
  js.terminateExecutionNow();
}
}  // namespace

jsg::JsObject ProcessModule::getVersions(jsg::Lock& js) const {
  auto versions = js.obj();
  // Node.js version - represents the most current Node.js version supported
  // by the platform, as defined in node-version.h
  versions.set(js, "node"_kj, js.str(nodeVersion));
  return versions;
}

void ProcessModule::exitImpl(jsg::Lock& js, int code) {
  if (IoContext::hasCurrent()) {
    handleProcessExit(js, code);
  }

  // Create an error object so we can easily capture the stack where the
  // process.exit call was made.
  auto err = KJ_ASSERT_NONNULL(
      js.error("process.exit(...) called without a current request context. Ignoring.")
          .tryCast<jsg::JsObject>());
  err.set(js, "name"_kj, js.str());
  js.logWarning(kj::str(err.get(js, "stack"_kj)));
}

}  // namespace workerd::api::node
