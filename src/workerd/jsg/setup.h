// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Public API for setting up JavaScript context. Only high-level code needs to include this file.

#include "jsg.h"
#include "async-context.h"
#include "type-wrapper.h"
#include "v8-platform-wrapper.h"
#include "v8-profiler.h"
#include <workerd/util/batch-queue.h>
#include <kj/map.h>
#include <kj/mutex.h>
#include <workerd/jsg/observer.h>

namespace workerd::jsg {

// Construct a default V8 platform, with the given background thread pool size.
//
// Passing zero for `backgroundThreadCount` causes V8 to ask glibc how many processors there are.
// Now, glibc *could* answer this problem easily by calling `sched_getaffinity()`, which would
// not only tell it how many cores exist, but also how many cores are available to this specific
// process. But does glibc do that? No, it does not. Instead, it frantically tries to open
// `/sys/devices/system/cpu/online`, then `/proc/stat`, then `/proc/cpuinfo`, and parses the text
// it reads from whichever file successfully opens to find out the number of processors. Of course,
// if you're in a sandbox, that probably won't work. And anyway, you probably don't actually want
// V8 to consume all available cores with background work. So, please specify a thread pool size.
kj::Own<v8::Platform> defaultPlatform(uint backgroundThreadCount);

// In order to use any part of the JSG API, you must first construct a V8System. You can only
// construct one of these per process. This performs process-wide initialization of the V8
// library.
class V8System {
public:
  // Use the default v8::Platform implementation, as if by:
  //   auto v8Platform = jsg::defaultPlatform();
  //   auto v8System = V8System(*v8Platform);
  V8System();

  // `flags` is a list of command-line flags to pass to V8, like "--expose-gc" or
  // "--single_threaded_gc". An exception will be thrown if any flags are not recognized.
  explicit V8System(kj::ArrayPtr<const kj::StringPtr> flags);

  // Use a possibly-custom v8::Platform implementation. Use this if you need to override any
  // functionality provided by the v8::Platform API.
  explicit V8System(v8::Platform& platform);

  // Use a possibly-custom v8::Platform implementation, and apply flags.
  explicit V8System(v8::Platform& platform, kj::ArrayPtr<const kj::StringPtr> flags);

  ~V8System() noexcept(false);

  typedef void FatalErrorCallback(kj::StringPtr location, kj::StringPtr message);
  static void setFatalErrorCallback(FatalErrorCallback* callback);

private:
  kj::Own<v8::Platform> platformInner;
  V8PlatformWrapper platformWrapper;
  friend class IsolateBase;

  explicit V8System(kj::Own<v8::Platform>, kj::ArrayPtr<const kj::StringPtr>);
};

// Base class of Isolate<T> containing parts that don't need to be templated, to avoid code
// bloat.
class IsolateBase {
public:
  static IsolateBase& from(v8::Isolate* isolate);

  // Unwraps a JavaScript exception as a kj::Exception.
  virtual kj::Exception unwrapException(v8::Local<v8::Context> context,
                                        v8::Local<v8::Value> exception) = 0;

  // Wraps a kj::Exception as a JavaScript Exception.
  virtual v8::Local<v8::Value> wrapException(v8::Local<v8::Context> context,
                                             kj::Exception&& exception) = 0;

  // Used by Serializer/Deserializer implementations, calls into DynamicResourceTypeMap
  // serializerMap and deserializerMap.
  virtual bool serialize(
      Lock& js, std::type_index type, jsg::Object& instance, Serializer& serializer) = 0;
  virtual kj::Maybe<v8::Local<v8::Object>> deserialize(
      Lock& js, uint tag, Deserializer& deserializer) = 0;

  // Immediately cancels JavaScript execution in this isolate, causing an uncatchable exception to
  // be thrown. Safe to call across threads, without holding the lock.
  void terminateExecution() const;

  using Logger = Lock::Logger;
  inline void setLoggerCallback(kj::Badge<Lock>, kj::Function<Logger>&& logger) {
    maybeLogger = kj::mv(logger);
  }

  using ModuleFallbackCallback =
      kj::Maybe<kj::OneOf<kj::String, jsg::ModuleRegistry::ModuleInfo>>(
          jsg::Lock&,
          kj::StringPtr,
          kj::Maybe<kj::String>,
          jsg::CompilationObserver&,
          jsg::ModuleRegistry::ResolveMethod);
  inline void setModuleFallbackCallback(kj::Function<ModuleFallbackCallback>&& callback) {
    maybeModuleFallbackCallback = kj::mv(callback);
  }
  inline kj::Maybe<kj::Function<ModuleFallbackCallback>&> tryGetModuleFallback() {
    KJ_IF_SOME(moduleFallbackCallback, maybeModuleFallbackCallback) {
      return moduleFallbackCallback;
    }
    return kj::none;
  }

  inline void setAllowEval(kj::Badge<Lock>, bool allow) { evalAllowed = allow; }
  inline void setCaptureThrowsAsRejections(kj::Badge<Lock>, bool capture) {
    captureThrowsAsRejections = capture;
  }
  inline void setCommonJsExportDefault(kj::Badge<Lock>, bool exportDefault) {
    exportCommonJsDefault = exportDefault;
  }

  inline bool areWarningsLogged() const { return maybeLogger != kj::none; }

  // The logger will be optionally set by the isolate setup logic if there is anywhere
  // for the log to go (for instance, if debug logging is enabled or the inspector is
  // being used).
  inline void logWarning(Lock& js, kj::StringPtr message) {
    KJ_IF_SOME(logger, maybeLogger) { logger(js, message); }
  }

  // Returns a random UUID for this isolate instance.
  kj::StringPtr getUuid();

  IsolateObserver& getObserver() { return *observer; }

  // Implementation of MemoryRetainer
  void jsgGetMemoryInfo(MemoryTracker& tracker) const;
  kj::StringPtr jsgGetMemoryName() const { return "IsolateBase"_kjc; }
  size_t jsgGetMemorySelfSize() const { return sizeof(IsolateBase); }
  bool jsgGetMemoryInfoIsRootNode() const { return true; }

private:
  template <typename TypeWrapper>
  friend class Isolate;

  static void buildEmbedderGraph(v8::Isolate* isolate, v8::EmbedderGraph* graph, void* data);

  // The internals of a jsg::Ref<T> to be deleted.
  class RefToDelete {
  public:
    RefToDelete(bool strong, kj::Own<void> ownWrappable, Wrappable* wrappable)
        : strong(strong), ownWrappable(kj::mv(ownWrappable)), wrappable(wrappable) {}
    ~RefToDelete() noexcept(false) {
      if (ownWrappable.get() != nullptr && strong) {
        wrappable->removeStrongRef();
      }
    }
    RefToDelete(RefToDelete&&) = default;

    // Default move ctor okay because ownWrappable.get() will be null if moved-from.
    KJ_DISALLOW_COPY(RefToDelete);

  private:
    bool strong;
    // Keeps the `wrappable` pointer below valid.
    kj::Own<void> ownWrappable;
    Wrappable* wrappable;
  };

  using Item = kj::OneOf<v8::Global<v8::Data>, RefToDelete>;

  const V8System& system;
  v8::Isolate* ptr;
  kj::Maybe<kj::String> uuid;
  bool evalAllowed = false;

  // The Web Platform API specifications require that any API that returns a JavaScript Promise
  // should never throw errors synchronously. Rather, they are supposed to capture any synchronous
  // throws and return a rejected Promise. Historically, Workers did not follow that guideline
  // and there are a number of async APIs that currently throw. When the captureThrowsAsRejections
  // flag is set, that old behavior is changed to be correct.
  bool captureThrowsAsRejections = false;
  bool exportCommonJsDefault = false;
  bool asyncContextTrackingEnabled = false;

  kj::Maybe<kj::Function<Logger>> maybeLogger;
  kj::Maybe<kj::Function<ModuleFallbackCallback>> maybeModuleFallbackCallback;

  // FunctionTemplate used by Wrappable::attachOpaqueWrapper(). Just a constructor for an empty
  // object with 2 internal fields.
  v8::Global<v8::FunctionTemplate> opaqueTemplate;

  // We expect queues to remain relatively small -- 8 is the largest size I have observed from local
  // testing.
  static constexpr auto DESTRUCTION_QUEUE_INITIAL_SIZE = 8;

  // If a queue grows larger than this, we reset it back to the initial size.
  static constexpr auto DESTRUCTION_QUEUE_MAX_CAPACITY = 10'000;

  // We use a double buffer for our deferred destruction queue. This allows us to avoid any
  // allocations in the general, steady state case, and forces us to clear the vector (a O(n)
  // operation) outside of the queue lock.
  const kj::MutexGuarded<BatchQueue<Item>> queue {
    DESTRUCTION_QUEUE_INITIAL_SIZE,
    DESTRUCTION_QUEUE_MAX_CAPACITY
  };

  struct CodeBlockInfo {
    size_t size = 0;
    kj::Maybe<v8::JitCodeEvent::CodeType> type;
    kj::String name;

    struct PositionMapping {
      uint instructionOffset;
      uint sourceOffset;
    };
    kj::Array<PositionMapping> mapping;
    // Sorted
  };

  // Maps instructions to source code locations.
  kj::TreeMap<uintptr_t, CodeBlockInfo> codeMap;

  explicit IsolateBase(const V8System& system, v8::Isolate::CreateParams&& createParams,
    kj::Own<IsolateObserver> observer);
  ~IsolateBase() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(IsolateBase);

  void dropWrappers(kj::Own<void> typeWrapperInstance);

  bool getCaptureThrowsAsRejections() const { return captureThrowsAsRejections; }

  bool getCommonJsExportDefault() const { return exportCommonJsDefault; }

  // Add an item to the deferred destruction queue. Safe to call from any thread at any time.
  void deferDestruction(Item item);

  // Destroy everything in the deferred destruction queue. Must be called under the isolate lock.
  void clearDestructionQueue();

  static void fatalError(const char* location, const char* message);
  static void oomError(const char* location, const v8::OOMDetails& details);

  static v8::ModifyCodeGenerationFromStringsResult modifyCodeGenCallback(
      v8::Local<v8::Context> context, v8::Local<v8::Value> source, bool isCodeLike);
  static bool allowWasmCallback(v8::Local<v8::Context> context, v8::Local<v8::String> source);

  static void jitCodeEvent(const v8::JitCodeEvent* event) noexcept;

  friend class IsolateBase;
  friend kj::Maybe<kj::StringPtr> getJsStackTrace(void* ucontext, kj::ArrayPtr<char> scratch);

  HeapTracer heapTracer;
  kj::Own<IsolateObserver> observer;

  friend class Data;
  friend class Wrappable;
  friend class HeapTracer;

  friend bool getCaptureThrowsAsRejections(v8::Isolate* isolate);
  friend bool getCommonJsExportDefault(v8::Isolate* isolate);
  friend kj::Maybe<kj::StringPtr> getJsStackTrace(void* ucontext, kj::ArrayPtr<char> scratch);

  friend kj::Exception createTunneledException(v8::Isolate* isolate,
                                               v8::Local<v8::Value> exception);

  // Get a singleton ObjectTemplate used for opaque wrappers (which have an empty-object interface
  // in JavaScript). (Called by Wrappable::attachOpaqueWrapper().)
  //
  // This returns a FunctionTemplate which should be used as a constructor. That is, you can use
  // use `->InstanceTemplate()->NewInstance()` to construct an object, and you can pass this to
  // `FindInstanceInPrototypeChain()` on an existing object to check whether it was created using
  // this template.
  static v8::Local<v8::FunctionTemplate> getOpaqueTemplate(v8::Isolate* isolate);
};

// If JavaScript frames are currently on the stack, returns a string representing a stack trace
// through it. The trace is built inside `scratch` without performing any allocation. This is
// intended to be invoked from a signal handler.
kj::Maybe<kj::StringPtr> getJsStackTrace(void* ucontext, kj::ArrayPtr<char> scratch);

// Class representing a JavaScript execution engine, with the ability to wrap some set of API
// classes which you specify.
//
// To use this, you must declare your own custom specialization listing all of the API types that
// you want to support in this JavaScript context. API types are types which have
// JSG_RESOURCE_TYPE or JSG_STRUCT declarations, as well as TypeWrapperExtensions.
//
// To declare a specialization, do:
//
//     JSG_DECLARE_ISOLATE_TYPE(MyIsolateType, MyApiType1, MyApiType2, ...);
//
// This declares a class `MyIsolateType` which is a subclass of Isolate. You can then
// instantiate this class to begin executing JavaScript.
//
// You can instantiate multiple Isolates which can run on separate threads simultaneously.
//
// Example usage:
//
//     // Create once per process, probably in main().
//     V8System system;
//
//     // Create an isolate with the ability to wrap MyType and MyContextType.
//     JSG_DECLARE_ISOLATE_TYPE(MyIsolate, MyApiType, MyContextApiType);
//     MyIsolate isolate(system);
//
//     // Lock the isolate in this thread (creates a v8::Isolate::Scope).
//     isolate.runInLockScope([&] (MyIsolate::Lock& lock) {
//       // Create a context based on MyContextType.
//       v8::Local<v8::Context> context = lock.newContext(lock.isolate, MyContextType());
//
//       // Create an instance of MyType.
//       v8::Local<v8::Object> obj = lock.getTypeHandler<MyType>().wrap(context, MyType());
//     });
//
template <typename TypeWrapper>
class Isolate: public IsolateBase {
public:

  // Construct an isolate that requires configuration. `configuration` is a value that all
  // individual wrappers' configurations must be able to be constructed from. For example, if all
  // wrappers use the same configuration type, then `MetaConfiguration` should just be that type.
  // If different wrappers use different types, then `MetaConfiguration` should be some value that
  // inherits or defines conversion operators to each required type -- or the individual
  // configuration types must declare constructors from `MetaConfiguration`.
  template <typename MetaConfiguration>
  explicit Isolate(const V8System& system,
      MetaConfiguration&& configuration,
      kj::Own<IsolateObserver> observer,
      v8::Isolate::CreateParams createParams = {})
      : IsolateBase(system, kj::mv(createParams), kj::mv(observer)),
        wrapper(wrapperSpace.construct(ptr, kj::fwd<MetaConfiguration>(configuration))) {
          wrapper->initTypeWrapper();
        }

  // Use this constructor when no wrappers have any required configuration.
  explicit Isolate(const V8System& system,
      kj::Own<IsolateObserver> observer,
      v8::Isolate::CreateParams createParams = {})
      : Isolate(system, nullptr, kj::mv(observer), kj::mv(createParams)) {}

  ~Isolate() noexcept(false) { dropWrappers(kj::mv(wrapper)); }

  kj::Exception unwrapException(v8::Local<v8::Context> context,
                                v8::Local<v8::Value> exception) override {
    return wrapper->template unwrap<kj::Exception>(context, exception,
                                                   jsg::TypeErrorContext::other());
  }

  v8::Local<v8::Value> wrapException(v8::Local<v8::Context> context,
                                     kj::Exception&& exception) override {
    return wrapper->wrap(context, kj::none, kj::fwd<kj::Exception>(exception));
  }

  bool serialize(
      Lock& js, std::type_index type, jsg::Object& instance, Serializer& serializer) override {
    KJ_IF_SOME(func, wrapper->serializerMap.find(type)) {
      func(js, instance, serializer);
      return true;
    } else {
      return false;
    }
  }
  kj::Maybe<v8::Local<v8::Object>> deserialize(
      Lock& js, uint tag, Deserializer& deserializer) override {
    KJ_IF_SOME(func, wrapper->deserializerMap.find(tag)) {
      return func(*wrapper, js, tag, deserializer);
    } else {
      return kj::none;
    }
  }

  // Before you can execute code in your Isolate you must lock it to the current thread by
  // constructing a `Lock` on the stack.
  class Lock final: public jsg::Lock {

  public:
    // `V8StackScope` must be provided to prove that one has been created on the stack before
    // taking a lock. Any GC'd pointers stored on the stack must be kept within this scope in
    // order for V8's stack-scanning GC to find them.
    Lock(const Isolate& isolate, V8StackScope&)
        : jsg::Lock(isolate.ptr), jsgIsolate(const_cast<Isolate&>(isolate)) {
      jsgIsolate.clearDestructionQueue();
    }
    KJ_DISALLOW_COPY_AND_MOVE(Lock);
    KJ_DISALLOW_AS_COROUTINE_PARAM;

    // Creates a `TypeHandler` for the given type. You can use this to convert between the type
    // and V8 handles, as well as to allocate instances of the type on the V8 heap (if it is
    // a resource type).
    template <typename T>
    const TypeHandler<T>& getTypeHandler() {
      return TypeWrapper::template TYPE_HANDLER_INSTANCE<T>;
    }

    // Wrap a C++ value, returning a v8::Local (possibly of a specific type).
    template <typename T>
    auto wrap(v8::Local<v8::Context> context, T&& value) {
      return jsgIsolate.wrapper->wrap(context, kj::none, kj::fwd<T>(value));
    }

    // Wrap a context-independent value. Only a few built-in types, like numbers and strings,
    // can be wrapped without a context.
    template <typename T>
    auto wrapNoContext(T&& value) {
      return jsgIsolate.wrapper->wrap(v8Isolate, kj::none, kj::fwd<T>(value));
    }

    // Convert a JavaScript value to a C++ value, or throw a JS exception if the type doesn't
    // match.
    template <typename T>
    auto unwrap(v8::Local<v8::Context> context, v8::Local<v8::Value> handle) {
      return jsgIsolate.wrapper->template unwrap<T>(
          context, handle, jsg::TypeErrorContext::other());
    }

    // Returns the constructor function for a given type declared as JSG_RESOURCE_TYPE.
    //
    // Note there's a useful property of class constructor functions: A constructor's __proto__
    // is set to the parent type's constructor. Thus you can discover whether one class is a
    // subclass of another by following the __proto__ chain.
    //
    // TODO(cleanup): This should return `JsFunction`, but there is no such type. We only have
    //   `jsg::Function<...>` (or perhaps more appropriately, `jsg::Constructor<...>`), but we
    //   don't actually know the function signature so that's not useful here. Should we add a
    //   `JsFunction` that has no signature?
    template <typename T>
    jsg::JsObject getConstructor(v8::Local<v8::Context> context) {
      v8::EscapableHandleScope scope(v8Isolate);
      v8::Local<v8::FunctionTemplate> tpl =
          jsgIsolate.wrapper->template getTemplate(v8Isolate, (T*)nullptr);
      v8::Local<v8::Object> prototype = check(tpl->GetFunction(context));
      return jsg::JsObject(scope.Escape(prototype));
    }

    v8::Local<v8::ArrayBuffer> wrapBytes(kj::Array<byte> data) override {
      return jsgIsolate.wrapper->wrap(v8Isolate, kj::none, kj::mv(data));
    }
    v8::Local<v8::Function> wrapSimpleFunction(v8::Local<v8::Context> context,
        jsg::Function<void(const v8::FunctionCallbackInfo<v8::Value>& info)>
            simpleFunction) override {
      return jsgIsolate.wrapper->wrap(context, kj::none, kj::mv(simpleFunction));
    }
    v8::Local<v8::Function> wrapReturningFunction(v8::Local<v8::Context> context,
        jsg::Function<v8::Local<v8::Value>(const v8::FunctionCallbackInfo<v8::Value>& info)>
            returningFunction) override {
      return jsgIsolate.wrapper->wrap(context, kj::none, kj::mv(returningFunction));
    }
    v8::Local<v8::Function> wrapPromiseReturningFunction(v8::Local<v8::Context> context,
      jsg::Function<jsg::Promise<jsg::Value>(
          const v8::FunctionCallbackInfo<v8::Value>& info)> returningFunction) override {
      return jsgIsolate.wrapper->wrap(context, kj::none, kj::mv(returningFunction));
    }
    kj::String toString(v8::Local<v8::Value> value) override {
      return jsgIsolate.wrapper->template unwrap<kj::String>(
          v8Isolate->GetCurrentContext(), value, jsg::TypeErrorContext::other());
    }
    jsg::Dict<v8::Local<v8::Value>> toDict(v8::Local<v8::Value> value) override {
      return jsgIsolate.wrapper->template unwrap<jsg::Dict<v8::Local<v8::Value>>>(
          v8Isolate->GetCurrentContext(), value, jsg::TypeErrorContext::other());
    }
    jsg::Dict<jsg::JsValue> toDict(const jsg::JsValue& value) override {
      return jsgIsolate.wrapper->template unwrap<jsg::Dict<jsg::JsValue>>(
          v8Isolate->GetCurrentContext(), value, jsg::TypeErrorContext::other());
    }
    v8::Local<v8::Promise> wrapSimplePromise(jsg::Promise<jsg::Value> promise) override {
      return jsgIsolate.wrapper->wrap(v8Context(), kj::none, kj::mv(promise));
    }
    jsg::Promise<jsg::Value> toPromise(v8::Local<v8::Value> promise) override {
      return jsgIsolate.wrapper->template unwrap<jsg::Promise<jsg::Value>>(
          v8Isolate->GetCurrentContext(), promise, jsg::TypeErrorContext::other());
    }

    // Creates a new JavaScript "context", i.e. the global object. This is the first step to
    // executing JavaScript code. T should be one of your API types which you want to use as the
    // global object. `args...` are passed to the type's constructor.
    template <typename T, typename... Args>
    JsContext<T> newContext(Args&&... args) {
      // TODO(soon): Requiring move semantics for the global object is awkward. This should instead
      //   allocate the object (forwarding arguments to the constructor) and return something like
      //   a Ref.

      return jsgIsolate.wrapper->newContext(*this, jsgIsolate.getObserver(), (T*)nullptr, kj::fwd<Args>(args)...);
    }

  private:
    Isolate& jsgIsolate;
  };

  // The func must be a callback with the signature: void(jsg::Lock&)
  void runInLockScope(auto func) {
    runInV8Stack([&](V8StackScope& stackScope) {
      Lock lock(*this, stackScope);
      lock.withinHandleScope([&] {
        func(lock);
      });
    });
  }

private:
  kj::SpaceFor<TypeWrapper> wrapperSpace;
  kj::Own<TypeWrapper> wrapper;  // Needs to be destroyed under lock...
};

// This macro helps cut down on template spam in error messages. Instead of instantiating Isolate
// directly, do:
//
//     JSG_DECLARE_ISOLATE_TYPE(MyIsolate, SomeApiType, AnotherApiType, ...);
//
// `MyIsolate` becomes your custom Isolate type, which will support wrapping all of the listed
// API types.
#define JSG_DECLARE_ISOLATE_TYPE(Type, ...) \
  class Type##_TypeWrapper; \
  typedef ::workerd::jsg::TypeWrapper<Type##_TypeWrapper, jsg::DOMException, ##__VA_ARGS__> \
      Type##_TypeWrapperBase; \
  class Type##_TypeWrapper final: public Type##_TypeWrapperBase { \
  public: \
    using Type##_TypeWrapperBase::TypeWrapper; \
  }; \
  class Type final: public ::workerd::jsg::Isolate<Type##_TypeWrapper> { \
  public: \
    using ::workerd::jsg::Isolate<Type##_TypeWrapper>::Isolate; \
  }

}  // namespace workerd::jsg
