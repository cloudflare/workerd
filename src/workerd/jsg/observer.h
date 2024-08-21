// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/string.h>
#include <kj/exception.h>

// Forward declare v8::Isolate here, this allows us to avoid including the V8 header and compile
// some targets without depending on V8.
namespace v8 {
class Isolate;
}

namespace workerd::jsg {

class Url;

struct ResolveObserver {
  virtual ~ResolveObserver() noexcept(false) {}

  // Identifies the context in which a module resolution is being performed.
  enum class Context {
    // The resolve is being performed in the context of a worker bundle module
    // (that is, a worker script is calling import or require).
    BUNDLE,
    // The resolve is being performed in the context of a builtin module
    // (that is, one of the modules built into the worker runtime).
    BUILTIN,
    // Like builtin, the but it's a module that is *only* resolvable from a builtin
    // (like the `node-internal:...` modules)
    BUILTIN_ONLY,
  };

  enum class Source {
    // The resolve originated from a static import statement.
    STATIC_IMPORT,
    // The resolve originated from a dynamic import statement.
    DYNAMIC_IMPORT,
    // The resolve originated from a CommonJS require() call.
    REQUIRE,
    // The resolve originated from an internal direct call to
    // the ModuleRegistry.
    INTERNAL,
    // The resolve originated from some other source (to be defined).
    OTHER,
  };

  // Used to report the status of a module resolution.
  class ResolveStatus {
  public:
    ResolveStatus() = default;
    KJ_DISALLOW_COPY_AND_MOVE(ResolveStatus);
    virtual ~ResolveStatus() noexcept(false) {}

    // Indicates that the module resolution was successful and a
    // matching module was found in the registry.
    virtual void found() {}

    // Indicates that the module resolution failed because no matching
    // module was found in the registry.
    virtual void notFound() {}

    // Indicates that the module resolution failed because an error
    // occurred.
    virtual void exception(kj::Exception&& exception) {}
  };

  // Called when a module is being resolved. The returned ResolveStatus
  // object will be used to report the result of the resolution.
  // It is guaranteed that isolate lock is not held during invocation.
  virtual kj::Own<ResolveStatus> onResolveModule(
      const Url& specifier, Context context, Source source) const {
    static ResolveStatus nonopStatus;
    return {&nonopStatus, kj::NullDisposer::instance};
  }

  // Called when a module is being resolved. The returned ResolveStatus
  // object will be used to report the result of the resolution.
  // It is guaranteed that isolate lock is not held during invocation.
  virtual kj::Own<ResolveStatus> onResolveModule(
      kj::StringPtr specifier, Context context, Source source) const {
    static ResolveStatus nonopStatus;
    return {&nonopStatus, kj::NullDisposer::instance};
  }
};

struct CompilationObserver {
  virtual ~CompilationObserver() noexcept(false) {}

  // see ModuleInfoCompileOption
  enum class Option { BUNDLE, BUILTIN };

  // Monitors behavior of compilation processes.

  // Called at the start of ESM compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onEsmCompilationStart(
      v8::Isolate* isolate, kj::StringPtr name, Option option) const {
    return kj::Own<void>();
  }

  // Called at the start of Script (e.g. non-ESM) compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onScriptCompilationStart(
      v8::Isolate* isolate, kj::Maybe<kj::StringPtr> name = kj::none) const {
    return kj::Own<void>();
  }

  // Called at the start of wasm compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onWasmCompilationStart(v8::Isolate* isolate, size_t codeSize) const {
    return kj::Own<void>();
  }

  // Variation that is called at the start of wasm compilation from cache.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onWasmCompilationFromCacheStart(v8::Isolate* isolate) const {
    return kj::Own<void>();
  }

  // Called at the start of json module parsing.
  // Returned value will be destroyed when parsing completes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onJsonCompilationStart(v8::Isolate* isolate, size_t inputSize) const {
    return kj::Own<void>();
  }
};

struct InternalExceptionObserver {
  virtual ~InternalExceptionObserver() noexcept(false) {}

  struct Detail {
    bool isInternal;
    bool isFromRemote;
    bool isDurableObjectReset;
  };

  // Called when an internal exception is created (see makeInternalError).
  // Used to collect metrics on various internal error conditions.
  virtual void reportInternalException(const kj::Exception&, Detail detail) {}
};

struct IsolateObserver: public CompilationObserver,
                        public InternalExceptionObserver,
                        public ResolveObserver {
  virtual ~IsolateObserver() noexcept(false) {}
};

}  // namespace workerd::jsg
