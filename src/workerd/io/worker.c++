// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <cstdint>
#include <workerd/io/worker.h>
#include <workerd/io/promise-wrapper.h>
#include "actor-cache.h"
#include <workerd/util/batch-queue.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/streams.h>  // for api::StreamEncoding
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/jsg.h>
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
#include <windows.h>
#include <kj/windows-sanity.h>
#else
#include <sys/syscall.h>
#endif

namespace v8_inspector {
  kj::String KJ_STRINGIFY(const v8_inspector::StringView& view) {
    if (view.is8Bit()) {
      auto bytes = kj::arrayPtr(view.characters8(), view.length());
      for (auto b: bytes) {
        if (b & 0x80) {
          // Ugh, the bytes aren't just ASCII. We need to re-encode.
          auto utf16 = kj::heapArray<char16_t>(bytes.size());
          for (auto i: kj::indices(bytes)) {
            utf16[i] = bytes[i];
          }
          return kj::decodeUtf16(utf16);
        }
      }

      // Looks like it's all ASCII.
      return kj::str(bytes.asChars());
    } else {
      return kj::decodeUtf16(kj::arrayPtr(
          reinterpret_cast<const char16_t*>(view.characters16()), view.length()));
    }
  }
}

namespace workerd {

namespace {

class StringViewWithScratch: public v8_inspector::StringView {
public:
  StringViewWithScratch(v8_inspector::StringView text, kj::Array<char16_t>&& scratch)
      : v8_inspector::StringView(text), scratch(kj::mv(scratch)) {}

private:
  kj::Array<char16_t> scratch;
};

StringViewWithScratch toStringView(kj::StringPtr text) {
  bool isAscii = true;
  for (char c: text) {
    if (c & 0x80) {
      isAscii = false;
      break;
    }
  }

  if (isAscii) {
    return { v8_inspector::StringView(text.asBytes().begin(), text.size()), nullptr };
  } else {
    kj::Array<char16_t> scratch = kj::encodeUtf16(text);
    return {
      v8_inspector::StringView(reinterpret_cast<uint16_t*>(scratch.begin()), scratch.size()),
      kj::mv(scratch)
    };
  }
}

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

void stackTraceToCDP(v8::Isolate* isolate, cdp::Runtime::StackTrace::Builder builder) {
  // TODO(cleanup): Maybe use V8Inspector::captureStackTrace() which does this for us. However, it
  //   produces protocol objects in its own format which want to handle their whole serialization
  //   to JSON. Also, those protocol objects are defined in generated code which we currently don't
  //   include in our cached V8 build artifacts; we'd need to fix that. But maybe we should really
  //   be using the V8-generated protocol objects rather than our parallel capnp versions!

  auto stackTrace = v8::StackTrace::CurrentStackTrace(isolate, 10);
  auto frameCount = stackTrace->GetFrameCount();
  auto callFrames = builder.initCallFrames(frameCount);
  for (int i = 0; i < frameCount; i++) {
    auto src = stackTrace->GetFrame(isolate, i);
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

kj::StringPtr KJ_STRINGIFY(UncaughtExceptionSource value) {
  switch (value) {
    case UncaughtExceptionSource::INTERNAL:         return "Uncaught"_kj;
    case UncaughtExceptionSource::INTERNAL_ASYNC:   return "Uncaught (in promise)"_kj;
    case UncaughtExceptionSource::ASYNC_TASK:       return "Uncaught (async)"_kj;
    case UncaughtExceptionSource::REQUEST_HANDLER:  return "Uncaught (in response)"_kj;
    case UncaughtExceptionSource::TRACE_HANDLER:    return "Uncaught (in trace)"_kj;
    case UncaughtExceptionSource::ALARM_HANDLER:    return "Uncaught (in alarm)"_kj;
  };
  KJ_UNREACHABLE;
}

namespace {

void addJsStackTrace(v8::Local<v8::Context> context,
                     kj::Vector<kj::String>& lines, v8::Local<v8::Message> message) {
  // TODO(someday): Relying on v8::Message to pass around source locations means
  // we can't provide the module name for errors like compiling wasm modules. We
  // should have our own type, but it requires a refactor of how we pass around errors
  // for script startup.

  auto addLineCol = [](kj::StringTree str, int line, int col) {
    if (line != v8::Message::kNoLineNumberInfo) {
      str = kj::strTree(kj::mv(str), ":", line);
      if (col != v8::Message::kNoColumnInfo) {
        str = kj::strTree(kj::mv(str), ":", col);
      }
    }
    return str;
  };

  if (!message.IsEmpty()) {
    auto trace = message->GetStackTrace();
    if (trace.IsEmpty() || trace->GetFrameCount() == 0) {
      kj::StringTree locationStr;

      auto resourceNameVal = message->GetScriptResourceName();
      if (resourceNameVal->IsString()) {
        auto resourceName = resourceNameVal.As<v8::String>();
        if (!resourceName.IsEmpty() && resourceName->Length() != 0) {
          locationStr = kj::strTree("  at ", resourceName);
        }
      }

      auto lineNumber = jsg::check(message->GetLineNumber(context));
      auto columnNumber = jsg::check(message->GetStartColumn(context));
      locationStr = addLineCol(kj::mv(locationStr), lineNumber, columnNumber);

      if (locationStr.size() > 0) {
        lines.add(locationStr.flatten());
      }
    } else {
      for (auto i: kj::zeroTo(trace->GetFrameCount())) {
        auto frame = trace->GetFrame(context->GetIsolate(), i);
        kj::StringTree locationStr;

        auto scriptName = frame->GetScriptName();
        if (!scriptName.IsEmpty() && scriptName->Length() != 0) {
          locationStr = kj::strTree("  at ", scriptName);
        } else {
          locationStr = kj::strTree("  at worker.js");
        }

        auto lineNumber = frame->GetLineNumber();
        auto columnNumber = frame->GetColumn();
        locationStr = addLineCol(kj::mv(locationStr), lineNumber, columnNumber);

        auto func = frame->GetFunctionName();
        if (!func.IsEmpty() && func->Length() != 0) {
          locationStr = kj::strTree(kj::mv(locationStr), " in ", func);
        }

        lines.add(locationStr.flatten());
      }
    }
  }
}

void sendExceptionToInspector(v8_inspector::V8Inspector& inspector, v8::Local<v8::Context> context,
                              kj::StringPtr description) {
  // Inform the inspector of a problem not associated with any particular exception object.
  //
  // Passes `description` as the exception's detailed message, dummy values for everything else.

  inspector.exceptionThrown(context, v8_inspector::StringView(), v8::Local<v8::Value>(),
      toStringView(description), v8_inspector::StringView(),
      0, 0, nullptr, 0);
}

void sendExceptionToInspector(v8_inspector::V8Inspector& inspector, v8::Local<v8::Context> context,
                              UncaughtExceptionSource source, v8::Local<v8::Value> exception,
                              v8::Local<v8::Message> message) {
  // Inform the inspector of an exception thrown.
  //
  // Passes `source` as the exception's short message. Reconstructs `message` from `exception` if
  // `message` is empty.

  if (message.IsEmpty()) {
    // This exception didn't come with a Message. This can happen for exceptions delivered via
    // v8::Promise::Catch(), or for exceptions which were tunneled through C++ promises. In the
    // latter case, V8 will create a Message based on the current stack trace, but it won't be
    // super meaningful.
    message = v8::Exception::CreateMessage(context->GetIsolate(), exception);
    KJ_ASSERT(!message.IsEmpty());
  }

  auto stackTrace = message->GetStackTrace();

  // The resource name is whatever we set in the Script ctor, e.g. "worker.js".
  auto scriptResourceName = message->GetScriptResourceName();

  auto lineNumber = message->GetLineNumber(context).FromMaybe(0);
  auto startColumn = message->GetStartColumn(context).FromMaybe(0);

  // TODO(soon): EW-2636 Pass a real "script ID" as the last parameter instead of 0. I suspect this
  //   has something to do with the incorrect links in the console when it logs uncaught exceptions.
  inspector.exceptionThrown(context, toStringView(kj::str(source)), exception,
      toStringView(kj::str(message->Get())), toStringView(kj::str(scriptResourceName)),
      lineNumber, startColumn, inspector.createStackTrace(stackTrace), 0);
}

void addExceptionToTrace(jsg::Lock& js, IoContext &ioContext, WorkerTracer& tracer,
                         v8::Local<v8::Context> context, UncaughtExceptionSource source,
                         v8::Local<v8::Value> exception,
                         const jsg::TypeHandler<Worker::ApiIsolate::ErrorInterface>&
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
  KJ_IF_MAYBE(n, error.name) {
    name = kj::str(*n);
  } else {
    name = kj::str("Error");
  }
  kj::String message;
  KJ_IF_MAYBE(m, error.message) {
    message = kj::str(*m);
  }
  // TODO(someday): Limit size of exception content?
  tracer.addException(timestamp, kj::mv(name), kj::mv(message));
}

void reportStartupError(
    kj::StringPtr id,
    jsg::Lock& lock,
    const kj::Maybe<std::unique_ptr<v8_inspector::V8Inspector>>& inspector,
    v8::Local<v8::Context> context,
    const IsolateLimitEnforcer& limitEnforcer,
    kj::Maybe<kj::Exception> maybeLimitError,
    v8::TryCatch& catcher,
    kj::Maybe<Worker::ValidationErrorReporter&> errorReporter,
    kj::Maybe<kj::Exception>& permanentException) {
  v8::TryCatch catcher2(lock.v8Isolate);
  kj::Maybe<kj::Exception> maybeLimitError2;
  try {
    KJ_IF_MAYBE(limitError, maybeLimitError) {
      auto description = jsg::extractTunneledExceptionDescription(limitError->getDescription());

      auto& ex = permanentException.emplace(kj::mv(*limitError));
      KJ_IF_MAYBE(e, errorReporter) {
        e->addError(kj::heapString(description));
      } else KJ_IF_MAYBE(i, inspector) {
        // We want to extend just enough cpu time as is necessary to report the exception
        // to the inspector here. 10 milliseconds should be more than enough.
        auto limitScope = limitEnforcer.enterLoggingJs(lock, maybeLimitError2);
        sendExceptionToInspector(*i->get(), context, description);
        // When the inspector is active, we don't want to throw here because then the inspector
        // won't be able to connect and the developer will never know what happened.
      } else {
        // We should never get here in production if we've validated scripts before deployment.
        KJ_LOG(ERROR, "script startup exceeded resource limits", id, ex);
        kj::throwFatalException(kj::cp(ex));
      }
    } else if (catcher.HasCaught()) {
      v8::HandleScope handleScope(lock.v8Isolate);
      auto exception = catcher.Exception();

      permanentException = lock.exceptionToKj(jsg::Value(lock.v8Isolate, exception));

      KJ_IF_MAYBE(e, errorReporter) {
        auto limitScope = limitEnforcer.enterLoggingJs(lock, maybeLimitError2);

        kj::Vector<kj::String> lines;
        lines.add(kj::str("Uncaught ", jsg::extractTunneledExceptionDescription(
            KJ_ASSERT_NONNULL(permanentException).getDescription())));
        addJsStackTrace(context, lines, catcher.Message());
        e->addError(kj::strArray(lines, "\n"));

      } else KJ_IF_MAYBE(i, inspector) {
        auto limitScope = limitEnforcer.enterLoggingJs(lock, maybeLimitError2);
        sendExceptionToInspector(*i->get(), context, UncaughtExceptionSource::INTERNAL,
                                 exception, catcher.Message());
        // When the inspector is active, we don't want to throw here because then the inspector
        // won't be able to connect and the developer will never know what happened.
      } else {
        // We should never get here in production if we've validated scripts before deployment.
        kj::Vector<kj::String> lines;
        addJsStackTrace(context, lines, catcher.Message());
        auto trace = kj::strArray(lines, "; ");
        auto description = KJ_ASSERT_NONNULL(permanentException).getDescription();
        if (description == "jsg.SyntaxError: \\8 and \\9 are not allowed in template strings.") {
          // HACK: There are two scripts in production that throw this at startup and we can't get
          //   in contact with the owners to fix them. It should be impossible to upload new
          //   scripts with this problem as the validator will block it. We'll return normally
          //   here, which means that script startup will appear to succeed, but all requests to
          //   the isolate will throw the original exception, via `permanentException`. This avoids
          //   log spam and avoids reloading the script from scratch on every request.
          //
          // TODO(soon): We add logging here to see if this hack is still necessary or if it can be
          // removed. Adding this additional logging should be temporary! If we hit this log in
          // sentry even once, then we'll keep the hack, otherwise we can likely safely remove it.
          JSG_WARN_ONCE("reportStartupError() customer-specific SyntaxError hack "
                        "is still relevant.");
        } else {
          KJ_LOG(ERROR, "script startup threw exception", id, description, trace);
          KJ_FAIL_REQUIRE("script startup threw exception");
        }
      }
    } else {
      kj::throwFatalException(kj::cp(permanentException.emplace(
          KJ_EXCEPTION(FAILED, "returned empty handle but didn't throw exception?", id))));
    }
  } catch (const jsg::JsExceptionThrown&) {
#define LOG_AND_SET_PERM_EXCEPTION(...) \
    KJ_LOG(ERROR, __VA_ARGS__); \
    if (permanentException == nullptr) { \
      permanentException = KJ_EXCEPTION(FAILED, __VA_ARGS__); \
    }

    KJ_IF_MAYBE(limitError2, maybeLimitError2) {
      // TODO(cleanup): If we see this error show up in production, stop logging it, because I
      //   guess it's not necessarily an error? The other two cases below are more worrying though.
      KJ_LOG(ERROR, *limitError2);
      if (permanentException == nullptr) {
        permanentException = kj::mv(*limitError2);
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

class Worker::InspectorClient: public v8_inspector::V8InspectorClient {
public:
  double currentTimeMS() override {
    // Wall time in milliseconds with millisecond precision. console.time() and friends rely on this
    // function to implement timers.

    auto timePoint = kj::UNIX_EPOCH;

    if (IoContext::hasCurrent()) {
      // We're on a request-serving thread.
      auto& ioContext = IoContext::current();
      timePoint = ioContext.now();
    } else KJ_IF_MAYBE(info, inspectorTimerInfo) {
      if (info->threadId == getCurrentThreadId()) {
        // We're on an inspector-serving thread.
        timePoint = info->timer.now() + info->timerOffset
                  - kj::origin<kj::TimePoint>() + kj::UNIX_EPOCH;
      }
    }

    // If we're on neither a request- nor inspector-serving thread, then we're at script startup
    // time -- just return the Epoch.

    return (timePoint - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }

  // Nothing else. We ignore everything the inspector tells us, because we only care about the
  // devtools inspector protocol, which is handled separately.

  void setInspectorTimerInfo(kj::Timer& timer, kj::Duration timerOffset) {
    // Helper for attachInspector().
    inspectorTimerInfo = InspectorTimerInfo { timer, timerOffset, getCurrentThreadId() };
  }

private:
  struct InspectorTimerInfo {
    kj::Timer& timer;
    kj::Duration timerOffset;
    uint64_t threadId;
  };

  kj::Maybe<InspectorTimerInfo> inspectorTimerInfo;
  // The timer and offset for the inspector-serving thread.
};

void setWebAssemblyModuleHasInstance(jsg::Lock& lock, v8::Local<v8::Context> context);
// Defined later in this file.

static thread_local uint warnAboutIsolateLockScopeCount = 0;
static thread_local const Worker::ApiIsolate* currentApiIsolate = nullptr;

const Worker::ApiIsolate& Worker::ApiIsolate::current() {
  KJ_REQUIRE(currentApiIsolate != nullptr, "not running JavaScript");
  return *currentApiIsolate;
}

Worker::WarnAboutIsolateLockScope::WarnAboutIsolateLockScope() {
  ++warnAboutIsolateLockScopeCount;
}

void Worker::WarnAboutIsolateLockScope::release() {
  if (!released) {
    --warnAboutIsolateLockScopeCount;
    released = true;
  }
}

struct Worker::Impl {
  kj::Maybe<jsg::JsContext<api::ServiceWorkerGlobalScope>> context;

  kj::Maybe<jsg::Value> env;
  // The environment blob to pass to handlers.

  kj::Maybe<api::ExportedHandler> defaultHandler;
  kj::HashMap<kj::String, api::ExportedHandler> namedHandlers;
  kj::HashMap<kj::String, DurableObjectConstructor> actorClasses;

  kj::Maybe<kj::Exception> permanentException;
  // If set, then any attempt to use this worker shall throw this exception.
};

struct Worker::Isolate::Impl {
  // Note that Isolate mutable state is protected by locking the JsgWorkerIsolate unless otherwise
  // noted.

  IsolateObserver& metrics;
  InspectorClient inspectorClient;
  kj::Maybe<std::unique_ptr<v8_inspector::V8Inspector>> inspector;
  InspectorPolicy inspectorPolicy;
  kj::Maybe<kj::Own<v8::CpuProfiler>> profiler;
  ActorCache::SharedLru actorCacheLru;

  kj::Vector<kj::String> queuedNotifications;
  // Notification messages to deliver to the next inspector client when it connects.

  kj::HashSet<kj::String> warningOnceDescriptions;
  // Set of warning log lines that should not be logged to the inspector again.

  kj::HashSet<kj::String> errorOnceDescriptions;
  // Set of error log lines that should not be logged again.

  mutable uint lockAttemptGauge = 0;
  // Instantaneous count of how many threads are trying to or have successfully obtained an
  // AsyncLock on this isolate, used to implement getCurrentLoad().

  mutable uint64_t lockSuccessCount = 0;
  // Atomically incremented upon every successful lock. The ThreadProgressCounter in Impl::Lock
  // registers a reference to `lockSuccessCounter` as the thread's progress counter during a lock
  // attempt. This allows watchdogs to see evidence of forward progress in other threads, even if
  // their own thread has blocked waiting for the lock for a long time.

  class Lock {
    // Wrapper around JsgWorkerIsolate::Lock and various RAII objects which help us report metrics,
    // measure instantaneous load, avoid spurious watchdog kills, and defer context destruction.
    //
    // Always use this wrapper in code which may face lock contention (that's mostly everywhere).

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
          oldCurrentApiIsolate(currentApiIsolate),
          limitEnforcer(isolate.getLimitEnforcer()),
          lock(isolate.apiIsolate->lock(stackScope)) {
      if (warnAboutIsolateLockScopeCount > 0) {
        KJ_LOG(WARNING, "taking isolate lock at a bad time", kj::getStackTrace());
      }

      // Increment the success count to expose forward progress to all threads.
      __atomic_add_fetch(&impl.lockSuccessCount, 1, __ATOMIC_RELAXED);
      metrics.locked();

      // We record the current lock so our GC prologue/epilogue callbacks can report GC time via
      // Jaeger tracing.
      KJ_DASSERT(impl.currentLock == nullptr, "Isolate lock taken recursively");
      impl.currentLock = *this;

      // Now's a good time to destroy any workers queued up for destruction.
      auto workersToDestroy = impl.workerDestructionQueue.lockExclusive()->pop();
      for (auto& workerImpl: workersToDestroy.asArrayPtr()) {
        KJ_IF_MAYBE(c, workerImpl->context) {
          disposeContext(kj::mv(*c));
        }
        workerImpl = nullptr;
      }

      currentApiIsolate = isolate.apiIsolate.get();
    }
    ~Lock() noexcept(false) {
      currentApiIsolate = oldCurrentApiIsolate;

#ifdef KJ_DEBUG
      // We lack a KJ_DASSERT_NONNULL because it would have to look a lot like KJ_IF_MAYBE, thus
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
      KJ_IF_MAYBE(i, impl.inspector) {
        i->get()->contextCreated(v8_inspector::V8ContextInfo(context, 1, toStringView("Worker")));
      }

      if (impl.inspector == nullptr) {
        // When not running in preview mode, we replace the default V8 console.log(), etc. methods,
        // to give the worker access to logged content.
        auto global = context->Global();
        auto consoleStr = jsg::v8StrIntern(lock->v8Isolate, "console");
        auto console = jsg::check(global->Get(context, consoleStr));

        auto setHandler = [&](const char* method, LogLevel level) {
          auto methodStr = jsg::v8StrIntern(lock->v8Isolate, method);

          auto f = lock->wrapSimpleFunction(context,
              [level](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
            handleLog(js, level, info);
          });
          jsg::check(console.As<v8::Object>()->Set(context, methodStr, f));
        };

        setHandler("debug", LogLevel::DEBUG_);
        setHandler("error", LogLevel::ERROR);
        setHandler("info", LogLevel::INFO);
        setHandler("log", LogLevel::LOG);
        setHandler("warn", LogLevel::WARN);
      }
    }

    void disposeContext(jsg::JsContext<api::ServiceWorkerGlobalScope> context) {
      v8::HandleScope handleScope(lock->v8Isolate);
      context->clear();
      KJ_IF_MAYBE(i, impl.inspector) {
        i->get()->contextDestroyed(context.getHandle(lock->v8Isolate));
      }
      { auto drop = kj::mv(context); }
      lock->v8Isolate->ContextDisposedNotification(false);
    }

    void gcPrologue() {
      metrics.gcPrologue();
    }
    void gcEpilogue() {
      metrics.gcEpilogue();
    }

    bool checkInWithLimitEnforcer(Worker::Isolate& isolate);
    // Call limitEnforcer->exitJs(), and also schedule to call limitEnforcer->reportMetrics()
    // later. Returns true if condemned. We take a mutable reference to it to make sure the caller
    // believes it has exclusive access.

  private:
    const Impl& impl;
    IsolateObserver::LockRecord metrics;
    ThreadProgressCounter progressCounter;
    bool shouldReportIsolateMetrics = false;
    const ApiIsolate* oldCurrentApiIsolate;

    const IsolateLimitEnforcer& limitEnforcer;  // only so we can call getIsolateStats()

  public:
    kj::Own<jsg::Lock> lock;
  };

  mutable kj::Maybe<Lock&> currentLock;
  // Protected by v8::Locker -- if v8::Locker::IsLocked(isolate) is true, then it is safe to access
  // this variable.

  static constexpr auto WORKER_DESTRUCTION_QUEUE_INITIAL_SIZE = 8;
  static constexpr auto WORKER_DESTRUCTION_QUEUE_MAX_CAPACITY = 100;
  const kj::MutexGuarded<BatchQueue<kj::Own<Worker::Impl>>> workerDestructionQueue {
    WORKER_DESTRUCTION_QUEUE_INITIAL_SIZE,
    WORKER_DESTRUCTION_QUEUE_MAX_CAPACITY
  };
  // Similar in spirit to the deferred destruction queue in jsg::IsolateBase. When a Worker is
  // destroyed, it puts its Impl, which contains objects that need to be destroyed under the isolate
  // lock, into this queue. Our own Isolate::Impl::Lock implementation then clears this queue the
  // next time the isolate is locked, whether that be by a connection thread, or the Worker's own
  // destructor if it owns the last `kj::Own<const Script>` reference.
  //
  // Fairly obviously, this member is protected by its own mutex, not the isolate lock.
  //
  // TODO(cleanup): The only reason this exists and we can't just rely on the isolate's regular
  //   deferred destruction queue to lazily destroy the various V8 objects in Worker::Impl is
  //   because our GlobalScope object needs to have a function called on it, and any attached
  //   inspector needs to be notified. JSG doesn't know about these things.

  Impl(const ApiIsolate& apiIsolate, IsolateObserver& metrics,
       IsolateLimitEnforcer& limitEnforcer, InspectorPolicy inspectorPolicy)
      : metrics(metrics),
        inspectorPolicy(inspectorPolicy),
        actorCacheLru(limitEnforcer.getActorCacheLruOptions()) {
    jsg::V8StackScope stackScope;
    auto lock = apiIsolate.lock(stackScope);
    limitEnforcer.customizeIsolate(lock->v8Isolate);

    if (inspectorPolicy != InspectorPolicy::DISALLOW) {
      // We just created our isolate, so we don't need to use Isolate::Impl::Lock.
      KJ_ASSERT(!isMultiTenantProcess(), "inspector is not safe in multi-tenant processes");
      inspector = v8_inspector::V8Inspector::create(lock->v8Isolate, &inspectorClient);
    }
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

static void startProfiling(v8::CpuProfiler& profiler, v8::Isolate* isolate) {
  v8::HandleScope handleScope(isolate);
  v8::CpuProfilingOptions options(
    v8::kLeafNodeLineNumbers,
    v8::CpuProfilingOptions::kNoSampleLimit
  );
  profiler.StartProfiling(jsg::v8StrIntern(isolate, PROFILE_NAME.cStr()), kj::mv(options));
}

static void stopProfiling(v8::CpuProfiler& profiler,v8::Isolate* isolate,
    cdp::Command::Builder& cmd) {
  v8::HandleScope handleScope(isolate);
  auto cpuProfile = profiler.StopProfiling(jsg::v8StrIntern(isolate, PROFILE_NAME.cStr()));
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
    v8::CpuProfileNode::LineTick* lineBuffer =
        (v8::CpuProfileNode::LineTick*)malloc(
        hitLineCount * sizeof(v8::CpuProfileNode::LineTick));
    KJ_DEFER(free(lineBuffer));
    allNodes[i]->GetLineTicks(lineBuffer, hitLineCount);

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
}

} // anonymous namespace

struct Worker::Script::Impl {
  kj::OneOf<jsg::NonModuleScript, kj::Path> unboundScriptOrMainModule;

  kj::Array<CompiledGlobal> globals;

  kj::Maybe<jsg::JsContext<api::ServiceWorkerGlobalScope>> moduleContext;

  kj::Maybe<kj::Exception> permanentException;
  // If set, then any attempt to use this script shall throw this exception.

  kj::Maybe<const jsg::ModuleRegistry&> getModuleRegistry() const {
    return moduleRegistry;
  }

  kj::Maybe<jsg::ModuleRegistry&> getModuleRegistry() {
    return moduleRegistry;
  }

  void setModuleRegistry(jsg::Lock& js, kj::Own<jsg::ModuleRegistry> modules) {
    struct DynamicImportResult {
      jsg::Value value;
      bool isException = false;
    };

    modules->setDynamicImportCallback([](jsg::Lock& js, auto handler) mutable {
      if (IoContext::hasCurrent()) {
        // If we are within the scope of a IoContext, then we are going to pop
        // out of it to perform the actual module instantiation.

        auto& ioContext = IoContext::current();
        auto& worker = ioContext.getWorker();

        return ioContext.awaitIo(
            kj::evalLater([&worker, handler = kj::mv(handler),
                           asyncContext = jsg::AsyncContextFrame::currentRef(js)]() mutable {
          return worker.takeAsyncLockWithoutRequest(nullptr)
              .then([&worker, handler = kj::mv(handler), asyncContext = kj::mv(asyncContext)]
                    (Worker::AsyncLock asyncLock) mutable -> DynamicImportResult {
            Worker::Lock lock(worker, asyncLock);
            auto isolate = lock.getIsolate();
            v8::HandleScope scope(isolate);
            v8::Context::Scope contextScope(lock.getContext());
            jsg::AsyncContextFrame::Scope asyncContextScope(lock, asyncContext);

            auto& workerIsolate = worker.getIsolate();

            // We have to wrap the call to handler in a try catch here because
            // we have to tunnel any jsg::JsExceptionThrown instances back.
            v8::TryCatch tryCatch(isolate);
            kj::Maybe<kj::Exception> maybeLimitError;
            try {
              auto limitScope = workerIsolate.getLimitEnforcer()
                  .enterDynamicImportJs(lock, maybeLimitError);
              return { .value = handler() };
            } catch (jsg::JsExceptionThrown&) {
              // Handled below...
            } catch (kj::Exception& ex) {
              kj::throwFatalException(kj::mv(ex));
            }

            KJ_ASSERT(tryCatch.HasCaught());
            if (!tryCatch.CanContinue()) {
              // There's nothing else we can do here but throw a generic fatal exception.
              KJ_IF_MAYBE(limitError, maybeLimitError) {
                kj::throwFatalException(kj::mv(*limitError));
              } else {
                kj::throwFatalException(JSG_KJ_EXCEPTION(FAILED, Error,
                    "Failed to load dynamic module."));
              }
            }
            return { .value = jsg::Value(isolate, tryCatch.Exception()), .isException = true };
          });
        }).attach(kj::atomicAddRef(worker)), [isolate=js.v8Isolate](auto result) {
          if (result.isException) {
            return jsg::rejectedPromise<jsg::Value>(isolate, kj::mv(result.value));
          }
          return jsg::resolvedPromise(isolate, kj::mv(result.value));
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

    moduleRegistry = kj::mv(modules);
  }

private:
  kj::Maybe<kj::Own<jsg::ModuleRegistry>> moduleRegistry;
};

namespace {

kj::Maybe<kj::String> makeCompatJson(kj::ArrayPtr<kj::StringPtr> enableFlags) {
  // Given an array of strings, return a valid serialized JSON string like:
  //   {"flags":["minimal_subrequests",...]}
  //
  // Return null if the array is empty.

  if (enableFlags.size() == 0) {
    return nullptr;
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

jsg::Promise<void> addCrossThreadPromiseWaiter(jsg::Lock& js,
                                               v8::Local<v8::Promise>& promise) {
  // When a promise is created in a different IoContext, we need to use a
  // kj::CrossThreadFulfiller in order to wait on it. The Waiter instance will
  // be held on the Promise itself, and will be fulfilled/rejected when the
  // promise is resolved or rejected. This will signal all of the waiters
  // from other IoContexts.

  auto waiter = kj::newPromiseAndCrossThreadFulfiller<void>();

  struct Waiter: public kj::Refcounted {
    kj::Maybe<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> fulfiller;
    void done() {
      KJ_IF_MAYBE(f, fulfiller) {
        // Done this way so that the fulfiller is released as soon as possible
        // when done as the JS promise may not clean up reactions right away.
        (*f)->fulfill();
        fulfiller = nullptr;
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

}  // namespace

Worker::Isolate::Isolate(kj::Own<ApiIsolate> apiIsolateParam,
                         kj::Own<IsolateObserver>&& metricsParam,
                         kj::StringPtr id,
                         kj::Own<IsolateLimitEnforcer> limitEnforcerParam,
                         InspectorPolicy inspectorPolicy)
    : id(kj::str(id)),
      limitEnforcer(kj::mv(limitEnforcerParam)),
      apiIsolate(kj::mv(apiIsolateParam)),
      featureFlagsForFl(makeCompatJson(decompileCompatibilityFlagsForFl(apiIsolate->getFeatureFlags()))),
      metrics(kj::mv(metricsParam)),
      impl(kj::heap<Impl>(*apiIsolate, *metrics, *limitEnforcer, inspectorPolicy)),
      weakIsolateRef(kj::atomicRefcounted<WeakIsolateRef>(this)),
      traceAsyncContextKey(kj::refcounted<jsg::AsyncContextFrame::StorageKey>()) {
  metrics->created();
  // We just created our isolate, so we don't need to use Isolate::Impl::Lock (nor an async lock).
  jsg::V8StackScope stackScope;
  auto lock = apiIsolate->lock(stackScope);
  auto features = apiIsolate->getFeatureFlags();

  lock->setCaptureThrowsAsRejections(features.getCaptureThrowsAsRejections());
  lock->setCommonJsExportDefault(features.getExportCommonJsDefaultNamespace());

  if (impl->inspector != nullptr || ::kj::_::Debug::shouldLog(::kj::LogSeverity::INFO)) {
    lock->setLoggerCallback([this](jsg::Lock& js, kj::StringPtr message) {
      if (impl->inspector != nullptr) {
        // TODO(cleanup): The logger will only ever be called while the isolate lock is
        // held. However, can we also safely assume there's already a v8::HandleScope
        // on the stack? Once logMessage is updated to take a jsg::Lock reference we
        // can remove the v8::HandleScope here.
        v8::HandleScope scope(js.v8Isolate);
        logMessage(js.v8Context(), static_cast<uint16_t>(cdp::LogType::WARNING), message);
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
    KJ_IF_MAYBE(currentLock, self.impl->currentLock) {
      currentLock->gcPrologue();
    }
  }, this);
  lock->v8Isolate->AddGCEpilogueCallback(
      [](v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags, void* data) noexcept {
    // We make similar assumptions about v8::Locker and currentLock as in the prologue callback.
    KJ_DASSERT(v8::Locker::IsLocked(isolate));
    auto& self = *reinterpret_cast<Isolate*>(data);
    KJ_IF_MAYBE(currentLock, self.impl->currentLock) {
      currentLock->gcEpilogue();
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
  // When the IoContext::Scope is entered, we set the "promise context tag" associated
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
  });
}

Worker::Script::Script(kj::Own<const Isolate> isolateParam, kj::StringPtr id,
                       Script::Source source, IsolateObserver::StartType startType,
                       bool logNewScript, kj::Maybe<ValidationErrorReporter&> errorReporter)
    : isolate(kj::mv(isolateParam)), id(kj::str(id)), impl(kj::heap<Impl>()) {
  auto parseMetrics = isolate->metrics->parse(startType);
  // TODO(perf): It could make sense to take an async lock when constructing a script if we
  //   co-locate multiple scripts in the same isolate. As of this writing, we do not, except in
  //   previews, where it doesn't matter. If we ever do co-locate multiple scripts in the same
  //   isolate, we may wish to make the RequestObserver object available here, in order to
  //   attribute lock timing to that request.
  jsg::V8StackScope stackScope;
  Isolate::Impl::Lock recordedLock(*isolate, Worker::Lock::TakeSynchronously(nullptr), stackScope);
  auto& lock = *recordedLock.lock;

  // If we throw an exception, it's important that `impl` is destroyed under lock.
  KJ_ON_SCOPE_FAILURE({
    auto implToDestroy = kj::mv(impl);
    KJ_IF_MAYBE(c, implToDestroy->moduleContext) {
      recordedLock.disposeContext(kj::mv(*c));
    }
  });

  v8::HandleScope handleScope(lock.v8Isolate);

  if (isolate->impl->inspector != nullptr || errorReporter != nullptr) {
    lock.v8Isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  }

  v8::Local<v8::Context> context;
  if (source.is<ModulesSource>()) {
    // Modules can't be compiled for multiple contexts. We need to create the real context now.
    auto& mContext = impl->moduleContext.emplace(isolate->apiIsolate->newContext(lock));
    mContext->enableWarningOnSpecialEvents();
    context = mContext.getHandle(lock.v8Isolate);
    recordedLock.setupContext(context);
  } else {
    // Although we're going to compile a script independent of context, V8 requires that there be
    // an active context, otherwise it will segfault, I guess. So we create a dummy context.
    // (Undocumented, as usual.)
    context = v8::Context::New(
        lock.v8Isolate, nullptr, v8::ObjectTemplate::New(lock.v8Isolate));
  }

  v8::Context::Scope context_scope(context);

  // const_cast OK because we hold the isolate lock.
  Worker::Isolate& lockedWorkerIsolate = const_cast<Isolate&>(*isolate);

  if (logNewScript) {
    // HACK: Log a message indicating that a new script was loaded. This is used only when the
    //   inspector is enabled. We want to do this immediately after the context is created,
    //   before the user gets a chance to modify the behavior of the console, which if they did,
    //   we'd then need to be more careful to apply time limits and such.
    lockedWorkerIsolate.logMessage(context,
        static_cast<uint16_t>(cdp::LogType::WARNING), "Script modified; context reset.");
  }

  // We need to register this context with the inspector, otherwise errors won't be reported. But
  // we want it to be un-registered as soon as the script has been compiled, otherwise the
  // inspector will end up with multiple contexts active which is very confusing for the user
  // (since they'll have to select from the drop-down which context to use).
  //
  // (For modules, the context was already registered by `setupContext()`, above.
  KJ_IF_MAYBE(i, isolate->impl->inspector) {
    if (!source.is<ModulesSource>()) {
      i->get()->contextCreated(v8_inspector::V8ContextInfo(context,
          1, toStringView("Compiler")));
    }
  }
  KJ_DEFER({
    if (!source.is<ModulesSource>()) {
      KJ_IF_MAYBE(i, isolate->impl->inspector) {
        i->get()->contextDestroyed(context);
      }
    }
  });

  v8::TryCatch catcher(lock.v8Isolate);
  kj::Maybe<kj::Exception> maybeLimitError;

  try {
    try {
      KJ_SWITCH_ONEOF(source) {
        KJ_CASE_ONEOF(script, ScriptSource) {
          impl->globals = script.compileGlobals(lock, *isolate->apiIsolate);

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

        KJ_CASE_ONEOF(modules, ModulesSource) {
          auto limitScope = isolate->getLimitEnforcer().enterStartupJs(lock, maybeLimitError);
          impl->setModuleRegistry(lock, modules.compileModules(lock, *isolate->apiIsolate));

          impl->unboundScriptOrMainModule = kj::Path::parse(modules.mainModule);
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
                       context,
                       isolate->getLimitEnforcer(),
                       kj::mv(maybeLimitError),
                       catcher,
                       errorReporter,
                       impl->permanentException);
  }
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
  jsg::V8StackScope stackScope;
  Isolate::Impl::Lock recordedLock(*this, Worker::Lock::TakeSynchronously(nullptr), stackScope);
  metrics->teardownLockAcquired();
  auto inspector = kj::mv(impl->inspector);
  auto dropTraceAsyncContextKey = kj::mv(traceAsyncContextKey);
}

Worker::Script::~Script() noexcept(false) {
  // Make sure to destroy things under lock.
  // TODO(perf): It could make sense to try to obtain an async lock before destroying a script if
  //   multiple scripts are co-located in the same isolate. As of this writing, that doesn't happen
  //   except in preview. In any case, Scripts are destroyed in the GC thread, where we don't care
  //   too much about lock latency.
  jsg::V8StackScope stackScope;
  Isolate::Impl::Lock recordedLock(*isolate, Worker::Lock::TakeSynchronously(nullptr), stackScope);
  KJ_IF_MAYBE(c, impl->moduleContext) {
    recordedLock.disposeContext(kj::mv(*c));
  }
  impl = nullptr;
}

bool Worker::Script::isModular() const {
  return impl->getModuleRegistry() != nullptr;
}

bool Worker::Isolate::Impl::Lock::checkInWithLimitEnforcer(Worker::Isolate& isolate) {
  shouldReportIsolateMetrics = true;
  return limitEnforcer.exitJs(*lock);
}

void setWebAssemblyModuleHasInstance(jsg::Lock& lock, v8::Local<v8::Context> context) {
  // EW-1319: Set WebAssembly.Module @@HasInstance
  //
  // The instanceof operator can be changed by setting the @@HasInstance method
  // on the object, https://tc39.es/ecma262/#sec-instanceofoperator.

  auto instanceof = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    v8::HandleScope scope(isolate);
    info.GetReturnValue().Set(v8::Boolean::New(isolate, info[0]->IsWasmModuleObject()));
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
                      jsg::Lock& lock, const ApiIsolate& apiIsolate,
                      v8::Local<v8::Object> target)> compileBindings,
               IsolateObserver::StartType startType,
               SpanParent parentSpan, LockType lockType,
               kj::Maybe<ValidationErrorReporter&> errorReporter)
    : script(kj::mv(scriptParam)),
      metrics(kj::mv(metricsParam)),
      impl(kj::heap<Impl>()){
  // Enter/lock isolate.
  jsg::V8StackScope stackScope;
  Isolate::Impl::Lock recordedLock(*script->isolate, lockType, stackScope);
  auto& lock = *recordedLock.lock;

  // If we throw an exception, it's important that `impl` is destroyed under lock.
  KJ_ON_SCOPE_FAILURE({
    auto implToDestroy = kj::mv(impl);
    KJ_IF_MAYBE(c, implToDestroy->context) {
      recordedLock.disposeContext(kj::mv(*c));
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
  v8::HandleScope handleScope(lock.v8Isolate);

  v8::Local<v8::Context> context;
  KJ_IF_MAYBE(c, script->impl->moduleContext) {
    // Use the shared context from the script.
    // const_cast OK because guarded by `lock`.
    context = const_cast<jsg::JsContext<api::ServiceWorkerGlobalScope>*>(c)
        ->getHandle(lock.v8Isolate);
    currentSpan.setTag("module_context"_kjc, true);
  } else {
    // Create a new context.
    context = this->impl->context.emplace(script->isolate->apiIsolate->newContext(lock))
        .getHandle(lock.v8Isolate);
    recordedLock.setupContext(context);
  }

  if (script->impl->unboundScriptOrMainModule == nullptr) {
    // Script failed to parse. Act as if the script was empty -- i.e. do nothing.
    impl->permanentException =
        script->impl->permanentException.map([](auto& e) { return kj::cp(e); });
    return;
  }

  // Enter the context for compiling and running the script.
  v8::Context::Scope contextScope(context);

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
        bool setResult = jsg::check(bindingsScope
            ->Set(context,
                global.name.getHandle(lock.v8Isolate),
                global.value.getHandle(lock.v8Isolate)));

        if (!setResult) {
          // Can this actually happen? What does it mean?
          KJ_LOG(ERROR, "Set() returned false?");
        }
      }

      compileBindings(lock, *script->isolate->apiIsolate, bindingsScope);

      // Execute script.
      currentSpan = maybeMakeSpan("lw:top_level_execution"_kjc);

      KJ_SWITCH_ONEOF(script->impl->unboundScriptOrMainModule) {
        KJ_CASE_ONEOF(unboundScript, jsg::NonModuleScript) {
          auto limitScope = script->isolate->getLimitEnforcer().enterStartupJs(lock, maybeLimitError);
          unboundScript.run(lock.v8Context());
        }
        KJ_CASE_ONEOF(mainModule, kj::Path) {
          // const_cast OK because we hold the lock.
          auto& registry = KJ_ASSERT_NONNULL(const_cast<Script&>(*script).impl->getModuleRegistry());
          KJ_IF_MAYBE(entry, registry.resolve(lock, mainModule)) {
            JSG_REQUIRE(entry->maybeSynthetic == nullptr, TypeError,
                        "Main module must be an ES module.");
            auto module = entry->module.getHandle(lock);

            {
              auto limitScope = script->isolate->getLimitEnforcer()
                  .enterStartupJs(lock, maybeLimitError);

              jsg::instantiateModule(lock, module);
            }

            if (maybeLimitError != nullptr) {
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

            impl->env = jsg::Value(lock.v8Isolate, bindingsScope);

            auto handlers = script->isolate->apiIsolate->unwrapExports(lock, ns);

            for (auto& handler: handlers.fields) {
              KJ_SWITCH_ONEOF(handler.value) {
                KJ_CASE_ONEOF(obj, api::ExportedHandler) {
                  obj.env = jsg::Value(lock.v8Isolate, bindingsScope);
                  obj.ctx = jsg::alloc<api::ExecutionContext>();

                  if (handler.name == "default") {
                    // The default export is given the string name "default". I guess that means that
                    // you can't actually name an export "default"? Anyway, this is our default
                    // handler.
                    impl->defaultHandler = kj::mv(obj);
                  } else {
                    impl->namedHandlers.insert(kj::mv(handler.name), kj::mv(obj));
                  }
                }
                KJ_CASE_ONEOF(cls, DurableObjectConstructor) {
                  impl->actorClasses.insert(kj::mv(handler.name), kj::mv(cls));
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
                       context,
                       script->isolate->getLimitEnforcer(),
                       kj::mv(maybeLimitError),
                       catcher,
                       errorReporter,
                       impl->permanentException);
  }
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

void Worker::handleLog(jsg::Lock& js, LogLevel level, const v8::FunctionCallbackInfo<v8::Value>& info) {
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
      v8::HandleScope handleScope(js.v8Isolate);
      auto context = js.v8Context();
      bool shouldSerialiseToJson = false;
      if (arg->IsNull() || arg->IsNumber() || arg->IsArray() || arg->IsBoolean() || arg->IsString() ||
          arg->IsUndefined()) { // This is special cased for backwards compatibility.
        shouldSerialiseToJson = true;
      }
      if (arg->IsObject()) {
        v8::Local<v8::Object> obj = arg.As<v8::Object>();
        v8::Local<v8::Object> freshObj = v8::Object::New(js.v8Isolate);

        // Determine whether `obj` is constructed using `{}` or `new Object()`. This ensures
        // we don't serialise values like Promises to JSON.
        if (
          obj->GetPrototype()->SameValue(freshObj->GetPrototype()) || obj->GetPrototype()->IsNull()
        ) {
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

      KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
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
      })) {
        stringified.add(kj::str("{}"));
      };
    }
    return kj::str("[", kj::delimited(stringified, ", "_kj), "]");
  };

  // Only check tracing if console.log() was not invoked at the top level.
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    KJ_IF_MAYBE(tracer, ioContext.getWorkerTracer()) {
      auto timestamp = ioContext.now();
      tracer->log(timestamp, level, message());
    }
  }

  // Lets us dump console.log()s to stdout when running test-runner with --verbose flag, to make
  // it easier to debug tests.  Note that when --verbose is not passed, KJ_LOG(INFO, ...) will not
  // even evaluate its arguments, so `message()` will not be called at all.
  KJ_LOG(INFO, "console.log()", message());
}

Worker::Lock::TakeSynchronously::TakeSynchronously(
    kj::Maybe<RequestObserver&> requestParam) {
  KJ_IF_MAYBE(r, requestParam) {
    request = r;
  }
}

kj::Maybe<RequestObserver&> Worker::Lock::TakeSynchronously::getRequest() {
  if (request != nullptr) {
    return *request;
  }
  return nullptr;
}

struct Worker::Lock::Impl {
  Isolate::Impl::Lock recordedLock;
  jsg::Lock& inner;

  Impl(const Worker& worker, LockType lockType, jsg::V8StackScope& stackScope)
      : recordedLock(worker.getIsolate(), lockType, stackScope),
        inner(*recordedLock.lock) {}
};

Worker::Lock::Lock(const Worker& constWorker, LockType lockType)
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
  KJ_IF_MAYBE(e, worker.impl->permanentException) {
    // Block taking lock when worker failed to start up.
    kj::throwFatalException(kj::cp(*e));
  }
}

Worker::Lock::operator jsg::Lock&() {
  return impl->inner;
}

v8::Isolate* Worker::Lock::getIsolate() {
  return impl->inner.v8Isolate;
}

v8::Local<v8::Context> Worker::Lock::getContext() {
  KJ_IF_MAYBE(c, worker.impl->context) {
    return c->getHandle(impl->inner.v8Isolate);
  } else KJ_IF_MAYBE(c, const_cast<Script&>(*worker.script).impl->moduleContext) {
    return c->getHandle(impl->inner.v8Isolate);
  } else {
    KJ_UNREACHABLE;
  }
}

kj::Maybe<api::ExportedHandler&> Worker::Lock::getExportedHandler(
    kj::Maybe<kj::StringPtr> name, kj::Maybe<Worker::Actor&> actor) {
  KJ_IF_MAYBE(a, actor) {
    KJ_IF_MAYBE(h, a->getHandler()) {
      return *h;
    }
  }

  KJ_IF_MAYBE(n, name) {
    KJ_IF_MAYBE(h, worker.impl->namedHandlers.find(*n)){
      return *h;
    } else {
      if (worker.impl->actorClasses.find(*n) != nullptr) {
        LOG_ERROR_PERIODICALLY("worker is not an actor but class name was requested", *n);
      } else {
        LOG_ERROR_PERIODICALLY("worker has no such named entrypoint", *n);
      }

      KJ_FAIL_ASSERT("worker_do_not_log; Unable to get exported handler");
    }
  } else {
    return worker.impl->defaultHandler;
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
  return worker.script->isolate->impl->inspector != nullptr;
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

  KJ_IF_MAYBE(i, worker.script->isolate->impl->inspector) {
    auto isolate = getIsolate();
    v8::HandleScope scope(isolate);
    auto context = getContext();
    v8::Context::Scope contextScope(context);
    sendExceptionToInspector(*i->get(), context, description);
  }

  // Run with --verbose to log JS exceptions to stderr. Useful when running tests.
  KJ_LOG(INFO, "uncaught exception", description);
}

void Worker::Lock::logUncaughtException(UncaughtExceptionSource source,
                                        v8::Local<v8::Value> exception,
                                        v8::Local<v8::Message> message) {
  // Only add exception to trace when running within an I/O context with a tracer.
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    KJ_IF_MAYBE(tracer, ioContext.getWorkerTracer()) {
      auto isolate = getIsolate();
      v8::HandleScope scope(isolate);
      auto context = getContext();
      v8::Context::Scope contextScope(context);
      addExceptionToTrace(impl->inner, ioContext, *tracer, context, source, exception,
          worker.getIsolate().apiIsolate->getErrorInterfaceTypeHandler(*this));
    }
  }

  KJ_IF_MAYBE(i, worker.script->isolate->impl->inspector) {
    auto isolate = getIsolate();
    v8::HandleScope scope(isolate);
    auto context = getContext();
    v8::Context::Scope contextScope(context);
    sendExceptionToInspector(*i->get(), context, source, exception, message);
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
  jsg::Lock& js = *this;
  v8::HandleScope scope(js.v8Isolate);
  v8::Context::Scope contextScope(getContext());

  // Ignore event types that represent internally-generated events.
  kj::HashSet<kj::StringPtr> ignoredHandlers;
  ignoredHandlers.insert("alarm"_kj);
  ignoredHandlers.insert("unhandledrejection"_kj);
  ignoredHandlers.insert("rejectionhandled"_kj);

  KJ_IF_MAYBE(c, worker.impl->context) {
    auto handlerNames = (*c)->getHandlerNames();
    bool foundAny = false;
    for (auto& name: handlerNames) {
      if (!ignoredHandlers.contains(name)) {
        errorReporter.addHandler(nullptr, name);
        foundAny = true;
      }
    }
    if (!foundAny) {
      errorReporter.addError(kj::str(
          "No event handlers were registered. This script does nothing."));
    }
  } else {
    auto report = [&](kj::Maybe<kj::StringPtr> name, api::ExportedHandler& exported) {
      auto handle = exported.self.getHandle(js.v8Isolate);
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

    KJ_IF_MAYBE(h, worker.impl->defaultHandler) {
      report(nullptr, *h);
    }
    for (auto& entry: worker.impl->namedHandlers) {
      report(kj::StringPtr(entry.key), entry.value);
    }
    for (auto& entry: worker.impl->actorClasses) {
      errorReporter.addHandler(kj::StringPtr(entry.key), "class");
    }
  }
}

// =======================================================================================
// AsyncLock implementation

thread_local Worker::AsyncWaiter* Worker::AsyncWaiter::threadCurrentWaiter = nullptr;

Worker::Isolate::AsyncWaiterList::~AsyncWaiterList() noexcept {
  // It should be impossible for this list to be non-empty since each member of the list holds a
  // strong reference back to us. But if the list is non-empty, we'd better crash here, to avoid
  // dangling pointers.
  KJ_ASSERT(head == nullptr, "destroying non-empty waiter list?");
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
  if (lockTiming != nullptr) {
    currentLoad = getCurrentLoad();
  }

  for (uint threadWaitingDifferentLockCount = 0; ; ++threadWaitingDifferentLockCount) {
    AsyncWaiter* waiter = AsyncWaiter::threadCurrentWaiter;

    if (waiter == nullptr) {
      // Thread is not currently waiting on a lock.
      KJ_IF_MAYBE(lt, lockTiming) {
        lt->get()->reportAsyncInfo(
            KJ_ASSERT_NONNULL(currentLoad), false /* threadWaitingSameLock */,
            threadWaitingDifferentLockCount);
      }
      auto newWaiter = kj::refcounted<AsyncWaiter>(kj::atomicAddRef(*this));
      co_await newWaiter->readyPromise.addBranch();
      co_return AsyncLock(kj::mv(newWaiter), kj::mv(lockTiming));
    } else if (waiter->isolate == this) {
      // Thread is waiting on a lock already, and it's for the same isolate. We can coalesce the
      // locks.
      KJ_IF_MAYBE(lt, lockTiming) {
        lt->get()->reportAsyncInfo(
            KJ_ASSERT_NONNULL(currentLoad), true /* threadWaitingSameLock */,
            threadWaitingDifferentLockCount);
      }
      auto newWaiterRef = kj::addRef(*waiter);
      co_await newWaiterRef->readyPromise.addBranch();
      co_return AsyncLock(kj::mv(newWaiterRef), kj::mv(lockTiming));
    } else {
      // Thread is already waiting for or holding a different isolate lock. Wait for that one to
      // be released before we try to lock a different isolate.
      // TODO(perf): Use of ForkedPromise leads to thundering herd here. Should be minor in practice,
      //   but we could consider creating another linked list instead...
      KJ_IF_MAYBE(lt, lockTiming) {
        lt->get()->waitingForOtherIsolate(waiter->isolate->getId());
      }
      co_await waiter->releasePromise.addBranch();
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
  KJ_IF_MAYBE(n, next) {
    n->prev = prev;
  } else {
    lock->tail = prev;
  }

  if (prev == &lock->head) {
    // We held the lock before now. Alert the next waiter that they are now at the front of the
    // line.
    KJ_IF_MAYBE(n, next) {
      n->readyFulfiller->fulfill();
    }
  }

  KJ_ASSERT(threadCurrentWaiter == this);
  threadCurrentWaiter = nullptr;
}

kj::Promise<void> Worker::AsyncLock::whenThreadIdle() {
  auto waiter = AsyncWaiter::threadCurrentWaiter;
  if (waiter != nullptr) {
    return waiter->releasePromise.addBranch().then([]() { return whenThreadIdle(); });
  }

  return kj::evalLast([]() -> kj::Promise<void> {
    if (AsyncWaiter::threadCurrentWaiter != nullptr) {
      // Whoops, a new lock attempt appeared, loop.
      return whenThreadIdle();
    } else {
      return kj::READY_NOW;
    }
  });
}

// =======================================================================================

class Worker::Isolate::LimitedBodyWrapper: public kj::OutputStream {
  // A proxy for OutputStream that internally buffers data as long as it's beyond a given limit.
  // Also, it counts size of all the data it has seen (whether it has hit the limit or not).
  //
  // We use this in the Network tab to report response stats and preview [decompressed] bodies,
  // but we don't want to keep buffering extremely large ones, so just discard buffered data
  // upon hitting a limit and don't return any body to the devtools frontend afterwards.
public:
  LimitedBodyWrapper(size_t limit = 1 * 1024 * 1024): limit(limit) {
    if (limit > 0) {
      inner.emplace();
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(LimitedBodyWrapper);

  void reset() {
    this->inner = nullptr;
  }

  void write(const void* buffer, size_t size) override {
    this->size += size;
    KJ_IF_MAYBE(inner, this->inner) {
      if (this->size <= this->limit) {
        inner->write(buffer, size);
      } else {
        reset();
      }
    }
  }

  size_t getWrittenSize() {
    return this->size;
  }

  kj::Maybe<kj::ArrayPtr<byte>> getArray() {
    KJ_IF_MAYBE(inner, this->inner) {
      return inner->getArray();
    } else {
      return nullptr;
    }
  }

private:
  size_t size = 0;
  size_t limit = 0;
  kj::Maybe<kj::VectorOutputStream> inner;
};

class Worker::Isolate::InspectorChannelImpl final: public v8_inspector::V8Inspector::Channel {
public:
  InspectorChannelImpl(kj::Own<const Worker::Isolate> isolateParam,
                       kj::WebSocket& webSocket)
      : webSocket(webSocket),
        state(kj::heap<State>(this, kj::mv(isolateParam))) {}

  using InspectorLock = Worker::Lock::TakeSynchronously;
  // In preview sessions, synchronous locks are not an issue. We declare an alternate spelling of
  // the type so that all the individual locks below don't turn up in a search for synchronous
  // locks.

  ~InspectorChannelImpl() noexcept try {
    KJ_DEFER(outgoingQueueNotifier->clear());

    // Delete session under lock.
    auto state = this->state.lockExclusive();

    jsg::V8StackScope stackScope;
    Isolate::Impl::Lock recordedLock(*state->get()->isolate, InspectorLock(nullptr), stackScope);
    KJ_IF_MAYBE(p, state->get()->isolate->currentInspectorSession) {
      if (p == this) {
        const_cast<Isolate&>(*state->get()->isolate).currentInspectorSession = nullptr;;
      }
    }
    state->get()->teardownUnderLock();
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
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      throw;
    })) {
      KJ_LOG(ERROR, "uncaught exception in ~Script() and the C++ standard is broken", *exception);
    }
  }

  void disconnect() {
    // Fake like the client requested close. This will cause outgoingLoop() to exit and everything
    // will be cleaned up.
    receivedClose = true;
    outgoingQueueNotifier->notify();
  }

  kj::Promise<void> outgoingLoop() {
    return outgoingQueueNotifier->awaitNotification().then([this]() {
      auto messages = kj::mv(*outgoingQueue.lockExclusive());
      auto promise = sendToWebsocket(messages).attach(kj::mv(messages));

      if (receivedClose) {
        return promise.then([this]() {
          return webSocket.close(1000, "client closed connection");
        });
      } else if (*state.lockShared() == nullptr) {
        // Another connection superseded us, or the isolate died.
        return promise.then([this]() {
          // TODO(soon): What happens if the other side never hangs up?
          return webSocket.disconnect();
        });
      }

      return promise.then([this]() { return outgoingLoop(); });
    });
  }

  kj::Promise<void> incomingLoop() {
    return webSocket.receive().then([this](kj::WebSocket::Message&& message) -> kj::Promise<void> {
      KJ_SWITCH_ONEOF(message) {
        KJ_CASE_ONEOF(text, kj::String) {
          {
            capnp::MallocMessageBuilder message;
            auto cmd = message.initRoot<cdp::Command>();

            getCdpJsonCodec().decode(text, cmd);

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
                auto state = this->state.lockExclusive();
                Isolate& isolate = const_cast<Isolate&>(*state->get()->isolate);
                KJ_IF_MAYBE(p, isolate.impl->profiler) {
                  jsg::V8StackScope stackScope;
                  Isolate::Impl::Lock recordedLock(isolate, InspectorLock(nullptr), stackScope);
                  auto& lock = *recordedLock.lock;
                  stopProfiling(**p, lock.v8Isolate, cmd);
                }
                break;
              }

              case cdp::Command::PROFILER_START: {
                auto state = this->state.lockExclusive();
                Isolate& isolate = const_cast<Isolate&>(*state->get()->isolate);
                KJ_IF_MAYBE(p, isolate.impl->profiler) {
                  jsg::V8StackScope stackScope;
                  Isolate::Impl::Lock recordedLock(isolate, InspectorLock(nullptr), stackScope);
                  auto& lock = *recordedLock.lock;
                  startProfiling(**p, lock.v8Isolate);
                }
                break;
              }

              case cdp::Command::PROFILER_SET_SAMPLING_INTERVAL: {
                auto state = this->state.lockExclusive();
                Isolate& isolate = const_cast<Isolate&>(*state->get()->isolate);
                KJ_IF_MAYBE(p, isolate.impl->profiler) {
                  jsg::V8StackScope stackScope;
                  Isolate::Impl::Lock recordedLock(isolate, InspectorLock(nullptr), stackScope);
                  auto interval = cmd.getProfilerSetSamplingInterval().getParams().getInterval();
                  setSamplingInterval(**p, interval);
                }
                break;
              }
              case cdp::Command::PROFILER_ENABLE: {
                auto state = this->state.lockExclusive();
                Isolate& isolate = const_cast<Isolate&>(*state->get()->isolate);
                jsg::V8StackScope stackScope;
                Isolate::Impl::Lock recordedLock(isolate, InspectorLock(nullptr), stackScope);
                auto& lock = *recordedLock.lock;
                isolate.impl->profiler = kj::Own<v8::CpuProfiler>(
                    v8::CpuProfiler::New(lock.v8Isolate, v8::kDebugNaming, v8::kLazyLogging),
                    CpuProfilerDisposer::instance);
                break;
              }
              case cdp::Command::TAKE_HEAP_SNAPSHOT: {
                auto state = this->state.lockExclusive();
                Isolate& isolate = const_cast<Isolate&>(*state->get()->isolate);
                jsg::V8StackScope stackScope;
                Isolate::Impl::Lock recordedLock(isolate, InspectorLock(nullptr), stackScope);
                auto& lock = *recordedLock.lock;
                auto params = cmd.getTakeHeapSnapshot().getParams();
                takeHeapSnapshot(lock,
                    params.getExposeInternals(),
                    params.getCaptureNumericValue());
                break;
              }
            }

            if (!cmd.isUnknown()) {
              sendNotification(cmd);
              return incomingLoop();
            }
          }

          auto state = this->state.lockExclusive();

          // const_cast OK because we're going to lock it
          Isolate& isolate = const_cast<Isolate&>(*state->get()->isolate);
          jsg::V8StackScope stackScope;
          Isolate::Impl::Lock recordedLock(isolate, InspectorLock(nullptr), stackScope);
          auto& lock = *recordedLock.lock;

          // We have at times observed V8 bugs where the inspector queues a background task and
          // then synchronously waits for it to complete, which would deadlock if background
          // threads are disallowed. Since the inspector is in a process sandbox anyway, it's not
          // a big deal to just permit those background threads.
          AllowV8BackgroundThreadsScope allowBackgroundThreads;

          kj::Maybe<kj::Exception> maybeLimitError;
          {
            auto limitScope = isolate.getLimitEnforcer().enterInspectorJs(lock, maybeLimitError);
            state->get()->session->dispatchProtocolMessage(toStringView(text));
          }

          // Run microtasks in case the user made an async call.
          if (maybeLimitError == nullptr) {
            auto limitScope = isolate.getLimitEnforcer().enterInspectorJs(lock, maybeLimitError);
            lock.v8Isolate->PerformMicrotaskCheckpoint();
          } else {
            // Oops, we already exceeded the limit, so force the microtask queue to be thrown away.
            lock.v8Isolate->TerminateExecution();
            lock.v8Isolate->PerformMicrotaskCheckpoint();
          }

          KJ_IF_MAYBE(limitError, maybeLimitError) {
            v8::HandleScope scope(lock.v8Isolate);

            // HACK: We want to print the error, but we need a context to do that.
            //   We don't know which contexts exist in this isolate, so I guess we have to
            //   create one. Ugh.
            auto dummyContext = v8::Context::New(lock.v8Isolate);
            auto& inspector = *KJ_ASSERT_NONNULL(isolate.impl->inspector);
            inspector.contextCreated(
                v8_inspector::V8ContextInfo(dummyContext, 1, v8_inspector::StringView(
                    reinterpret_cast<const uint8_t*>("Worker"), 6)));
            sendExceptionToInspector(inspector, dummyContext,
                jsg::extractTunneledExceptionDescription(limitError->getDescription()));
            inspector.contextDestroyed(dummyContext);
          }

          if (recordedLock.checkInWithLimitEnforcer(isolate)) {
            disconnect();
          }
        }
        KJ_CASE_ONEOF(bytes, kj::Array<byte>) {
          // ignore
        }
        KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
          // all done
          receivedClose = true;
          outgoingQueueNotifier->notify();

          // The outgoing loop will wake up and will exit out. It is exclusively joined with the
          // incoming loop, so we'll be canceled there. We use NEVER_DONE here to make sure we
          // don't inadvertently cancel the outgoing loop.
          return kj::NEVER_DONE;
        }
      }
      return incomingLoop();
    });
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
    outgoingQueue.lockExclusive()->add(kj::mv(message));
    outgoingQueueNotifier->notify();

    // TODO(someday): Should we implement some sort of backpressure if the queue gets large? Will
    //   need to be careful about deadlock if so, since presumably the isolate is locked during
    //   these callbacks.
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

private:
  kj::WebSocket& webSocket;

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

    std::unique_ptr<const v8::HeapSnapshot> snapshot(
        js.v8Isolate->GetHeapProfiler()->TakeHeapSnapshot(&activity, nullptr,
            exposeInternals, captureNumericValue));
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
        jsg::V8StackScope stackScope;
        Isolate::Impl::Lock recordedLock(*isolate, InspectorLock(nullptr), stackScope);
        session = nullptr;
      }
    }

    void teardownUnderLock() {
      // Must be called with the worker isolate locked. Should be called immediately before
      // destruction.
      session = nullptr;
    }

    KJ_DISALLOW_COPY_AND_MOVE(State);
  };
  kj::MutexGuarded<kj::Own<State>> state;
  // Mutex ordering: You must lock this *before* locking the isolate.

  class XThreadNotifier final: public kj::AtomicRefcounted {
    // Class encapsulating the ability to notify the inspector thread from other threads when
    // messages are pushed to the outgoing queue.
    //
    // TODO(cleanup): This could be a lot simpler if only it were possible to cancel
    //   an executor.executeAsync() promise from an arbitrary thread. Then, if the inspector
    //   session was destroyed in its thread while a cross-thread notification was in-flight, it
    //   could cancel that notification directly.
  public:
    void clear() {
      // Must call in main thread before it drops its reference.
      paf = nullptr;
    }

    kj::Promise<void> awaitNotification() {
      return kj::mv(KJ_ASSERT_NONNULL(paf).promise).then([this]() {
        paf = kj::newPromiseAndFulfiller<void>();
        __atomic_store_n(&inFlight, false, __ATOMIC_RELAXED);
      });
    }

    void notify() const {
      // TODO(perf): Figure out why this commented-out optimization sometimes randomly misses
      //   messages, particularly under load.
      // if (__atomic_exchange_n(&inFlight, true, __ATOMIC_RELAXED)) {
      //   // A notifciation is already in-flight, no need to send another one.
      // } else {
        executor.executeAsync([ref = kj::atomicAddRef(*this)]() {
          KJ_IF_MAYBE(p, ref->paf) {
            p->fulfiller->fulfill();
          }
        }).detach([](kj::Exception&& exception) {
          KJ_LOG(ERROR, exception);
        });
      // }
    }

  private:
    const kj::Executor& executor = kj::getCurrentThreadExecutor();

    mutable kj::Maybe<kj::PromiseFulfillerPair<void>> paf = kj::newPromiseAndFulfiller<void>();
    // Accessed only in notifier's owning thread.

    mutable bool inFlight = false;
    // Is a notification already in-flight?
  };

  kj::Own<XThreadNotifier> outgoingQueueNotifier = kj::atomicRefcounted<XThreadNotifier>();
  // Whenever another thread adds messages to the outgoing queue, it notifies the inspector
  // connection thread using this.

  kj::MutexGuarded<kj::Vector<kj::String>> outgoingQueue;
  bool receivedClose = false;

  volatile bool networkEnabled = false;
  // Not under `state` lock due to lock ordering complications.

  kj::Promise<void> sendToWebsocket(kj::ArrayPtr<kj::String> messages) {
    if (messages.size() == 0) {
      return kj::READY_NOW;
    } else {
      auto first = kj::mv(messages[0]);
      auto rest = messages.slice(1, messages.size());
      return webSocket.send(first).attach(kj::mv(first)).then([this, rest]() mutable {
        return sendToWebsocket(rest);
      });
    }
  }
};

kj::Promise<void> Worker::Isolate::attachInspector(
    kj::Timer& timer,
    kj::Duration timerOffset,
    kj::HttpService::Response& response,
    const kj::HttpHeaderTable& headerTable,
    kj::HttpHeaderId controlHeaderId) const {
  KJ_REQUIRE(impl->inspector != nullptr);

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
  KJ_REQUIRE(impl->inspector != nullptr);

  jsg::V8StackScope stackScope;
  Isolate::Impl::Lock recordedLock(*this, InspectorChannelImpl::InspectorLock(nullptr), stackScope);
  auto& lock = *recordedLock.lock;
  auto& lockedSelf = const_cast<Worker::Isolate&>(*this);

  // If another inspector was already connected, boot it, on the assumption that that connection
  // is dead and this is why the user reconnected. While we could actually allow both inspector
  // sessions to stay open (V8 supports this!), we'd then need to store a set of all connected
  // inspectors in order to be able to disconnect all of them in case of an isolate purge... let's
  // just not.
  lockedSelf.disconnectInspector();

  auto channel = kj::heap<Worker::Isolate::InspectorChannelImpl>(
      kj::atomicAddRef(*this), webSocket);
  lockedSelf.currentInspectorSession = *channel;

  lockedSelf.impl->inspectorClient.setInspectorTimerInfo(timer, timerOffset);

  // Send any queued notifications.
  {
    v8::HandleScope handleScope(lock.v8Isolate);
    for (auto& notification: lockedSelf.impl->queuedNotifications) {
      channel->sendNotification(kj::mv(notification));
    }
    lockedSelf.impl->queuedNotifications.clear();
  }

  return channel->incomingLoop()
      .exclusiveJoin(channel->outgoingLoop())
      .attach(kj::mv(channel));
}

void Worker::Isolate::disconnectInspector() {
  // If an inspector session is connected, proactively drop it, so as to force it to drop its
  // reference on the script, so that the script can be deleted.

  KJ_IF_MAYBE(current, currentInspectorSession) {
    current->disconnect();
  }
}

void Worker::Isolate::logWarning(kj::StringPtr description, Lock& lock) {
  if (impl->inspector != nullptr) {
    // getContext requires a HandleScope
    v8::HandleScope scope(lock.getIsolate());

    logMessage(lock.getContext(), static_cast<uint16_t>(cdp::LogType::WARNING), description);
  }

  // Run with --verbose to log JS exceptions to stderr. Useful when running tests.
  KJ_LOG(INFO, "console warning", description);
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

void Worker::Isolate::logMessage(v8::Local<v8::Context> context,
                                 uint16_t type, kj::StringPtr description) {
  if (impl->inspector != nullptr) {
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

    v8::Isolate* isolate = context->GetIsolate();
    v8::HandleScope scope(isolate);

    capnp::MallocMessageBuilder message;
    auto event = message.initRoot<cdp::Event>();

    auto params = event.initRuntimeConsoleApiCalled();
    params.setType(static_cast<cdp::LogType>(type));
    params.initArgs(1)[0].initString().setValue(description);
    params.setExecutionContextId(v8_inspector::V8ContextInfo::executionContextId(context));
    params.setTimestamp(impl->inspectorClient.currentTimeMS());
    stackTraceToCDP(isolate, params.initStackTrace());

    auto notification = getCdpJsonCodec().encode(event);
    KJ_IF_MAYBE(i, currentInspectorSession) {
      i->sendNotification(kj::mv(notification));
    } else {
      impl->queuedNotifications.add(kj::mv(notification));
    }
  }
}

// =======================================================================================

struct Worker::Actor::Impl {
  Actor::Id actorId;
  MakeStorageFunc makeStorage;

  kj::Own<ActorObserver> metrics;

  kj::Maybe<jsg::Value> transient;
  kj::Maybe<kj::Own<ActorCacheInterface>> actorCache;

  struct NoClass {};
  struct Initializing {};

  kj::OneOf<
    NoClass,                         // not class-based
    DurableObjectConstructor*,       // constructor not run yet
    Initializing,                    // constructor currently running
    api::ExportedHandler,            // fully constructed
    kj::Exception                    // constructor threw
  > classInstance;
  // If the actor is backed by a class, this field tracks the instance through its stages. The
  // instance is constructed as part of the first request to be delivered.

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
      return timerChannel.afterLimitTimeout(timeout)
          .then([]() -> kj::Promise<void> {
        return KJ_EXCEPTION(FAILED,
            "broken.outputGateBroken; jsg.Error: Durable Object storage operation exceeded "
            "timeout which caused object to be reset.");
      });
    }

    void outputGateLocked() override { metrics.outputGateLocked(); }
    void outputGateReleased() override { metrics.outputGateReleased(); }
    void outputGateWaiterAdded() override { metrics.outputGateWaiterAdded(); }
    void outputGateWaiterRemoved() override { metrics.outputGateWaiterRemoved(); }
    // Implements OutputGate::Hooks.

    void updateAlarmInMemory(kj::Maybe<kj::Date> newAlarmTime) override;
    // Implements ActorCache::Hooks

  private:
    kj::Own<Loopback> loopback;    // only for updateAlarmInMemory()
    TimerChannel& timerChannel;    // only for afterLimitTimeout() and updateAlarmInMemory()
    ActorObserver& metrics;

    kj::Maybe<kj::Promise<void>> maybeAlarmPreviewTask;
  };

  HooksImpl hooks;

  InputGate inputGate;
  // Handles both input locks and request locks.

  OutputGate outputGate;
  // Handles output locks.

  kj::Maybe<kj::Own<IoContext>> ioContext;
  // `ioContext` is initialized upon delivery of the first request.

  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Promise<void>>>> abortFulfiller;
  // If onBroken() is called while `ioContext` is still null, this is initialized. When
  // `ioContext` is constructed, this will be fulfilled with `ioContext.onAbort()`.

  kj::Maybe<kj::Promise<void>> metricsFlushLoopTask;
  // Task which periodically flushes metrics. Initialized after `ioContext` is initialized.

  kj::Own<Loopback> loopback;
  // Allows sending requests back into this actor, recreating it as necessary. Safe to hold longer
  // than the Worker::Actor is alive.

  TimerChannel& timerChannel;

  kj::ForkedPromise<void> shutdownPromise;
  kj::Own<kj::PromiseFulfiller<void>> shutdownFulfiller;

  kj::Maybe<kj::Own<HibernationManager>> hibernationManager;
  // If this Actor has a HibernationManager, it means the Actor has recently accepted a Hibernatable
  // websocket. We eventually move the HibernationManager into the DeferredProxy task
  // (since it's long lived), but can still refer to the HibernationManager by passing a reference
  // in each CustomEvent.
  kj::Maybe<uint16_t> hibernationEventType;
  kj::PromiseFulfillerPair<void> constructorFailedPaf = kj::newPromiseAndFulfiller<void>();

  struct ScheduledAlarm {
    ScheduledAlarm(kj::Date scheduledTime, kj::PromiseFulfillerPair<AlarmResult> pf)
      : scheduledTime(scheduledTime), resultFulfiller(kj::mv(pf.fulfiller)),
        resultPromise(pf.promise.fork()) {}
    KJ_DISALLOW_COPY(ScheduledAlarm);
    ScheduledAlarm(ScheduledAlarm&&) = default;
    ~ScheduledAlarm() noexcept(false) {}

    kj::Date scheduledTime;
    AlarmFulfiller resultFulfiller;
    kj::ForkedPromise<AlarmResult> resultPromise;
    kj::Promise<void> cleanupPromise =
        resultPromise.addBranch().then([](AlarmResult&&){}, [](kj::Exception&&){});
    // The first thing we do after we get a result should be to remove the running alarm (if we got
    // that far). So we grab the first branch now and ignore any results, before anyone else has a
    // chance to do so.
  };
  struct RunningAlarm {
    kj::Date scheduledTime;
    kj::ForkedPromise<AlarmResult> resultPromise;
  };
  kj::Maybe<ScheduledAlarm> maybeScheduledAlarm;
  // If valid, we have an alarm invocation that has not yet received an `AlarmFulfiller` and thus
  // is either waiting for a running alarm or its scheduled time.

  kj::Maybe<RunningAlarm> maybeRunningAlarm;
  // If valid, we have an alarm invocation that has received an `AlarmFulfiller` and is currently
  // considered running. This alarm is no longer cancellable.

  kj::ForkedPromise<void> runningAlarmTask = kj::Promise<void>(kj::READY_NOW).fork();
  // This is a forked promise so that we can schedule and then cancel multiple alarms while an alarm
  // is running.

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
    v8::Isolate* isolate = lock.getIsolate();
    v8::HandleScope scope(isolate);
    v8::Context::Scope contextScope(lock.getContext());
    if (hasTransient) {
      transient.emplace(isolate, v8::Object::New(isolate));
    }

    actorCache = makeActorCache(self.worker->getIsolate().impl->actorCacheLru, outputGate, hooks);
  }
};

kj::Promise<Worker::AsyncLock> Worker::takeAsyncLockWhenActorCacheReady(
    kj::Date now, Actor& actor, RequestObserver& request) const {
  auto lockTiming = getIsolate().getMetrics()
      .tryCreateLockTiming(kj::Maybe<RequestObserver&>(request));

  KJ_IF_MAYBE(c, actor.impl->actorCache) {
    KJ_IF_MAYBE(p, c->get()->evictStale(now)) {
      // Got backpressure, wait for it.
      // TODO(someday): Count this time period differently in lock timing data?
      return p->then([this, lockTiming = kj::mv(lockTiming)]() mutable {
        return getIsolate().takeAsyncLockImpl(kj::mv(lockTiming));
      });
    }
  }

  return getIsolate().takeAsyncLockImpl(kj::mv(lockTiming));
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

  KJ_IF_MAYBE(c, className) {
    KJ_IF_MAYBE(cls, lock.getWorker().impl->actorClasses.find(*c)) {
      impl->classInstance = &(*cls);
    } else {
      kj::throwFatalException(KJ_EXCEPTION(FAILED, "broken.ignored; no such actor class", *c));
    }
  } else {
    impl->classInstance = Impl::NoClass();
  }
}

void Worker::Actor::ensureConstructed(IoContext& context) {
  KJ_IF_MAYBE(cls, impl->classInstance.tryGet<DurableObjectConstructor*>()) {
    context.addWaitUntil(context.run([this, &cls = **cls](Worker::Lock& lock) {
      auto isolate = lock.getIsolate();

      kj::Maybe<jsg::Ref<api::DurableObjectStorage>> storage;
      KJ_IF_MAYBE(c, impl->actorCache) {
        storage = impl->makeStorage(lock, *worker->getIsolate().apiIsolate, **c);
      }
      auto handler = cls(lock,
          jsg::alloc<api::DurableObjectState>(cloneId(), kj::mv(storage)),
          KJ_ASSERT_NONNULL(lock.getWorker().impl->env).addRef(isolate));

      // HACK: We set handler.env to undefined because we already passed the real env into the
      //   constructor, and we want the handler methods to act like they take just one parameter.
      //   We do the same for handler.ctx, as ExecutionContext related tasks are performed
      //   on the actor's state field instead.
      handler.env = jsg::Value(isolate, v8::Undefined(isolate));
      handler.ctx = nullptr;

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
  Worker::Lock lock(*worker, Worker::Lock::TakeSynchronously(nullptr));
  impl = nullptr;
}

void Worker::Actor::shutdown(uint16_t reasonCode, kj::Maybe<const kj::Exception&> error) {
  // We're officially canceling all background work and we're going to destruct the Actor as soon
  // as all IoContexts that reference it go out of scope. We might still log additional
  // periodic messages, and that's good because we might care about that information. That said,
  // we're officially "broken" from this point because we cannot service background work and our
  // capability server should have triggered this (potentially indirectly) via its destructor.
  KJ_IF_MAYBE(r, impl->ioContext) {
    impl->metrics->shutdown(reasonCode, r->get()->getLimitEnforcer());
  } else {
    // The actor was shut down before the IoContext was even constructed, so no metrics are
    // written.
  }

  shutdownActorCache(error);

  impl->shutdownFulfiller->fulfill();
}

void Worker::Actor::shutdownActorCache(kj::Maybe<const kj::Exception&> error) {
  KJ_IF_MAYBE(ac, impl->actorCache) {
    ac->get()->shutdown(error);
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

  KJ_IF_MAYBE(rc, impl->ioContext) {
    abortPromise = rc->get()->onAbort();
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

kj::Maybe<jsg::Value> Worker::Actor::getTransient(Worker::Lock& lock) {
  KJ_REQUIRE(&lock.getWorker() == worker.get());
  return impl->transient.map([&](jsg::Value& val) { return val.addRef(lock.getIsolate()); });
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
    return impl->makeStorage(lock, *worker->getIsolate().apiIsolate, *cache);
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
    KJ_CASE_ONEOF(_, DurableObjectConstructor*) {
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
  if (newTime == nullptr) {
    maybeAlarmPreviewTask = nullptr;
    return;
  }

  auto scheduledTime = KJ_ASSERT_NONNULL(newTime);

  auto retry = kj::coCapture([this, originalTime = scheduledTime]()
      -> kj::Promise<void> {
    kj::Date scheduledTime = originalTime;

    for(auto i : kj::zeroTo(WorkerInterface::ALARM_RETRY_MAX_TRIES)) {
      auto result = co_await timerChannel.atTime(scheduledTime)
          .then([this, originalTime]() {
        return loopback->getWorker(IoChannelFactory::SubrequestMetadata{})->runAlarm(originalTime);
      });

      if (result.outcome != EventOutcome::OK && result.retry) {
        auto delay = (WorkerInterface::ALARM_RETRY_START_SECONDS << i++) * kj::SECONDS;
        scheduledTime = timerChannel.now() + delay;
      } else {
        break;
      }
    }
  });

  maybeAlarmPreviewTask = retry();
}

auto Worker::Actor::getAlarm(kj::Date scheduledTime) -> kj::Maybe<kj::Promise<AlarmResult>> {
  KJ_IF_MAYBE(runningAlarm, impl->maybeRunningAlarm) {
    if (runningAlarm->scheduledTime == scheduledTime) {
      // The running alarm has the same time, we can just wait for it.
      return runningAlarm->resultPromise.addBranch();
    }
  }

  KJ_IF_MAYBE(scheduledAlarm, impl->maybeScheduledAlarm) {
    if (scheduledAlarm->scheduledTime == scheduledTime) {
      // The scheduled alarm has the same time, we can just wait for it.
      return scheduledAlarm->resultPromise.addBranch();
    }
  }

  return nullptr;
}

auto Worker::Actor::scheduleAlarm(kj::Date scheduledTime) -> kj::Promise<ScheduleAlarmResult> {
  KJ_IF_MAYBE(runningAlarm, impl->maybeRunningAlarm) {
    if (runningAlarm->scheduledTime == scheduledTime) {
      // The running alarm has the same time, we can just wait for it.
      auto result = co_await runningAlarm->resultPromise.addBranch();
      co_return result;
    }
  }

  KJ_IF_MAYBE(scheduledAlarm, impl->maybeScheduledAlarm) {
      // We had a previously scheduled alarm, let's cancel it.
      scheduledAlarm->resultFulfiller.cancel();
      impl->maybeScheduledAlarm = nullptr;
  }

  KJ_IASSERT(impl->maybeScheduledAlarm == nullptr);
  auto& scheduledAlarm = impl->maybeScheduledAlarm.emplace(
      scheduledTime, kj::newPromiseAndFulfiller<AlarmResult>());

  auto whenCanceled = scheduledAlarm.resultPromise.addBranch().then(
      [](AlarmResult result) -> ScheduleAlarmResult {
    // We've been cancelled, so return that result. Note that we cannot be resolved any other
    // way until we return an AlarmFulfiller below.
    return result;
  });

  // Let's wait for any running alarm to cleanup before we even delay.
  auto whenReady = impl->runningAlarmTask.addBranch().then([this, scheduledTime](){
    // Date.now() < scheduledTime when the alarm comes in, since we subtract elapsed CPU time from
    // the time of last I/O in the implementation of Date.now(). This difference could be used to
    // implement a spectre timer, so we have to wait a little longer until
    // `Date.now() == scheduledTime`. Note that this also means that we could invoke ahead of its
    // `scheduledTime` and we'll delay until appropriate, this may be useful in cases of clock skew.
    return KJ_ASSERT_NONNULL(impl->ioContext)->atTime(scheduledTime);
  });

  co_return co_await whenReady.then([this]() -> ScheduleAlarmResult {
    // It's time to run! Let's tear apart the scheduled alarm and make a running alarm.

    // `maybeScheduledAlarm` should have the same value we emplaced above. If another call to
    // `scheduleAlarm()` emplaced a new value, then `whenCanceled` should have resolved which
    // cancels this this promise chain.
    auto scheduledAlarm = KJ_ASSERT_NONNULL(kj::mv(impl->maybeScheduledAlarm));
    impl->maybeScheduledAlarm = nullptr;

    impl->maybeRunningAlarm.emplace(Impl::RunningAlarm{
      .scheduledTime = scheduledAlarm.scheduledTime,
      .resultPromise = kj::mv(scheduledAlarm.resultPromise),
    });
    impl->runningAlarmTask = scheduledAlarm.cleanupPromise.attach(kj::defer([this](){
      // As soon as we get fulfilled or rejected, let's unset this alarm as the running alarm.
      impl->maybeRunningAlarm = nullptr;
    })).eagerlyEvaluate([](kj::Exception&& e){
      LOG_EXCEPTION("actorAlarmCleanup", e);
    }).fork();
    return kj::mv(scheduledAlarm.resultFulfiller);
  }).exclusiveJoin(kj::mv(whenCanceled));
}

kj::Maybe<api::ExportedHandler&> Worker::Actor::getHandler() {
  KJ_SWITCH_ONEOF(impl->classInstance) {
    KJ_CASE_ONEOF(_, Impl::NoClass) {
      return nullptr;
    }
    KJ_CASE_ONEOF(_, DurableObjectConstructor*) {
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
  KJ_REQUIRE(impl->ioContext == nullptr);
  KJ_IF_MAYBE(f, impl->abortFulfiller) {
    f->get()->fulfill(context->onAbort());
    impl->abortFulfiller = nullptr;
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
  KJ_REQUIRE(impl->hibernationManager == nullptr);
  hib->setTimerChannel(impl->timerChannel);
  // Not the cleanest way to provide hibernation manager with a timer channel reference, but
  // where HibernationManager is constructed (actor-state), we don't have a timer channel ref.
  impl->hibernationManager = kj::mv(hib);
}

kj::Maybe<uint16_t> Worker::Actor::getHibernationEventType() {
  return impl->hibernationEventType;
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

kj::Own<WorkerInterface> Worker::Isolate::wrapSubrequestClient(
    kj::Own<WorkerInterface> client,
    kj::HttpHeaderId contentEncodingHeaderId,
    RequestObserver& requestMetrics) const {
  if (impl->inspector != nullptr) {
    client = kj::heap<SubrequestClient>(
        kj::atomicAddRef(*this), kj::mv(client), contentEncodingHeaderId, requestMetrics);
  }

  return client;
}

void Worker::Isolate::completedRequest() const {
  limitEnforcer->completedRequest(id);
}

bool Worker::Isolate::isInspectorEnabled() const {
  return impl->inspector != nullptr;
}

namespace {

// We only run the inspector within process sandboxes. There, it is safe to query the real clock
// for some things, and we do so because we may not have a IoContext available to get
// Spectre-safe time.

double getMonotonicTimeForProcessSandboxOnly() {
  // Monotonic time in seconds with millisecond precision.

  KJ_REQUIRE(!isMultiTenantProcess(), "precise timing not safe in multi-tenant processes");
  auto timePoint = kj::systemPreciseMonotonicClock().now();
  return (timePoint - kj::origin<kj::TimePoint>()) / kj::MILLISECONDS / 1e3;
}

double getWallTimeForProcessSandboxOnly() {
  // Wall time in seconds with millisecond precision.

  KJ_REQUIRE(!isMultiTenantProcess(), "precise timing not safe in multi-tenant processes");
  auto timePoint = kj::systemPreciseCalendarClock().now();
  return (timePoint - kj::UNIX_EPOCH) / kj::MILLISECONDS / 1e3;
}

class NullOutputStream final: public kj::AsyncOutputStream {
public:
  kj::Promise<void> write(const void* buffer, size_t size) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
};

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
    jsg::V8StackScope stackScope;
    Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(requestMetrics), stackScope);
    auto& isolate = const_cast<Isolate&>(*constIsolate);

    KJ_IF_MAYBE(i, isolate.currentInspectorSession) {
      capnp::MallocMessageBuilder message;

      auto event = message.initRoot<cdp::Event>();

      auto params = event.initNetworkLoadingFinished();
      params.setRequestId(requestId);
      params.setEncodedDataLength(rawSize);
      params.setTimestamp(getMonotonicTimeForProcessSandboxOnly());
      auto response = params.initCfResponse();
      KJ_IF_MAYBE(body, decodedBuf.getArray()) {
        response.setBase64Encoded(true);
        response.setBody(kj::encodeBase64(*body));
      }

      i->sendNotification(event);
    }
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
    KJ_IF_MAYBE(comp, compStream) {
      KJ_SWITCH_ONEOF(*comp) {
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

    jsg::V8StackScope stackScope;
    Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(requestMetrics), stackScope);
    auto& isolate = const_cast<Isolate&>(*constIsolate);

    KJ_IF_MAYBE(i, isolate.currentInspectorSession) {
      capnp::MallocMessageBuilder message;

      auto event = message.initRoot<cdp::Event>();

      auto params = event.initNetworkDataReceived();
      params.setRequestId(requestId);
      params.setEncodedDataLength(buffer.size());
      params.setDataLength(decodedChunkSize);
      params.setTimestamp(getMonotonicTimeForProcessSandboxOnly());

      i->sendNotification(event);
    }
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

kj::Promise<void> Worker::Isolate::SubrequestClient::request(
    kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody, kj::HttpService::Response& response) {
  using InspectorLock = InspectorChannelImpl::InspectorLock;

  auto signalRequest =
      [this, method, urlCopy = kj::str(url), headersCopy = headers.clone()]
      () -> kj::Maybe<kj::String> {
    jsg::V8StackScope stackScope;
    Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(*requestMetrics), stackScope);
    auto& lock = *recordedLock.lock;
    auto& isolate = const_cast<Isolate&>(*constIsolate);

    if (isolate.currentInspectorSession == nullptr) {
      return nullptr;
    }

    auto& i = KJ_ASSERT_NONNULL(isolate.currentInspectorSession);
    if (!i.isNetworkEnabled()) {
      return nullptr;
    }

    v8::HandleScope handleScope(lock.v8Isolate);

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
    stackTraceToCDP(lock.v8Isolate, initiator.initStack());

    auto request = params.initRequest();
    request.setUrl(urlCopy);
    request.setMethod(kj::str(method));

    headersToCDP(headersCopy, request.initHeaders());

    i.sendNotification(event);
    return kj::mv(requestId);
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
    KJ_IF_MAYBE(type, headers.get(kj::HttpHeaderId::CONTENT_TYPE)) {
      KJ_IF_MAYBE(semiColon, type->findFirst(';')) {
        response.setMimeType(kj::str(type->slice(0, *semiColon)));
      } else {
        response.setMimeType(*type);
      }

      auto mimeType = response.getMimeType().asString();

      // Normally Chrome would know what it's loading based on an element or API used for
      // the request. We don't have that privilege, but still want network filters to work,
      // so we do our best-effort guess of the resource type based on its mime type.
      if (mimeType == "text/html" || mimeType == "application/xhtml+xml") {
        params.setType(cdp::Page::ResourceType::DOCUMENT);
      } else if (mimeType == "text/css") {
        params.setType(cdp::Page::ResourceType::STYLESHEET);
      } else if (mimeType == "application/javascript" ||
                  mimeType == "text/javascript" ||
                  mimeType == "application/x-javascript") {
        params.setType(cdp::Page::ResourceType::SCRIPT);
      } else if (mimeType.startsWith("image/")) {
        params.setType(cdp::Page::ResourceType::IMAGE);
      } else if (mimeType.startsWith("audio/") ||
                  mimeType.startsWith("video/")) {
        params.setType(cdp::Page::ResourceType::MEDIA);
      } else if (mimeType.startsWith("font/") ||
                  mimeType.startsWith("application/font-") ||
                  mimeType.startsWith("application/x-font-")) {
        params.setType(cdp::Page::ResourceType::FONT);
      } else if (mimeType == "application/manifest+json") {
        params.setType(cdp::Page::ResourceType::MANIFEST);
      } else if (mimeType == "text/vtt") {
        params.setType(cdp::Page::ResourceType::TEXT_TRACK);
      } else if (mimeType == "text/event-stream") {
        params.setType(cdp::Page::ResourceType::EVENT_SOURCE);
      } else if (mimeType.endsWith("/xml") ||
                  mimeType.endsWith("/json") ||
                  mimeType.endsWith("+xml") ||
                  mimeType.endsWith("+json")) {
        params.setType(cdp::Page::ResourceType::XHR);
      }
    } else {
      response.setMimeType("text/plain");
    }
    headersToCDP(headers, response.initHeaders());

    auto encoding = api::StreamEncoding::IDENTITY;
    KJ_IF_MAYBE(encodingStr, headers.get(contentEncodingHeaderId)) {
      if (*encodingStr == "gzip") {
        encoding = api::StreamEncoding::GZIP;
      } else if (*encodingStr == "br") {
        encoding = api::StreamEncoding::BROTLI;
      }
    }

    // Defer to a later turn of the event loop so that it's safe to take a lock.
    return kj::newPromisedStream(kj::evalLater(
        [this, responseBody = kj::mv(responseBody), message = kj::mv(message), event, encoding,
         requestId = kj::mv(requestId)]
        () mutable -> kj::Own<kj::AsyncOutputStream> {
      // Now we know we can lock...
      jsg::V8StackScope stackScope;
      Isolate::Impl::Lock recordedLock(*constIsolate, InspectorLock(*requestMetrics), stackScope);
      auto& isolate = const_cast<Isolate&>(*constIsolate);

      // We shouldn't even get here if network inspection isn't active since signalRequest() would
      // have returned null... but double-check anyway.
      if (isolate.currentInspectorSession == nullptr) {
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
        kj::Maybe<uint64_t> expectedBodySize = nullptr) override {
      auto body = inner.send(statusCode, statusText, headers, expectedBodySize);
      return signalResponse(kj::mv(requestId), statusCode, statusText, headers, kj::mv(body));
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
      auto webSocket = inner.acceptWebSocket(headers);
      // TODO(someday): Support sending WebSocket frames over CDP. For now we fake an empty
      //   response.
      signalResponse(kj::mv(requestId), 101, "Switching Protocols", headers,
                     kj::heap<NullOutputStream>());
      return kj::mv(webSocket);
    }

  private:
    kj::HttpService::Response& inner;
    kj::String requestId;
    SignalResponse signalResponse;
  };

  // For accurate lock metrics, we want to avoid taking a recursive isolate lock, so we postpone
  // the request until a later turn of the event loop.
  return kj::evalLater(kj::mv(signalRequest))
      .then([this, method, url, &headers, &requestBody, &response,
             signalResponse = kj::mv(signalResponse)]
            (kj::Maybe<kj::String> maybeRequestId) {
    KJ_IF_MAYBE(rid, maybeRequestId) {
      auto wrapper = kj::heap<ResponseWrapper>(response, kj::mv(*rid), kj::mv(signalResponse));
      return inner->request(method, url, headers, requestBody, *wrapper)
          .attach(kj::mv(wrapper));
    } else {
      return inner->request(method, url, headers, requestBody, response);
    }
  });
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
    kj::Date scheduledTime) {
  return inner->runAlarm(scheduledTime);
}
kj::Promise<WorkerInterface::CustomEvent::Result>
    Worker::Isolate::SubrequestClient::customEvent(kj::Own<CustomEvent> event) {
  return inner->customEvent(kj::mv(event));
}

}  // namespace workerd
