// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/string.h>
#include <kj/exception.h>
#include <workerd/jsg/url.h>

// Forward declare v8::Isolate here, this allows us to avoid including the V8 header and compile
// some targets without depending on V8.
namespace v8 {
  class Isolate;
}

namespace workerd::jsg {

struct ResolveObserver {
  virtual ~ResolveObserver() noexcept(false) { }

  enum class Context {
    BUNDLE,
    BUILTIN,
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

  class ResolveStatus {
  public:
    ResolveStatus() = default;
    KJ_DISALLOW_COPY_AND_MOVE(ResolveStatus);
    virtual ~ResolveStatus() {}

    // Indicates that the module resolution was successful and a
    // matching module was found in the registry.
    virtual void found() = 0;

    // Indicates that the module resolution failed and no matching
    // module was found in the registry.
    virtual void notFound() = 0;
  };

  // Called when a module is resolved.
  // It is guaranteed that isolate lock is not held during invocation.
  virtual kj::Own<ResolveStatus> onResolveModule(const Url& specifier,
                                                 Context context,
                                                 Source source) const {
    class ResolveStatusImpl final : public ResolveStatus {
    public:
      void found() override {}
      void notFound() override {}
    };
    return kj::heap<ResolveStatusImpl>();
  }
};

struct CompilationObserver {
  virtual ~CompilationObserver() noexcept(false) { }

  // see ModuleInfoCompileOption
  enum class Option { BUNDLE, BUILTIN };

  // Monitors behavior of compilation processes.

  // Called at the start of ESM module compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onEsmCompilationStart(
      v8::Isolate* isolate, kj::StringPtr name, Option option) const { return kj::Own<void>(); }

  // Called at the start of CJS-style module compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onCjsStyleCompilationStart(
      v8::Isolate* isolate, kj::StringPtr name) const { return kj::Own<void>(); }

  // Called at the start of wasm compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onWasmCompilationStart(v8::Isolate* isolate, size_t codeSize) const {
    return kj::Own<void>();
  }

  // Called at the start of wasm compilation from cache.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onWasmCompilationStart(v8::Isolate* isolate) const {
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
  virtual ~InternalExceptionObserver() noexcept(false) { }

struct Detail {
  bool isInternal;
  bool isFromRemote;
  bool isDurableObjectReset;
};

  // Called when an internal exception is created (see makeInternalError).
  // Used to collect metrics on various internal error conditions.
  virtual void reportInternalException(const kj::Exception&, Detail detail) { }
};

struct IsolateObserver : public CompilationObserver,
                         public InternalExceptionObserver,
                         public ResolveObserver {
  virtual ~IsolateObserver() noexcept(false) { }
};


} // namespace workerd::jsg
