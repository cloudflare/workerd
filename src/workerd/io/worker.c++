// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <cstdint>
#include <workerd/io/worker.h>
#include <workerd/io/promise-wrapper.h>
#include "actor-cache.h"
#include <workerd/util/batch-queue.h>
#include <workerd/util/color-util.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/xthreadnotifier.h>
#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/sockets.h>
#include <workerd/api/streams.h>  // for api::StreamEncoding
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/inspector.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/util.h>
#include <workerd/io/cdp.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <capnp/compat/json.h>
#include <kj/compat/gzip.h>
#include <kj/compat/brotli.h>
#include <kj/encoding.h>
#include <kj/filesystem.h>
#include <kj/map.h>
#include <v8-inspector.h>
#include <v8-profiler.h>
#include <map>
#include <time.h>
#include <numeric>

#if _WIN32
#include <kj/win32-api-version.h>
#include <io.h>
#include <windows.h>
#include <kj/windows-sanity.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace workerd {

namespace {

void headersToCDP(const kj::HttpHeaders& in, capnp::JsonValue::Builder out) {
  std::map<kj::StringPtr, kj::Vector<kj::StringPtr>> inMap;
  in.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    inMap.try_emplace(name, 1).first->second.add(value);
  });

  auto outObj = out.initObject(inMap.size());
  auto headersPos = 0;
  for (auto& entry: inMap) {
    auto field = outObj[headersPos++];
    field.setName(entry.first);

    // CDP uses strange header representation where headers with multiple
    // values are merged into one newline-delimited string
    field.initValue().setString(kj::strArray(entry.second, "\n"));
  }
}

void stackTraceToCDP(jsg::Lock& js, cdp::Runtime::StackTrace::Builder builder) {
  // TODO(cleanup): Maybe use V8Inspector::captureStackTrace() which does this for us. However, it
  //   produces protocol objects in its own format which want to handle their whole serialization
  //   to JSON. Also, those protocol objects are defined in generated code which we currently don't
  //   include in our cached V8 build artifacts; we'd need to fix that. But maybe we should really
  //   be using the V8-generated protocol objects rather than our parallel capnp versions!

  auto stackTrace = v8::StackTrace::CurrentStackTrace(js.v8Isolate, 10);
  auto frameCount = stackTrace->GetFrameCount();
  auto callFrames = builder.initCallFrames(frameCount);
  for (int i = 0; i < frameCount; i++) {
    auto src = stackTrace->GetFrame(js.v8Isolate, i);
    auto dest = callFrames[i];
    auto url = src->GetScriptNameOrSourceURL();
    if (!url.IsEmpty()) {
      dest.setUrl(kj::str(url));
    } else {
      dest.setUrl(""_kj);
    }
    dest.setScriptId(kj::str(src->GetScriptId()));
    auto func = src->GetFunctionName();
    if (!func.IsEmpty()) {
      dest.setFunctionName(kj::str(func));
    } else {
      dest.setFunctionName(""_kj);
    }
    // V8 locations are 1-based, but CDP locations are 0-based... oh, well
    dest.setLineNumber(src->GetLineNumber() - 1);
    dest.setColumnNumber(src->GetColumn() - 1);
  }
}

kj::Own<capnp::JsonCodec> makeCdpJsonCodec() {
  auto codec = kj::heap<capnp::JsonCodec>();
  codec->handleByAnnotation<cdp::Command>();
  codec->handleByAnnotation<cdp::Event>();
  return codec;
}
const capnp::JsonCodec& getCdpJsonCodec() {
  static const kj::Own<capnp::JsonCodec> codec = makeCdpJsonCodec();
  return *codec;
}

}  // namespace

// =======================================================================================

namespace {

// Inform the inspector of an exception thrown.
//
// Passes `source` as the exception's short message. Reconstructs `message` from `exception` if
// `message` is empty.
void sendExceptionToInspector(jsg::Lock& js,
                              v8_inspector::V8Inspector& inspector,
                              UncaughtExceptionSource source,
                              const jsg::JsValue& exception,
                              jsg::JsMessage message) {
  jsg::sendExceptionToInspector(js, inspector, kj::str(source), exception, message);
}

void addExceptionToTrace(jsg::Lock& js,
                         IoContext &ioContext,
                         WorkerTracer& tracer,
                         UncaughtExceptionSource source,
                         const jsg::JsValue& exception,
                         const jsg::TypeHandler<Worker::Api::ErrorInterface>&
                             errorTypeHandler) {
  if (source == UncaughtExceptionSource::INTERNAL ||
      source == UncaughtExceptionSource::INTERNAL_ASYNC) {
    // Skip redundant intermediate JS->C++ exception reporting.  See: IoContext::runImpl(),
    // PromiseWrapper::tryUnwrap()
    //
    // TODO(someday): Arguably it could make sense to store these exceptions off to the side and
    //   report them only if they don't end up being duplicates of a later exception that has a more
    //   specific context. This would cover cases where the C++ code that eventually received the
    //   exception never ended up reporting it.
    return;
  }

  auto timestamp = ioContext.now();
  auto error = KJ_REQUIRE_NONNULL(errorTypeHandler.tryUnwrap(js, exception),
      "Should always be possible to unwrap error interface from an object.");

  kj::String name;
  KJ_IF_SOME(n, error.name) {
    name = kj::str(n);
  } else {
    name = kj::str("Error");
  }
  kj::String message;
  KJ_IF_SOME(m, error.message) {
    message = kj::str(m);
  }
  // TODO(someday): Limit size of exception content?
  tracer.addException(timestamp, kj::mv(name), kj::mv(message));
}

void reportStartupError(
    kj::StringPtr id,
    jsg::Lock& js,
    const kj::Maybe<std::unique_ptr<v8_inspector::V8Inspector>>& inspector,
    const IsolateLimitEnforcer& limitEnforcer,
    kj::Maybe<kj::Exception> maybeLimitError,
    v8::TryCatch& catcher,
    kj::Maybe<Worker::ValidationErrorReporter&> errorReporter,
    kj::Maybe<kj::Exception>& permanentException) {
  v8::TryCatch catcher2(js.v8Isolate);
  kj::Maybe<kj::Exception> maybeLimitError2;
  try {
    KJ_IF_SOME(limitError, maybeLimitError) {
      auto description = jsg::extractTunneledExceptionDescription(limitError.getDescription());

      auto& ex = permanentException.emplace(kj::mv(limitError));
      KJ_IF_SOME(e, errorReporter) {
        e.addError(kj::heapString(description));
      } else KJ_IF_SOME(i, inspector) {
        // We want to extend just enough cpu time as is necessary to report the exception
        // to the inspector here. 10 milliseconds should be more than enough.
        auto limitScope = limitEnforcer.enterLoggingJs(js, maybeLimitError2);
        jsg::sendExceptionToInspector(js, *i.get(), description);
        // When the inspector is active, we don't want to throw here because then the inspector
        // won't be able to connect and the developer will never know what happened.
      } else {
        // We should never get here in production if we've validated scripts before deployment.
        KJ_LOG(ERROR, "script startup exceeded resource limits", id, ex);
        kj::throwFatalException(kj::cp(ex));
      }
    } else if (catcher.HasCaught()) {
      js.withinHandleScope([&] {
        auto exception = catcher.Exception();

        permanentException = js.exceptionToKj(js.v8Ref(exception));

        KJ_IF_SOME(e, errorReporter) {
          auto limitScope = limitEnforcer.enterLoggingJs(js, maybeLimitError2);

          kj::Vector<kj::String> lines;
          lines.add(kj::str("Uncaught ", jsg::extractTunneledExceptionDescription(
              KJ_ASSERT_NONNULL(permanentException).getDescription())));
          jsg::JsMessage message(catcher.Message());
          message.addJsStackTrace(js, lines);
          e.addError(kj::strArray(lines, "\n"));

        } else KJ_IF_SOME(i, inspector) {
          auto limitScope = limitEnforcer.enterLoggingJs(js, maybeLimitError2);
          sendExceptionToInspector(js, *i.get(),
                                   UncaughtExceptionSource::INTERNAL,
                                   jsg::JsValue(exception),
                                   jsg::JsMessage(catcher.Message()));
          // When the inspector is active, we don't want to throw here because then the inspector
          // won't be able to connect and the developer will never know what happened.
        } else {
          // We should never get here in production if we've validated scripts before deployment.
          kj::Vector<kj::String> lines;
          jsg::JsMessage message(catcher.Message());
          message.addJsStackTrace(js, lines);
          auto trace = kj::strArray(lines, "; ");
          auto description = KJ_ASSERT_NONNULL(permanentException).getDescription();
          KJ_LOG(ERROR, "script startup threw exception", id, description, trace);
          KJ_FAIL_REQUIRE("script startup threw exception");
        }
      });
    } else {
      kj::throwFatalException(kj::cp(permanentException.emplace(
          KJ_EXCEPTION(FAILED, "returned empty handle but didn't throw exception?", id))));
    }
  } catch (const jsg::JsExceptionThrown&) {
#define LOG_AND_SET_PERM_EXCEPTION(...) \
    KJ_LOG(ERROR, __VA_ARGS__); \
    if (permanentException == kj::none) { \
      permanentException = KJ_EXCEPTION(FAILED, __VA_ARGS__); \
    }

    KJ_IF_SOME(limitError2, maybeLimitError2) {
      // TODO(cleanup): If we see this error show up in production, stop logging it, because I
      //   guess it's not necessarily an error? The other two cases below are more worrying though.
      KJ_LOG(ERROR, limitError2);
      if (permanentException == kj::none) {
        permanentException = kj::mv(limitError2);
      }
    } else if (catcher2.HasTerminated()) {
      LOG_AND_SET_PERM_EXCEPTION(
          "script startup threw exception; during our attempt to stringify the exception, "
          "the script apparently was terminated for non-resource-limit reasons.", id);
    } else {
      LOG_AND_SET_PERM_EXCEPTION(
          "script startup threw exception; furthermore, an attempt to stringify the exception "
          "threw another exception, which shouldn't be possible?", id);
    }
#undef LOG_AND_SET_PERM_EXCEPTION
  }
}

uint64_t getCurrentThreadId() {
#if __linux__
  return syscall(SYS_gettid);
#elif _WIN32
  return GetCurrentThreadId();
#else
  // Assume MacOS or BSD
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  return tid;
#endif
}

}  // namespace

// Represents a thread's attempt to take an async lock. Each Isolate has a linked list of
// `AsyncWaiter`s. A particular thread only ever owns one `AsyncWaiter` at a time.
class Worker::AsyncWaiter: public kj::Refcounted {
public:
  AsyncWaiter(kj::Own<const Isolate> isolate);
  ~AsyncWaiter() noexcept;
  KJ_DISALLOW_COPY_AND_MOVE(AsyncWaiter);

private:
  // Executor for this waiter's thread.
  const kj::Executor& executor;

  // The isolate for which this waiter is currently waiting.
  kj::Own<const Isolate> isolate;

  // Promise/fulfiller to fire when the waiter reaches the front of the list for the corresponding
  // isolate.
  kj::ForkedPromise<void> readyPromise = nullptr;
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> readyFulfiller;

  // Promise/fulfiller to fire when the AsyncLock is finally released. This is used when a thread
  // tries to take locks on multiple different isolates concurrently, in order to serialize the
  // locks so only one is taken at a time. This is NOT a cross-thread fulfiller; it can only be
  // fulfilled by the thread that owns the waiter.
  kj::ForkedPromise<void> releasePromise = nullptr;
  kj::Own<kj::PromiseFulfiller<void>> releaseFulfiller;

  // Protected by the lock on `Isolate::asyncWaiters` for the isolate identified by
  // `currentIsolate`. Must be null if `currentIsolate` is null. (All other members of `Waiter`
  // can only be accessed by the thread that created the `Waiter`.)
  kj::Maybe<AsyncWaiter&> next;
  kj::Maybe<AsyncWaiter&>* prev;

  static thread_local AsyncWaiter* threadCurrentWaiter;

  friend class Worker::Isolate;
  friend class Worker::AsyncLock;
};

class Worker::InspectorClient: public v8_inspector::V8InspectorClient {
public:
  // Wall time in milliseconds with millisecond precision. console.time() and friends rely on this
  // function to implement timers.
  double currentTimeMS() override {
    auto timePoint = kj::UNIX_EPOCH;

    if (IoContext::hasCurrent()) {
      // We're on a request-serving thread.
      auto& ioContext = IoContext::current();
      timePoint = ioContext.now();
    } else {
      auto lockedState = state.lockExclusive();
      KJ_IF_SOME(info, lockedState->inspectorTimerInfo) {
        if (info.threadId == getCurrentThreadId()) {
          // We're on an inspector-serving thread.
          timePoint = info.timer.now() + info.timerOffset
                    - kj::origin<kj::TimePoint>() + kj::UNIX_EPOCH;
        }
      }
      // We're at script startup time -- just return the Epoch.
    }
    return (timePoint - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }

  void setInspectorTimerInfo(kj::Timer& timer, kj::Duration timerOffset) {
    auto lockedState = state.lockExclusive();
    lockedState->inspectorTimerInfo = InspectorTimerInfo { timer, timerOffset, getCurrentThreadId() };
  }

  void setChannel(Worker::Isolate::InspectorChannelImpl& channel) {
    auto lockedState = state.lockExclusive();
    // There is only one active inspector channel at a time in workerd. The teardown of any
    // previous channel should have invalidated `lockedState->channel`.
    KJ_REQUIRE(lockedState->channel == kj::none);
    lockedState->channel = channel;
  }

  void resetChannel() {
    auto lockedState = state.lockExclusive();
    lockedState->channel = {};
  }

  // This method is called by v8 when a breakpoint or debugger statement is hit. This method
  // processes debugger messages until `Debugger.resume()` is called, when v8 then calls
  // `quitMessageLoopOnPause()`.
  //
  // This method is ultimately called from the `InspectorChannelImpl` and the isolate lock is
  // held when this method is called.
  void runMessageLoopOnPause(int contextGroupId) override {
    auto lockedState = state.lockExclusive();
    KJ_IF_SOME(channel, lockedState->channel) {
      runMessageLoop = true;
      do {
        if (!dispatchOneMessageDuringPause(channel)) {
          break;
        }
      } while (runMessageLoop);
    }
  }

  // This method is called by v8 to resume execution after a breakpoint is hit.
  void quitMessageLoopOnPause() override {
    runMessageLoop = false;
  }

private:
  static bool dispatchOneMessageDuringPause(Worker::Isolate::InspectorChannelImpl& channel);

  struct InspectorTimerInfo {
    kj::Timer& timer;
    kj::Duration timerOffset;
    uint64_t threadId;
  };

  bool runMessageLoop;

  // State that may be set on a thread other than the isolate thread.
  // These are typically set in attachInspector when an inspector connection is
  // made.
  struct State {
    // Inspector channel to use to pump messages.
    kj::Maybe<Worker::Isolate::InspectorChannelImpl&> channel;

    // The timer and offset for the inspector-serving thread.
    kj::Maybe<InspectorTimerInfo> inspectorTimerInfo;
  };
  kj::MutexGuarded<State> state;
};

// Defined later in this file.
void setWebAssemblyModuleHasInstance(jsg::Lock& lock, v8::Local<v8::Context> context);

static thread_local const Worker::Api* currentApi = nullptr;

const Worker::Api& Worker::Api::current() {
  KJ_REQUIRE(currentApi != nullptr, "not running JavaScript");
  return *currentApi;
}

struct Worker::Impl {
  kj::Maybe<jsg::JsContext<api::ServiceWorkerGlobalScope>> context;

  // The environment blob to pass to handlers.
  kj::Maybe<jsg::Value> env;

  struct ActorClassInfo {
    EntrypointClass cls;
    bool missingSuperclass;
  };

  // Note: The default export is given the string name "default", because that's what V8 tells us,
  // and so it's easiest to go with it. I guess that means that you can't actually name an export
  // "default"?
  kj::HashMap<kj::String, api::ExportedHandler> namedHandlers;
  kj::HashMap<kj::String, ActorClassInfo> actorClasses;
  kj::HashMap<kj::String, EntrypointClass> statelessClasses;

  // If set, then any attempt to use this worker shall throw this exception.
  kj::Maybe<kj::Exception> permanentException;
};

// Note that Isolate mutable state is protected by locking the JsgWorkerIsolate unless otherwise
// noted.
struct Worker::Isolate::Impl {
  IsolateObserver& metrics;
  InspectorClient inspectorClient;
  kj::Maybe<std::unique_ptr<v8_inspector::V8Inspector>> inspector;
  InspectorPolicy inspectorPolicy;
  kj::Maybe<kj::Own<v8::CpuProfiler>> profiler;
  ActorCache::SharedLru actorCacheLru;

  // Notification messages to deliver to the next inspector client when it connects.
  kj::Vector<kj::String> queuedNotifications;

  // Set of warning log lines that should not be logged to the inspector again.
  kj::HashSet<kj::String> warningOnceDescriptions;

  // Set of error log lines that should not be logged again.
  kj::HashSet<kj::String> errorOnceDescriptions;

  // Instantaneous count of how many threads are trying to or have successfully obtained an
  // AsyncLock on this isolate, used to implement getCurrentLoad().
  mutable uint lockAttemptGauge = 0;

  // Atomically incremented upon every successful lock. The ThreadProgressCounter in Impl::Lock
  // registers a reference to `lockSuccessCounter` as the thread's progress counter during a lock
  // attempt. This allows watchdogs to see evidence of forward progress in other threads, even if
  // their own thread has blocked waiting for the lock for a long time.
  mutable uint64_t lockSuccessCount = 0;

  // Wrapper around JsgWorkerIsolate::Lock and various RAII objects which help us report metrics,
  // measure instantaneous load, avoid spurious watchdog kills, and defer context destruction.
  //
  // Always use this wrapper in code which may face lock contention (that's mostly everywhere).
  class Lock {

  public:
    explicit Lock(const Worker::Isolate& isolate, Worker::LockType lockType,
                  jsg::V8StackScope& stackScope)
        : impl(*isolate.impl),
          metrics([&isolate, &lockType]()
              -> kj::Maybe<kj::Own<IsolateObserver::LockTiming>> {
            KJ_SWITCH_ONEOF(lockType.origin) {
              KJ_CASE_ONEOF(sync, Worker::Lock::TakeSynchronously) {
                // TODO(perf): We could do some tracking here to discover overly harmful synchronous
                //   locks.
                return isolate.getMetrics().tryCreateLockTiming(sync.getRequest());
              }
              KJ_CASE_ONEOF(async, AsyncLock*) {
                KJ_REQUIRE(async->waiter->isolate.get() == &isolate,
                    "async lock was taken against a different isolate than the synchronous lock");
                return kj::mv(async->lockTiming);
              }
            }
            KJ_UNREACHABLE;
          }()),
          progressCounter(impl.lockSuccessCount),
          oldCurrentApi(currentApi),
          limitEnforcer(isolate.getLimitEnforcer()),
          consoleMode(isolate.consoleMode),
          lock(isolate.api->lock(stackScope)) {
      WarnAboutIsolateLockScope::maybeWarn();

      // Increment the success count to expose forward progress to all threads.
      __atomic_add_fetch(&impl.lockSuccessCount, 1, __ATOMIC_RELAXED);
      metrics.locked();

      // We record the current lock so our GC prologue/epilogue callbacks can report GC time via
      // Jaeger tracing.
      KJ_DASSERT(impl.currentLock == kj::none, "Isolate lock taken recursively");
      impl.currentLock = *this;

      // Now's a good time to destroy any workers queued up for destruction.
      auto workersToDestroy = impl.workerDestructionQueue.lockExclusive()->pop();
      for (auto& workerImpl: workersToDestroy.asArrayPtr()) {
        KJ_IF_SOME(c, workerImpl->context) {
          disposeContext(kj::mv(c));
        }
        workerImpl = nullptr;
      }

      currentApi = isolate.api.get();
    }
    ~Lock() noexcept(false) {
      currentApi = oldCurrentApi;

#ifdef KJ_DEBUG
      // We lack a KJ_DASSERT_NONNULL because it would have to look a lot like KJ_IF_SOME, thus
      // we use a pragma around KJ_DEBUG here.
      auto& implCurrentLock = KJ_ASSERT_NONNULL(impl.currentLock, "Isolate lock released twice");
      KJ_ASSERT(&implCurrentLock == this, "Isolate lock released recursively");
#endif

      if (shouldReportIsolateMetrics) {
        // The isolate asked this lock to report the stats when it released. Let's do it.
        limitEnforcer.reportMetrics(impl.metrics);
      }
      impl.currentLock = nullptr;
    }
    KJ_DISALLOW_COPY_AND_MOVE(Lock);

    void setupContext(v8::Local<v8::Context> context) {
      // Set WebAssembly.Module @@HasInstance
      setWebAssemblyModuleHasInstance(*lock, context);

      // The V8Inspector implements the `console` object.
      KJ_IF_SOME(i, impl.inspector) {
        i.get()->contextCreated(v8_inspector::V8ContextInfo(context, 1,
            jsg::toInspectorStringView("Worker")));
      }

      // We replace the default V8 console.log(), etc. methods, to give the worker access to
      // logged content, and log formatted values to stdout/stderr locally.
      auto global = context->Global();
      auto consoleStr = jsg::v8StrIntern(lock->v8Isolate, "console");
      auto console = jsg::check(global->Get(context, consoleStr)).As<v8::Object>();
      auto mode = consoleMode;

      auto setHandler = [&](const char* method, LogLevel level) {
        auto methodStr = jsg::v8StrIntern(lock->v8Isolate, method);
        v8::Global<v8::Function> original(
          lock->v8Isolate, jsg::check(console->Get(context, methodStr)).As<v8::Function>());

        auto f = lock->wrapSimpleFunction(context,
            [mode, level, original = kj::mv(original)](jsg::Lock& js,
                const v8::FunctionCallbackInfo<v8::Value>& info) {
          handleLog(js, mode, level, original, info);
        });
        jsg::check(console->Set(context, methodStr, f));
      };

      setHandler("debug", LogLevel::DEBUG_);
      setHandler("error", LogLevel::ERROR);
      setHandler("info", LogLevel::INFO);
      setHandler("log", LogLevel::LOG);
      setHandler("warn", LogLevel::WARN);
    }

    void disposeContext(jsg::JsContext<api::ServiceWorkerGlobalScope> context) {
      lock->withinHandleScope([&] {
        context->clear();
        KJ_IF_SOME(i, impl.inspector) {
          i.get()->contextDestroyed(context.getHandle(*lock));
        }
        { auto drop = kj::mv(context); }
        lock->v8Isolate->ContextDisposedNotification(false);
      });
    }

    void gcPrologue() {
      metrics.gcPrologue();
    }
    void gcEpilogue() {
      metrics.gcEpilogue();
    }

    // Call limitEnforcer->exitJs(), and also schedule to call limitEnforcer->reportMetrics()
    // later. Returns true if condemned. We take a mutable reference to it to make sure the caller
    // believes it has exclusive access.
    bool checkInWithLimitEnforcer(Worker::Isolate& isolate);

  private:
    const Impl& impl;
    IsolateObserver::LockRecord metrics;
    ThreadProgressCounter progressCounter;
    bool shouldReportIsolateMetrics = false;
    const Api* oldCurrentApi;

    const IsolateLimitEnforcer& limitEnforcer;  // only so we can call getIsolateStats()

    ConsoleMode consoleMode;

  public:
    kj::Own<jsg::Lock> lock;
  };

  // Protected by v8::Locker -- if v8::Locker::IsLocked(isolate) is true, then it is safe to access
  // this variable.
  mutable kj::Maybe<Lock&> currentLock;

  static constexpr auto WORKER_DESTRUCTION_QUEUE_INITIAL_SIZE = 8;
  static constexpr auto WORKER_DESTRUCTION_QUEUE_MAX_CAPACITY = 100;

  // Similar in spirit to the deferred destruction queue in jsg::IsolateBase. When a Worker is
  // destroyed, it puts its Impl, which contains objects that need to be destroyed under the isolate
  // lock, into this queue. Our own Isolate::Impl::Lock implementation then clears this queue the
  // next time the isolate is locked, whether that be by a connection thread, or the Worker's own
  // destructor if it owns the last `kj::Own<const Script>` reference.
  //
  // Fairly obviously, this member is protected by its own mutex, not the isolate lock.
  const kj::MutexGuarded<BatchQueue<kj::Own<Worker::Impl>>> workerDestructionQueue {
    WORKER_DESTRUCTION_QUEUE_INITIAL_SIZE,
    WORKER_DESTRUCTION_QUEUE_MAX_CAPACITY
  };
  // TODO(cleanup): The only reason this exists and we can't just rely on the isolate's regular
  //   deferred destruction queue to lazily destroy the various V8 objects in Worker::Impl is
  //   because our GlobalScope object needs to have a function called on it, and any attached
  //   inspector needs to be notified. JSG doesn't know about these things.

  Impl(const Api& api, IsolateObserver& metrics,
       IsolateLimitEnforcer& limitEnforcer, InspectorPolicy inspectorPolicy)
      : metrics(metrics),
        inspectorPolicy(inspectorPolicy),
        actorCacheLru(limitEnforcer.getActorCacheLruOptions()) {
    jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
      auto lock = api.lock(stackScope);
      limitEnforcer.customizeIsolate(lock->v8Isolate);

      if (inspectorPolicy != InspectorPolicy::DISALLOW) {
        // We just created our isolate, so we don't need to use Isolate::Impl::Lock.
        KJ_ASSERT(!isMultiTenantProcess(), "inspector is not safe in multi-tenant processes");
        inspector = v8_inspector::V8Inspector::create(lock->v8Isolate, &inspectorClient);
      }
    });
  }
};

namespace {

class CpuProfilerDisposer final: public kj::Disposer {
public:
  virtual void disposeImpl(void* pointer) const override {
    reinterpret_cast<v8::CpuProfiler*>(pointer)->Dispose();
  }

  static const CpuProfilerDisposer instance;
};

const CpuProfilerDisposer CpuProfilerDisposer::instance {};

static constexpr kj::StringPtr PROFILE_NAME = "Default Profile"_kj;

static void setSamplingInterval(v8::CpuProfiler& profiler, int interval) {
  profiler.SetSamplingInterval(interval);
}

static void startProfiling(jsg::Lock& js, v8::CpuProfiler& profiler) {
  js.withinHandleScope([&] {
    v8::CpuProfilingOptions options(
      v8::kLeafNodeLineNumbers,
      v8::CpuProfilingOptions::kNoSampleLimit
    );
    profiler.StartProfiling(jsg::v8StrIntern(js.v8Isolate, PROFILE_NAME), kj::mv(options));
  });
}

static void stopProfiling(jsg::Lock& js,
                          v8::CpuProfiler& profiler,
                          cdp::Command::Builder& cmd) {
  js.withinHandleScope([&] {
    auto cpuProfile = profiler.StopProfiling(jsg::v8StrIntern(js.v8Isolate, PROFILE_NAME));
    if (cpuProfile == nullptr) return; // profiling never started

    kj::Vector<const v8::CpuProfileNode*> allNodes;
    kj::Vector<const v8::CpuProfileNode*> unvisited;

    unvisited.add(cpuProfile->GetTopDownRoot());
    while (!unvisited.empty()) {
      auto next = unvisited.back();
      allNodes.add(next);
      unvisited.removeLast();
      for (int i=0; i < next->GetChildrenCount(); i++) {
        unvisited.add(next->GetChild(i));
      }
    }

    auto res = cmd.getProfilerStop().initResult();
    auto profile = res.initProfile();
    profile.setStartTime(cpuProfile->GetStartTime());
    profile.setEndTime(cpuProfile->GetEndTime());

    auto nodes = profile.initNodes(allNodes.size());
    for (auto i : kj::indices(allNodes)) {
      auto nodeBuilder = nodes[i];
      nodeBuilder.setId(allNodes[i]->GetNodeId());

      auto callFrame = nodeBuilder.initCallFrame();
      callFrame.setFunctionName(allNodes[i]->GetFunctionNameStr());
      callFrame.setScriptId(kj::str(allNodes[i]->GetScriptId()));
      callFrame.setUrl(allNodes[i]->GetScriptResourceNameStr());
      // V8 locations are 1-based, but CDP locations are 0-based...
      callFrame.setLineNumber(allNodes[i]->GetLineNumber() - 1);
      callFrame.setColumnNumber(allNodes[i]->GetColumnNumber() - 1);

      nodeBuilder.setHitCount(allNodes[i]->GetHitCount());

      auto children = nodeBuilder.initChildren(allNodes[i]->GetChildrenCount());
      for (int j=0; j < allNodes[i]->GetChildrenCount(); j++) {
        children.set(j, allNodes[i]->GetChild(j)->GetNodeId());
      }

      auto hitLineCount = allNodes[i]->GetHitLineCount();
      auto lineBuffer = kj::heapArray<v8::CpuProfileNode::LineTick>(hitLineCount);
      allNodes[i]->GetLineTicks(lineBuffer.begin(), lineBuffer.size());

      auto positionTicks = nodeBuilder.initPositionTicks(hitLineCount);
      for (uint j=0; j < hitLineCount; j++) {
        auto positionTick = positionTicks[j];
        positionTick.setLine(lineBuffer[j].line);
        positionTick.setTicks(lineBuffer[j].hit_count);
      }
    }

    auto sampleCount = cpuProfile->GetSamplesCount();
    auto samples = profile.initSamples(sampleCount);
    auto timeDeltas = profile.initTimeDeltas(sampleCount);
    auto lastTimestamp = cpuProfile->GetStartTime();
    for (int i=0; i < sampleCount; i++) {
      samples.set(i, cpuProfile->GetSample(i)->GetNodeId());
      auto sampleTime = cpuProfile->GetSampleTimestamp(i);
      timeDeltas.set(i, sampleTime - lastTimestamp);
      lastTimestamp = sampleTime;
    }
  });
}

} // anonymous namespace

struct Worker::Script::Impl {
  kj::OneOf<jsg::NonModuleScript, kj::Path> unboundScriptOrMainModule;

  kj::Array<CompiledGlobal> globals;

  kj::Maybe<jsg::JsContext<api::ServiceWorkerGlobalScope>> moduleContext;

  // If set, then any attempt to use this script shall throw this exception.
  kj::Maybe<kj::Exception> permanentException;

  struct DynamicImportResult {
    jsg::Value value;
    bool isException = false;
    DynamicImportResult(jsg::Value value, bool isException = false)
        : value(kj::mv(value)), isException(isException) {}
  };
  using DynamicImportHandler = kj::Function<jsg::Value()>;

  void configureDynamicImports(jsg::Lock& js, jsg::ModuleRegistry& modules) {
    static auto constexpr handleDynamicImport =
        [](kj::Own<const Worker> worker,
           DynamicImportHandler handler,
           kj::Maybe<jsg::Ref<jsg::AsyncContextFrame>> asyncContext) ->
               kj::Promise<DynamicImportResult> {
      co_await kj::evalLater([] {});
      auto asyncLock = co_await worker->takeAsyncLockWithoutRequest(nullptr);

      co_return worker->runInLockScope(asyncLock, [&](Worker::Lock& lock) {
        return JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(),
            [&](jsg::Lock& js) {
          jsg::AsyncContextFrame::Scope asyncContextScope(js, asyncContext);

          // We have to wrap the call to handler in a try catch here because
          // we have to tunnel any jsg::JsExceptionThrown instances back.
          v8::TryCatch tryCatch(js.v8Isolate);
          kj::Maybe<kj::Exception> maybeLimitError;
          try {
            auto limitScope = worker->getIsolate().getLimitEnforcer()
                .enterDynamicImportJs(lock, maybeLimitError);
            return DynamicImportResult(handler());
          } catch (jsg::JsExceptionThrown&) {
            // Handled below...
          } catch (kj::Exception& ex) {
            kj::throwFatalException(kj::mv(ex));
          }

          KJ_ASSERT(tryCatch.HasCaught());
          if (!tryCatch.CanContinue() || tryCatch.Exception().IsEmpty()) {
            // There's nothing else we can do here but throw a generic fatal exception.
            KJ_IF_SOME(limitError, maybeLimitError) {
              kj::throwFatalException(kj::mv(limitError));
            } else {
              kj::throwFatalException(JSG_KJ_EXCEPTION(FAILED, Error,
                  "Failed to load dynamic module."));
            }
          }
          return DynamicImportResult(js.v8Ref(tryCatch.Exception()), true);
        });
      });
    };

    modules.setDynamicImportCallback([](jsg::Lock& js, DynamicImportHandler handler) mutable {
      if (IoContext::hasCurrent()) {
        // If we are within the scope of a IoContext, then we are going to pop
        // out of it to perform the actual module instantiation.

        auto& context = IoContext::current();

        return context.awaitIo(js,
            handleDynamicImport(kj::atomicAddRef(context.getWorker()),
                                kj::mv(handler),
                                jsg::AsyncContextFrame::currentRef(js)),
            [](jsg::Lock& js, DynamicImportResult result) {
          if (result.isException) {
            return js.rejectedPromise<jsg::Value>(kj::mv(result.value));
          }
          return js.resolvedPromise(kj::mv(result.value));
        });
      }

      // If we got here, there is no current IoContext. We're going to perform the
      // module resolution synchronously and we do not have to worry about blocking any
      // i/o. We get here, for instance, when dynamic import is used at the top level of
      // a script (which is weird, but allowed).
      //
      // We do not need to use limitEnforcer.enterDynamicImportJs() here because this should
      // already be covered by the startup resource limiter.
      return js.resolvedPromise(handler());
    });
  }
};

namespace {

// Given an array of strings, return a valid serialized JSON string like:
//   {"flags":["minimal_subrequests",...]}
//
// Return null if the array is empty.
kj::Maybe<kj::String> makeCompatJson(kj::ArrayPtr<kj::StringPtr> enableFlags) {
  if (enableFlags.size() == 0) {
    return kj::none;
  }

  // Calculate the size of the string we're going to generate.
  constexpr auto PREFIX = "{\"flags\":["_kj;
  constexpr auto SUFFIX = "]}"_kj;
  uint size = std::accumulate(enableFlags.begin(), enableFlags.end(),
      // We need two quotes and one comma for each enable-flag past the first, plus a NUL char.
      PREFIX.size() + SUFFIX.size() + 3 * enableFlags.size(),
      [](uint z, kj::StringPtr s) { return z + s.size(); });

  kj::Vector<char> json(size);

  json.addAll(PREFIX);

  bool first = true;
  for (auto flag: enableFlags) {
    if (first) {
      first = false;
    } else {
      json.add(',');
    }

    json.add('"');

    for (auto& c: flag.asArray()) {
      // TODO(cleanup): Copied from simpleJsonStringCheck(). Hopefully this will
      //   go away forever soon.
      KJ_REQUIRE(c != '\"');
      KJ_REQUIRE(c != '\\');
      KJ_REQUIRE(c >= 0x20);
    }
    json.addAll(flag);

    json.add('"');
  }

  json.addAll(SUFFIX);
  json.add('\0');

  return kj::String(json.releaseAsArray());
}

// When a promise is created in a different IoContext, we need to use a
// kj::CrossThreadFulfiller in order to wait on it. The Waiter instance will
// be held on the Promise itself, and will be fulfilled/rejected when the
// promise is resolved or rejected. This will signal all of the waiters
// from other IoContexts.
jsg::Promise<void> addCrossThreadPromiseWaiter(jsg::Lock& js,
                                               v8::Local<v8::Promise>& promise) {
  auto waiter = kj::newPromiseAndCrossThreadFulfiller<void>();

  struct Waiter: public kj::Refcounted {
    kj::Maybe<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> fulfiller;
    void done() {
      KJ_IF_SOME(f, fulfiller) {
        // Done this way so that the fulfiller is released as soon as possible
        // when done as the JS promise may not clean up reactions right away.
        f->fulfill();
        fulfiller = kj::none;
      }
    }
    Waiter(kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller)
        : fulfiller(kj::mv(fulfiller)) {}
  };

  auto fulfiller = kj::refcounted<Waiter>(kj::mv(waiter.fulfiller));

  auto onSuccess = [waiter=kj::addRef(*fulfiller)](jsg::Lock& js, jsg::Value value) mutable {
    waiter->done();
  };

  auto onFailure = [waiter=kj::mv(fulfiller)](jsg::Lock& js, jsg::Value exception) mutable {
    waiter->done();
  };

  js.toPromise(promise).then(js, kj::mv(onSuccess), kj::mv(onFailure));

  return IoContext::current().awaitIo(js, kj::mv(waiter.promise));
}

struct HeapSnapshotDeleter: public kj::Disposer {
  static const HeapSnapshotDeleter INSTANCE;
  void disposeImpl(void* ptr) const override {
    auto snapshot = const_cast<v8::HeapSnapshot*>(static_cast<const v8::HeapSnapshot*>(ptr));
    snapshot->Delete();
  }
};
const HeapSnapshotDeleter HeapSnapshotDeleter::INSTANCE;

}  // namespace

Worker::Isolate::Isolate(kj::Own<Api> apiParam,
                         kj::Own<IsolateObserver>&& metricsParam,
                         kj::StringPtr id,
                         kj::Own<IsolateLimitEnforcer> limitEnforcerParam,
                         InspectorPolicy inspectorPolicy,
                         ConsoleMode consoleMode)
    : id(kj::str(id)),
      limitEnforcer(kj::mv(limitEnforcerParam)),
      api(kj::mv(apiParam)),
      consoleMode(consoleMode),
      featureFlagsForFl(makeCompatJson(decompileCompatibilityFlagsForFl(api->getFeatureFlags()))),
      metrics(kj::mv(metricsParam)),
      impl(kj::heap<Impl>(*api, *metrics, *limitEnforcer, inspectorPolicy)),
      weakIsolateRef(WeakIsolateRef::wrap(this)),
      traceAsyncContextKey(kj::refcounted<jsg::AsyncContextFrame::StorageKey>()) {
  metrics->created();
  // We just created our isolate, so we don't need to use Isolate::Impl::Lock (nor an async lock).
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    auto lock = api->lock(stackScope);
    auto features = api->getFeatureFlags();

    KJ_DASSERT(lock->v8Isolate->GetNumberOfDataSlots() >= 3);
    KJ_DASSERT(lock->v8Isolate->GetData(3) == nullptr);
    lock->v8Isolate->SetData(3, this);

    lock->setCaptureThrowsAsRejections(features.getCaptureThrowsAsRejections());
    lock->setCommonJsExportDefault(features.getExportCommonJsDefaultNamespace());

    if (impl->inspector != kj::none || ::kj::_::Debug::shouldLog(::kj::LogSeverity::INFO)) {
      lock->setLoggerCallback([this](jsg::Lock& js, kj::StringPtr message) {
        if (impl->inspector != kj::none) {
          logMessage(js, static_cast<uint16_t>(cdp::LogType::WARNING), message);
        }
        KJ_LOG(INFO, "console warning", message);
      });
    }

    // By default, V8's memory pressure level is "none". This tells V8 that no one else on the
    // machine is competing for memory so it might as well use all it wants and be lazy about GC.
    //
    // In our production environment, however, we can safely assume that there is always memory
    // pressure, because every machine is handling thousands of tenants all the time. So we might
    // as well just throw the switch to "moderate" right away.
    lock->v8Isolate->MemoryPressureNotification(v8::MemoryPressureLevel::kModerate);

    // Register GC prologue and epilogue callbacks so that we can report GC CPU time via the
    // "request_context" Jaeger span.
    lock->v8Isolate->AddGCPrologueCallback(
        [](v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags, void* data) noexcept {
      // We assume that a v8::Locker is alive during GC.
      KJ_DASSERT(v8::Locker::IsLocked(isolate));
      auto& self = *reinterpret_cast<Isolate*>(data);
      // However, currentLock might not be available, if (like in our Worker::Isolate constructor) we
      // don't use a Worker::Isolate::Impl::Lock.
      KJ_IF_SOME(currentLock, self.impl->currentLock) {
        currentLock.gcPrologue();
      }
    }, this);
    lock->v8Isolate->AddGCEpilogueCallback(
        [](v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags, void* data) noexcept {
      // We make similar assumptions about v8::Locker and currentLock as in the prologue callback.
      KJ_DASSERT(v8::Locker::IsLocked(isolate));
      auto& self = *reinterpret_cast<Isolate*>(data);
      KJ_IF_SOME(currentLock, self.impl->currentLock) {
        currentLock.gcEpilogue();
      }
    }, this);
    lock->v8Isolate->SetPromiseRejectCallback([](v8::PromiseRejectMessage message) {
      // TODO(cleanup): IoContext doesn't really need to be involved here. We are trying to call
      // a method of ServiceWorkerGlobalScope, which is the context object. So we should be able to
      // do something like unwrap(isolate->GetCurrentContext()).emitPromiseRejection(). However, JSG
      // doesn't currently provide an easy way to do this.
      if (IoContext::hasCurrent()) {
        try {
          IoContext::current().reportPromiseRejectEvent(message);
        } catch (jsg::JsExceptionThrown&) {
          // V8 expects us to just return.
          return;
        }
      }
    });

    // The PromiseCrossContextCallback is used to allow cross-IoContext promise following.
    // When the IoContext scope is entered, we set the "promise context tag" associated
    // with the IoContext on the Isolate that is locked. Any Promise that is created within
    // that scope will be tagged with the same promise context tag. When an attempt to
    // follow a promise occurs (e.g. either using Promise.prototype.then() or await, etc)
    // our patched v8 logic will check to see if the followed promise's tag matches the
    // current Isolate tag. If they do not, then v8 will invoke this callback. The promise
    // here is the promise that belongs to a different IoContext.
    lock->v8Isolate->SetPromiseCrossContextCallback([](v8::Local<v8::Context> context,
                                                      v8::Local<v8::Promise> promise,
                                                      v8::Local<v8::Object> tag) ->
                                                        v8::MaybeLocal<v8::Promise> {
      try {
        auto& js = jsg::Lock::from(context->GetIsolate());

        // Generally this condition is only going to happen when using dynamic imports.
        // It should not be common.
        JSG_REQUIRE(IoContext::hasCurrent(), Error,
            "Unable to wait on a promise created within a request when not running within a "
            "request.");

        return js.wrapSimplePromise(addCrossThreadPromiseWaiter(js, promise).then(js,
            [promise=js.v8Ref(promise.As<v8::Value>())](auto& js) mutable {
          // Once the waiter has been resolved, return the now settled promise.
          // Since the promise has been settled, it is now safe to access from
          // other requests. Note that the resolved value of the promise still
          // might not be safe to access! (e.g. if it contains any IoOwns attached
          // to the other request IoContext).
          return kj::mv(promise);
        }));
      } catch (jsg::JsExceptionThrown&) {
        // Exceptions here are generally unexpected but possible because the jsg::Promise
        // then can fail if the isolate is in the process of being torn down. Let's just
        // return control back to V8 which should handle the case.
        return v8::MaybeLocal<v8::Promise>();
      } catch (...) {
        auto ex = kj::getCaughtExceptionAsKj();
        KJ_LOG(ERROR, "Setting promise cross context follower failed unexpectedly", ex);
        jsg::throwInternalError(context->GetIsolate(), kj::mv(ex));
        return v8::MaybeLocal<v8::Promise>();
      }
    });
  });
}

Worker::Script::Script(kj::Own<const Isolate> isolateParam, kj::StringPtr id,
                       Script::Source source, IsolateObserver::StartType startType,
                       bool logNewScript, kj::Maybe<ValidationErrorReporter&> errorReporter)
    : isolate(kj::mv(isolateParam)),
      id(kj::str(id)),
      modular(source.is<ModulesSource>()),
      impl(kj::heap<Impl>()) {
  auto parseMetrics = isolate->metrics->parse(startType);
  // TODO(perf): It could make sense to take an async lock when constructing a script if we
  //   co-locate multiple scripts in the same isolate. As of this writing, we do not, except in
  //   previews, where it doesn't matter. If we ever do co-locate multiple scripts in the same
  //   isolate, we may wish to make the RequestObserver object available here, in order to
  //   attribute lock timing to that request.
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    Isolate::Impl::Lock recordedLock(*isolate,
        Worker::Lock::TakeSynchronously(kj::none), stackScope);
    auto& lock = *recordedLock.lock;

    // If we throw an exception, it's important that `impl` is destroyed under lock.
    KJ_ON_SCOPE_FAILURE({
      auto implToDestroy = kj::mv(impl);
      KJ_IF_SOME(c, implToDestroy->moduleContext) {
        recordedLock.disposeContext(kj::mv(c));
      } else {
        // Else block to avoid dangling else clang warning.
      }
    });

    lock.withinHandleScope([&] {
      if (isolate->impl->inspector != kj::none || errorReporter != kj::none) {
        lock.v8Isolate->SetCaptureStackTraceForUncaughtExceptions(true);
      }

      v8::Local<v8::Context> context;
      if (modular) {
        // Modules can't be compiled for multiple contexts. We need to create the real context now.
        auto& mContext = impl->moduleContext.emplace(isolate->getApi().newContext(lock));
        mContext->enableWarningOnSpecialEvents();
        context = mContext.getHandle(lock);
        recordedLock.setupContext(context);
      } else {
        // Although we're going to compile a script independent of context, V8 requires that there be
        // an active context, otherwise it will segfault, I guess. So we create a dummy context.
        // (Undocumented, as usual.)
        context = v8::Context::New(
            lock.v8Isolate, nullptr, v8::ObjectTemplate::New(lock.v8Isolate));
      }

      JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](jsg::Lock& js) {
        // const_cast OK because we hold the isolate lock.
        Worker::Isolate& lockedWorkerIsolate = const_cast<Isolate&>(*isolate);

        if (logNewScript) {
          // HACK: Log a message indicating that a new script was loaded. This is used only when the
          //   inspector is enabled. We want to do this immediately after the context is created,
          //   before the user gets a chance to modify the behavior of the console, which if they did,
          //   we'd then need to be more careful to apply time limits and such.
          lockedWorkerIsolate.logMessage(lock,
              static_cast<uint16_t>(cdp::LogType::WARNING), "Script modified; context reset.");
        }

        // We need to register this context with the inspector, otherwise errors won't be reported. But
        // we want it to be un-registered as soon as the script has been compiled, otherwise the
        // inspector will end up with multiple contexts active which is very confusing for the user
        // (since they'll have to select from the drop-down which context to use).
        //
        // (For modules, the context was already registered by `setupContext()`, above.
        KJ_IF_SOME(i, isolate->impl->inspector) {
          if (!source.is<ModulesSource>()) {
            i.get()->contextCreated(v8_inspector::V8ContextInfo(context,
                1, jsg::toInspectorStringView("Compiler")));
          }
        } else {}  // Here to squash a compiler warning
        KJ_DEFER({
          if (!source.is<ModulesSource>()) {
            KJ_IF_SOME(i, isolate->impl->inspector) {
              i.get()->contextDestroyed(context);
            } else {
              // Else block to avoid dangling else clang warning.
            }
          }
        });

        v8::TryCatch catcher(lock.v8Isolate);
        kj::Maybe<kj::Exception> maybeLimitError;

        try {
          try {
            KJ_SWITCH_ONEOF(source) {
              KJ_CASE_ONEOF(script, ScriptSource) {
                impl->globals = script.compileGlobals(lock, isolate->getApi(), isolate->impl->metrics);

                {
                  // It's unclear to me if CompileUnboundScript() can get trapped in any infinite loops or
                  // excessively-expensive computation requiring a time limit. We'll go ahead and apply a time
                  // limit just to be safe. Don't add it to the rollover bank, though.
                  auto limitScope = isolate->getLimitEnforcer().enterStartupJs(lock, maybeLimitError);
                  impl->unboundScriptOrMainModule =
                      jsg::NonModuleScript::compile(script.mainScript, lock, script.mainScriptName);
                }

                break;
              }

              KJ_CASE_ONEOF(modulesSource, ModulesSource) {
                auto limitScope = isolate->getLimitEnforcer().enterStartupJs(lock, maybeLimitError);
                auto& modules = KJ_ASSERT_NONNULL(impl->moduleContext)->getModuleRegistry();
                impl->configureDynamicImports(lock, modules);
                modulesSource.compileModules(lock, isolate->getApi());
                impl->unboundScriptOrMainModule = kj::Path::parse(modulesSource.mainModule);
                break;
              }
            }

            parseMetrics->done();
          } catch (const kj::Exception& e) {
            lock.throwException(kj::cp(e));
            // lock.throwException() here will throw a jsg::JsExceptionThrown which we catch
            // in the outer try/catch.
          }
        } catch (const jsg::JsExceptionThrown&) {
          reportStartupError(id,
                            lock,
                            isolate->impl->inspector,
                            isolate->getLimitEnforcer(),
                            kj::mv(maybeLimitError),
                            catcher,
                            errorReporter,
                            impl->permanentException);
        }
      });
    });
  });
}

kj::Own<const Worker::Isolate::WeakIsolateRef> Worker::Isolate::getWeakRef() const {
  return weakIsolateRef->addRef();
}

Worker::Isolate::~Isolate() noexcept(false) {
  metrics->teardownStarted();

  // Update the isolate stats one last time to make sure we're accurate for cleanup in
  // `evicted()`.
  limitEnforcer->reportMetrics(*metrics);

  metrics->evicted();
  weakIsolateRef->invalidate();

  // Make sure to destroy things under lock. This lock should never be contended since the isolate
  // is about to be destroyed, but we have to take the lock in order to enter the isolate.
  // It's also important that we lock one last time, in order to destroy any remaining workers in
  // worker destruction queue.
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    Isolate::Impl::Lock recordedLock(*this, Worker::Lock::TakeSynchronously(kj::none), stackScope);
    metrics->teardownLockAcquired();
    auto inspector = kj::mv(impl->inspector);
    auto dropTraceAsyncContextKey = kj::mv(traceAsyncContextKey);
  });
}

Worker::Script::~Script() noexcept(false) {
  // Make sure to destroy things under lock.
  // TODO(perf): It could make sense to try to obtain an async lock before destroying a script if
  //   multiple scripts are co-located in the same isolate. As of this writing, that doesn't happen
  //   except in preview. In any case, Scripts are destroyed in the GC thread, where we don't care
  //   too much about lock latency.
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    Isolate::Impl::Lock recordedLock(*isolate,
        Worker::Lock::TakeSynchronously(kj::none), stackScope);
    KJ_IF_SOME(c, impl->moduleContext) {
      recordedLock.disposeContext(kj::mv(c));
    }
    impl = nullptr;
  });
}

const Worker::Isolate& Worker::Isolate::from(jsg::Lock& js) {
  auto ptr = js.v8Isolate->GetData(3);
  KJ_ASSERT(ptr != nullptr);
  return *static_cast<const Worker::Isolate*>(ptr);
}

bool Worker::Isolate::Impl::Lock::checkInWithLimitEnforcer(Worker::Isolate& isolate) {
  shouldReportIsolateMetrics = true;
  return limitEnforcer.exitJs(*lock);
}

// EW-1319: Set WebAssembly.Module @@HasInstance
//
// The instanceof operator can be changed by setting the @@HasInstance method
// on the object, https://tc39.es/ecma262/#sec-instanceofoperator.
void setWebAssemblyModuleHasInstance(jsg::Lock& lock, v8::Local<v8::Context> context) {
  auto instanceof = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    jsg::Lock::from(info.GetIsolate()).withinHandleScope([&] {
      info.GetReturnValue().Set(info[0]->IsWasmModuleObject());
    });
  };
  v8::Local<v8::Function> function = jsg::check(v8::Function::New(context, instanceof));

  v8::Object* webAssembly = v8::Object::Cast(*jsg::check(
      context->Global()->Get(context, jsg::v8StrIntern(lock.v8Isolate, "WebAssembly"))));
  v8::Object* module = v8::Object::Cast(*jsg::check(
      webAssembly->Get(context, jsg::v8StrIntern(lock.v8Isolate, "Module"))));

  jsg::check(module->DefineOwnProperty(
      context, v8::Symbol::GetHasInstance(lock.v8Isolate), function));
}

// =======================================================================================

Worker::Worker(kj::Own<const Script> scriptParam,
               kj::Own<WorkerObserver> metricsParam,
               kj::FunctionParam<void(
                      jsg::Lock& lock, const Api& api,
                      v8::Local<v8::Object> target)> compileBindings,
               IsolateObserver::StartType startType,
               SpanParent parentSpan, LockType lockType,
               kj::Maybe<ValidationErrorReporter&> errorReporter)
    : script(kj::mv(scriptParam)),
      metrics(kj::mv(metricsParam)),
      impl(kj::heap<Impl>()){
  // Enter/lock isolate.
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    Isolate::Impl::Lock recordedLock(*script->isolate, lockType, stackScope);
    auto& lock = *recordedLock.lock;

    // If we throw an exception, it's important that `impl` is destroyed under lock.
    KJ_ON_SCOPE_FAILURE({
      auto implToDestroy = kj::mv(impl);
      KJ_IF_SOME(c, implToDestroy->context) {
        recordedLock.disposeContext(kj::mv(c));
      } else {
        // Else block to avoid dangling else clang warning.
      }
    });

    auto maybeMakeSpan = [&](auto operationName) -> SpanBuilder {
      auto span = parentSpan.newChild(kj::mv(operationName));
      if (span.isObserved()) {
        span.setTag("truncated_script_id"_kjc, truncateScriptId(script->getId()));
      }
      return span;
    };

    auto currentSpan = maybeMakeSpan("lw:new_startup_metrics"_kjc);

    auto startupMetrics = metrics->startup(startType);

    currentSpan = maybeMakeSpan("lw:new_context"_kjc);

    // Create a stack-allocated handle scope.
    lock.withinHandleScope([&] {
      jsg::JsContext<api::ServiceWorkerGlobalScope>* jsContext;

      KJ_IF_SOME(c, script->impl->moduleContext) {
        // Use the shared context from the script.
        // const_cast OK because guarded by `lock`.
        jsContext = const_cast<jsg::JsContext<api::ServiceWorkerGlobalScope>*>(&c);
        currentSpan.setTag("module_context"_kjc, true);
      } else {
        // Create a new context.
        jsContext = &this->impl->context.emplace(script->isolate->getApi().newContext(lock));
      }

      v8::Local<v8::Context> context = KJ_REQUIRE_NONNULL(jsContext).getHandle(lock);
      if (!script->modular) {
        recordedLock.setupContext(context);
      }

      if (script->impl->unboundScriptOrMainModule == nullptr) {
        // Script failed to parse. Act as if the script was empty -- i.e. do nothing.
        impl->permanentException =
            script->impl->permanentException.map([](auto& e) { return kj::cp(e); });
        return;
      }

      // Enter the context for compiling and running the script.
      JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](jsg::Lock& js) {
        v8::TryCatch catcher(lock.v8Isolate);
        kj::Maybe<kj::Exception> maybeLimitError;

        try {
          try {
            currentSpan = maybeMakeSpan("lw:globals_instantiation"_kjc);

            v8::Local<v8::Object> bindingsScope;
            if (script->isModular()) {
              // Use `env` variable.
              bindingsScope = v8::Object::New(lock.v8Isolate);
            } else {
              // Use global-scope bindings.
              bindingsScope = context->Global();
            }

            // Load globals.
            // const_cast OK because we hold the lock.
            for (auto& global: const_cast<Script&>(*script).impl->globals) {
              lock.v8Set(bindingsScope, global.name, global.value);
            }

            compileBindings(lock, script->isolate->getApi(), bindingsScope);

            // Execute script.
            currentSpan = maybeMakeSpan("lw:top_level_execution"_kjc);

            KJ_SWITCH_ONEOF(script->impl->unboundScriptOrMainModule) {
              KJ_CASE_ONEOF(unboundScript, jsg::NonModuleScript) {
                auto limitScope = script->isolate->getLimitEnforcer().enterStartupJs(lock, maybeLimitError);
                unboundScript.run(lock.v8Context());
              }
              KJ_CASE_ONEOF(mainModule, kj::Path) {
                auto& registry = (*jsContext)->getModuleRegistry();
                KJ_IF_SOME(entry, registry.resolve(lock, mainModule, kj::none)) {
                  JSG_REQUIRE(entry.maybeSynthetic == kj::none, TypeError,
                              "Main module must be an ES module.");
                  auto module = entry.module.getHandle(lock);

                  {
                    auto limitScope = script->isolate->getLimitEnforcer()
                        .enterStartupJs(lock, maybeLimitError);

                    jsg::instantiateModule(lock, module);
                  }

                  if (maybeLimitError != kj::none) {
                    // If we hit the limit in PerformMicrotaskCheckpoint() we may not have actually
                    // thrown an exception.
                    throw jsg::JsExceptionThrown();
                  }

                  v8::Local<v8::Value> ns = module->GetModuleNamespace();

                  {
                    // The V8 module API is weird. Only the first call to Evaluate() will evaluate the
                    // module, even if subsequent calls pass a different context. Verify that we didn't
                    // switch contexts.
                    auto creationContext = jsg::check(ns.As<v8::Object>()->GetCreationContext());
                    KJ_ASSERT(creationContext == context,
                        "module was originally instantiated in a different context");
                  }

                  impl->env = lock.v8Ref(bindingsScope.As<v8::Value>());

                  auto& api = script->isolate->getApi();
                  auto handlers = api.unwrapExports(lock, ns);
                  auto entrypointClasses = api.getEntrypointClasses(lock);

                  for (auto& handler: handlers.fields) {
                    KJ_SWITCH_ONEOF(handler.value) {
                      KJ_CASE_ONEOF(obj, api::ExportedHandler) {
                        obj.env = lock.v8Ref(bindingsScope.As<v8::Value>());
                        obj.ctx = jsg::alloc<api::ExecutionContext>();

                        impl->namedHandlers.insert(kj::mv(handler.name), kj::mv(obj));
                      }
                      KJ_CASE_ONEOF(cls, EntrypointClass) {
                        js.withinHandleScope([&]() {
                          jsg::JsObject handle(KJ_ASSERT_NONNULL(cls.tryGetHandle(js.v8Isolate)));

                          for (;;) {
                            if (handle == entrypointClasses.durableObject) {
                              impl->actorClasses.insert(kj::mv(handler.name), Impl::ActorClassInfo {
                                .cls = kj::mv(cls),
                                .missingSuperclass = false,
                              });
                              return;
                            } else if (handle == entrypointClasses.workerEntrypoint) {
                              impl->statelessClasses.insert(kj::mv(handler.name), kj::mv(cls));
                              return;
                            }

                            handle = KJ_UNWRAP_OR(handle.getPrototype().tryCast<jsg::JsObject>(), {
                              // Reached end of prototype chain.

                              // For historical reasons, we assume a class is a Durable Object
                              // class if it doesn't inherit anything.
                              // TODO(someday): Log a warning suggesting extending DurableObject.
                              // TODO(someday): Introduce a compat flag that makes this required.
                              impl->actorClasses.insert(kj::mv(handler.name), Impl::ActorClassInfo {
                                .cls = kj::mv(cls),
                                .missingSuperclass = true,
                              });
                              return;
                            });
                          }
                        });
                      }
                    }
                  }
                } else {
                  JSG_FAIL_REQUIRE(TypeError, "Main module name is not present in bundle.");
                }
              }
            }

            startupMetrics->done();
          } catch (const kj::Exception& e) {
            lock.throwException(kj::cp(e));
            // lock.throwException() here will throw a jsg::JsExceptionThrown which we catch
            // in the outer try/catch.
          }
        } catch (const jsg::JsExceptionThrown&) {
          reportStartupError(script->id,
                            lock,
                            script->isolate->impl->inspector,
                            script->isolate->getLimitEnforcer(),
                            kj::mv(maybeLimitError),
                            catcher,
                            errorReporter,
                            impl->permanentException);
        }
      });
    });
  });
}

Worker::~Worker() noexcept(false) {
  metrics->teardownStarted();

  auto& isolateImpl = *script->getIsolate().impl;
  auto lock = isolateImpl.workerDestructionQueue.lockExclusive();

  // Previously, this metric meant the isolate lock. We might as well make it mean the worker
  // destruction queue lock now to verify it is much less-contended than the isolate lock.
  metrics->teardownLockAcquired();

  // Defer destruction of our V8 objects, in particular our jsg::Context, which requires some
  // finalization.
  lock->push(kj::mv(impl));
}

void Worker::handleLog(jsg::Lock& js, ConsoleMode consoleMode, LogLevel level,
                          const v8::Global<v8::Function>& original,
                          const v8::FunctionCallbackInfo<v8::Value>& info) {
  // Call original V8 implementation so messages sent to connected inspector if any
  auto context = js.v8Context();
  int length = info.Length();
  v8::Local<v8::Value> args[length + 1]; // + 1 used for `colors` later
  for (auto i: kj::zeroTo(length)) args[i] = info[i];
  jsg::check(original.Get(js.v8Isolate)->Call(context, info.This(), length, args));

  // The TryCatch is initialised here to catch cases where the v8 isolate's execution is
  // terminating, usually as a result of an infinite loop. We need to perform the initialisation
  // here because `message` is called multiple times.
  v8::TryCatch tryCatch(js.v8Isolate);
  auto message = [&]() {
    int length = info.Length();
    kj::Vector<kj::String> stringified(length);
    for (auto i: kj::zeroTo(length)) {
      auto arg = info[i];
      // serializeJson and v8::Value::ToString can throw JS exceptions
      // (e.g. for recursive objects) so we eat them here, to ensure logging and non-logging code
      // have the same exception behavior.
      if (!tryCatch.CanContinue()) {
        stringified.add(kj::str("{}"));
        break;
      }
      // The following code checks the `arg` to see if it should be serialised to JSON.
      //
      // We use the following criteria: if arg is null, a number, a boolean, an array, a string, an
      // object or it defines a `toJSON` property that is a function, then the arg gets serialised
      // to JSON.
      //
      // Otherwise we stringify the argument.
      js.withinHandleScope([&] {
        auto context = js.v8Context();
        bool shouldSerialiseToJson = false;
        if (arg->IsNull() ||
            arg->IsNumber() ||
            arg->IsArray() ||
            arg->IsBoolean() ||
            arg->IsString() ||
            arg->IsUndefined()) { // This is special cased for backwards compatibility.
          shouldSerialiseToJson = true;
        }
        if (arg->IsObject()) {
          v8::Local<v8::Object> obj = arg.As<v8::Object>();
          v8::Local<v8::Object> freshObj = v8::Object::New(js.v8Isolate);

          // Determine whether `obj` is constructed using `{}` or `new Object()`. This ensures
          // we don't serialise values like Promises to JSON.
          if (obj->GetPrototype()->SameValue(freshObj->GetPrototype()) ||
              obj->GetPrototype()->IsNull()) {
            shouldSerialiseToJson = true;
          }

          // Check if arg has a `toJSON` property which is a function.
          auto toJSONStr = jsg::v8StrIntern(js.v8Isolate, "toJSON"_kj);
          v8::MaybeLocal<v8::Value> toJSON = obj->GetRealNamedProperty(context, toJSONStr);
          if (!toJSON.IsEmpty()) {
            if (jsg::check(toJSON)->IsFunction()) {
              shouldSerialiseToJson = true;
            }
          }
        }

       if (kj::runCatchingExceptions([&]() {
          // On the off chance the the arg is the request.cf object, let's make
          // sure we do not log proxied fields here.
          if (shouldSerialiseToJson) {
            auto s = js.serializeJson(arg);
            // serializeJson returns the string "undefined" for some values (undefined,
            // Symbols, functions).  We remap these values to null to ensure valid JSON output.
            if (s == "undefined"_kj) {
              stringified.add(kj::str("null"));
            } else {
              stringified.add(kj::mv(s));
            }
          } else {
            stringified.add(js.serializeJson(jsg::check(arg->ToString(context))));
          }
        }) != kj::none) {
          stringified.add(kj::str("{}"));
        };
      });
    }
    return kj::str("[", kj::delimited(stringified, ", "_kj), "]");
  };

  // Only check tracing if console.log() was not invoked at the top level.
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    KJ_IF_SOME(tracer, ioContext.getWorkerTracer()) {
      auto timestamp = ioContext.now();
      tracer.log(timestamp, level, message());
    }
  }

  if (consoleMode == ConsoleMode::INSPECTOR_ONLY) {
    // Lets us dump console.log()s to stdout when running test-runner with --verbose flag, to make
    // it easier to debug tests.  Note that when --verbose is not passed, KJ_LOG(INFO, ...) will
    // not even evaluate its arguments, so `message()` will not be called at all.
    KJ_LOG(INFO, "console.log()", message());
  } else {
    // Write to stdio if allowed by console mode
    static ColorMode COLOR_MODE = permitsColor();
#if _WIN32
    static bool STDOUT_TTY = _isatty(_fileno(stdout));
    static bool STDERR_TTY = _isatty(_fileno(stderr));
#else
    static bool STDOUT_TTY = isatty(STDOUT_FILENO);
    static bool STDERR_TTY = isatty(STDERR_FILENO);
#endif

    // Log warnings and errors to stderr
    auto useStderr = level >= LogLevel::WARN;
    auto fd  = useStderr ? stderr     : stdout;
    auto tty = useStderr ? STDERR_TTY : STDOUT_TTY;
    auto colors = COLOR_MODE == ColorMode::ENABLED ||
      (COLOR_MODE == ColorMode::ENABLED_IF_TTY && tty);

    auto registry = jsg::ModuleRegistry::from(js);
    auto inspectModule = registry->resolveInternalImport(js, "node-internal:internal_inspect"_kj);
    auto inspectModuleHandle = inspectModule.getHandle(js).As<v8::Object>();
    auto formatLog = js.v8Get(inspectModuleHandle, "formatLog"_kj).As<v8::Function>();

    auto recv = js.v8Undefined();
    args[length] = v8::Boolean::New(js.v8Isolate, colors);
    auto formatted = js.toString(jsg::check(formatLog->Call(context, recv, length + 1, args)));
    fprintf(fd, "%s\n", formatted.cStr());
    fflush(fd);
  }
}

Worker::Lock::TakeSynchronously::TakeSynchronously(
    kj::Maybe<RequestObserver&> requestParam) {
  KJ_IF_SOME(r, requestParam) {
    request = &r;
  }
}

kj::Maybe<RequestObserver&> Worker::Lock::TakeSynchronously::getRequest() {
  if (request != nullptr) {
    return *request;
  }
  return kj::none;
}

struct Worker::Lock::Impl {
  Isolate::Impl::Lock recordedLock;
  jsg::Lock& inner;

  Impl(const Worker& worker, LockType lockType, jsg::V8StackScope& stackScope)
      : recordedLock(worker.getIsolate(), lockType, stackScope),
        inner(*recordedLock.lock) {}
};

Worker::Lock::Lock(const Worker& constWorker, LockType lockType, jsg::V8StackScope& stackScope)
    : // const_cast OK because we took out a lock.
      worker(const_cast<Worker&>(constWorker)),
      impl(kj::heap<Impl>(worker, lockType, stackScope)) {
  kj::requireOnStack(this, "Worker::Lock MUST be allocated on the stack.");
}

Worker::Lock::~Lock() noexcept(false) {
  // const_cast OK because we hold -- nay, we *are* -- a lock on the script.
  auto& isolate = const_cast<Isolate&>(worker.getIsolate());
  if (impl->recordedLock.checkInWithLimitEnforcer(isolate)) {
    isolate.disconnectInspector();
  }
}

void Worker::Lock::requireNoPermanentException() {
  KJ_IF_SOME(e, worker.impl->permanentException) {
    // Block taking lock when worker failed to start up.
    kj::throwFatalException(kj::cp(e));
  }
}

Worker::Lock::operator jsg::Lock&() {
  return impl->inner;
}

v8::Isolate* Worker::Lock::getIsolate() {
  return impl->inner.v8Isolate;
}

v8::Local<v8::Context> Worker::Lock::getContext() {
  KJ_IF_SOME(c, worker.impl->context) {
    return c.getHandle(impl->inner);
  } else KJ_IF_SOME(c, const_cast<Script&>(*worker.script).impl->moduleContext) {
    return c.getHandle(impl->inner);
  } else {
    KJ_UNREACHABLE;
  }
}

template <typename T>
static inline kj::Own<T> fakeOwn(T& ref) {
  return kj::Own<T>(&ref, kj::NullDisposer::instance);
}

kj::Maybe<kj::Own<api::ExportedHandler>> Worker::Lock::getExportedHandler(
    kj::Maybe<kj::StringPtr> name, kj::Maybe<Worker::Actor&> actor) {
  KJ_IF_SOME(a, actor) {
    KJ_IF_SOME(h, a.getHandler()) {
      return fakeOwn(h);
    }
  }

  kj::StringPtr n = name.orDefault("default"_kj);
  KJ_IF_SOME(h, worker.impl->namedHandlers.find(n)){
    return fakeOwn(h);
  } else KJ_IF_SOME(cls, worker.impl->statelessClasses.find(n)) {
    jsg::Lock& js = *this;
    auto handler = kj::heap(cls(js, jsg::alloc<api::ExecutionContext>(),
        KJ_ASSERT_NONNULL(worker.impl->env).addRef(js)));

    // HACK: We set handler.env and handler.ctx to undefined because we already passed the real
    //   env and ctx into the constructor, and we want the handler methods to act like they take
    //   just one parameter.
    handler->env = js.v8Ref(js.v8Undefined());
    handler->ctx = kj::none;

    return handler;
  } else if (name == kj::none) {
    // If the default export was requested, and we didn't find a handler for it, we'll fall back
    // to addEventListener().
    //
    // Note: The original intention was that we only use addEventListener() for
    //   service-worker-syntax scripts, but apparently the code has long allowed it for
    //   modules-based script too, if they lacked an `export default`. Yikes! Sadly, there are
    //   Workers in production relying on this so we are stuck with it.
    return kj::none;
  } else {
    if (worker.impl->actorClasses.find(n) != kj::none) {
      LOG_ERROR_PERIODICALLY("worker is not an actor but class name was requested", n);
    } else {
      LOG_ERROR_PERIODICALLY("worker has no such named entrypoint", n);
    }

    KJ_FAIL_ASSERT("worker_do_not_log; Unable to get exported handler");
  }
}

api::ServiceWorkerGlobalScope& Worker::Lock::getGlobalScope() {
  return *reinterpret_cast<api::ServiceWorkerGlobalScope*>(
      getContext()->GetAlignedPointerFromEmbedderData(1));
}

jsg::AsyncContextFrame::StorageKey& Worker::Lock::getTraceAsyncContextKey() {
  // const_cast OK because we are a lock on this isolate.
  auto& isolate = const_cast<Isolate&>(worker.getIsolate());
  return *(isolate.traceAsyncContextKey);
}

bool Worker::Lock::isInspectorEnabled() {
  return worker.script->isolate->impl->inspector != kj::none;
}

void Worker::Lock::logWarning(kj::StringPtr description) {
  // const_cast OK because we are a lock on this isolate.
  const_cast<Isolate&>(worker.getIsolate()).logWarning(description, *this);
}

void Worker::Lock::logWarningOnce(kj::StringPtr description) {
  // const_cast OK because we are a lock on this isolate.
  const_cast<Isolate&>(worker.getIsolate()).logWarningOnce(description, *this);
}

void Worker::Lock::logErrorOnce(kj::StringPtr description) {
  // const_cast OK because we are a lock on this isolate.
  const_cast<Isolate&>(worker.getIsolate()).logErrorOnce(description);
}

void Worker::Lock::logUncaughtException(kj::StringPtr description) {
  // We don't add the exception to traces here, since it turns out that this path only gets hit by
  // intermediate exception handling.
  KJ_IF_SOME(i, worker.script->isolate->impl->inspector) {
    JSG_WITHIN_CONTEXT_SCOPE(*this, getContext(), [&](jsg::Lock& js) {
      jsg::sendExceptionToInspector(js, *i.get(), description);
    });
  }

  // Run with --verbose to log JS exceptions to stderr. Useful when running tests.
  KJ_LOG(INFO, "uncaught exception", description);
}

void Worker::Lock::logUncaughtException(UncaughtExceptionSource source,
                                        const jsg::JsValue& exception,
                                        const jsg::JsMessage& message) {
  // Only add exception to trace when running within an I/O context with a tracer.
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    KJ_IF_SOME(tracer, ioContext.getWorkerTracer()) {
      JSG_WITHIN_CONTEXT_SCOPE(*this, getContext(), [&](jsg::Lock& js) {
        addExceptionToTrace(impl->inner, ioContext, tracer, source, exception,
            worker.getIsolate().getApi().getErrorInterfaceTypeHandler(*this));
      });
    }
  }

  KJ_IF_SOME(i, worker.script->isolate->impl->inspector) {
    JSG_WITHIN_CONTEXT_SCOPE(*this, getContext(), [&](jsg::Lock& js) {
      sendExceptionToInspector(js, *i.get(), source, exception, message);
    });
  }

  // Run with --verbose to log JS exceptions to stderr. Useful when running tests.
  KJ_LOG(INFO, "uncaught exception", source, exception);
}

void Worker::Lock::reportPromiseRejectEvent(v8::PromiseRejectMessage& message) {
  getGlobalScope().emitPromiseRejection(
      *this, message.GetEvent(),
      jsg::V8Ref<v8::Promise>(getIsolate(), message.GetPromise()),
      jsg::V8Ref<v8::Value>(getIsolate(), message.GetValue()));
}

void Worker::Lock::validateHandlers(ValidationErrorReporter& errorReporter) {
  JSG_WITHIN_CONTEXT_SCOPE(*this, getContext(), [&](jsg::Lock& js) {
    kj::HashSet<kj::StringPtr> ignoredHandlers;
    ignoredHandlers.insert("alarm"_kj);
    ignoredHandlers.insert("unhandledrejection"_kj);
    ignoredHandlers.insert("rejectionhandled"_kj);

    KJ_IF_SOME(c, worker.impl->context) {
      auto handlerNames = c->getHandlerNames();
      bool foundAny = false;
      for (auto& name: handlerNames) {
        if (!ignoredHandlers.contains(name)) {
          errorReporter.addHandler(kj::none, name);
          foundAny = true;
        }
      }
      if (!foundAny) {
        errorReporter.addError(kj::str(
            "No event handlers were registered. This script does nothing."));
      }
    } else {
      auto report = [&](kj::Maybe<kj::StringPtr> name, api::ExportedHandler& exported) {
        auto handle = exported.self.getHandle(js);
        if (handle->IsArray()) {
          // HACK: toDict() will throw a TypeError if given an array, because jsg::DictWrapper is
          //   designed to treat arrays as not matching when a dict is expected. However,
          //   StructWrapper has no such restriction, and therefore an exported array will
          //   successfully produce an ExportedHandler (presumably with no handler functions), and
          //   hence we will see it here. Rather than try to correct this inconsistency between
          //   struct and dict handling (which could have unintended consequences), let's just
          //   work around by ignoring arrays here.
          return;
        }

        auto dict = js.toDict(handle);
        for (auto& field: dict.fields) {
          if (!ignoredHandlers.contains(field.name)) {
            errorReporter.addHandler(name, field.name);
          }
        }
      };

      auto getEntrypointName = [&](kj::StringPtr key) -> kj::Maybe<kj::StringPtr> {
        if (key == "default"_kj) {
          return kj::none;
        } else {
          return key;
        }
      };

      for (auto& entry: worker.impl->namedHandlers) {
        report(getEntrypointName(entry.key), entry.value);
      }
      for (auto& entry: worker.impl->actorClasses) {
        errorReporter.addHandler(getEntrypointName(entry.key), "class");
      }
      for (auto& entry: worker.impl->statelessClasses) {
        // We want to report all of the stateless class's members. To do this, we examine its
        // prototype, and it's prototype's prototype, and so on, until we get to Object's
        // prototype, which we ignore.
        auto entrypointName = getEntrypointName(entry.key);
        js.withinHandleScope([&]() {
          // Find the prototype for `Object` by creating one.
          auto obj = js.obj();
          jsg::JsValue prototypeOfObject = obj.getPrototype();

          // Walk the prototype chain.
          jsg::JsObject ctor(KJ_ASSERT_NONNULL(entry.value.tryGetHandle(js.v8Isolate)));
          jsg::JsValue proto = ctor.get(js, "prototype");
          kj::HashSet<kj::String> seenNames;
          for (;;) {
            auto protoObj = JSG_REQUIRE_NONNULL(proto.tryCast<jsg::JsObject>(),
                TypeError, "Exported entrypoint class's prototype chain does not end in Object.");
            if (protoObj == prototypeOfObject) {
              // Reached the prototype for `Object`. Stop here.
              break;
            }

            // Awkwardly, the prototype's members are not typically enumerable, so we have to
            // enumerate them rather directly.
            jsg::JsArray properties = protoObj.getPropertyNames(
                js, jsg::KeyCollectionFilter::OWN_ONLY,
                jsg::PropertyFilter::SKIP_SYMBOLS,
                jsg::IndexFilter::SKIP_INDICES);
            for (auto i: kj::zeroTo(properties.size())) {
              auto name = properties.get(js, i).toString(js);
              if (name == "constructor"_kj) {
                // Don't treat special method `constructor` as an exported handler.
                continue;
              }

              // Only report each method name once, even if it overrides a method in a superclass.
              bool isNew = true;
              kj::StringPtr namePtr = seenNames.upsert(kj::mv(name), [&](auto&, auto&&) {
                isNew = false;
              });
              if (isNew) {
                errorReporter.addHandler(entrypointName, namePtr);
              }
            }

            proto = protoObj.getPrototype();
          }
        });
      }
    }
  });
}

// =======================================================================================
// AsyncLock implementation

thread_local Worker::AsyncWaiter* Worker::AsyncWaiter::threadCurrentWaiter = nullptr;

Worker::Isolate::AsyncWaiterList::~AsyncWaiterList() noexcept {
  // It should be impossible for this list to be non-empty since each member of the list holds a
  // strong reference back to us. But if the list is non-empty, we'd better crash here, to avoid
  // dangling pointers.
  KJ_ASSERT(head == kj::none, "destroying non-empty waiter list?");
  KJ_ASSERT(tail == &head, "tail pointer corrupted?");
}

kj::Promise<Worker::AsyncLock> Worker::Isolate::takeAsyncLockWithoutRequest(
    SpanParent parentSpan) const {
  auto lockTiming = getMetrics().tryCreateLockTiming(kj::mv(parentSpan));
  return takeAsyncLockImpl(kj::mv(lockTiming));
}

kj::Promise<Worker::AsyncLock> Worker::Isolate::takeAsyncLock(
    RequestObserver& request) const {
  auto lockTiming = getMetrics().tryCreateLockTiming(kj::Maybe<RequestObserver&>(request));
  return takeAsyncLockImpl(kj::mv(lockTiming));
}

kj::Promise<Worker::AsyncLock> Worker::Isolate::takeAsyncLockImpl(
    kj::Maybe<kj::Own<IsolateObserver::LockTiming>> lockTiming) const {
  kj::Maybe<uint> currentLoad;
  if (lockTiming != kj::none) {
    currentLoad = getCurrentLoad();
  }

  for (uint threadWaitingDifferentLockCount = 0; ; ++threadWaitingDifferentLockCount) {
    AsyncWaiter* waiter = AsyncWaiter::threadCurrentWaiter;

    if (waiter == nullptr) {
      // Thread is not currently waiting on a lock.
      KJ_IF_SOME(lt, lockTiming) {
        lt.get()->reportAsyncInfo(
            KJ_ASSERT_NONNULL(currentLoad), false /* threadWaitingSameLock */,
            threadWaitingDifferentLockCount);
      }
      auto newWaiter = kj::refcounted<AsyncWaiter>(kj::atomicAddRef(*this));
      co_await newWaiter->readyPromise;
      co_return AsyncLock(kj::mv(newWaiter), kj::mv(lockTiming));
    } else if (waiter->isolate == this) {
      // Thread is waiting on a lock already, and it's for the same isolate. We can coalesce the
      // locks.
      KJ_IF_SOME(lt, lockTiming) {
        lt.get()->reportAsyncInfo(
            KJ_ASSERT_NONNULL(currentLoad), true /* threadWaitingSameLock */,
            threadWaitingDifferentLockCount);
      }
      auto newWaiterRef = kj::addRef(*waiter);
      co_await newWaiterRef->readyPromise;
      co_return AsyncLock(kj::mv(newWaiterRef), kj::mv(lockTiming));
    } else {
      // Thread is already waiting for or holding a different isolate lock. Wait for that one to
      // be released before we try to lock a different isolate.
      // TODO(perf): Use of ForkedPromise leads to thundering herd here. Should be minor in practice,
      //   but we could consider creating another linked list instead...
      KJ_IF_SOME(lt, lockTiming) {
        lt.get()->waitingForOtherIsolate(waiter->isolate->getId());
      }
      co_await waiter->releasePromise;
    }
  }
}

kj::Promise<Worker::AsyncLock> Worker::takeAsyncLockWithoutRequest(SpanParent parentSpan) const {
  return script->getIsolate().takeAsyncLockWithoutRequest(kj::mv(parentSpan));
}

kj::Promise<Worker::AsyncLock> Worker::takeAsyncLock(RequestObserver& request) const {
  return script->getIsolate().takeAsyncLock(request);
}

Worker::AsyncWaiter::AsyncWaiter(kj::Own<const Isolate> isolateParam)
    : executor(kj::getCurrentThreadExecutor()),
      isolate(kj::mv(isolateParam)) {
  // Init `releasePromise` / `releaseFulfiller`.
  {
    auto paf = kj::newPromiseAndFulfiller<void>();
    releasePromise = paf.promise.fork();
    releaseFulfiller = kj::mv(paf.fulfiller);
  }

  // Add ourselves to the wait queue for this isolate.
  auto lock = isolate->asyncWaiters.lockExclusive();
  if (lock->tail == &lock->head) {
    // Looks like the queue is empty, so we immediately get the lock.
    readyPromise = kj::Promise<void>(kj::READY_NOW).fork();
    // We can leave `readyFulfiller` null as no one will ever invoke it anyway.
  } else {
    // Arrange to get notified later.
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    readyPromise = paf.promise.fork();
    readyFulfiller = kj::mv(paf.fulfiller);
  }

  next = nullptr;
  prev = lock->tail;
  *lock->tail = this;
  lock->tail = &next;

  threadCurrentWaiter = this;

  __atomic_add_fetch(&isolate->impl->lockAttemptGauge, 1, __ATOMIC_RELAXED);
}

Worker::AsyncWaiter::~AsyncWaiter() noexcept {
  // This destructor is `noexcept` because an exception here probably leaves the process in a bad
  // state.

  __atomic_sub_fetch(&isolate->impl->lockAttemptGauge, 1, __ATOMIC_RELAXED);

  auto lock = isolate->asyncWaiters.lockExclusive();

  releaseFulfiller->fulfill();

  // Remove ourselves from the list.
  *prev = next;
  KJ_IF_SOME(n, next) {
    n.prev = prev;
  } else {
    lock->tail = prev;
  }

  if (prev == &lock->head) {
    // We held the lock before now. Alert the next waiter that they are now at the front of the
    // line.
    KJ_IF_SOME(n, next) {
      n.readyFulfiller->fulfill();
    }
  }

  KJ_ASSERT(threadCurrentWaiter == this);
  threadCurrentWaiter = nullptr;
}

kj::Promise<void> Worker::AsyncLock::whenThreadIdle() {
  for (;;) {
    if (auto waiter = AsyncWaiter::threadCurrentWaiter; waiter != nullptr) {
      co_await waiter->releasePromise;
      continue;
    }

    co_await kj::evalLast([] {});

    if (AsyncWaiter::threadCurrentWaiter == nullptr) {
      co_return;
    }
    // Whoops, a new lock attempt appeared, loop.
  }
}

// =======================================================================================

// A proxy for OutputStream that internally buffers data as long as it's beyond a given limit.
// Also, it counts size of all the data it has seen (whether it has hit the limit or not).
//
// We use this in the Network tab to report response stats and preview [decompressed] bodies,
// but we don't want to keep buffering extremely large ones, so just discard buffered data
// upon hitting a limit and don't return any body to the devtools frontend afterwards.
class Worker::Isolate::LimitedBodyWrapper: public kj::OutputStream {
public:
  LimitedBodyWrapper(size_t limit = 1 * 1024 * 1024): limit(limit) {
    if (limit > 0) {
      inner.emplace();
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(LimitedBodyWrapper);

  void reset() {
    this->inner = kj::none;
  }

  void write(const void* buffer, size_t size) override {
    this->size += size;
    KJ_IF_SOME(inner, this->inner) {
      if (this->size <= this->limit) {
        inner.write(buffer, size);
      } else {
        reset();
      }
    }
  }

  size_t getWrittenSize() {
    return this->size;
  }

  kj::Maybe<kj::ArrayPtr<byte>> getArray() {
    KJ_IF_SOME(inner, this->inner) {
      return inner.getArray();
    } else {
      return kj::none;
    }
  }

private:
  size_t size = 0;
  size_t limit = 0;
  kj::Maybe<kj::VectorOutputStream> inner;
};

struct MessageQueue {
  kj::Vector<kj::String> messages;
  size_t head;
  enum class Status { ACTIVE, CLOSED } status;
};

class Worker::Isolate::InspectorChannelImpl final: public v8_inspector::V8Inspector::Channel {
public:
  InspectorChannelImpl(kj::Own<const Worker::Isolate> isolateParam, kj::WebSocket& webSocket)
      : ioHandler(webSocket), state(kj::heap<State>(this, kj::mv(isolateParam))) {
    ioHandler.connect(*this);
  }

  // In preview sessions, synchronous locks are not an issue. We declare an alternate spelling of
  // the type so that all the individual locks below don't turn up in a search for synchronous
  // locks.
  using InspectorLock = Worker::Lock::TakeSynchronously;

  ~InspectorChannelImpl() noexcept try {
    // Stop message pump.
    ioHandler.disconnect();

    // Delete session under lock.
    auto state = this->state.lockExclusive();

    jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
      Isolate::Impl::Lock recordedLock(*state->get()->isolate, InspectorLock(kj::none), stackScope);
      if (state->get()->isolate->currentInspectorSession != kj::none) {
        const_cast<Isolate&>(*state->get()->isolate).disconnectInspector();
      }
      state->get()->teardownUnderLock();
    });
  } catch (...) {
    // Unfortunately since we're inheriting from Channel which declares a virtual destructor with
    // default exception constraints, we have to catch all exceptions here and log them.
    // But different kinds of exceptions call for different ways to stringify the exception.
    // kj::runCatchingExceptions() normally does this for us, but there's no way to use it while
    // wrapping the whole destructor (including destructors of members). So... we do a native
    // catch(...) and then we rethrow the exception inside a kj::runCatchingExceptions and then log
    // that. Yeah.
    //
    // TODO(cleanup): Maybe we could add a kj::stringifyCurrentException() or
    //     kj::logUncaughtException() or something?
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      throw;
    })) {
      KJ_LOG(ERROR, "uncaught exception in ~Script() and the C++ standard is broken", exception);
    }
  }

  void disconnect() {
    // Fake like the client requested close. This will cause outgoingLoop() to exit and everything
    // will be cleaned up.
    ioHandler.disconnect();
  }

  void dispatchProtocolMessage(kj::String message,
                               v8_inspector::V8InspectorSession& session,
                               Isolate& isolate,
                               jsg::V8StackScope& stackScope,
                               Isolate::Impl::Lock& recordedLock) {
    capnp::MallocMessageBuilder messageBuilder;
    auto cmd = messageBuilder.initRoot<cdp::Command>();
    getCdpJsonCodec().decode(message, cmd);

    switch (cmd.which()) {
      case cdp::Command::UNKNOWN: {
        break;
      }
      case cdp::Command::NETWORK_ENABLE: {
        setNetworkEnabled(true);
        cmd.getNetworkEnable().initResult();
        break;
      }
      case cdp::Command::NETWORK_DISABLE: {
        setNetworkEnabled(false);
        cmd.getNetworkDisable().initResult();
        break;
      }
      case cdp::Command::NETWORK_GET_RESPONSE_BODY: {
        auto err = cmd.getNetworkGetResponseBody().initError();
        err.setCode(-32600);
        err.setMessage("Network.getResponseBody is not supported in this fork");
        break;
      }
      case cdp::Command::PROFILER_STOP: {
        KJ_IF_SOME(p, isolate.impl->profiler) {
          auto& lock = recordedLock.lock;
          stopProfiling(*lock, *p, cmd);
        }
        break;
      }
      case cdp::Command::PROFILER_START: {
        KJ_IF_SOME(p, isolate.impl->profiler) {
          auto& lock = recordedLock.lock;
          startProfiling(*lock, *p);
        }
        break;
      }
      case cdp::Command::PROFILER_SET_SAMPLING_INTERVAL: {
        KJ_IF_SOME(p, isolate.impl->profiler) {
          auto interval = cmd.getProfilerSetSamplingInterval().getParams().getInterval();
          setSamplingInterval(*p, interval);
        }
        break;
      }
      case cdp::Command::PROFILER_ENABLE: {
        auto& lock = recordedLock.lock;
        isolate.impl->profiler = kj::Own<v8::CpuProfiler>(
            v8::CpuProfiler::New(lock->v8Isolate, v8::kDebugNaming, v8::kLazyLogging),
            CpuProfilerDisposer::instance);
        break;
      }
      case cdp::Command::TAKE_HEAP_SNAPSHOT: {
        auto& lock = recordedLock.lock;
        auto params = cmd.getTakeHeapSnapshot().getParams();
        takeHeapSnapshot(*lock,
            params.getExposeInternals(),
            params.getCaptureNumericValue());
        break;
      }
    }

    if (!cmd.isUnknown()) {
      sendNotification(cmd);
      return;
    }

    auto& lock = recordedLock.lock;

    // We have at times observed V8 bugs where the inspector queues a background task and
    // then synchronously waits for it to complete, which would deadlock if background
    // threads are disallowed. Since the inspector is in a process sandbox anyway, it's not
    // a big deal to just permit those background threads.
    AllowV8BackgroundThreadsScope allowBackgroundThreads;

    kj::Maybe<kj::Exception> maybeLimitError;
    {
      auto limitScope = isolate.getLimitEnforcer().enterInspectorJs(*lock, maybeLimitError);
      session.dispatchProtocolMessage(jsg::toInspectorStringView(message));
    }

    // Run microtasks in case the user made an async call.
    if (maybeLimitError == kj::none) {
      auto limitScope = isolate.getLimitEnforcer().enterInspectorJs(*lock, maybeLimitError);
      lock->runMicrotasks();
    } else {
      // Oops, we already exceeded the limit, so force the microtask queue to be thrown away.
      lock->terminateExecution();
      lock->runMicrotasks();
    }

    KJ_IF_SOME(limitError, maybeLimitError) {
      lock->withinHandleScope([&] {
        // HACK: We want to print the error, but we need a context to do that.
        //   We don't know which contexts exist in this isolate, so I guess we have to
        //   create one. Ugh.
        auto dummyContext = v8::Context::New(lock->v8Isolate);
        auto& inspector = *KJ_ASSERT_NONNULL(isolate.impl->inspector);
        inspector.contextCreated(
            v8_inspector::V8ContextInfo(dummyContext, 1, v8_inspector::StringView(
                reinterpret_cast<const uint8_t*>("Worker"), 6)));
        JSG_WITHIN_CONTEXT_SCOPE(*lock, dummyContext, [&](jsg::Lock& js) {
          jsg::sendExceptionToInspector(js, inspector,
              jsg::extractTunneledExceptionDescription(limitError.getDescription()));
        });
        inspector.contextDestroyed(dummyContext);
      });
    }

    if (recordedLock.checkInWithLimitEnforcer(isolate)) {
      disconnect();
    }
  }

  kj::Promise<void> messagePump() {
    return ioHandler.messagePump();
  }

  void handleDispatchProtocolMessage(
      Worker::AsyncLock& asyncLock,
      kj::MutexGuarded<MessageQueue>& incomingQueue) {
    auto lockedState = state.lockExclusive();
    v8_inspector::V8InspectorSession& session = *lockedState->get()->session;
    Isolate& isolate = const_cast<Isolate&>(*lockedState->get()->isolate);
    jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
      Isolate::Impl::Lock recordedLock(isolate, asyncLock, stackScope);

      auto lockedQueue = incomingQueue.lockExclusive();
      if (lockedQueue->status != MessageQueue::Status::ACTIVE) {
        return;
      }

      auto messages = lockedQueue->messages.slice(lockedQueue->head, lockedQueue->messages.size());
      for (auto& message : messages) {
        dispatchProtocolMessage(kj::mv(message), session, isolate, stackScope, recordedLock);
      }
      lockedQueue->messages.clear();
      lockedQueue->head = 0;
    });
  }

  kj::Promise<void> dispatchProtocolMessages(kj::MutexGuarded<MessageQueue>& incomingQueue) {
    // This method is called on the I/O thread, which also adds messages to the `incomingQueue`.
    // So long as this method does not yield/resume mid-way, there is no concern about how
    // long the queue lock is held for whilst dispatching messages.
    auto i = kj::atomicAddRef(*this->state.lockExclusive()->get()->isolate);
    auto asyncLock = co_await i->takeAsyncLockWithoutRequest(nullptr);
    handleDispatchProtocolMessage(asyncLock, incomingQueue);
  }

  // ---------------------------------------------------------------------------
  // implements Channel
  //
  // Keep in mind that these methods will be called from various threads!

  void sendResponse(int callId, std::unique_ptr<v8_inspector::StringBuffer> message) override {
    // callId is encoded in the message, too. Unsure why this method even exists.
    sendNotification(kj::mv(message));
  }

  bool isNetworkEnabled() {
    return __atomic_load_n(&networkEnabled, __ATOMIC_RELAXED);
  }

  void setNetworkEnabled(bool enable) {
    __atomic_store_n(&networkEnabled, enable, __ATOMIC_RELAXED);
  }

  void sendNotification(kj::String message) {
    ioHandler.send(kj::mv(message));
  }

  template <typename T>
  void sendNotification(T&& message) {
    sendNotification(getCdpJsonCodec().encode(message));
  }

  void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
    sendNotification(kj::str(message->string()));
  }

  void flushProtocolNotifications() override {
    // Are we supposed to do anything here? There's no documentation, so who knows? Maybe we could
    // delay signaling the outgoing loop until this call?
  }

  // Dispatches one message whilst automatic CDP messages on the I/O worker thread is paused, called
  // on the thread executing the isolate whilst execution is suspended due to a breakpoint or
  // debugger statement.
  bool dispatchOneMessageDuringPause();

private:
  // Class that manages the I/O for devtools connections. I/O is performed on the
  // thread associated with the InspectorService (the thread that calls attachInspector).
  // Most of the public API is intended for code running on the isolate thread, such as
  // the InspectorChannelImpl and the InspectorClient.
  class WebSocketIoHandler final {
  public:
    WebSocketIoHandler(kj::WebSocket& webSocket)
        : webSocket(webSocket) {
      // Assume we are being instantiated on the InspectorService thread, the thread that will do
      // I/O for CDP messages. Messages are delivered to the InspectorChannelImpl on the Isolate thread.
      incomingQueueNotifier = XThreadNotifier::create();
      outgoingQueueNotifier = XThreadNotifier::create();
    }

    // Sets the channel that messages are delivered to.
    void connect(InspectorChannelImpl& inspectorChannel) {
      channel = inspectorChannel;
    }

    void disconnect() {
      channel = {};
      shutdown();
    }

    // Blocked the current thread until a message arrives. This is intended
    // for use in the InspectorClient when breakpoints are hit. The InspectorClient
    // has to remain in runMessageLoopOnPause() but still receive CDP messages
    // (e.g. resume).
    kj::Maybe<kj::String> waitForMessage() {
      return incomingQueue.when(
          [](const MessageQueue& incomingQueue) {
            return (incomingQueue.head < incomingQueue.messages.size() ||
                    incomingQueue.status == MessageQueue::Status::CLOSED);
          },
          [](MessageQueue& incomingQueue) -> kj::Maybe<kj::String> {
            if (incomingQueue.status == MessageQueue::Status::CLOSED) return {};
            return pollMessage(incomingQueue);
          });
    }

    // Message pumping promise that should be evaluated on the InspectorService
    // thread.
    kj::Promise<void> messagePump() {
      return receiveLoop().exclusiveJoin(dispatchLoop()).exclusiveJoin(transmitLoop());
    }

    void send(kj::String message) {
      auto lockedOutgoingQueue = outgoingQueue.lockExclusive();
      if (lockedOutgoingQueue->status == MessageQueue::Status::CLOSED) return;
      lockedOutgoingQueue->messages.add(kj::mv(message));
      outgoingQueueNotifier->notify();
    }

  private:
    static kj::Maybe<kj::String> pollMessage(MessageQueue& messageQueue) {
      if (messageQueue.head < messageQueue.messages.size()) {
        kj::String message = kj::mv(messageQueue.messages[messageQueue.head++]);
        if (messageQueue.head == messageQueue.messages.size()) {
          messageQueue.head = 0;
          messageQueue.messages.clear();
        }
        return kj::mv(message);
      }
      return {};
    }

    void shutdown() {
    // Drain incoming queue, the isolate thread may be waiting on it
    // on will notice it is closed if woken without any messages to
    // deliver in WebSocketIoWorker::waitForMessage().
      {
        auto lockedIncomingQueue = incomingQueue.lockExclusive();
        lockedIncomingQueue->head = 0;
        lockedIncomingQueue->messages.clear();
        lockedIncomingQueue->status = MessageQueue::Status::CLOSED;
      }
      {
        auto lockedOutgoingQueue = outgoingQueue.lockExclusive();
        lockedOutgoingQueue->status = MessageQueue::Status::CLOSED;
      }
      // Wake any waiters since queue status fields have been updated.
      outgoingQueueNotifier->notify();
    }

    kj::Promise<void> receiveLoop() {
     for (;;) {
        auto message = co_await webSocket.receive(MAX_MESSAGE_SIZE);
        KJ_SWITCH_ONEOF(message) {
          KJ_CASE_ONEOF(text, kj::String) {
            incomingQueue.lockExclusive()->messages.add(kj::mv(text));
            incomingQueueNotifier->notify();
          }
          KJ_CASE_ONEOF(blob, kj::Array<byte>){
            // Ignore.
          }
          KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
            shutdown();
          }
        }
      }
    }

    kj::Promise<void> dispatchLoop() {
      for (;;) {
        co_await incomingQueueNotifier->awaitNotification();
        KJ_IF_SOME(c, channel) {
          co_await c.dispatchProtocolMessages(this->incomingQueue);
        }
      }
    }

    kj::Promise<void> transmitLoop() {
      for (;;) {
        co_await outgoingQueueNotifier->awaitNotification();
        try {
          auto lockedOutgoingQueue = outgoingQueue.lockExclusive();
          auto messages = kj::mv(lockedOutgoingQueue->messages);
          bool receivedClose = lockedOutgoingQueue->status == MessageQueue::Status::CLOSED;
          lockedOutgoingQueue.release();
          co_await sendToWebSocket(kj::mv(messages));
          if (receivedClose) {
            co_await webSocket.close(1000, "client closed connection");
            co_return;
          }
        } catch (kj::Exception& e) {
            shutdown();
            throw;
        }
      }
    }

    kj::Promise<void> sendToWebSocket(kj::Vector<kj::String> messages) {
      for (auto& message : messages) {
        co_await webSocket.send(message);
      }
    }

    kj::MutexGuarded<MessageQueue> incomingQueue;
    kj::Own<XThreadNotifier> incomingQueueNotifier;

    kj::MutexGuarded<MessageQueue> outgoingQueue;
    kj::Own<XThreadNotifier> outgoingQueueNotifier;

    kj::WebSocket& webSocket;                 // only accessed on the InspectorService thread.
    std::atomic_bool receivedClose;           // accessed on any thread (only transitions false -> true).
    kj::Maybe<InspectorChannelImpl&> channel; // only accessed on the isolate thread.

    // Sometimes the inspector protocol sends large messages. KJ defaults to a 1MB size limit
    // for WebSocket messages, which makes sense for production use cases, but for debug we should
    // be OK to go larger. So, we'll accept 128MB.
    static constexpr size_t MAX_MESSAGE_SIZE = 128u << 20;
  };

  WebSocketIoHandler ioHandler;

  void takeHeapSnapshot(jsg::Lock& js, bool exposeInternals, bool captureNumericValue) {
    struct Activity: public v8::ActivityControl {
      InspectorChannelImpl& channel;
      Activity(InspectorChannelImpl& channel) : channel(channel) {}

      ControlOption ReportProgressValue(uint32_t done, uint32_t total) {
        capnp::MallocMessageBuilder message;
        auto event = message.initRoot<cdp::Event>();
        auto params = event.initReportHeapSnapshotProgress();
        params.setDone(done);
        params.setTotal(total);
        if (done == total) {
          params.setFinished(true);
        }
        auto notification = getCdpJsonCodec().encode(event);
        channel.sendNotification(kj::mv(notification));
        return ControlOption::kContinue;
      }
    };

    struct Writer: public v8::OutputStream {
      InspectorChannelImpl& channel;

      Writer(InspectorChannelImpl& channel) : channel(channel) {}
      void EndOfStream() override {}

      int GetChunkSize() override {
        return 65536;  // big chunks == faster
        // The chunk size here will determine the actual number of individual
        // messages that are sent. The default is... rather small. Experience
        // node and node-heapdump shows that this can be bumped up
        // much higher to get better performance. Here we use the value
        // that Node.js uses (see Node.js' FileOutputStream impl).
      }

      v8::OutputStream::WriteResult WriteAsciiChunk(char* data, int size) override {
        capnp::MallocMessageBuilder message;
        auto event = message.initRoot<cdp::Event>();

        auto params = event.initAddHeapSnapshotChunk();
        params.setChunk(kj::heapString(data, size));
        auto notification = getCdpJsonCodec().encode(event);
        channel.sendNotification(kj::mv(notification));

        return v8::OutputStream::WriteResult::kContinue;
      }
    };

    Activity activity(*this);
    Writer writer(*this);

    auto profiler = js.v8Isolate->GetHeapProfiler();
    auto snapshot = kj::Own<const v8::HeapSnapshot>(
        profiler->TakeHeapSnapshot(&activity, nullptr, exposeInternals, captureNumericValue),
        HeapSnapshotDeleter::INSTANCE);
    snapshot->Serialize(&writer);
  }

  struct State {
    kj::Own<const Worker::Isolate> isolate;
    std::unique_ptr<v8_inspector::V8InspectorSession> session;

    State(InspectorChannelImpl* self, kj::Own<const Worker::Isolate> isolateParam)
        : isolate(kj::mv(isolateParam)),
          session(KJ_ASSERT_NONNULL(isolate->impl->inspector)
              ->connect(1, self, v8_inspector::StringView(),
                        isolate->impl->inspectorPolicy == InspectorPolicy::ALLOW_UNTRUSTED ?
                            v8_inspector::V8Inspector::kUntrusted :
                            v8_inspector::V8Inspector::kFullyTrusted)) {}
    ~State() noexcept(false) {
      if (session != nullptr) {
        KJ_LOG(ERROR, "Deleting InspectorChannelImpl::State without having called "
                      "teardownUnderLock()", kj::getStackTrace());

        // Isolate locks are recursive so it should be safe to lock here.
        jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
          Isolate::Impl::Lock recordedLock(*isolate, InspectorLock(kj::none), stackScope);
          session = nullptr;
        });
      }
    }

    // Must be called with the worker isolate locked. Should be called immediately before
    // destruction.
    void teardownUnderLock() {
      session = nullptr;
    }

    KJ_DISALLOW_COPY_AND_MOVE(State);
  };
  // Mutex ordering: You must lock this *before* locking the isolate.
  kj::MutexGuarded<kj::Own<State>> state;

  // Not under `state` lock due to lock ordering complications.
  volatile bool networkEnabled = false;
};

bool Worker::Isolate::InspectorChannelImpl::dispatchOneMessageDuringPause() {
  auto maybeMessage = ioHandler.waitForMessage();
  // We can be paused by either hitting a debugger statement in a script or from hitting
  // a breakpoint or someone hit break.
  KJ_IF_SOME(message, maybeMessage) {
    auto lockedState = this->state.lockExclusive();
    // Received a message whilst script is running, probably in a breakpoint.
    v8_inspector::V8InspectorSession& session = *lockedState->get()->session;
    // const_cast OK because the IoContext has the lock.
    Isolate& isolate = const_cast<Isolate&>(*lockedState->get()->isolate);
    Worker::Lock& workerLock = IoContext::current().getCurrentLock();
    Isolate::Impl::Lock& recordedLock = workerLock.impl->recordedLock;
    jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
      dispatchProtocolMessage(kj::mv(message), session, isolate, stackScope, recordedLock);
    });
    return true;
  } else {
    // No message from waitForMessage() implies the connection is broken.
    return false;
  }
}

bool Worker::InspectorClient::dispatchOneMessageDuringPause(Worker::Isolate::InspectorChannelImpl& channel) {
  return channel.dispatchOneMessageDuringPause();
}

kj::Promise<void> Worker::Isolate::attachInspector(
    kj::Timer& timer,
    kj::Duration timerOffset,
    kj::HttpService::Response& response,
    const kj::HttpHeaderTable& headerTable,
    kj::HttpHeaderId controlHeaderId) const {
  KJ_REQUIRE(impl->inspector != kj::none);

  kj::HttpHeaders headers(headerTable);
  headers.set(controlHeaderId, "{\"ewLog\":{\"status\":\"ok\"}}");
  auto webSocket = response.acceptWebSocket(headers);

  return attachInspector(timer, timerOffset, *webSocket)
      .attach(kj::mv(webSocket));
}

kj::Promise<void> Worker::Isolate::attachInspector(
    kj::Timer& timer,
    kj::Duration timerOffset,
    kj::WebSocket& webSocket) const {
  KJ_REQUIRE(impl->inspector != kj::none);

  return jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    Isolate::Impl::Lock recordedLock(*this,
        InspectorChannelImpl::InspectorLock(kj::none), stackScope);
    auto& lock = *recordedLock.lock;
    auto& lockedSelf = const_cast<Worker::Isolate&>(*this);

    // If another inspector was already connected, boot it, on the assumption that that connection
    // is dead and this is why the user reconnected. While we could actually allow both inspector
    // sessions to stay open (V8 supports this!), we'd then need to store a set of all connected
    // inspectors in order to be able to disconnect all of them in case of an isolate purge... let's
    // just not.
    lockedSelf.disconnectInspector();

    lockedSelf.impl->inspectorClient.setInspectorTimerInfo(timer, timerOffset);

    auto channel = kj::heap<Worker::Isolate::InspectorChannelImpl>(kj::atomicAddRef(*this), webSocket);
    lockedSelf.currentInspectorSession = *channel;
    lockedSelf.impl->inspectorClient.setChannel(*channel);

    // Send any queued notifications.
    lock.withinHandleScope([&] {
      for (auto& notification: lockedSelf.impl->queuedNotifications) {
        channel->sendNotification(kj::mv(notification));
      }
      lockedSelf.impl->queuedNotifications.clear();
    });

    return channel->messagePump().attach(kj::mv(channel));
  });
}

void Worker::Isolate::disconnectInspector() {
  // If an inspector session is connected, proactively drop it, so as to force it to drop its
  // reference on the script, so that the script can be deleted.
  KJ_IF_SOME(current, currentInspectorSession) {
    current.disconnect();
    currentInspectorSession = {};
  }
  impl->inspectorClient.resetChannel();
}

void Worker::Isolate::logWarning(kj::StringPtr description, Lock& lock) {
  if (impl->inspector != kj::none) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      logMessage(js, static_cast<uint16_t>(cdp::LogType::WARNING), description);
    });
  }

  if (consoleMode == ConsoleMode::INSPECTOR_ONLY) {
    // Run with --verbose to log JS exceptions to stderr. Useful when running tests.
    KJ_LOG(INFO, "console warning", description);
  } else {
    fprintf(stderr, "%s\n", description.cStr());
    fflush(stderr);
  }
}

void Worker::Isolate::logWarningOnce(kj::StringPtr description, Lock& lock) {
  impl->warningOnceDescriptions.findOrCreate(description, [&] {
    logWarning(description, lock);
    return kj::str(description);
  });
}

void Worker::Isolate::logErrorOnce(kj::StringPtr description) {
  impl->errorOnceDescriptions.findOrCreate(description, [&] {
    KJ_LOG(ERROR, description);
    return kj::str(description);
  });
}

void Worker::Isolate::logMessage(jsg::Lock& js,
                                 uint16_t type, kj::StringPtr description) {
  if (impl->inspector != kj::none) {
    // We want to log a warning to the devtools console, as if `console.warn()` were called.
    // However, the only public interface to call the real `console.warn()` is via JavaScript,
    // where it could have been monkey-patched by the guest. We'd like to avoid having to worry
    // about that blowing up in our face. So instead we arrange to send the proper devtools
    // protocol messages ourselves.
    //
    // TODO(cleanup): It would be better if we could directly add the message to the inspector's
    //   console log (without calling through JavaScript). What we're doing here has some problems.
    //   In particular, if no client is connected yet, we attempt to queue up the messages to send
    //   later, much like the real inspector does. This is kind of complicated, and doesn't quite
    //   work right:
    //   - The messages won't necessarily be in the right order with normal console logs made at
    //     the same time (with identical timestamps).
    //   - In theory we should queue *all* logged warnings and deliver them to every future client,
    //     not just the next client to connect. But if we do that, we also need to respect the
    //     protocol command to clear the history when requested. This was further than I cared to
    //     go.
    //   To fix these problems, maybe we should just patch V8 with a direct interface into the
    //   inspector's own log. (Also, how does Chrome handle this?)

    js.withinHandleScope([&] {
      capnp::MallocMessageBuilder message;
      auto event = message.initRoot<cdp::Event>();

      auto params = event.initRuntimeConsoleApiCalled();
      params.setType(static_cast<cdp::LogType>(type));
      params.initArgs(1)[0].initString().setValue(description);
      params.setExecutionContextId(v8_inspector::V8ContextInfo::executionContextId(js.v8Context()));
      params.setTimestamp(impl->inspectorClient.currentTimeMS());
      stackTraceToCDP(js, params.initStackTrace());

      auto notification = getCdpJsonCodec().encode(event);
      KJ_IF_SOME(i, currentInspectorSession) {
        i.sendNotification(kj::mv(notification));
      } else {
        impl->queuedNotifications.add(kj::mv(notification));
      }
    });
  }
}

// =======================================================================================

struct Worker::Actor::Impl {
  Actor::Id actorId;
  MakeStorageFunc makeStorage;

  kj::Own<ActorObserver> metrics;

  kj::Maybe<jsg::JsRef<jsg::JsValue>> transient;
  kj::Maybe<kj::Own<ActorCacheInterface>> actorCache;

  struct NoClass {};
  struct Initializing {};

  // If the actor is backed by a class, this field tracks the instance through its stages. The
  // instance is constructed as part of the first request to be delivered.
  kj::OneOf<
    NoClass,                         // not class-based
    Worker::Impl::ActorClassInfo*,   // constructor not run yet
    Initializing,                    // constructor currently running
    api::ExportedHandler,            // fully constructed
    kj::Exception                    // constructor threw
  > classInstance;

  class HooksImpl: public InputGate::Hooks, public OutputGate::Hooks, public ActorCache::Hooks {
  public:
    HooksImpl(kj::Own<Loopback> loopback, TimerChannel& timerChannel, ActorObserver& metrics)
        : loopback(kj::mv(loopback)), timerChannel(timerChannel), metrics(metrics) {}

    void inputGateLocked() override { metrics.inputGateLocked(); }
    void inputGateReleased() override { metrics.inputGateReleased(); }
    void inputGateWaiterAdded() override { metrics.inputGateWaiterAdded(); }
    void inputGateWaiterRemoved() override { metrics.inputGateWaiterRemoved(); }
    // Implements InputGate::Hooks.

    kj::Promise<void> makeTimeoutPromise() override {
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
      // Give more time under ASAN.
      //
      // TODO(cleanup): Should this be configurable?
      auto timeout = 20 * kj::SECONDS;
#else
      auto timeout = 10 * kj::SECONDS;
#endif
      co_await timerChannel.afterLimitTimeout(timeout);
      kj::throwFatalException(KJ_EXCEPTION(FAILED,
            "broken.outputGateBroken; jsg.Error: Durable Object storage operation exceeded "
            "timeout which caused object to be reset."));
    }

    // Implements OutputGate::Hooks.

    void outputGateLocked() override { metrics.outputGateLocked(); }
    void outputGateReleased() override { metrics.outputGateReleased(); }
    void outputGateWaiterAdded() override { metrics.outputGateWaiterAdded(); }
    void outputGateWaiterRemoved() override { metrics.outputGateWaiterRemoved(); }

    // Implements ActorCache::Hooks

    void updateAlarmInMemory(kj::Maybe<kj::Date> newAlarmTime) override;

  private:
    kj::Own<Loopback> loopback;    // only for updateAlarmInMemory()
    TimerChannel& timerChannel;    // only for afterLimitTimeout() and updateAlarmInMemory()
    ActorObserver& metrics;

    kj::Maybe<kj::Promise<void>> maybeAlarmPreviewTask;
  };

  HooksImpl hooks;

  // Handles both input locks and request locks.
  InputGate inputGate;

  // Handles output locks.
  OutputGate outputGate;

  // `ioContext` is initialized upon delivery of the first request.
  kj::Maybe<kj::Own<IoContext>> ioContext;

  // If onBroken() is called while `ioContext` is still null, this is initialized. When
  // `ioContext` is constructed, this will be fulfilled with `ioContext.onAbort()`.
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Promise<void>>>> abortFulfiller;

  // Task which periodically flushes metrics. Initialized after `ioContext` is initialized.
  kj::Maybe<kj::Promise<void>> metricsFlushLoopTask;

  // Allows sending requests back into this actor, recreating it as necessary. Safe to hold longer
  // than the Worker::Actor is alive.
  kj::Own<Loopback> loopback;

  TimerChannel& timerChannel;

  kj::ForkedPromise<void> shutdownPromise;
  kj::Own<kj::PromiseFulfiller<void>> shutdownFulfiller;

  // If this Actor has a HibernationManager, it means the Actor has recently accepted a Hibernatable
  // websocket. We eventually move the HibernationManager into the DeferredProxy task
  // (since it's long lived), but can still refer to the HibernationManager by passing a reference
  // in each CustomEvent.
  kj::Maybe<kj::Own<HibernationManager>> hibernationManager;
  kj::Maybe<uint16_t> hibernationEventType;
  kj::PromiseFulfillerPair<void> constructorFailedPaf = kj::newPromiseAndFulfiller<void>();

  struct ScheduledAlarm {
    ScheduledAlarm(kj::Date scheduledTime,
                   kj::PromiseFulfillerPair<WorkerInterface::AlarmResult> pf)
      : scheduledTime(scheduledTime), resultFulfiller(kj::mv(pf.fulfiller)),
        resultPromise(pf.promise.fork()) {}
    KJ_DISALLOW_COPY(ScheduledAlarm);
    ScheduledAlarm(ScheduledAlarm&&) = default;
    ~ScheduledAlarm() noexcept(false) {}

    kj::Date scheduledTime;
    WorkerInterface::AlarmFulfiller resultFulfiller;
    kj::ForkedPromise<WorkerInterface::AlarmResult> resultPromise;
    kj::Promise<void> cleanupPromise =
        resultPromise.addBranch().then([](WorkerInterface::AlarmResult&&){}, [](kj::Exception&&){});
    // The first thing we do after we get a result should be to remove the running alarm (if we got
    // that far). So we grab the first branch now and ignore any results, before anyone else has a
    // chance to do so.
  };
  struct RunningAlarm {
    kj::Date scheduledTime;
    kj::ForkedPromise<WorkerInterface::AlarmResult> resultPromise;
  };
  // If valid, we have an alarm invocation that has not yet received an `AlarmFulfiller` and thus
  // is either waiting for a running alarm or its scheduled time.
  kj::Maybe<ScheduledAlarm> maybeScheduledAlarm;

  // If valid, we have an alarm invocation that has received an `AlarmFulfiller` and is currently
  // considered running. This alarm is no longer cancellable.
  kj::Maybe<RunningAlarm> maybeRunningAlarm;

  // This is a forked promise so that we can schedule and then cancel multiple alarms while an alarm
  // is running.
  kj::ForkedPromise<void> runningAlarmTask = kj::Promise<void>(kj::READY_NOW).fork();

  Impl(Worker::Actor& self, Worker::Lock& lock, Actor::Id actorId,
       bool hasTransient, MakeActorCacheFunc makeActorCache,
       MakeStorageFunc makeStorage, kj::Own<Loopback> loopback,
       TimerChannel& timerChannel, kj::Own<ActorObserver> metricsParam,
       kj::Maybe<kj::Own<HibernationManager>> manager, kj::Maybe<uint16_t>& hibernationEventType,
       kj::PromiseFulfillerPair<void> paf = kj::newPromiseAndFulfiller<void>())
      : actorId(kj::mv(actorId)), makeStorage(kj::mv(makeStorage)),
        metrics(kj::mv(metricsParam)),
        hooks(loopback->addRef(), timerChannel, *metrics),
        inputGate(hooks), outputGate(hooks),
        loopback(kj::mv(loopback)),
        timerChannel(timerChannel),
        shutdownPromise(paf.promise.fork()),
        shutdownFulfiller(kj::mv(paf.fulfiller)),
        hibernationManager(kj::mv(manager)),
        hibernationEventType(kj::mv(hibernationEventType)) {
    JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
      if (hasTransient) {
        transient.emplace(js, js.obj());
      }

      actorCache = makeActorCache(self.worker->getIsolate().impl->actorCacheLru,
          outputGate, hooks);
    });
  }
};

kj::Promise<Worker::AsyncLock> Worker::takeAsyncLockWhenActorCacheReady(
    kj::Date now, Actor& actor, RequestObserver& request) const {
  auto lockTiming = getIsolate().getMetrics()
      .tryCreateLockTiming(kj::Maybe<RequestObserver&>(request));

  KJ_IF_SOME(c, actor.impl->actorCache) {
    KJ_IF_SOME(p, c.get()->evictStale(now)) {
      // Got backpressure, wait for it.
      // TODO(someday): Count this time period differently in lock timing data?
      co_await p;
    }
  }

  co_return co_await getIsolate().takeAsyncLockImpl(kj::mv(lockTiming));
}

void Worker::setConnectOverride(kj::String networkAddress, ConnectFn connectFn) {
  connectOverrides.upsert(kj::mv(networkAddress), kj::mv(connectFn));
}

kj::Maybe<Worker::ConnectFn&> Worker::getConnectOverride(kj::StringPtr networkAddress) {
  return connectOverrides.find(networkAddress);
}

Worker::Actor::Actor(const Worker& worker, kj::Maybe<RequestTracker&> tracker, Actor::Id actorId,
    bool hasTransient, MakeActorCacheFunc makeActorCache,
    kj::Maybe<kj::StringPtr> className, MakeStorageFunc makeStorage, Worker::Lock& lock,
    kj::Own<Loopback> loopback, TimerChannel& timerChannel, kj::Own<ActorObserver> metrics,
    kj::Maybe<kj::Own<HibernationManager>> manager, kj::Maybe<uint16_t> hibernationEventType)
    : worker(kj::atomicAddRef(worker)), tracker(tracker.map([](RequestTracker& tracker){
      return tracker.addRef();
    })) {
  impl = kj::heap<Impl>(*this, lock, kj::mv(actorId), hasTransient, kj::mv(makeActorCache),
                        kj::mv(makeStorage), kj::mv(loopback), timerChannel, kj::mv(metrics),
                        kj::mv(manager), hibernationEventType);

  KJ_IF_SOME(c, className) {
    KJ_IF_SOME(cls, lock.getWorker().impl->actorClasses.find(c)) {
      impl->classInstance = &(cls);
    } else {
      kj::throwFatalException(KJ_EXCEPTION(FAILED, "broken.ignored; no such actor class", c));
    }
  } else {
    impl->classInstance = Impl::NoClass();
  }
}

void Worker::Actor::ensureConstructed(IoContext& context) {
  KJ_IF_SOME(info, impl->classInstance.tryGet<Worker::Impl::ActorClassInfo*>()) {
    context.addWaitUntil(context.run([this, &info = *info](Worker::Lock& lock) {
      jsg::Lock& js = lock;

      kj::Maybe<jsg::Ref<api::DurableObjectStorage>> storage;
      KJ_IF_SOME(c, impl->actorCache) {
        storage = impl->makeStorage(lock, worker->getIsolate().getApi(), *c);
      }
      auto handler = info.cls(lock,
          jsg::alloc<api::DurableObjectState>(cloneId(), kj::mv(storage)),
          KJ_ASSERT_NONNULL(lock.getWorker().impl->env).addRef(js));

      // HACK: We set handler.env to undefined because we already passed the real env into the
      //   constructor, and we want the handler methods to act like they take just one parameter.
      //   We do the same for handler.ctx, as ExecutionContext related tasks are performed
      //   on the actor's state field instead.
      handler.env = js.v8Ref(js.v8Undefined());
      handler.ctx = kj::none;
      handler.missingSuperclass = info.missingSuperclass;

      impl->classInstance = kj::mv(handler);
    }).catch_([this](kj::Exception&& e) {
      auto msg = e.getDescription();

      if (!msg.startsWith("broken."_kj) && !msg.startsWith("remote.broken."_kj)) {
        // If we already set up a brokeness reason, we shouldn't override it.

        auto description = jsg::annotateBroken(msg, "broken.constructorFailed");
        e.setDescription(kj::mv(description));
      }

      impl->constructorFailedPaf.fulfiller->reject(kj::cp(e));
      impl->classInstance = kj::mv(e);
    }));

    impl->classInstance = Impl::Initializing();
  }
}

Worker::Actor::~Actor() noexcept(false) {
  // TODO(someday) Each IoContext contains a strong reference to its Actor, so a IoContext
  // object must be destroyed before their Actor. However, IoContext has its lifetime extended
  // by the IoContext::drain() promise which is stored in waitUntilTasks.
  // IoContext::drain() may hang if Actor::onShutdown() never resolves/rejects, which means the
  // IoContext and the Actor will not destruct as we'd expect. Ideally, we'd want an object
  // that represents Actor liveness that does what shutdown() does now. It should be reasonable to
  // implement that once we have tests that invoke the Actor dtor.

  // Destroy under lock.
  //
  // TODO(perf): In principle it could make sense to defer destruction of the actor until an async
  //   lock can be obtained. But, actor destruction is not terribly common and is not done when
  //   the actor is idle (so, no one is waiting), so it's not a huge deal. The runtime does
  //   potentially colocate multiple actors on the same thread, but they are always from the same
  //   namespace and hence would be locking the same isolate anyway -- it's not like one of the
  //   other actors could be running while we wait for this lock.
  worker->runInLockScope(Worker::Lock::TakeSynchronously(kj::none), [&](Worker::Lock& lock) {
    impl = nullptr;
  });
}

void Worker::Actor::shutdown(uint16_t reasonCode, kj::Maybe<const kj::Exception&> error) {
  // We're officially canceling all background work and we're going to destruct the Actor as soon
  // as all IoContexts that reference it go out of scope. We might still log additional
  // periodic messages, and that's good because we might care about that information. That said,
  // we're officially "broken" from this point because we cannot service background work and our
  // capability server should have triggered this (potentially indirectly) via its destructor.
  KJ_IF_SOME(r, impl->ioContext) {
    impl->metrics->shutdown(reasonCode, r.get()->getLimitEnforcer());
  } else {
    // The actor was shut down before the IoContext was even constructed, so no metrics are
    // written.
  }

  shutdownActorCache(error);

  impl->shutdownFulfiller->fulfill();
}

void Worker::Actor::shutdownActorCache(kj::Maybe<const kj::Exception&> error) {
  KJ_IF_SOME(ac, impl->actorCache) {
    ac.get()->shutdown(error);
  } else {
    // The actor was aborted before the actor cache was constructed, nothing to do.
  }
}

kj::Promise<void> Worker::Actor::onShutdown() {
  return impl->shutdownPromise.addBranch();
}

kj::Promise<void> Worker::Actor::onBroken() {
  // TODO(soon): Detect and report other cases of brokenness, as described in worker.capnp.

  kj::Promise<void> abortPromise = nullptr;

  KJ_IF_SOME(rc, impl->ioContext) {
    abortPromise = rc.get()->onAbort();
  } else {
    auto paf = kj::newPromiseAndFulfiller<kj::Promise<void>>();
    abortPromise = kj::mv(paf.promise);
    impl->abortFulfiller = kj::mv(paf.fulfiller);
  }

  return abortPromise
    // inputGate.onBroken() is covered by IoContext::onAbort(), but outputGate.onBroken() is
    // not.
    .exclusiveJoin(impl->outputGate.onBroken())
    .exclusiveJoin(kj::mv(impl->constructorFailedPaf.promise));
}

const Worker::Actor::Id& Worker::Actor::getId() {
  return impl->actorId;
}

Worker::Actor::Id Worker::Actor::cloneId(Worker::Actor::Id& id) {
  KJ_SWITCH_ONEOF(id) {
    KJ_CASE_ONEOF(coloLocalId, kj::String) {
      return kj::str(coloLocalId);
    }
    KJ_CASE_ONEOF(globalId, kj::Own<ActorIdFactory::ActorId>) {
      return globalId->clone();
    }
  }
  KJ_UNREACHABLE;
}

Worker::Actor::Id Worker::Actor::cloneId() {
  return cloneId(impl->actorId);
}

kj::Maybe<jsg::JsRef<jsg::JsValue>> Worker::Actor::getTransient(Worker::Lock& lock) {
  KJ_REQUIRE(&lock.getWorker() == worker.get());
  return impl->transient.map([&](jsg::JsRef<jsg::JsValue>& val) {
    return val.addRef(lock);
  });
}

kj::Maybe<ActorCacheInterface&> Worker::Actor::getPersistent() {
  return impl->actorCache;
}

kj::Own<Worker::Actor::Loopback> Worker::Actor::getLoopback() {
  return impl->loopback->addRef();
}

kj::Maybe<jsg::Ref<api::DurableObjectStorage>>
    Worker::Actor::makeStorageForSwSyntax(Worker::Lock& lock) {
  return impl->actorCache.map([&](kj::Own<ActorCacheInterface>& cache) {
    return impl->makeStorage(lock, worker->getIsolate().getApi(), *cache);
  });
}

void Worker::Actor::assertCanSetAlarm() {
  KJ_SWITCH_ONEOF(impl->classInstance) {
    KJ_CASE_ONEOF(_, Impl::NoClass) {
      // Once upon a time, we allowed actors without classes. Let's make a nicer message if we
      // we somehow see a classless actor attempt to run an alarm in the wild.
      JSG_FAIL_REQUIRE(TypeError,
          "Your Durable Object must be class-based in order to call setAlarm()");
    }
    KJ_CASE_ONEOF(_, Worker::Impl::ActorClassInfo*) {
      KJ_FAIL_ASSERT("setAlarm() invoked before Durable Object ctor");
    }
    KJ_CASE_ONEOF(_, Impl::Initializing) {
      // We don't explicitly know if we have an alarm handler or not, so just let it happen. We'll
      // handle it when we go to run the alarm.
      return;
    }
    KJ_CASE_ONEOF(handler, api::ExportedHandler) {
      JSG_REQUIRE(handler.alarm != nullptr, TypeError,
          "Your Durable Object class must have an alarm() handler in order to call setAlarm()");
      return;
    }
    KJ_CASE_ONEOF(exception, kj::Exception) {
      // We've failed in the ctor, might as well just throw that exception for now.
      kj::throwFatalException(kj::cp(exception));
    }
  }
  KJ_UNREACHABLE;
}

void Worker::Actor::Impl::HooksImpl::updateAlarmInMemory(kj::Maybe<kj::Date> newTime) {
  if (newTime == kj::none) {
    maybeAlarmPreviewTask = kj::none;
    return;
  }

  auto scheduledTime = KJ_ASSERT_NONNULL(newTime);

  auto retry = kj::coCapture([this, originalTime = scheduledTime]() -> kj::Promise<void> {
    kj::Date scheduledTime = originalTime;

    for (auto i : kj::zeroTo(WorkerInterface::ALARM_RETRY_MAX_TRIES)) {
      co_await timerChannel.atTime(scheduledTime);
      auto result = co_await loopback->getWorker(IoChannelFactory::SubrequestMetadata{})
          ->runAlarm(originalTime, i);

      if (result.outcome == EventOutcome::OK || !result.retry) {
        break;
      }

      auto delay = (WorkerInterface::ALARM_RETRY_START_SECONDS << i++) * kj::SECONDS;
      scheduledTime = timerChannel.now() + delay;
    }
  });

  maybeAlarmPreviewTask = retry();
}

kj::Maybe<kj::Promise<WorkerInterface::AlarmResult>> Worker::Actor::getAlarm(
    kj::Date scheduledTime) {
  KJ_IF_SOME(runningAlarm, impl->maybeRunningAlarm) {
    if (runningAlarm.scheduledTime == scheduledTime) {
      // The running alarm has the same time, we can just wait for it.
      return runningAlarm.resultPromise.addBranch();
    }
  }

  KJ_IF_SOME(scheduledAlarm, impl->maybeScheduledAlarm) {
    if (scheduledAlarm.scheduledTime == scheduledTime) {
      // The scheduled alarm has the same time, we can just wait for it.
      return scheduledAlarm.resultPromise.addBranch();
    }
  }

  return kj::none;
}

kj::Promise<WorkerInterface::ScheduleAlarmResult> Worker::Actor::scheduleAlarm(
    kj::Date scheduledTime) {
  KJ_IF_SOME(runningAlarm, impl->maybeRunningAlarm) {
    if (runningAlarm.scheduledTime == scheduledTime) {
      // The running alarm has the same time, we can just wait for it.
      auto result = co_await runningAlarm.resultPromise;
      co_return result;
    }
  }

  KJ_IF_SOME(scheduledAlarm, impl->maybeScheduledAlarm) {
      // We had a previously scheduled alarm, let's cancel it.
      scheduledAlarm.resultFulfiller.cancel();
      impl->maybeScheduledAlarm = kj::none;
  }

  KJ_IASSERT(impl->maybeScheduledAlarm == kj::none);
  auto& scheduledAlarm = impl->maybeScheduledAlarm.emplace(
      scheduledTime, kj::newPromiseAndFulfiller<WorkerInterface::AlarmResult>());

  // Probably don't need to use kj::coCapture for this but doing so just to be on the
  // safe side...
  auto whenCanceled = (kj::coCapture([&scheduledAlarm]()
      -> kj::Promise<WorkerInterface::ScheduleAlarmResult> {
    // We've been cancelled, so return that result. Note that we cannot be resolved any other
    // way until we return an AlarmFulfiller below.
    co_return co_await scheduledAlarm.resultPromise;
  }))();

  // Date.now() < scheduledTime when the alarm comes in, since we subtract elapsed CPU time from
  // the time of last I/O in the implementation of Date.now(). This difference could be used to
  // implement a spectre timer, so we have to wait a little longer until
  // `Date.now() == scheduledTime`. Note that this also means that we could invoke ahead of its
  // `scheduledTime` and we'll delay until appropriate, this may be useful in cases of clock skew.

  co_return co_await handleAlarm(scheduledTime).exclusiveJoin(kj::mv(whenCanceled));
}

kj::Promise<WorkerInterface::ScheduleAlarmResult> Worker::Actor::handleAlarm(
    kj::Date scheduledTime) {
  // Let's wait for any running alarm to cleanup before we even delay.
  co_await impl->runningAlarmTask;

  co_await KJ_ASSERT_NONNULL(impl->ioContext)->atTime(scheduledTime);
  // It's time to run! Let's tear apart the scheduled alarm and make a running alarm.

  // `maybeScheduledAlarm` should have the same value we emplaced above. If another call to
  // `scheduleAlarm()` emplaced a new value, then `whenCanceled` should have resolved which
  // cancels this this promise chain.
  auto scheduledAlarm = KJ_ASSERT_NONNULL(kj::mv(impl->maybeScheduledAlarm));
  impl->maybeScheduledAlarm = kj::none;

  impl->maybeRunningAlarm.emplace(Impl::RunningAlarm{
    .scheduledTime = scheduledAlarm.scheduledTime,
    .resultPromise = kj::mv(scheduledAlarm.resultPromise),
  });
  impl->runningAlarmTask = scheduledAlarm.cleanupPromise.attach(kj::defer([this](){
    // As soon as we get fulfilled or rejected, let's unset this alarm as the running alarm.
    impl->maybeRunningAlarm = kj::none;
  })).eagerlyEvaluate([](kj::Exception&& e){
    LOG_EXCEPTION("actorAlarmCleanup", e);
  }).fork();
  co_return kj::mv(scheduledAlarm.resultFulfiller);
}

kj::Maybe<api::ExportedHandler&> Worker::Actor::getHandler() {
  KJ_SWITCH_ONEOF(impl->classInstance) {
    KJ_CASE_ONEOF(_, Impl::NoClass) {
      return kj::none;
    }
    KJ_CASE_ONEOF(_, Worker::Impl::ActorClassInfo*) {
      KJ_FAIL_ASSERT("ensureConstructed() wasn't called");
    }
    KJ_CASE_ONEOF(_, Impl::Initializing) {
      // This shouldn't be possible because ensureConstructed() would have initiated the
      // construction task which would have taken an input lock as well as the isolate lock,
      // which should have prevented any other code from executing on the actor until they
      // were released.
      KJ_FAIL_ASSERT("actor still initializing when getHandler() called");
    }
    KJ_CASE_ONEOF(handler, api::ExportedHandler) {
      return handler;
    }
    KJ_CASE_ONEOF(exception, kj::Exception) {
      kj::throwFatalException(kj::cp(exception));
    }
  }
  KJ_UNREACHABLE;
}

ActorObserver& Worker::Actor::getMetrics() {
  return *impl->metrics;
}

InputGate& Worker::Actor::getInputGate() {
  return impl->inputGate;
}

OutputGate& Worker::Actor::getOutputGate() {
  return impl->outputGate;
}

kj::Maybe<IoContext&> Worker::Actor::getIoContext() {
  return impl->ioContext.map([](kj::Own<IoContext>& rc) -> IoContext& {
    return *rc;
  });
}

void Worker::Actor::setIoContext(kj::Own<IoContext> context) {
  KJ_REQUIRE(impl->ioContext == kj::none);
  KJ_IF_SOME(f, impl->abortFulfiller) {
    f.get()->fulfill(context->onAbort());
    impl->abortFulfiller = kj::none;
  }
  auto& limitEnforcer = context->getLimitEnforcer();
  impl->ioContext = kj::mv(context);
  impl->metricsFlushLoopTask = impl->metrics->flushLoop(impl->timerChannel, limitEnforcer)
      .eagerlyEvaluate([](kj::Exception&& e) {
    LOG_EXCEPTION("actorMetricsFlushLoop", e);
  });
}

kj::Maybe<Worker::Actor::HibernationManager&> Worker::Actor::getHibernationManager() {
  return impl->hibernationManager.map([](kj::Own<HibernationManager>& hib) -> HibernationManager& {
    return *hib;
  });
}

void Worker::Actor::setHibernationManager(kj::Own<HibernationManager> hib) {
  KJ_REQUIRE(impl->hibernationManager == kj::none);
  hib->setTimerChannel(impl->timerChannel);
  // Not the cleanest way to provide hibernation manager with a timer channel reference, but
  // where HibernationManager is constructed (actor-state), we don't have a timer channel ref.
  impl->hibernationManager = kj::mv(hib);
}

kj::Maybe<uint16_t> Worker::Actor::getHibernationEventType() {
  return impl->hibernationEventType;
}

kj::Own<Worker::Actor> Worker::Actor::addRef() {
  KJ_IF_SOME(t, tracker) {
    return kj::addRef(*this).attach(t.get()->startRequest());
  } else {
    return kj::addRef(*this);
  }
}

// =======================================================================================

uint Worker::Isolate::getCurrentLoad() const {
  return __atomic_load_n(&impl->lockAttemptGauge, __ATOMIC_RELAXED);
}

uint Worker::Isolate::getLockSuccessCount() const {
  return __atomic_load_n(&impl->lockSuccessCount, __ATOMIC_RELAXED);
}

kj::Own<const Worker::Script> Worker::Isolate::newScript(
    kj::StringPtr scriptId, Script::Source source,
    IsolateObserver::StartType startType, bool logNewScript,
    kj::Maybe<ValidationErrorReporter&> errorReporter) const {
  // Script doesn't already exist, so compile it.
  return kj::atomicRefcounted<Script>(kj::atomicAddRef(*this), scriptId, kj::mv(source),
                                      startType, logNewScript, errorReporter);
}

void Worker::Isolate::completedRequest() const {
  limitEnforcer->completedRequest(id);
}

bool Worker::Isolate::isInspectorEnabled() const {
  return impl->inspector != kj::none;
}

namespace {

// We only run the inspector within process sandboxes. There, it is safe to query the real clock
// for some things, and we do so because we may not have a IoContext available to get
// Spectre-safe time.

// Monotonic time in seconds with millisecond precision.
double getMonotonicTimeForProcessSandboxOnly() {
  KJ_REQUIRE(!isMultiTenantProcess(), "precise timing not safe in multi-tenant processes");
  auto timePoint = kj::systemPreciseMonotonicClock().now();
  return (timePoint - kj::origin<kj::TimePoint>()) / kj::MILLISECONDS / 1e3;
}

// Wall time in seconds with millisecond precision.
double getWallTimeForProcessSandboxOnly() {
  KJ_REQUIRE(!isMultiTenantProcess(), "precise timing not safe in multi-tenant processes");
  auto timePoint = kj::systemPreciseCalendarClock().now();
  return (timePoint - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1e3;
}
}  // namespace

class Worker::Isolate::ResponseStreamWrapper final: public kj::AsyncOutputStream {
public:
  ResponseStreamWrapper(kj::Own<const Isolate> isolate,
                        kj::String requestId,
                        kj::Own<kj::AsyncOutputStream> inner,
                        api::StreamEncoding encoding,
                        RequestObserver& requestMetrics)
      : constIsolate(kj::mv(isolate)), requestId(kj::mv(requestId)), inner(kj::mv(inner)),
        requestMetrics(requestMetrics) {
    if (encoding == api::StreamEncoding::GZIP) {
      compStream.emplace().init<kj::GzipOutputStream>(decodedBuf,
          kj::GzipOutputStream::DECOMPRESS);
    } else if (encoding == api::StreamEncoding::BROTLI) {
      compStream.emplace().init<kj::BrotliOutputStream>(decodedBuf,
          kj::BrotliOutputStream::DECOMPRESS);
    }
  }

  ~ResponseStreamWrapper() noexcept(false) {
    jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
      Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(requestMetrics), stackScope);
      auto& isolate = const_cast<Isolate&>(*constIsolate);

      KJ_IF_SOME(i, isolate.currentInspectorSession) {
        capnp::MallocMessageBuilder message;

        auto event = message.initRoot<cdp::Event>();

        auto params = event.initNetworkLoadingFinished();
        params.setRequestId(requestId);
        params.setEncodedDataLength(rawSize);
        params.setTimestamp(getMonotonicTimeForProcessSandboxOnly());
        auto response = params.initCfResponse();
        KJ_IF_SOME(body, decodedBuf.getArray()) {
          response.setBase64Encoded(true);
          response.setBody(kj::encodeBase64(body));
        }

        i.sendNotification(event);
      }
    });
  }

  kj::Promise<void> write(const void* buffer, size_t size) override {
    reportBytes(kj::arrayPtr(reinterpret_cast<const byte*>(buffer), size));
    return inner->write(buffer, size);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    for (auto& piece: pieces) {
      reportBytes(piece);
    }
    return inner->write(pieces);
  }
  void reportBytes(kj::ArrayPtr<const byte> buffer) {
    if (buffer.size() == 0) {
      return;
    }

    rawSize += buffer.size();

    auto prevDecodedSize = decodedBuf.getWrittenSize();
    KJ_IF_SOME(comp, compStream) {
      KJ_SWITCH_ONEOF(comp) {
        KJ_CASE_ONEOF(gzip, kj::GzipOutputStream) {
          // On invalid gzip discard the previously decoded body and rethrow to stop the stream.
          // This way we will report sizes up to this point but won't read any more invalid data.
          KJ_ON_SCOPE_FAILURE(decodedBuf.reset());

          gzip.write(buffer.begin(), buffer.size());
          gzip.flush();
        }
        KJ_CASE_ONEOF(brotli, kj::BrotliOutputStream) {
          KJ_ON_SCOPE_FAILURE(decodedBuf.reset());

          brotli.write(buffer.begin(), buffer.size());
          brotli.flush();
        }
      }
    } else {
      decodedBuf.write(buffer.begin(), buffer.size());
    }
    auto decodedChunkSize = decodedBuf.getWrittenSize() - prevDecodedSize;

    jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
      Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(requestMetrics), stackScope);
      auto& isolate = const_cast<Isolate&>(*constIsolate);

      KJ_IF_SOME(i, isolate.currentInspectorSession) {
        capnp::MallocMessageBuilder message;

        auto event = message.initRoot<cdp::Event>();

        auto params = event.initNetworkDataReceived();
        params.setRequestId(requestId);
        params.setEncodedDataLength(buffer.size());
        params.setDataLength(decodedChunkSize);
        params.setTimestamp(getMonotonicTimeForProcessSandboxOnly());

        i.sendNotification(event);
      }
    });
  }

  // Intentionally not wrapping `tryPumpFrom` to force consumer to use `write` in a loop which,
  // in turn, will report each chunk to the inspector to show progress of a slow response.

  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }

private:
  using InspectorLock = InspectorChannelImpl::InspectorLock;

  kj::Own<const Isolate> constIsolate;
  kj::String requestId;
  kj::Own<kj::AsyncOutputStream> inner;
  size_t rawSize = 0;
  LimitedBodyWrapper decodedBuf;
  kj::Maybe<kj::OneOf<kj::GzipOutputStream, kj::BrotliOutputStream>> compStream;
  RequestObserver& requestMetrics;
};

class Worker::Isolate::SubrequestClient final: public WorkerInterface {
public:
  explicit SubrequestClient(kj::Own<const Isolate> isolate,
      kj::Own<WorkerInterface> inner, kj::HttpHeaderId contentEncodingHeaderId,
      RequestObserver& requestMetrics)
      : constIsolate(kj::mv(isolate)), inner(kj::mv(inner)),
        contentEncodingHeaderId(contentEncodingHeaderId),
        requestMetrics(kj::addRef(requestMetrics)) {}
  KJ_DISALLOW_COPY_AND_MOVE(SubrequestClient);
  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) override;
  kj::Promise<void> connect(
      kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
      kj::HttpService::ConnectResponse& tunnel,
      kj::HttpConnectSettings settings) override;
  void prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

private:
  kj::Own<const Isolate> constIsolate;
  kj::Own<WorkerInterface> inner;
  kj::HttpHeaderId contentEncodingHeaderId;
  kj::Own<RequestObserver> requestMetrics;
};

kj::Promise<void> Worker::Isolate::SubrequestClient::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) {
  using InspectorLock = InspectorChannelImpl::InspectorLock;

  auto signalRequest =
      [this, method, urlCopy = kj::str(url), headersCopy = headers.clone()]
      () -> kj::Maybe<kj::String> {
    return jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) -> kj::Maybe<kj::String> {
      Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(*requestMetrics), stackScope);
      auto& lock = *recordedLock.lock;
      auto& isolate = const_cast<Isolate&>(*constIsolate);

      if (isolate.currentInspectorSession == kj::none) {
        return kj::none;
      }

      auto& i = KJ_ASSERT_NONNULL(isolate.currentInspectorSession);
      if (!i.isNetworkEnabled()) {
        return kj::none;
      }

      return lock.withinHandleScope([&] {
        auto requestId = kj::str(isolate.nextRequestId++);

        capnp::MallocMessageBuilder message;

        auto event = message.initRoot<cdp::Event>();

        auto params = event.initNetworkRequestWillBeSent();
        params.setRequestId(requestId);
        params.setLoaderId("");
        params.setTimestamp(getMonotonicTimeForProcessSandboxOnly());
        params.setWallTime(getWallTimeForProcessSandboxOnly());
        params.setType(cdp::Page::ResourceType::FETCH);

        auto initiator = params.initInitiator();
        initiator.setType(cdp::Network::Initiator::Type::SCRIPT);
        stackTraceToCDP(lock, initiator.initStack());

        auto request = params.initRequest();
        request.setUrl(urlCopy);
        request.setMethod(kj::str(method));

        headersToCDP(headersCopy, request.initHeaders());

        i.sendNotification(event);
        return kj::mv(requestId);
      });
    });
  };

  auto signalResponse = [this](kj::String requestId,
      uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
      kj::Own<kj::AsyncOutputStream> responseBody) -> kj::Own<kj::AsyncOutputStream> {
    // Note that we cannot take the isolate lock here, because if this is a worker-to-worker
    // subrequest, the destination isolate's lock may already be held, and we can't take multiple
    // isolate locks at once as this could lead to deadlock if the lock orders aren't consistent.
    //
    // Meanwhile, though, `statusText` and `headers` may point to things that will go away
    // immediately after we return. So, let's construct our message now, so that we don't have to
    // make redundant copies.
    //
    // Note that signalResponse() is only called at all if signalRequest() determined that network
    // inspection is enabled.

    auto message = kj::heap<capnp::MallocMessageBuilder>();

    auto event = message->initRoot<cdp::Event>();

    auto params = event.initNetworkResponseReceived();
    params.setRequestId(requestId);
    params.setTimestamp(getMonotonicTimeForProcessSandboxOnly());
    params.setType(cdp::Page::ResourceType::OTHER);

    auto response = params.initResponse();
    response.setStatus(statusCode);
    response.setStatusText(statusText);
    response.setProtocol("http/1.1");
    KJ_IF_SOME(type, headers.get(kj::HttpHeaderId::CONTENT_TYPE)) {
      KJ_IF_SOME(parsed, MimeType::tryParse(type, MimeType::IGNORE_PARAMS)) {
        response.setMimeType(parsed.toString());

        // Normally Chrome would know what it's loading based on an element or API used for
        // the request. We don't have that privilege, but still want network filters to work,
        // so we do our best-effort guess of the resource type based on its mime type.
        if (MimeType::HTML == parsed || MimeType::XHTML == parsed) {
          params.setType(cdp::Page::ResourceType::DOCUMENT);
        } else if (MimeType::CSS == parsed) {
          params.setType(cdp::Page::ResourceType::STYLESHEET);
        } else if (MimeType::isJavascript(parsed)) {
          params.setType(cdp::Page::ResourceType::SCRIPT);
        } else if (MimeType::isImage(parsed)) {
          params.setType(cdp::Page::ResourceType::IMAGE);
        } else if (MimeType::isAudio(parsed) || MimeType::isVideo(parsed)) {
          params.setType(cdp::Page::ResourceType::MEDIA);
        } else if (MimeType::isFont(parsed)) {
          params.setType(cdp::Page::ResourceType::FONT);
        } else if (MimeType::MANIFEST_JSON == parsed) {
          params.setType(cdp::Page::ResourceType::MANIFEST);
        } else if (MimeType::VTT == parsed) {
          params.setType(cdp::Page::ResourceType::TEXT_TRACK);
        } else if (MimeType::EVENT_STREAM == parsed) {
          params.setType(cdp::Page::ResourceType::EVENT_SOURCE);
        } else if (MimeType::isXml(parsed) || MimeType::isJson(parsed)) {
          params.setType(cdp::Page::ResourceType::XHR);
        }

      } else {
        response.setMimeType(MimeType::PLAINTEXT_STRING);
      }
    } else {
      response.setMimeType(MimeType::PLAINTEXT_STRING);
    }
    headersToCDP(headers, response.initHeaders());

    auto encoding = api::StreamEncoding::IDENTITY;
    KJ_IF_SOME(encodingStr, headers.get(contentEncodingHeaderId)) {
      if (encodingStr == "gzip") {
        encoding = api::StreamEncoding::GZIP;
      } else if (encodingStr == "br") {
        encoding = api::StreamEncoding::BROTLI;
      }
    }

    // Defer to a later turn of the event loop so that it's safe to take a lock.
    return kj::newPromisedStream(kj::evalLater(
        [this, responseBody = kj::mv(responseBody), message = kj::mv(message), event, encoding,
         requestId = kj::mv(requestId)]
        () mutable -> kj::Own<kj::AsyncOutputStream> {
      // Now we know we can lock...
      return jsg::runInV8Stack([&](jsg::V8StackScope& stackScope)
          mutable -> kj::Own<kj::AsyncOutputStream> {
        Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(*requestMetrics), stackScope);
        auto& isolate = const_cast<Isolate&>(*constIsolate);

        // We shouldn't even get here if network inspection isn't active since signalRequest() would
        // have returned null... but double-check anyway.
        if (isolate.currentInspectorSession == kj::none) {
          return kj::mv(responseBody);
        }

        auto& i = KJ_ASSERT_NONNULL(isolate.currentInspectorSession);
        if (!i.isNetworkEnabled()) {
          return kj::mv(responseBody);
        }

        i.sendNotification(event);

        return kj::heap<ResponseStreamWrapper>(kj::atomicAddRef(*constIsolate),
                                              kj::mv(requestId),
                                              kj::mv(responseBody),
                                              encoding,
                                              *requestMetrics);
      });
    }));
  };
  typedef decltype(signalResponse) SignalResponse;

  class ResponseWrapper final: public kj::HttpService::Response {
  public:
    ResponseWrapper(kj::HttpService::Response& inner, kj::String requestId,
                    SignalResponse signalResponse)
        : inner(inner), requestId(kj::mv(requestId)), signalResponse(kj::mv(signalResponse)) {}

    kj::Own<kj::AsyncOutputStream> send(
        uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
      auto body = inner.send(statusCode, statusText, headers, expectedBodySize);
      return signalResponse(kj::mv(requestId), statusCode, statusText, headers, kj::mv(body));
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
      auto webSocket = inner.acceptWebSocket(headers);
      // TODO(someday): Support sending WebSocket frames over CDP. For now we fake an empty
      //   response.
      signalResponse(kj::mv(requestId), 101, "Switching Protocols", headers,
                     newNullOutputStream());
      return kj::mv(webSocket);
    }

  private:
    kj::HttpService::Response& inner;
    kj::String requestId;
    SignalResponse signalResponse;
  };

  // For accurate lock metrics, we want to avoid taking a recursive isolate lock, so we postpone
  // the request until a later turn of the event loop.
  auto maybeRequestId = co_await kj::evalLater(kj::mv(signalRequest));

  KJ_IF_SOME(rid, maybeRequestId) {
    ResponseWrapper wrapper(response, kj::mv(rid), kj::mv(signalResponse));
    co_await inner->request(method, url, headers, requestBody, wrapper);
  } else {
    co_await inner->request(method, url, headers, requestBody, response);
  }
}

kj::Promise<void> Worker::Isolate::SubrequestClient::connect(
    kj::StringPtr host, const kj::HttpHeaders& headers, kj::AsyncIoStream& connection,
    kj::HttpService::ConnectResponse& tunnel, kj::HttpConnectSettings settings) {
  // TODO(someday): EW-7116 Figure out how to represent TCP connections in the devtools network tab.
  return inner->connect(host, headers, connection, tunnel, kj::mv(settings));
}

// TODO(someday): Log other kinds of subrequests?
void Worker::Isolate::SubrequestClient::prewarm(kj::StringPtr url) {
  inner->prewarm(url);
}
kj::Promise<WorkerInterface::ScheduledResult> Worker::Isolate::SubrequestClient::runScheduled(
    kj::Date scheduledTime, kj::StringPtr cron) {
  return inner->runScheduled(scheduledTime, cron);
}
kj::Promise<WorkerInterface::AlarmResult> Worker::Isolate::SubrequestClient::runAlarm(
    kj::Date scheduledTime, uint32_t retryCount) {
  return inner->runAlarm(scheduledTime, retryCount);
}
kj::Promise<WorkerInterface::CustomEvent::Result>
    Worker::Isolate::SubrequestClient::customEvent(kj::Own<CustomEvent> event) {
  return inner->customEvent(kj::mv(event));
}

kj::Own<WorkerInterface> Worker::Isolate::wrapSubrequestClient(
    kj::Own<WorkerInterface> client,
    kj::HttpHeaderId contentEncodingHeaderId,
    RequestObserver& requestMetrics) const {
  if (impl->inspector != kj::none) {
    client = kj::heap<SubrequestClient>(
        kj::atomicAddRef(*this), kj::mv(client), contentEncodingHeaderId, requestMetrics);
  }

  return client;
}

}  // namespace workerd
