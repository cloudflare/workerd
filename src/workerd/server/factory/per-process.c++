// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "per-process.h"

#include "loop-io.h"

#include <workerd/io/async-trace.h>
#include <workerd/io/release-version.embed.h>
#include <workerd/jsg/setup.h>
#include <workerd/server/v8-platform-impl.h>
#include <workerd/util/autogate.h>
#include <workerd/util/perfetto-tracing.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/use-perfetto-categories.h>

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/mutex.h>

#include <mutex>
#include <thread>

#ifdef WORKERD_USE_TCMALLOC
#include <pthread.h>
#include <tcmalloc/malloc_extension.h>
#endif

namespace workerd::server {
namespace {

// tcmalloc sizes its per-CPU slabs and transfer caches at startup, and only
// ProcessBackgroundActions() grows them toward the size classes that miss. Without that thread,
// the caches keep their startup sizes, and a server's allocation-heavy request loop keeps falling
// through to the central free lists and the page heap. edgeworker runs the same thread
// (setupBackgroundTcmalloc()). ProcessBackgroundActions() never returns, so the thread is detached
// and lives as long as the process. A start that failed and is tried again does not start another.
void startTcmallocBackgroundThread() {
#ifdef WORKERD_USE_TCMALLOC
  static std::once_flag once;
  std::call_once(once, []() {
    if (!tcmalloc::MallocExtension::NeedsProcessBackgroundActions()) return;
    std::thread([]() {
      pthread_setname_np(pthread_self(), "tcmalloc-bg");
      tcmalloc::MallocExtension::ProcessBackgroundActions();
    }).detach();
  });
#endif
}

// tcmalloc samples an allocation about every 2 MiB by default and records its stack for heap
// profiles, but nothing in workerd ever reads those profiles. On x86-64, the stack is walked by
// following frame pointers, and release builds omit them, in Rust as well as C++. A sampled
// allocation on a path that does not keep the frame chain, such as hyper's HTTP parsing, can then
// walk off the end of the stack and crash with SIGSEGV (cloudflare/workerd#8801). Turning sampling
// off removes both the crash and the cost of sampling.
void disableTcmallocHeapSampling() {
#ifdef WORKERD_USE_TCMALLOC
  tcmalloc::MallocExtension::SetProfileSamplingInterval(0);
#endif
}

void initAutogates(kj::ArrayPtr<const kj::String> names) {
  capnp::MallocMessageBuilder message;
  auto gates = message.getOrphanage().newOrphan<capnp::List<capnp::Text>>(names.size());
  for (auto i: kj::indices(names)) {
    gates.get().set(i, names[i]);
  }
  util::Autogate::initAutogate(gates.getReader());
}

// Set once V8 has started in this process.
kj::MutexGuarded<bool> v8Started(false);

}  // namespace

// Members are declared in construction order, which is the reverse of destruction: V8 goes before
// the async trace file and the perfetto session.
class ProcessState::Impl {
 public:
  explicit Impl(const Options& options) {
    KJ_IF_SOME(path, options.perfettoTracePath) {
#ifdef WORKERD_USE_PERFETTO
      perfettoSession = PerfettoSession(path, options.perfettoTraceCategories);
#else
      KJ_UNIMPLEMENTED("perfetto tracing is not supported by this build", path);
#endif
    }
    KJ_IF_SOME(path, options.asyncTracePath) {
      asyncTraceWriter = AsyncTraceWriter::open(path, RELEASE_VERSION);
    }
    TRACE_EVENT("workerd", "Bootstrap()");
    startTcmallocBackgroundThread();
    disableTcmallocHeapSampling();

    initAutogates(options.autogates);
    if (options.benchMode) {
      setBenchMode();
    }
    if (options.infoLogging) {
      kj::_::Debug::setLogLevel(kj::LogSeverity::INFO);
    }
    if (options.predictable) {
      setPredictableModeForTest();
    }
    if (options.gcStress) {
      setGcStressModeForTest();
    }
    if (options.allAutogates) {
      util::Autogate::initAllAutogates();
    }

    platform = jsg::defaultPlatform(0);
    v8Platform = kj::heap<WorkerdPlatform>(*platform);
    v8System = kj::heap<jsg::V8System>(
        *v8Platform,
        KJ_MAP(flag, options.v8Flags) -> kj::StringPtr { return flag; }, platform.get());
  }

  // Ends the --async-trace file. Every factory is gone, ending every context, so the exit line
  // follows their ends.
  ~Impl() noexcept(false) {
    KJ_IF_SOME(writer, asyncTraceWriter) {
      writer->finish();
    }
  }

  KJ_DISALLOW_COPY_AND_MOVE(Impl);

#ifdef WORKERD_USE_PERFETTO
  kj::Maybe<PerfettoSession> perfettoSession;
#endif
  kj::Maybe<kj::Own<AsyncTraceWriter>> asyncTraceWriter;
  kj::Own<v8::Platform> platform;
  kj::Own<WorkerdPlatform> v8Platform;
  kj::Own<jsg::V8System> v8System;
};

kj::Arc<ProcessState> ProcessState::start(const Options& options) {
  auto started = v8Started.lockExclusive();
  KJ_REQUIRE(!*started, "the process is set up once: V8 cannot start again");
  auto state = kj::arc<ProcessState>(kj::heap<Impl>(options));
  *started = true;
  return state;
}

ProcessState::ProcessState(kj::Own<Impl> implParam)
    : impl(kj::mv(implParam)),
      v8System(*impl->v8System) {}

ProcessState::~ProcessState() noexcept(false) {}

kj::Maybe<const AsyncTraceWriter&> ProcessState::getAsyncTraceWriter() const {
  return impl->asyncTraceWriter.map(
      [](const kj::Own<AsyncTraceWriter>& writer) -> const AsyncTraceWriter& { return *writer; });
}

kj::Own<WorkerFactory> newWorkerFactory(const ProcessState& process,
    kj::Rc<LoopIo> io,
    kj::Own<WorkerFactory::Options> options,
    ::rust::Vec<uint64_t> config) {
  KJ_IF_SOME(writer, process.getAsyncTraceWriter()) {
    options->asyncTraceWriter = writer;
  }
  auto& on = io->get();
  auto factory = kj::heap<WorkerFactory>(process.getV8System(), on.timer, on.monotonicClock,
      on.network, on.entropySource, on.fs, kj::mv(options), kj::mv(config));
  // Destroyed in this order: the factory, its reference to the process, then its I/O's.
  return factory.attach(process.addRef(), kj::mv(io));
}

}  // namespace workerd::server
