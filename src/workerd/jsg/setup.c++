// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#if __APPLE__
// We need to define `_XOPEN_SOURCE` to get `ucontext_t` on Mac.
#define _XOPEN_SOURCE
#endif

#include "setup.h"
#include <workerd/util/uuid.h>
#include "libplatform/libplatform.h"
#include <v8-cppgc.h>

#if !_WIN32
#include <cxxabi.h>
#include <ucontext.h>
#endif

#ifdef WORKERD_ICU_DATA_EMBED
#include <icudata-embed.capnp.h>
#include <unicode/udata.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach.h>
#endif

namespace workerd::jsg {

static bool v8Initialized = false;
static V8System::FatalErrorCallback* v8FatalErrorCallback = nullptr;
static void reportV8FatalError(kj::StringPtr location, kj::StringPtr message) {
  if (v8FatalErrorCallback == nullptr) {
    KJ_LOG(FATAL, "V8 fatal error", location, message);
    abort();
  } else {
    v8FatalErrorCallback(location, message);
  }
}
static void v8DcheckError(const char* file, int line, const char* message) {
  reportV8FatalError(kj::str(file, ':', line), message);
}

class PlatformDisposer final: public kj::Disposer {
public:
  virtual void disposeImpl(void* pointer) const override {
    delete reinterpret_cast<v8::Platform*>(pointer);
  }

  static const PlatformDisposer instance;
};

const PlatformDisposer PlatformDisposer::instance {};

kj::Own<v8::Platform> defaultPlatform(uint backgroundThreadCount) {
  return kj::Own<v8::Platform>(
      v8::platform::NewDefaultPlatform(
        backgroundThreadCount,  // default thread pool size
        v8::platform::IdleTaskSupport::kDisabled,  // TODO(perf): investigate enabling
        v8::platform::InProcessStackDumping::kDisabled,  // KJ's stack traces are better
        nullptr)  // default TracingController
      .release(), PlatformDisposer::instance);
}

static kj::Own<v8::Platform> userPlatform(v8::Platform& platform) {
  // Make a fake kj::Own that wraps a user-specified platform reference. V8's default platform can
  // only be created with manual memory management, so V8System::Platform needs to be able to store
  // a smart pointer. However, requiring user platforms to come in via kj::Owns feels unnatural.
  return kj::Own<v8::Platform>(&platform, kj::NullDisposer::instance);
}

V8System::V8System(): V8System(defaultPlatform(0), nullptr) {}
V8System::V8System(kj::ArrayPtr<const kj::StringPtr> flags): V8System(defaultPlatform(0), flags) {}
V8System::V8System(v8::Platform& platformParam): V8System(platformParam, nullptr) {}
V8System::V8System(v8::Platform& platformParam, kj::ArrayPtr<const kj::StringPtr> flags)
    : V8System(userPlatform(platformParam), flags) {}
V8System::V8System(kj::Own<v8::Platform> platformParam, kj::ArrayPtr<const kj::StringPtr> flags)
    : platformInner(kj::mv(platformParam)), platformWrapper(*platformInner) {
#if V8_HAS_STACK_START_MARKER
  v8::StackStartMarker::EnableForProcess();
#endif

  v8::V8::SetDcheckErrorHandler(&v8DcheckError);

  // Note that v8::V8::SetFlagsFromString() simply ignores flags it doesn't recognize, which means
  // typos don't generate any error. SetFlagsFromCommandLine() has the `remove_flags` option which
  // leaves behind the flags V8 didn't recognize, so we'd like to use that for error checking
  // purposes. Unfortunately, the interface is rather awkward, since it assumes you're going to
  // run it on the raw argv array.
  //
  // Especially annoying is that V8 expects an array of `char*` -- not `const`. It won't actually
  // modify the strings, so we'll just const_cast them here...
  int argc = flags.size() + 1;
  KJ_STACK_ARRAY(char*, argv, flags.size() + 2, 32, 32);
  argv[0] = const_cast<char*>("fake-binary-name");
  for (auto i: kj::zeroTo(flags.size())) {
    argv[i + 1] = const_cast<char*>(flags[i].cStr());
  }
  argv[argc] = nullptr;  // V8 probably doesn't need this but technically argv is NULL-terminated.

  v8::V8::SetFlagsFromCommandLine(&argc, argv.begin(), true);

  KJ_REQUIRE(argc == 1, "unrecognized V8 flag", argv[1]);

  // At present, we're not confident the JSG GC integration works with incremental marking. We have
  // seen bugs in the past that were fixed by adding this flag, although that was a long time ago
  // and the code has changed a lot since then. Since Worker heaps are generally relatively small
  // (limited to 128MB in Cloudflare Workers), incremental marking is probably not a win anyway,
  // and can be disabled. If we want to support significantly larger heaps, we may want to revisit
  // this. We'll want to do some stress testing first, and fix any bugs seen.
  //
  // (It turns out you can call v8::V8::SetFlagsFromString() as many times as you want to add
  // more flags.)
  v8::V8::SetFlagsFromString("--noincremental-marking");

#ifdef __APPLE__
  // On macOS arm64, we find that V8 can be collecting pages that contain compiled code when
  // handling requests in short succession. There are some specific differences for macOS arm64
  // that may be a factor:
  //   https://chromium.googlesource.com/v8/v8.git/+/refs/tags/11.5.150.4/src/heap/heap.h#2523
  //
  // Bugs attributable to this are https://github.com/cloudflare/workers-sdk/issues/2386 and
  // CUSTESC-29094.
  v8::V8::SetFlagsFromString("--single-threaded-gc");
#endif  // __APPLE__

#ifdef WORKERD_ICU_DATA_EMBED
  // V8's bazel build files currently don't support the option to embed ICU data, so we do it
  // ourselves. `WORKERD_ICU_DATA_EMBED`, if defined, will refer to a `kj::ArrayPtr<const byte>`
  // containing the data.
  UErrorCode err = U_ZERO_ERROR;
  udata_setCommonData(EMBEDDED_ICU_DATA_FILE->begin(), &err);
  udata_setFileAccess(UDATA_ONLY_PACKAGES, &err);
  KJ_ASSERT(err == U_ZERO_ERROR);
#else
  // We instruct V8 to compile in this data file, so passing nullptr should work here. If V8 is
  // built incorrectly, this will crash.
  v8::V8::InitializeICUDefaultLocation(nullptr);
#endif

  v8::V8::InitializePlatform(&platformWrapper);
  v8::V8::Initialize();
  cppgc::InitializeProcess(platformWrapper.GetPageAllocator());
  v8Initialized = true;
}

V8System::~V8System() noexcept(false) {
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  cppgc::ShutdownProcess();
}

void V8System::setFatalErrorCallback(FatalErrorCallback* callback) {
  v8FatalErrorCallback = callback;
}

IsolateBase& IsolateBase::from(v8::Isolate* isolate) {
  return *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
}

void IsolateBase::buildEmbedderGraph(v8::Isolate* isolate,
                                     v8::EmbedderGraph* graph,
                                     void* data) {
  try {
    const auto base = reinterpret_cast<IsolateBase*>(data);
    MemoryTracker tracker(isolate, graph);
    tracker.track(base);
  } catch (...) {
    // Generating the heap snapshot should be a safe process that does not
    // throw any exceptions. We'll treat any exception here as fatal, including
    // JsExceptionThrown. Note that we're not entered into any particular v8::Context
    // here so pulling out the details of the exception would be tricky anyway.
    kj::throwFatalException(kj::getCaughtExceptionAsKj());
  }
}

void IsolateBase::jsgGetMemoryInfo(MemoryTracker& tracker) const {
  tracker.trackField("uuid", uuid);
  tracker.trackField("heapTracer", heapTracer);
}

void IsolateBase::deferDestruction(Item item) {
  queue.lockExclusive()->push(kj::mv(item));
}

void IsolateBase::terminateExecution() const {
  ptr->TerminateExecution();
}

void IsolateBase::clearDestructionQueue() {
  // Safe to destroy the popped batch outside of the lock because the lock is only actually used to
  // guard the push buffer.
  DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;
  auto drop = queue.lockExclusive()->pop();
}

HeapTracer::HeapTracer(v8::Isolate* isolate)
    : v8::EmbedderRootsHandler(
          // Historically V8 would call IsRoot() to scan references, and then call ResetRoot()
          // on those where IsRoot() returned false. But later, V8 added the ability to mark a
          // reference "droppable", and it assumes droppable references are not roots. We only
          // want V8 to call ResetRoot() on droppable references, so we can tell it not to bother
          // even calling `IsRoot()` on anything else. See comment about droppable references
          // in Wrappable::attachWrapper() for details.
          v8::EmbedderRootsHandler::RootHandling::kDontQueryEmbedderForAnyReference),
      isolate(isolate) {
  isolate->AddGCPrologueCallback(
      [](v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags, void* data) {
    // We can expect that any freelisted shims will be collected during a major GC, because
    // they are not in use therefore not reachable. We should therefore clear the freelist now,
    // before the trace starts.
    //
    // Note that we cannot simply depend on the destructor of CppgcShim to remove objects from
    // the freelist, because destructors do not actually run at trace time. They may be deferred
    // to run some time after the trace is done. If we accidentally reuse a shim during that
    // time, we'll have a problem as the shim will still be destroyed as it was already
    // determined to be unreachable.
    //
    // We must clear the freelist in the GC prologue, not the epilogue, because when building in
    // ASAN mode, V8 will poison the objects' memory, so our attempt to clear the freelist after
    // the fact will trigger a spurious ASAN failure.
    reinterpret_cast<HeapTracer*>(data)->clearFreelistedShims();
  }, this, v8::GCType::kGCTypeMarkSweepCompact);

  isolate->AddGCEpilogueCallback(
      [](v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags, void* data) {
    auto& self = *reinterpret_cast<HeapTracer*>(data);
    for (Wrappable* wrappable: self.detachLater) {
      wrappable->detachWrapper(true);
    }
    self.detachLater.clear();
  }, this, v8::GCType::kGCTypeAll);
}

void HeapTracer::destroy() {
  DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;
  KJ_DEFER(isolate = nullptr);
}

HeapTracer& HeapTracer::getTracer(v8::Isolate* isolate) {
  return IsolateBase::from(isolate).heapTracer;
}

bool HeapTracer::IsRoot(const v8::TracedReference<v8::Value>& handle) {
  // V8 doesn't actually call this because we passed kDontQueryEmbedderForAnyReference to the
  // EmbedderRootsHandler constructor. V8 will potentially use ResetRoot() only on references that
  // were marked droppable.
  KJ_UNREACHABLE;
}

void HeapTracer::ResetRoot(const v8::TracedReference<v8::Value>& handle) {
  // V8 calls this to tell us when our wrapper can be dropped. See comment about droppable
  // references in Wrappable::attachWrapper() for details.
  auto& wrappable = *reinterpret_cast<Wrappable*>(
      handle.As<v8::Object>()->GetAlignedPointerFromInternalField(
          Wrappable::WRAPPED_OBJECT_FIELD_INDEX));

  // V8 gets angry if we do not EXPLICITLY call `Reset()` on the wrapper. If we merely destroy it
  // (which is what `detachWrapper()` will do) it is not satisfied, and will come back and try to
  // visit the reference again, but it will DCHECK-fail on that second attempt because the
  // reference is in an inconsistent state at that point.
  KJ_ASSERT_NONNULL(wrappable.wrapper).Reset();

  // We don't want to call `detachWrapper()` now because it may create new handles (specifically,
  // if the wrappable has strong references, which means that its outgoing references need to be
  // upgraded to strong).
  detachLater.add(&wrappable);
}

bool HeapTracer::TryResetRoot(const v8::TracedReference<v8::Value>& handle) {
  // This method is potentially called on a separate thread. Our ResetRoot() implementation,
  // though, only works on the main thread. Return false to request V8 schedule the call for the
  // main thread later on.
  return false;
}

namespace {
  static v8::Isolate* newIsolate(
      V8PlatformWrapper* system,
      v8::Isolate::CreateParams&& params) {
    return jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) -> v8::Isolate* {
      v8::CppHeapCreateParams heapParams {
        {},
        v8::WrapperDescriptor(
            Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
            Wrappable::CPPGC_SHIM_FIELD_INDEX,
            Wrappable::WRAPPABLE_TAG)
      };
      heapParams.marking_support = cppgc::Heap::MarkingType::kAtomic;
      heapParams.sweeping_support = cppgc::Heap::SweepingType::kAtomic;
      // We currently don't attempt to support incremental marking or sweeping. We probably could
      // support them, but it will take some careful investigation and testing. It's not clear if
      // this would be a win anyway, since Worker heaps are relatively small and therefore doing a
      // full atomic mark-sweep usually doesn't require much of a pause.
      //
      // We probably won't ever support concurrent marking or sweeping because concurrent GC is
      // only expected to be a win if there are idle CPU cores available. Workers normally run on
      // servers that are handling many requests at once, thus it's expected CPU cores will be
      // fully utilized. This differs from browser environments, where a user is typically doing
      // only one thing at a time and thus likely has CPU cores to spare.

      // v8 takes ownership of the v8::CppHeap when passed this way.
      params.cpp_heap = v8::CppHeap::Create(system, heapParams).release();

      if (params.array_buffer_allocator == nullptr &&
          params.array_buffer_allocator_shared == nullptr) {
        params.array_buffer_allocator_shared = std::shared_ptr<v8::ArrayBuffer::Allocator>(
            v8::ArrayBuffer::Allocator::NewDefaultAllocator());
      }
      return v8::Isolate::New(params);
    });
  }
}

IsolateBase::IsolateBase(const V8System& system, v8::Isolate::CreateParams&& createParams,
                         kj::Own<IsolateObserver> observer)
    : system(system),
      ptr(newIsolate(const_cast<V8PlatformWrapper*>(&system.platformWrapper),
                     kj::mv(createParams))),
      heapTracer(ptr),
      observer(kj::mv(observer)) {
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    ptr->SetEmbedderRootsHandler(&heapTracer);

    ptr->SetFatalErrorHandler(&fatalError);
    ptr->SetOOMErrorHandler(&oomError);

    ptr->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
    ptr->SetData(0, this);

    ptr->SetModifyCodeGenerationFromStringsCallback(&modifyCodeGenCallback);
    ptr->SetAllowWasmCodeGenerationCallback(&allowWasmCallback);

    // We don't support SharedArrayBuffer so Atomics.wait() doesn't make sense, and might allow DoS
    // attacks.
    ptr->SetAllowAtomicsWait(false);

    ptr->SetJitCodeEventHandler(v8::kJitCodeEventDefault, &jitCodeEvent);

    // V8 10.5 introduced this API which is used to resolve the promise returned by
    // WebAssembly.compile(). For some reason, the default implementation of the callback does not
    // work -- the promise is never resolved. The only thing the default version does differently
    // is it creates a `MicrotasksScope` with `kDoNotRunMicrotasks`. I do not understand what that
    // is even supposed to do, but it seems related to `MicrotasksPolicy::kScoped`, which we don't
    // use, we use `kExplicit`. Replacing the callback seems to solve the problem?
    ptr->SetWasmAsyncResolvePromiseCallback(
        [](v8::Isolate* isolate, v8::Local<v8::Context> context,
          v8::Local<v8::Promise::Resolver> resolver,
          v8::Local<v8::Value> result, v8::WasmAsyncSuccess success) {
      switch (success) {
        case v8::WasmAsyncSuccess::kSuccess:
          resolver->Resolve(context, result).FromJust();
          break;
        case v8::WasmAsyncSuccess::kFail:
          resolver->Reject(context, result).FromJust();
          break;
      }
    });

    ptr->GetHeapProfiler()->AddBuildEmbedderGraphCallback(buildEmbedderGraph, this);

    {
      // We don't need a v8::Locker here since there's no way another thread could be using the
      // isolate yet, but we do need v8::Isolate::Scope.
      v8::Isolate::Scope isolateScope(ptr);
      v8::HandleScope scope(ptr);

      // Create opaqueTemplate
      auto opaqueTemplate = v8::FunctionTemplate::New(ptr, &throwIllegalConstructor);
      opaqueTemplate->InstanceTemplate()->SetInternalFieldCount(Wrappable::INTERNAL_FIELD_COUNT);
      this->opaqueTemplate.Reset(ptr, opaqueTemplate);

      // Create Symbol.dispose and Symbol.asyncDispose.
      symbolDispose.Reset(ptr, v8::Symbol::New(ptr,
          v8::String::NewFromUtf8(ptr, "dispose",
              v8::NewStringType::kInternalized).ToLocalChecked()));
      symbolAsyncDispose.Reset(ptr, v8::Symbol::New(ptr,
          v8::String::NewFromUtf8(ptr, "asyncDispose",
              v8::NewStringType::kInternalized).ToLocalChecked()));
    }
  });
}

IsolateBase::~IsolateBase() noexcept(false) {
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    ptr->Dispose();
  });
}

v8::Local<v8::FunctionTemplate> IsolateBase::getOpaqueTemplate(v8::Isolate* isolate) {
  return reinterpret_cast<IsolateBase*>(isolate->GetData(0))->opaqueTemplate.Get(isolate);
}

void IsolateBase::dropWrappers(kj::Own<void> typeWrapperInstance) {
  // Delete all wrappers.
  jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
    v8::Locker lock(ptr);

    // Make sure everything in the deferred destruction queue is dropped.
    clearDestructionQueue();

    // We MUST call heapTracer.destroy(), but we can't do it yet because destroying other handles
    // may call into the heap tracer.
    KJ_DEFER(heapTracer.destroy());

    // Make sure v8::Globals are destroyed under lock (but not until later).
    KJ_DEFER(symbolAsyncDispose.Reset());
    KJ_DEFER(symbolDispose.Reset());
    KJ_DEFER(opaqueTemplate.Reset());

    // Make sure the TypeWrapper is destroyed under lock by declaring a new copy of the variable
    // that is destroyed before the lock is released.
    kj::Own<void> typeWrapperInstanceInner = kj::mv(typeWrapperInstance);

    // Destroy all wrappers.
    heapTracer.clearWrappers();
  });
}

void IsolateBase::fatalError(const char* location, const char* message) {
  reportV8FatalError(location, message);
}
void IsolateBase::oomError(const char* location, const v8::OOMDetails& oom) {
  kj::StringPtr detailPrefix, detail;
  if (oom.detail != nullptr) {
    detailPrefix = "; detail: "_kj;
    detail = oom.detail;
  }
  auto message = kj::str(
      oom.is_heap_oom ? ": allocation failed: JavaScript heap out of memory"_kj
                      : ": allocation failed: process out of memory"_kj,
      detailPrefix, detail);
  reportV8FatalError(location, message);
}

v8::ModifyCodeGenerationFromStringsResult IsolateBase::modifyCodeGenCallback(
    v8::Local<v8::Context> context, v8::Local<v8::Value> source, bool isCodeLike) {
  v8::Isolate* isolate = context->GetIsolate();
  IsolateBase* self = reinterpret_cast<IsolateBase*>(isolate->GetData(0));
  return { .codegen_allowed = self->evalAllowed, .modified_source = {} };
}

bool IsolateBase::allowWasmCallback(
    v8::Local<v8::Context> context, v8::Local<v8::String> source) {
  // Don't allow WASM unless arbitrary eval() is allowed.
  IsolateBase* self = reinterpret_cast<IsolateBase*>(context->GetIsolate()->GetData(0));
  return self->evalAllowed;
}

void IsolateBase::jitCodeEvent(const v8::JitCodeEvent* event) noexcept {
  // We register this callback with V8 in order to build a mapping of code addresses to source
  // code locations, which we use when reporting stack traces during crashes.

  IsolateBase* self = reinterpret_cast<IsolateBase*>(event->isolate->GetData(0));
  auto& codeMap = self->codeMap;

  // Pointer comparison between pointers not from the same array is UB so we'd better operate on
  // uintptr_t instead.
  uintptr_t startAddr = reinterpret_cast<uintptr_t>(event->code_start);

  struct UserData {
    // The type we'll use in JitCodeEvent::user_data...

    kj::Vector<CodeBlockInfo::PositionMapping> mapping;
  };

  switch (event->type) {
    case v8::JitCodeEvent::CODE_ADDED: {
      // Usually CODE_ADDED comes after CODE_END_LINE_INFO_RECORDING, but sometimes it doesn't,
      // particularly in the case of Wasm where it apperas no line info is provided.
      auto& info = codeMap.findOrCreate(startAddr, [&]() {
        return decltype(self->codeMap)::Entry {
          startAddr, CodeBlockInfo()
        };
      });
      info.size = event->code_len;
      info.name = kj::str(kj::arrayPtr(event->name.str, event->name.len));
      info.type = event->code_type;
      break;
    }

    case v8::JitCodeEvent::CODE_MOVED:
      KJ_IF_SOME(entry, codeMap.findEntry(startAddr)) {
        auto info = kj::mv(entry.value);
        codeMap.erase(entry);
        codeMap.upsert(reinterpret_cast<uintptr_t>(event->new_code_start), kj::mv(info),
            [&](CodeBlockInfo& existing, CodeBlockInfo&& replacement) {
          // It seems sometimes V8 tells us that it "moved" a block to a location that already
          // existed. Why? Who knows? There's no documentation. Let's do the best we can, which is:
          // replace the existing with the new values, unless the new values are not initialized.
          // (E.g. maybe the reason the block already exists is because CODE_ADDED or
          // CODE_END_LINE_INFO_RECORDING was already delivered to the new location for some
          // reason...)
          if (replacement.type != kj::none) {
            existing.size = replacement.size;
            existing.type = replacement.type;
            existing.name = kj::mv(replacement.name);
          }
          if (replacement.mapping != nullptr) {
            existing.mapping = kj::mv(replacement.mapping);
          }
        });
      } else {
        // TODO(someday): Figure out why this triggers. As of v8 10.3 it actually happens in one
        //   of our tests. This API is very undocumented, though, so I'm not sure what I should do.
        //   Change this back to DEBUG_FAIL_PROD_LOG once debugged.
        KJ_LOG(ERROR, "CODE_MOVED for unknown code block?");
      }
      break;

    case v8::JitCodeEvent::CODE_REMOVED:
      if (!codeMap.erase(startAddr)) {
        DEBUG_FATAL_RELEASE_LOG(ERROR, "CODE_REMOVED for unknown code block?");
      }
      break;

    case v8::JitCodeEvent::CODE_ADD_LINE_POS_INFO:
      // V8 reports multiple "position types", POSITION and STATEMENT_POSITION. These are intended
      // to produce two different mappings from instructions to locations. POSITION points to
      // a specific expression while STATEMENT_POSITION only points to the enclosing statement.
      // For our purposes, the former is strictly more useful than the latter, so we ignore
      // STATEMENT_POSITION.
      if (event->line_info.position_type == v8::JitCodeEvent::POSITION) {
        UserData* data = reinterpret_cast<UserData*>(event->user_data);
        data->mapping.add(CodeBlockInfo::PositionMapping {
          static_cast<uint>(event->line_info.offset),
          static_cast<uint>(event->line_info.pos)
        });
      }
      break;

    case v8::JitCodeEvent::CODE_START_LINE_INFO_RECORDING: {
      UserData* data = new UserData();
      data->mapping.reserve(256);

      // Yes we are actually supposed to const_cast the event in order to set the user_data. This
      // is nuts but it's what other users of this interface inside the V8 codebase actually do.
      const_cast<v8::JitCodeEvent*>(event)->user_data = data;
      break;
    }

    case v8::JitCodeEvent::CODE_END_LINE_INFO_RECORDING: {
      // Sometimes CODE_END_LINE_INFO_RECORDING comes after CODE_ADDED, in particular with
      // modules.
      auto& info = codeMap.findOrCreate(startAddr, [&]() {
        return decltype(self->codeMap)::Entry {
          startAddr, CodeBlockInfo()
        };
      });

      UserData* data = reinterpret_cast<UserData*>(event->user_data);
      info.mapping = data->mapping.releaseAsArray();
      delete data;

      break;
    }
  }
}

#if _WIN32
kj::Maybe<kj::StringPtr> getJsStackTrace(void* ucontext, kj::ArrayPtr<char> scratch) {
  // This function is only called by the internal build which just targets Linux.
  // Windows doesn't provide ucontext, so we'd need to rewrite this function's signature
  // if we were to support it. `v8/src/libsampler/sampler.cc` provides a suitable
  // implementation we could use.
  KJ_UNIMPLEMENTED("getJsStackTrace() is not implemented on Windows");
}
#else
kj::Maybe<kj::StringPtr> getJsStackTrace(void* ucontext, kj::ArrayPtr<char> scratch) {
  if (!v8Initialized) {
    return kj::none;
  }
  v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
  if (isolate == nullptr) {
    return kj::none;
  }

  char* pos = scratch.begin();
  char* limit = scratch.end() - 1;
  auto appendText = [&](const auto&... params) {
    pos = kj::_::fillLimited(pos, limit, kj::toCharSequence(params)...);
  };

  v8::RegisterState state;
  auto& mcontext = reinterpret_cast<ucontext_t*>(ucontext)->uc_mcontext;
#if defined(__APPLE__) && defined(__x86_64__)
  state.pc = reinterpret_cast<void*>(mcontext->__ss.__rip);
  state.sp = reinterpret_cast<void*>(mcontext->__ss.__rsp);
  state.fp = reinterpret_cast<void*>(mcontext->__ss.__rbp);
#elif defined(__APPLE__) && defined(__aarch64__)
  state.pc = reinterpret_cast<void*>(arm_thread_state64_get_pc(mcontext->__ss));
  state.sp = reinterpret_cast<void*>(arm_thread_state64_get_sp(mcontext->__ss));
  state.fp = reinterpret_cast<void*>(arm_thread_state64_get_fp(mcontext->__ss));
#elif defined(__linux__) && defined(__x86_64__)
  state.pc = reinterpret_cast<void*>(mcontext.gregs[REG_RIP]);
  state.sp = reinterpret_cast<void*>(mcontext.gregs[REG_RSP]);
  state.fp = reinterpret_cast<void*>(mcontext.gregs[REG_RBP]);
#elif defined(__linux__) && defined(__aarch64__)
  state.pc = reinterpret_cast<void*>(mcontext.pc);
  state.sp = reinterpret_cast<void*>(mcontext.sp);
  state.fp = reinterpret_cast<void*>(mcontext.regs[29]);
  state.lr = reinterpret_cast<void*>(mcontext.regs[30]);
#else
  #error "Please add architecture support. See FillRegisterState() in v8/src/libsampler/sampler.cc"
#endif

  v8::SampleInfo sampleInfo;
  void* traceSpace[32];
  isolate->GetStackSample(state, traceSpace, kj::size(traceSpace), &sampleInfo);

  kj::StringPtr vmState = "??";
  switch (sampleInfo.vm_state) {
    case v8::StateTag::JS:                vmState = "js"; break;
    case v8::StateTag::GC:                vmState = "gc"; break;
    case v8::StateTag::PARSER:            vmState = "parser"; break;
    case v8::StateTag::BYTECODE_COMPILER: vmState = "bytecode_compiler"; break;
    case v8::StateTag::COMPILER:          vmState = "compiler"; break;
    case v8::StateTag::OTHER:             vmState = "other"; break;
    case v8::StateTag::EXTERNAL:          vmState = "external"; break;
    case v8::StateTag::ATOMICS_WAIT:      vmState = "atomics_wait"; break;
    case v8::StateTag::IDLE:              vmState = "idle"; break;
  }
  appendText("js: (", vmState, ")");

  auto& codeMap = reinterpret_cast<IsolateBase*>(isolate->GetData(0))->codeMap;

  for (auto i: kj::zeroTo(sampleInfo.frames_count)) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(traceSpace[i]);
    auto range = codeMap.range(0, addr + 1);
    bool matched = false;
    kj::StringPtr prevName = nullptr;
    if (range.begin() != range.end()) {
      auto iter = range.end();
      --iter;
      auto& entry = *iter;
      if (entry.key + entry.value.size > addr) {
        // Yay, a match. Binary search it. We're looking for the first entry that is greater than
        // the target address (then we'll back up one).
        uint offset = addr - entry.key;
        auto& mapping = entry.value.mapping;
        size_t l = 0;
        size_t r = mapping.size();
        while (l < r) {
          size_t mid = (l + r) / 2;
          if (mapping[mid].instructionOffset <= offset) {
            l = mid + 1;
          } else {
            r = mid;
          }
        }

        matched = true;
        appendText(' ');
        if (entry.value.name != prevName) {
          appendText('\'', entry.value.name, '\'');
          prevName = entry.value.name;
        }
        if (l > 0) {
          appendText('@', mapping[l - 1].sourceOffset);
        } else {
          appendText("@?");
        }
      }
    }

    if (!matched) {
      appendText(" @?");
    }
  }

  *pos = '\0';
  return kj::StringPtr(scratch.begin(), pos - scratch.begin());
}
#endif

kj::StringPtr IsolateBase::getUuid() {
  // Lazily create a random UUID for this isolate.
  KJ_IF_SOME(u, uuid) { return u; }
  return uuid.emplace(randomUUID(kj::none));
}

}  // namespace workerd::jsg
