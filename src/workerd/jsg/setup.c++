// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "setup.h"
#include <cxxabi.h>
#include "libplatform/libplatform.h"
#include <ucontext.h>

#ifdef WORKERD_ICU_DATA_EMBED
#include <icudata-embed.capnp.h>
#include <unicode/udata.h>
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
    : platform(kj::mv(platformParam)) {
  v8::V8::SetDcheckErrorHandler(&v8DcheckError);

  // Note that v8::V8::SetFlagsFromString() simply ignores flags it doesn't recognize, which means
  // typos don't generate any error. SetFlagsFromCommandLine() has the `remove_flags` option which
  // leaves behind the flags V8 didn't recognize, so we'd like to use that for error checking
  // purposes. Unfortuntaely, the interface is rather awkward, since it assumes you're going to
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
  argv[argc] = nullptr;  // V8 probably doesn't need this but technically argv is NULL-teminated.

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

  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();
  v8Initialized = true;
}

V8System::~V8System() noexcept(false) {
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
}

void V8System::setFatalErrorCallback(FatalErrorCallback* callback) {
  v8FatalErrorCallback = callback;
}

IsolateBase& IsolateBase::from(v8::Isolate* isolate) {
  return *reinterpret_cast<IsolateBase*>(isolate->GetData(0));
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

void HeapTracer::destroy() {
  DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;
  KJ_DEFER(isolate = nullptr);
  isolate->SetEmbedderHeapTracer(nullptr);
  referencesToMarkLater.clear();
  wrappersToTrace.clear();
}

void HeapTracer::mark(TraceableHandle& handle) {
  if (handle.lastMarked == traceId) {
    return;
  }

  // mark() may be called even when we're not currently tracing, in order to inform us that the
  // object has become reachable from a new parent object. We must ensure that the TracedReference
  // is initialized in this case, to prevent the object from being collected during scavenging.
  // Scavenge passes do not actually perform a trace, and instead try to free objects that are
  // known not to be reachable because the only references V8 ever knew about have gone away. So
  // we have to make sure a TracedReference exists to tell V8 "this can't be collected without
  // tracing".

  // Calculate previous trace ID. See TracePrologue() wrap-around logic.
  uint prevTraceId = traceId == 1 ? uint(kj::maxValue) : traceId - 1;

  if (handle.lastMarked == prevTraceId) {
    // Handle should still be valid.
  } else {
    // Handle was probably collected, or hadn't been created yet. (Re)create it.
    v8::HandleScope scope(isolate);
    kj::ctor(handle.tracedRef, isolate, handle.Get(isolate));
  }

  handle.lastMarked = traceId;

  if (inAdvanceTracing) {
    // We actually are in the middle of a trace, so mark the reference for V8.
    RegisterEmbedderReference(handle.tracedRef);
  } else if (inTrace) {
    // A trace is happening but V8 hasn't called AdvanceTracing(), so we can't actually call back
    // to RegisterEmbedderReference() right now as V8 will not like it. But we can't save a pointer
    // to the handle itself to mark later becasue, who knows, maybe it'll be dropped on the C++
    // side in the meantime. So we make a copy TracedReference which we can mark during the next
    // AdvanceTracing(). Awkwardly, this means we don't actually mark `handle.tracedRef`, so it
    // can still be collected, but at least the object it points to cannot be.
    //
    // Note that this code path only ever executes if incremental tracing is enabled, which
    // as of this writing is disabled by default.
    v8::HandleScope scope(isolate);
    auto local = handle.Get(isolate);
    referencesToMarkLater.add(v8::TracedReference<v8::Data>(isolate, local));

    static bool logOnce KJ_UNUSED = ([]() {
      // I can't figure out how to get this code to actually run in tests, but it is apparently
      // happening in production and causing problems. Try to gather some stack traces from
      // production.
      KJ_LOG(ERROR, "mark() called during trace outside of AdvanceTracing?", kj::getStackTrace());
      return true;
    })();
  }
}

void HeapTracer::clearWrappers() {
  while (!wrappers.empty()) {
    wrappers.front().detachWrapper();
  }
}

void HeapTracer::RegisterV8References(
    const std::vector<std::pair<void*, void*>>& internalFields) {
  for (auto [dummy, obj]: internalFields) {
    KJ_ASSERT(dummy == obj);
    wrappersToTrace.add(reinterpret_cast<Wrappable*>(obj));
  }
}

void HeapTracer::TracePrologue(TraceFlags flags) {
  wrappersToTrace.clear();
  if (++traceId == 0) {
    KJ_LOG(ERROR, "trace ID wrapped around!");

    // We probably don't need to crash, because all of our logic only compares trace IDs for
    // equality, not less/greater. Just make sure we don't use ID 0.
    traceId = 1;
  }
  inTrace = true;
}

bool HeapTracer::AdvanceTracing(double deadlineMs) {
  // TODO(perf): Actually honor deadlineMs.

  inAdvanceTracing = true;
  KJ_DEFER(inAdvanceTracing = false);

  auto refs = kj::mv(referencesToMarkLater);
  for (auto& ref: refs) {
    RegisterEmbedderReference(ref);
  }

  auto wrappables = kj::mv(wrappersToTrace);
  for (auto wrappable: wrappables) {
    wrappable->traceFromV8(traceId);
  }

  return false;
}
bool HeapTracer::IsTracingDone() {
  return wrappersToTrace.empty() && referencesToMarkLater.empty();
}

void HeapTracer::TraceEpilogue(TraceSummary* trace_summary) {
  KJ_ASSERT(wrappersToTrace.empty());
  if (!referencesToMarkLater.empty()) {
    KJ_LOG(ERROR, "referencesToMarkLater is not empty at end of trace?");
    referencesToMarkLater.clear();
  }
  inTrace = false;
}

void HeapTracer::EnterFinalPause(EmbedderStackState stackState) {}

void IsolateBase::scavengePrologue(
    v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags) {
  HeapTracer::getTracer(isolate).startScavenge();

  // V8 frequently performs "scavenges" to quickly reclaim memory from short-lived objects. During
  // a scavenge, V8 does *not* invoke our EmbedderHeapTracer. It used to be that we therefore had
  // to iterate over all our weak handles and call MarkActive() on them in order to make sure the
  // scavenger didn't try to collect them. We no longer need to do that because we now use
  // TracedReference for any handle that is traceable, and V8 knows not to scavenge such handles.
  //
  // Note that the scavenge pass can still collect some of our wrapper objects -- specifically, the
  // ones that have only ever been reachable directly from JavaScript (and no longer are), but were
  // never reachable transitively through another C++ object, and therefore never had a
  // TracedReference allocated for them.
}

void IsolateBase::scavengeEpilogue(
    v8::Isolate* isolate, v8::GCType type, v8::GCCallbackFlags flags) {
  HeapTracer::getTracer(isolate).endScavenge();
}

namespace {
  static v8::Isolate* newIsolate(v8::Isolate::CreateParams&& params) {
    if (params.array_buffer_allocator == nullptr &&
        params.array_buffer_allocator_shared == nullptr) {
      params.array_buffer_allocator_shared = std::shared_ptr<v8::ArrayBuffer::Allocator>(
          v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    }
    return v8::Isolate::New(params);
  }
}

IsolateBase::IsolateBase(const V8System& system, v8::Isolate::CreateParams&& createParams)
    : system(system),
      ptr(newIsolate(kj::mv(createParams))),
      heapTracer(ptr) {
  ptr->SetFatalErrorHandler(&fatalError);
  ptr->SetOOMErrorHandler(&oomError);

  ptr->AddGCPrologueCallback(scavengePrologue, v8::kGCTypeScavenge);
  ptr->AddGCEpilogueCallback(scavengeEpilogue, v8::kGCTypeScavenge);

  ptr->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  ptr->SetData(0, this);

  ptr->SetModifyCodeGenerationFromStringsCallback(&modifyCodeGenCallback);
  ptr->SetAllowWasmCodeGenerationCallback(&allowWasmCallback);

  // We don't support SharedArrayBuffer so Atomics.wait() doesn't make sense, and might allow DoS
  // attacks.
  ptr->SetAllowAtomicsWait(false);

  ptr->SetJitCodeEventHandler(v8::kJitCodeEventDefault, &jitCodeEvent);

  // V8 10.5 introduced this API which is used to resolve the promise returned by
  // WebAssembly.compile(). For some reason, the default implemnetation of the callback does not
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

  // Create opaqueTemplate
  {
    v8::HandleScope scope(ptr);
    auto opaqueTemplate = v8::FunctionTemplate::New(ptr, &throwIllegalConstructor);
    opaqueTemplate->InstanceTemplate()->SetInternalFieldCount(2);
    this->opaqueTemplate.Reset(ptr, opaqueTemplate);
  }
}

IsolateBase::~IsolateBase() noexcept(false) {
  KJ_DEFER(ptr->Dispose());
}

v8::Local<v8::FunctionTemplate> IsolateBase::getOpaqueTemplate(v8::Isolate* isolate) {
  return reinterpret_cast<IsolateBase*>(isolate->GetData(0))->opaqueTemplate.Get(isolate);
}

void IsolateBase::dropWrappers(kj::Own<void> typeWrapperInstance) {
  // Delete all wrappers.
  v8::Locker lock(ptr);
  v8::Isolate::Scope scope(ptr);

  // Make sure everything in the deferred destruction queue is dropped.
  clearDestructionQueue();

  // We MUST call heapTracer.destroy(), but we can't do it yet because destroying other handles
  // may call into the heap tracer.
  KJ_DEFER(heapTracer.destroy());

  // Make sure opaqueTemplate is destroyed under lock (but not until later).
  KJ_DEFER(opaqueTemplate.Reset());

  // Make sure the TypeWrapper is destroyed under lock by declaring a new copy of the variable that
  // is destroyed before the lock is released.
  kj::Own<void> typeWrapperInstanceInner = kj::mv(typeWrapperInstance);

  // Destroy all wrappers.
  heapTracer.clearWrappers();
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

#ifdef KJ_DEBUG
#define DEBUG_FAIL_PROD_LOG(...) \
  KJ_FAIL_ASSERT(__VA_ARGS__);
#else
#define DEBUG_FAIL_PROD_LOG(...) \
  KJ_LOG(ERROR, __VA_ARGS__);
#endif

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
      KJ_IF_MAYBE(entry, codeMap.findEntry(startAddr)) {
        auto info = kj::mv(entry->value);
        codeMap.erase(*entry);
        codeMap.upsert(reinterpret_cast<uintptr_t>(event->new_code_start), kj::mv(info),
            [&](CodeBlockInfo& existing, CodeBlockInfo&& replacement) {
          // It seems sometimes V8 tells us that it "moved" a block to a location that already
          // existed. Why? Who knows? There's no documentation. Let's do the best we can, which is:
          // replace the existing with the new values, unless the new values are not initialized.
          // (E.g. maybe the reason the block already exists is because CODE_ADDED or
          // CODE_END_LINE_INFO_RECORDING was already delivered to the new location for some
          // reason...)
          if (replacement.type != nullptr) {
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
        DEBUG_FAIL_PROD_LOG("CODE_REMOVED for unknown code block?");
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

kj::Maybe<kj::StringPtr> getJsStackTrace(void* ucontext, kj::ArrayPtr<char> scratch) {
  if (!v8Initialized) {
    return nullptr;
  }
  v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
  if (isolate == nullptr) {
    return nullptr;
  }

  char* pos = scratch.begin();
  char* limit = scratch.end() - 1;
  auto appendText = [&](const auto&... params) {
    pos = kj::_::fillLimited(pos, limit, kj::toCharSequence(params)...);
  };

  v8::RegisterState state;
  auto& mcontext = reinterpret_cast<ucontext_t*>(ucontext)->uc_mcontext;
#if __x86_64__
  state.pc = reinterpret_cast<void*>(mcontext.gregs[REG_RIP]);
  state.sp = reinterpret_cast<void*>(mcontext.gregs[REG_RSP]);
  state.fp = reinterpret_cast<void*>(mcontext.gregs[REG_RBP]);
#elif __aarch64__
  state.pc = reinterpret_cast<void*>(mcontext.pc);
  state.sp = reinterpret_cast<void*>(mcontext.sp);
  state.fp = reinterpret_cast<void*>(mcontext.regs[29]);
  state.lr = reinterpret_cast<void*>(mcontext.regs[30]);
#else
  #error "Please add architecture support. See FillRegisterState() in v8/src/libsampler/sambler.cc"
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

}  // namespace workerd::jsg
