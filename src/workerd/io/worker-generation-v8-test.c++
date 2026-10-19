// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-generation.h"

#include <workerd/jsg/jsg-test.h>

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd::jsg::test {
namespace {

struct GenerationContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(GenerationContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(GenerationIsolate, GenerationContext);

V8System v8System;

void signalGuestStarted(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto& started = *static_cast<kj::MutexGuarded<bool>*>(
      info.Data().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  *started.lockExclusive() = true;
}

kj::Maybe<v8::Local<v8::Context>> installCell(GenerationIsolate::Lock& lock,
    const std::shared_ptr<v8::BackingStore>& backing,
    kj::Maybe<kj::MutexGuarded<bool>&> started = kj::none) {
  auto context = lock.newContext<GenerationContext>().getHandle(lock.v8Isolate);
  v8::Context::Scope contextScope(context);
  auto cell = v8::SharedArrayBuffer::New(lock.v8Isolate, backing);
  bool cellSet = false;
  if (!context->Global()->Set(context, v8Str(lock.v8Isolate, "cell"_kj), cell).To(&cellSet) ||
      !cellSet) {
    return kj::none;
  }
  KJ_IF_SOME(signal, started) {
    auto callback = v8::FunctionTemplate::New(lock.v8Isolate, signalGuestStarted,
        v8::External::New(lock.v8Isolate, &signal, v8::kExternalPointerTypeTagDefault));
    v8::Local<v8::Function> function;
    if (!callback->GetFunction(context).ToLocal(&function)) {
      return kj::none;
    }
    bool startedSet = false;
    if (!context->Global()
             ->Set(context, v8Str(lock.v8Isolate, "started"_kj), function)
             .To(&startedSet) ||
        !startedSet) {
      return kj::none;
    }
  }
  return context;
}

class BackgroundSignal: public kj::AtomicRefcounted {
 public:
  kj::MutexGuarded<bool> ran{false};
};

class SignalTask final: public v8::Task {
 public:
  explicit SignalTask(kj::Own<BackgroundSignal> signal): signal(kj::mv(signal)) {}
  ~SignalTask() noexcept override = default;

  void Run() override {
    *signal->ran.lockExclusive() = true;
  }

 private:
  kj::Own<BackgroundSignal> signal;
};

void proveBackgroundTaskRuns(V8System& system) {
  auto signal = kj::atomicRefcounted<BackgroundSignal>();
  GenerationIsolate isolate(system, kj::heap<IsolateObserver>());
  isolate.runInLockScope([&](GenerationIsolate::Lock& lock) {
    jsg::IsolateBase::from(lock.v8Isolate)
        .scheduleBackgroundTask(
            std::unique_ptr<v8::Task>(new SignalTask(kj::atomicAddRef(*signal))), 0);
  });

  signal->ran.when([](bool ran) { return ran; }, [](bool&) {}, 2 * kj::SECONDS);
  KJ_REQUIRE(*signal->ran.lockShared(), "V8 background task did not run before timeout");
}

class ObserveState final: public kj::AtomicRefcounted {
 public:
  kj::MutexGuarded<bool> guestStarted{false};
  kj::MutexGuarded<bool> guestDone{false};
  kj::MutexGuarded<bool> independentGuardFired{false};
  kj::MutexGuarded<bool> backgroundTaskDone{false};
  kj::MutexGuarded<int32_t> observedValue{-1};
  kj::MutexGuarded<kj::String> guestError{kj::str("")};
  kj::MutexGuarded<kj::String> backgroundError{kj::str("")};
};

void recordError(kj::MutexGuarded<kj::String>& target, kj::StringPtr message) {
  *target.lockExclusive() = kj::str(message);
}

class ObserveCellAndStopTask final: public v8::Task {
 public:
  ObserveCellAndStopTask(std::shared_ptr<v8::BackingStore> backing,
      kj::Own<WorkerGeneration> generation,
      kj::Own<ObserveState> state)
      : backing(kj::mv(backing)),
        generation(kj::mv(generation)),
        state(kj::mv(state)) {}
  ~ObserveCellAndStopTask() noexcept override = default;

  void Run() override {
    try {
      state->guestStarted.when([](bool started) { return started; }, [](bool&) {}, 2 * kj::SECONDS);
      if (!*state->guestStarted.lockShared()) {
        recordError(state->backgroundError, "child script did not start before timeout");
      } else {
        // A background task must not borrow a test-thread isolate: it can outlive the test's
        // bounded wait, so it owns the isolate it uses as well as all state it observes.
        GenerationIsolate parent(v8System, kj::heap<IsolateObserver>());
        parent.runInLockScope([&](GenerationIsolate::Lock& lock) {
          v8::TryCatch catcher(lock.v8Isolate);
          KJ_IF_SOME(context, installCell(lock, backing)) {
            v8::Context::Scope contextScope(context);
            v8::Local<v8::Script> script;
            auto source = v8Str(lock.v8Isolate,
                "const view = new Int32Array(cell);\n"
                "if (Atomics.load(view, 0) !== 41) "
                "throw new Error('shared cell was not visible');\n"
                "if (Atomics.load(view, 2) !== 0) "
                "throw new Error('child already completed');\n"
                "Atomics.store(view, 0, 42);\n"
                "Atomics.load(view, 0) * 100 + Atomics.load(view, 2);\n"_kj);
            if (!v8::Script::Compile(context, source).ToLocal(&script)) {
              recordError(state->backgroundError, "parent script failed to compile");
              return;
            }

            v8::Local<v8::Value> result;
            if (!script->Run(context).ToLocal(&result)) {
              recordError(state->backgroundError,
                  catcher.HasTerminated() ? "parent script terminated" : "parent script threw");
              return;
            }

            int32_t observedValue = -1;
            if (!result->Int32Value(context).To(&observedValue)) {
              recordError(state->backgroundError, "parent script returned no integer result");
              return;
            }
            *state->observedValue.lockExclusive() = observedValue;
          } else {
            recordError(state->backgroundError, "parent context setup failed");
          }
        });
      }
    } catch (kj::Exception&) {
      recordError(state->backgroundError, "background task failed");
    } catch (...) {
      recordError(state->backgroundError, "background task threw");
    }

    generation->requestStop();
    *state->backgroundTaskDone.lockExclusive() = true;
  }

 private:
  std::shared_ptr<v8::BackingStore> backing;
  kj::Own<WorkerGeneration> generation;
  kj::Own<ObserveState> state;
};

KJ_TEST("IsolateBase background tasks run without event-loop progress") {
  proveBackgroundTaskRuns(v8System);
}

KJ_TEST("a parent isolate observes shared Atomics during a stopped child turn") {
  proveBackgroundTaskRuns(v8System);

  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);

  std::shared_ptr<v8::BackingStore> backing;
  kj::Own<WorkerGeneration> generation;
  auto state = kj::atomicRefcounted<ObserveState>();
  bool guestWasTerminated = false;

  {
    GenerationIsolate child(v8System, kj::heap<IsolateObserver>());
    child.runInLockScope([&](GenerationIsolate::Lock& lock) {
      auto* rawIsolate = lock.v8Isolate;
      backing = v8::SharedArrayBuffer::NewBackingStore(rawIsolate, 3 * sizeof(int32_t));
      auto memories = kj::heapArray<WorkerGeneration::Memory>(1);
      memories[0] = WorkerGeneration::Memory{.name = kj::str("cell"), .backing = backing};
      generation = kj::atomicRefcounted<WorkerGeneration>(kj::mv(memories));
      generation->claim();
      generation->attachIsolate(rawIsolate);
      generation->finishStartup();
      auto context = KJ_ASSERT_NONNULL(installCell(lock, backing, state->guestStarted));
      v8::Context::Scope contextScope(context);

      auto task = std::unique_ptr<v8::Task>(new ObserveCellAndStopTask(
          backing, kj::atomicAddRef(*generation), kj::atomicAddRef(*state)));
      jsg::IsolateBase::from(rawIsolate).scheduleBackgroundTask(kj::mv(task), 0);

      {
        // The watchdog is joined before runInLockScope returns and the child isolate can be
        // disposed. It protects this finite CPU loop if the background stop path regresses.
        kj::Thread watchdog([state = kj::atomicAddRef(*state), rawIsolate]() mutable {
          state->guestDone.when([](bool done) { return done; }, [](bool&) {}, 5 * kj::SECONDS);
          if (!*state->guestDone.lockShared()) {
            *state->independentGuardFired.lockExclusive() = true;
            rawIsolate->TerminateExecution();
          }
        });

        v8::TryCatch catcher(lock.v8Isolate);
        v8::Local<v8::Script> script;
        auto source = v8Str(lock.v8Isolate,
            "const view = new Int32Array(cell);\n"
            "Atomics.store(view, 0, 41);\n"
            "started();\n"
            "for (let i = 0; i < 500000000; ++i) Atomics.store(view, 1, i);\n"
            "Atomics.store(view, 2, 1);\n"_kj);
        if (!v8::Script::Compile(context, source).ToLocal(&script)) {
          recordError(state->guestError, "child script failed to compile");
        } else {
          v8::Local<v8::Value> result;
          if (!script->Run(context).ToLocal(&result) && !catcher.HasTerminated()) {
            recordError(state->guestError, "child script threw");
          }
        }
        guestWasTerminated = catcher.HasTerminated();
        *state->guestDone.lockExclusive() = true;
      }
    });

    KJ_EXPECT(guestWasTerminated);
    KJ_EXPECT(*state->guestStarted.lockShared());
    KJ_EXPECT(generation->isStopRequested());
    KJ_EXPECT(!*state->independentGuardFired.lockShared());
    {
      auto guestError = state->guestError.lockShared();
      KJ_EXPECT(guestError->size() == 0, guestError->cStr());
    }
    state->backgroundTaskDone.when([](bool done) { return done; }, [](bool&) {}, 2 * kj::SECONDS);
    KJ_EXPECT(*state->backgroundTaskDone.lockShared());
    {
      auto backgroundError = state->backgroundError.lockShared();
      KJ_EXPECT(backgroundError->size() == 0, backgroundError->cStr());
    }
    KJ_EXPECT(*state->observedValue.lockShared() == 4200);

    generation->detachIsolate();
  }

  generation->isolateDisposed();
  generation->whenTerminated().wait(waitScope);
}

KJ_TEST("worker generation keeps named backing stores ordered and isolated") {
  GenerationIsolate isolate(v8System, kj::heap<IsolateObserver>());
  std::shared_ptr<v8::BackingStore> firstBacking;
  std::shared_ptr<v8::BackingStore> secondBacking;
  std::shared_ptr<v8::BackingStore> successorBacking;
  isolate.runInLockScope([&](GenerationIsolate::Lock& lock) {
    firstBacking = v8::SharedArrayBuffer::NewBackingStore(lock.v8Isolate, 16);
    secondBacking = v8::SharedArrayBuffer::NewBackingStore(lock.v8Isolate, 32);
    successorBacking = v8::SharedArrayBuffer::NewBackingStore(lock.v8Isolate, 16);
  });

  kj::Vector<WorkerGeneration::Memory> cells;
  cells.add(WorkerGeneration::Memory{.name = kj::str("first"), .backing = firstBacking});
  cells.add(WorkerGeneration::Memory{.name = kj::str("second"), .backing = secondBacking});
  auto generation = kj::atomicRefcounted<WorkerGeneration>(cells.releaseAsArray());

  kj::Vector<WorkerGeneration::Memory> successorCells;
  successorCells.add(
      WorkerGeneration::Memory{.name = kj::str("first"), .backing = successorBacking});
  auto successor = kj::atomicRefcounted<WorkerGeneration>(successorCells.releaseAsArray());

  auto memories = generation->getMemories();
  KJ_EXPECT(memories.size() == 2);
  KJ_EXPECT(memories[0].name == "first"_kj);
  KJ_EXPECT(memories[0].backing->ByteLength() == 16);
  KJ_EXPECT(memories[1].name == "second"_kj);
  KJ_EXPECT(memories[1].backing->ByteLength() == 32);
  KJ_EXPECT(memories[0].backing.get() == firstBacking.get());
  KJ_EXPECT(memories[1].backing.get() == secondBacking.get());
  KJ_EXPECT(memories[0].backing.get() != memories[1].backing.get());
  KJ_EXPECT(successor.get() != generation.get());
  KJ_EXPECT(successor->getMemories()[0].backing.get() == successorBacking.get());
  KJ_EXPECT(successor->getMemories()[0].backing.get() != memories[0].backing.get());

  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  generation->requestStop();
  generation->whenTerminated().wait(waitScope);
  successor->claim();
  successor->finishStartup();
  successor->requireRunning();
  KJ_EXPECT_THROW_MESSAGE(
      "jsg.Error: Worker generation can only be claimed once.", successor->claim());
}

}  // namespace
}  // namespace workerd::jsg::test
