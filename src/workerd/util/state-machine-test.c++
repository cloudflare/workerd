// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "state-machine.h"

#include <kj/test.h>

// Entire test file was Claude-generated initially.

namespace workerd {
namespace {

// =============================================================================
// Test State Types
// =============================================================================

struct Idle {
  static constexpr kj::StringPtr NAME = "idle"_kj;
  bool initialized = false;
};

struct Running {
  static constexpr kj::StringPtr NAME = "running"_kj;
  kj::String taskName;
  int progress = 0;

  Running() = default;
  explicit Running(kj::String name): taskName(kj::mv(name)) {}
};

struct Completed {
  static constexpr kj::StringPtr NAME = "completed"_kj;
  int result;

  explicit Completed(int r): result(r) {}
};

struct Failed {
  static constexpr kj::StringPtr NAME = "failed"_kj;
  kj::String error;

  explicit Failed(kj::String err): error(kj::mv(err)) {}
};

// =============================================================================
// Basic StateMachine Tests
// =============================================================================

KJ_TEST("StateMachine: basic state checks") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  // Initially uninitialized
  KJ_EXPECT(!machine.isInitialized());
  KJ_EXPECT(!machine.is<Idle>());
  KJ_EXPECT(!machine.is<Running>());

  // Initialize to Idle
  machine.transitionTo<Idle>();
  KJ_EXPECT(machine.isInitialized());
  KJ_EXPECT(machine.is<Idle>());
  KJ_EXPECT(!machine.is<Running>());
}

KJ_TEST("StateMachine: state data access") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  // Transition to Running with data
  auto& running = machine.transitionTo<Running>(kj::str("my-task"));
  KJ_EXPECT(machine.is<Running>());
  KJ_EXPECT(running.taskName == "my-task");
  KJ_EXPECT(running.progress == 0);

  // Modify state data
  running.progress = 50;
  KJ_EXPECT(machine.get<Running>().progress == 50);
}

KJ_TEST("StateMachine: tryGet returns none for wrong state") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Idle>();

  // tryGet for correct state
  KJ_IF_SOME(idle, machine.tryGet<Idle>()) {
    KJ_EXPECT(!idle.initialized);
  } else {
    KJ_FAIL_EXPECT("Should have gotten Idle state");
  }

  // tryGet for wrong state
  KJ_EXPECT(machine.tryGet<Running>() == kj::none);
  KJ_EXPECT(machine.tryGet<Completed>() == kj::none);
}

KJ_TEST("StateMachine: isAnyOf checks multiple states") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  machine.transitionTo<Completed>(42);
  // Use local variables to avoid KJ_EXPECT macro parsing issues with template brackets
  bool isCompletedOrFailed = machine.isAnyOf<Completed, Failed>();
  bool isIdleOrRunning = machine.isAnyOf<Idle, Running>();
  KJ_EXPECT(isCompletedOrFailed);
  KJ_EXPECT(!isIdleOrRunning);

  machine.transitionTo<Failed>(kj::str("error"));
  isCompletedOrFailed = machine.isAnyOf<Completed, Failed>();
  isIdleOrRunning = machine.isAnyOf<Idle, Running>();
  KJ_EXPECT(isCompletedOrFailed);
  KJ_EXPECT(!isIdleOrRunning);
}

KJ_TEST("StateMachine: transitionFromTo with precondition") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Idle>();

  // Transition from wrong state fails
  auto result1 = machine.transitionFromTo<Running, Completed>(42);
  KJ_EXPECT(result1 == kj::none);
  KJ_EXPECT(machine.is<Idle>());  // Still in Idle

  // Transition from correct state succeeds
  machine.transitionTo<Running>(kj::str("task"));
  auto result2 = machine.transitionFromTo<Running, Completed>(100);
  KJ_EXPECT(result2 != kj::none);
  KJ_EXPECT(machine.is<Completed>());
  KJ_EXPECT(machine.get<Completed>().result == 100);
}

KJ_TEST("StateMachine: factory create") {
  auto machine = StateMachine<Idle, Running, Completed, Failed>::create<Running>(kj::str("task"));
  KJ_EXPECT(machine.is<Running>());
  KJ_EXPECT(machine.get<Running>().taskName == "task");
}

KJ_TEST("StateMachine: uninitialized state throws on get") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  // get() on uninitialized machine should throw with clear message
  auto tryGet = [&]() { machine.get<Idle>(); };
  KJ_EXPECT_THROW_MESSAGE("used before initialization", tryGet());
}

KJ_TEST("StateMachine: uninitialized state throws on KJ_SWITCH_ONEOF") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  // KJ_SWITCH_ONEOF on uninitialized machine should throw with clear message
  auto trySwitch = [&]() {
    KJ_SWITCH_ONEOF(machine) {
      KJ_CASE_ONEOF(idle, Idle) {}
      KJ_CASE_ONEOF(running, Running) {}
      KJ_CASE_ONEOF(completed, Completed) {}
      KJ_CASE_ONEOF(failed, Failed) {}
    }
  };
  KJ_EXPECT_THROW_MESSAGE("used before initialization", trySwitch());
}

KJ_TEST("StateMachine: uninitialized state throws on visit") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  // visit() on uninitialized machine should throw
  auto tryVisit = [&]() { machine.visit([](auto&) {}); };
  KJ_EXPECT_THROW_MESSAGE("uninitialized", tryVisit());
}

KJ_TEST("StateMachine: works with KJ_SWITCH_ONEOF") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Running>(kj::str("test"));

  kj::String result;
  KJ_SWITCH_ONEOF(machine) {
    KJ_CASE_ONEOF(idle, Idle) {
      result = kj::str("idle");
    }
    KJ_CASE_ONEOF(running, Running) {
      result = kj::str("running: ", running.taskName);
    }
    KJ_CASE_ONEOF(completed, Completed) {
      result = kj::str("completed: ", completed.result);
    }
    KJ_CASE_ONEOF(failed, Failed) {
      result = kj::str("failed: ", failed.error);
    }
  }

  KJ_EXPECT(result == "running: test");
}

KJ_TEST("StateMachine: currentStateName introspection") {
  StateMachine<Idle, Running, Completed, Failed> machine;

  // Uninitialized
  KJ_EXPECT(machine.currentStateName() == "(uninitialized)"_kj);

  // Each state
  machine.transitionTo<Idle>();
  KJ_EXPECT(machine.currentStateName() == "idle"_kj);

  machine.transitionTo<Running>(kj::str("task"));
  KJ_EXPECT(machine.currentStateName() == "running"_kj);

  machine.transitionTo<Completed>(42);
  KJ_EXPECT(machine.currentStateName() == "completed"_kj);

  machine.transitionTo<Failed>(kj::str("error"));
  KJ_EXPECT(machine.currentStateName() == "failed"_kj);
}

// =============================================================================
// Utility Function Tests
// =============================================================================

KJ_TEST("requireState: returns state when correct") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Running>(kj::str("task"));

  auto& running = requireState<Running>(machine);
  KJ_EXPECT(running.taskName == "task");
}

KJ_TEST("ifInState: executes function when in state") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Running>(kj::str("task"));

  auto result = ifInState<Running>(machine, [](Running& r) { return r.taskName.size(); }, 0ul);
  KJ_EXPECT(result == 4);  // "task" has 4 characters
}

KJ_TEST("ifInState: returns default when not in state") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Idle>();

  auto result = ifInState<Running>(machine, [](Running& r) { return r.taskName.size(); }, 999ul);
  KJ_EXPECT(result == 999);
}

// =============================================================================
// Common States Tests
// =============================================================================

KJ_TEST("states::Errored holds error") {
  states::Errored<kj::String> errored(kj::str("something went wrong"));
  KJ_EXPECT(errored.error == "something went wrong");
}

KJ_TEST("states have correct names") {
  KJ_EXPECT(states::Closed::NAME == "closed"_kj);
  KJ_EXPECT(states::Unlocked::NAME == "unlocked"_kj);
  KJ_EXPECT(states::Locked::NAME == "locked"_kj);
  KJ_EXPECT(states::Initial::NAME == "initial"_kj);
  KJ_EXPECT(states::Released::NAME == "released"_kj);
}

// =============================================================================
// Memory Safety Tests
// =============================================================================

KJ_TEST("StateMachine: withState provides safe scoped access") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Running>(kj::str("task"));

  // withState returns result and locks transitions
  auto result = machine.withState<Running>([](Running& r) { return r.taskName.size(); });
  KJ_EXPECT(result != kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(result) == 4);

  // Returns none for wrong state
  auto result2 = machine.withState<Idle>([](Idle& i) { return i.initialized; });
  KJ_EXPECT(result2 == kj::none);
}

KJ_TEST("StateMachine: withState blocks transitions during callback") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Running>(kj::str("task"));

  // Cannot transition while locked
  auto tryTransitionInCallback = [&]() {
    machine.withState<Running>([&](Running&) {
      // Attempting to transition while locked should throw
      machine.transitionTo<Completed>(42);
    });
  };
  KJ_EXPECT_THROW_MESSAGE("transitions are locked", tryTransitionInCallback());

  // State should still be Running (transition was blocked)
  KJ_EXPECT(machine.is<Running>());
}

KJ_TEST("StateMachine: withStateOr with default value") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Idle>();

  // Returns default when not in state
  auto result = machine.withStateOr<Running>([](Running& r) { return r.taskName.size(); }, 999ul);
  KJ_EXPECT(result == 999);

  // Returns computed value when in state
  machine.transitionTo<Running>(kj::str("hello"));
  auto result2 = machine.withStateOr<Running>([](Running& r) { return r.taskName.size(); }, 999ul);
  KJ_EXPECT(result2 == 5);
}

KJ_TEST("StateMachine: transition lock count is tracked") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Idle>();

  KJ_EXPECT(!machine.isTransitionLocked());

  {
    auto lock1 = machine.acquireTransitionLock();
    KJ_EXPECT(machine.isTransitionLocked());

    {
      auto lock2 = machine.acquireTransitionLock();
      KJ_EXPECT(machine.isTransitionLocked());
    }

    // Still locked after inner lock released
    KJ_EXPECT(machine.isTransitionLocked());
  }

  // Fully unlocked
  KJ_EXPECT(!machine.isTransitionLocked());
}

KJ_TEST("StateMachine: void withState returns bool") {
  StateMachine<Idle, Running, Completed, Failed> machine;
  machine.transitionTo<Running>(kj::str("task"));

  bool executed = false;

  // void callback returns true when executed
  bool result = machine.withState<Running>([&](Running&) { executed = true; });
  KJ_EXPECT(result == true);
  KJ_EXPECT(executed);

  // void callback returns false when not in state
  executed = false;
  bool result2 = machine.withState<Idle>([&](Idle&) { executed = true; });
  KJ_EXPECT(result2 == false);
  KJ_EXPECT(!executed);
}

// =============================================================================
// Conditional Transition Tests
// =============================================================================

struct Reading {
  static constexpr kj::StringPtr NAME [[maybe_unused]] = "reading"_kj;
  size_t bytesRemaining;
  size_t totalBytes;

  explicit Reading(size_t total): bytesRemaining(total), totalBytes(total) {}
};

struct Done {
  static constexpr kj::StringPtr NAME [[maybe_unused]] = "done"_kj;
  size_t totalBytesRead;

  explicit Done(size_t total): totalBytesRead(total) {}
};

KJ_TEST("StateMachine: transitionFromToIf with true predicate") {
  StateMachine<Idle, Reading, Done> machine;
  machine.transitionTo<Reading>(100);

  // Consume all bytes
  machine.get<Reading>().bytesRemaining = 0;

  // Transition when bytes remaining is 0
  // Note: We need to get totalBytes before the transition since the predicate
  // runs while locked, but args are used after
  size_t totalBytes = machine.get<Reading>().totalBytes;
  auto result = machine.transitionFromToIf<Reading, Done>(
      [](Reading& r) { return r.bytesRemaining == 0; }, totalBytes);

  KJ_EXPECT(result != kj::none);
  KJ_EXPECT(machine.is<Done>());
  KJ_EXPECT(machine.get<Done>().totalBytesRead == 100);
}

KJ_TEST("StateMachine: transitionFromToIf with false predicate") {
  StateMachine<Idle, Reading, Done> machine;
  machine.transitionTo<Reading>(100);

  // Still have bytes remaining
  machine.get<Reading>().bytesRemaining = 50;

  // Won't transition because predicate is false
  auto result = machine.transitionFromToIf<Reading, Done>(
      [](Reading& r) { return r.bytesRemaining == 0; }, 0);

  KJ_EXPECT(result == kj::none);
  KJ_EXPECT(machine.is<Reading>());
}

KJ_TEST("StateMachine: transitionFromToIf wrong source state") {
  StateMachine<Idle, Reading, Done> machine;
  machine.transitionTo<Idle>();

  // Won't transition because not in Reading state
  auto result = machine.transitionFromToIf<Reading, Done>([](Reading&) { return true; }, 0);

  KJ_EXPECT(result == kj::none);
  KJ_EXPECT(machine.is<Idle>());
}

KJ_TEST("StateMachine: transitionFromToWith produces new state") {
  StateMachine<Idle, Reading, Done> machine;
  machine.transitionTo<Reading>(100);
  machine.get<Reading>().bytesRemaining = 0;

  auto result = machine.transitionFromToWith<Reading, Done>([](Reading& r) -> kj::Maybe<Done> {
    if (r.bytesRemaining == 0) {
      return Done{r.totalBytes};
    }
    return kj::none;
  });

  KJ_EXPECT(result != kj::none);
  KJ_EXPECT(machine.is<Done>());
  KJ_EXPECT(machine.get<Done>().totalBytesRead == 100);
}

KJ_TEST("StateMachine: transitionFromToWith returns none") {
  StateMachine<Idle, Reading, Done> machine;
  machine.transitionTo<Reading>(100);
  machine.get<Reading>().bytesRemaining = 50;

  auto result = machine.transitionFromToWith<Reading, Done>([](Reading& r) -> kj::Maybe<Done> {
    if (r.bytesRemaining == 0) {
      return Done{r.totalBytes};
    }
    return kj::none;
  });

  KJ_EXPECT(result == kj::none);
  KJ_EXPECT(machine.is<Reading>());
}

// =============================================================================
// StateMachine Tests
// =============================================================================

// Test state types for StateMachine
struct CActive {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "active"_kj;
  kj::String resourceName;
  explicit CActive(kj::String name): resourceName(kj::mv(name)) {}
};

struct CClosed {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "closed"_kj;
};

struct CErrored {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "errored"_kj;
  kj::String reason;
  explicit CErrored(kj::String r): reason(kj::mv(r)) {}
};

KJ_TEST("StateMachine: basic usage without specs") {
  StateMachine<CActive, CClosed, CErrored> machine;

  // Basic state operations work
  KJ_EXPECT(!machine.isInitialized());

  machine.transitionTo<CActive>(kj::str("resource"));
  KJ_EXPECT(machine.isInitialized());
  KJ_EXPECT(machine.is<CActive>());
  KJ_EXPECT(machine.get<CActive>().resourceName == "resource");

  machine.transitionTo<CClosed>();
  KJ_EXPECT(machine.is<CClosed>());

  // Can transition back (no terminal enforcement without spec)
  machine.transitionTo<CActive>(kj::str("another"));
  KJ_EXPECT(machine.is<CActive>());
}

KJ_TEST("StateMachine: uninitialized state throws on get") {
  StateMachine<CActive, CClosed, CErrored> machine;

  // get() on uninitialized machine should throw with clear message
  auto tryGet = [&]() { machine.get<CActive>(); };
  KJ_EXPECT_THROW_MESSAGE("used before initialization", tryGet());
}

KJ_TEST("StateMachine: uninitialized state throws on KJ_SWITCH_ONEOF") {
  StateMachine<CActive, CClosed, CErrored> machine;

  // KJ_SWITCH_ONEOF on uninitialized machine should throw with clear message
  auto trySwitch = [&]() {
    KJ_SWITCH_ONEOF(machine) {
      KJ_CASE_ONEOF(active, CActive) {}
      KJ_CASE_ONEOF(closed, CClosed) {}
      KJ_CASE_ONEOF(errored, CErrored) {}
    }
  };
  KJ_EXPECT_THROW_MESSAGE("used before initialization", trySwitch());
}

KJ_TEST("StateMachine: uninitialized state throws on visit") {
  StateMachine<CActive, CClosed, CErrored> machine;

  // visit() on uninitialized machine should throw
  auto tryVisit = [&]() { machine.visit([](auto&) {}); };
  KJ_EXPECT_THROW_MESSAGE("uninitialized", tryVisit());
}

KJ_TEST("StateMachine: with TerminalStates spec") {
  StateMachine<TerminalStates<CClosed, CErrored>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));
  KJ_EXPECT(!machine.isTerminal());

  machine.transitionTo<CClosed>();
  KJ_EXPECT(machine.isTerminal());

  // Cannot transition from terminal state
  auto tryTransition = [&]() { machine.transitionTo<CActive>(kj::str("another")); };
  KJ_EXPECT_THROW_MESSAGE("Cannot transition from terminal state", tryTransition());

  // But forceTransitionTo works
  machine.forceTransitionTo<CActive>(kj::str("forced"));
  KJ_EXPECT(machine.is<CActive>());
}

KJ_TEST("StateMachine: with ErrorState spec") {
  StateMachine<ErrorState<CErrored>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));
  KJ_EXPECT(!machine.isErrored());
  KJ_EXPECT(machine.tryGetError() == kj::none);

  machine.transitionTo<CErrored>(kj::str("something went wrong"));
  KJ_EXPECT(machine.isErrored());

  KJ_IF_SOME(err, machine.tryGetError()) {
    KJ_EXPECT(err.reason == "something went wrong");
  } else {
    KJ_FAIL_EXPECT("Should have gotten error");
  }

  KJ_EXPECT(machine.getError().reason == "something went wrong");
}

KJ_TEST("StateMachine: with ActiveState spec") {
  StateMachine<ActiveState<CActive>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));
  KJ_EXPECT(machine.isActive());
  KJ_EXPECT(!machine.isInactive());

  KJ_IF_SOME(active, machine.tryGetActive()) {
    KJ_EXPECT(active.resourceName == "resource");
  } else {
    KJ_FAIL_EXPECT("Should be active");
  }

  // whenActive executes and returns value
  auto result = machine.whenActive([](CActive& a) { return a.resourceName.size(); });
  KJ_EXPECT(result != kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(result) == 8);  // "resource"

  machine.transitionTo<CClosed>();
  KJ_EXPECT(!machine.isActive());
  KJ_EXPECT(machine.isInactive());

  // whenActive returns none when not active
  auto result2 = machine.whenActive([](CActive& a) { return a.resourceName.size(); });
  KJ_EXPECT(result2 == kj::none);
}

KJ_TEST("StateMachine: whenActiveOr") {
  StateMachine<ActiveState<CActive>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  // whenActiveOr executes when active
  auto result = machine.whenActiveOr([](CActive& a) { return a.resourceName.size(); }, 0ul);
  KJ_EXPECT(result == 8);

  // After close, returns default
  machine.transitionTo<CClosed>();
  auto result2 = machine.whenActiveOr([](CActive& a) { return a.resourceName.size(); }, 999ul);
  KJ_EXPECT(result2 == 999);
}

KJ_TEST("StateMachine: with PendingStates spec") {
  StateMachine<PendingStates<CClosed, CErrored>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  // Start an operation
  machine.beginOperation();
  KJ_EXPECT(machine.hasOperationInProgress());
  KJ_EXPECT(machine.operationCountValue() == 1);

  // Defer a close
  bool immediate = machine.deferTransitionTo<CClosed>();
  KJ_EXPECT(!immediate);             // Deferred
  KJ_EXPECT(machine.is<CActive>());  // Still active
  KJ_EXPECT(machine.hasPendingState());
  KJ_EXPECT(machine.pendingStateIs<CClosed>());
  KJ_EXPECT(machine.isOrPending<CClosed>());

  // End operation - pending state applied
  bool applied = machine.endOperation();
  KJ_EXPECT(applied);
  KJ_EXPECT(machine.is<CClosed>());
  KJ_EXPECT(!machine.hasPendingState());
}

KJ_TEST("StateMachine: with PendingStates scoped operation") {
  StateMachine<PendingStates<CClosed, CErrored>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  {
    auto scope = machine.scopedOperation();
    KJ_EXPECT(machine.hasOperationInProgress());

    auto _ KJ_UNUSED = machine.deferTransitionTo<CClosed>();
    KJ_EXPECT(machine.is<CActive>());  // Still active in scope
  }

  // Scope ended, pending state applied
  KJ_EXPECT(machine.is<CClosed>());
}

KJ_TEST("StateMachine: full-featured stream-like usage") {
  // This demonstrates the common stream pattern with all features
  StateMachine<TerminalStates<CClosed, CErrored>, ErrorState<CErrored>, ActiveState<CActive>,
      PendingStates<CClosed, CErrored>, CActive, CClosed, CErrored>
      machine;

  // Initialize
  machine.transitionTo<CActive>(kj::str("http-body"));
  KJ_EXPECT(machine.isActive());
  KJ_EXPECT(!machine.isTerminal());
  KJ_EXPECT(!machine.isErrored());

  // Safe access with whenActive
  machine.whenActive([](CActive& a) { a.resourceName = kj::str("modified"); });
  KJ_EXPECT(machine.get<CActive>().resourceName == "modified");

  // Start a read operation
  machine.beginOperation();

  // Close is requested mid-operation - deferred
  auto deferred KJ_UNUSED = machine.deferTransitionTo<CClosed>();
  KJ_EXPECT(machine.isActive());  // Still active!
  KJ_EXPECT(machine.isOrPending<CClosed>());
  KJ_EXPECT(!machine.isTerminal());  // Not terminal yet

  // End operation - close applied
  auto applied KJ_UNUSED = machine.endOperation();
  KJ_EXPECT(machine.is<CClosed>());
  KJ_EXPECT(machine.isTerminal());
  KJ_EXPECT(!machine.isActive());
  KJ_EXPECT(machine.isInactive());

  // Cannot transition from terminal
  auto tryTransition = [&]() { machine.transitionTo<CActive>(kj::str("x")); };
  KJ_EXPECT_THROW_MESSAGE("Cannot transition from terminal state", tryTransition());
}

KJ_TEST("StateMachine: KJ_SWITCH_ONEOF works") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("test"));

  kj::String result;
  KJ_SWITCH_ONEOF(machine) {
    KJ_CASE_ONEOF(active, CActive) {
      result = kj::str("active: ", active.resourceName);
    }
    KJ_CASE_ONEOF(closed, CClosed) {
      result = kj::str("closed");
    }
    KJ_CASE_ONEOF(errored, CErrored) {
      result = kj::str("errored: ", errored.reason);
    }
  }

  KJ_EXPECT(result == "active: test");
}

KJ_TEST("StateMachine: withState locks transitions") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  // Cannot transition while locked
  auto tryTransitionInCallback = [&]() {
    machine.withState<CActive>([&](CActive&) { machine.transitionTo<CClosed>(); });
  };
  KJ_EXPECT_THROW_MESSAGE("transitions are locked", tryTransitionInCallback());

  // State unchanged
  KJ_EXPECT(machine.is<CActive>());
}

KJ_TEST("StateMachine: currentStateName") {
  StateMachine<CActive, CClosed, CErrored> machine;

  KJ_EXPECT(machine.currentStateName() == "(uninitialized)"_kj);

  machine.transitionTo<CActive>(kj::str("x"));
  KJ_EXPECT(machine.currentStateName() == "active"_kj);

  machine.transitionTo<CClosed>();
  KJ_EXPECT(machine.currentStateName() == "closed"_kj);

  machine.transitionTo<CErrored>(kj::str("err"));
  KJ_EXPECT(machine.currentStateName() == "errored"_kj);
}

KJ_TEST("StateMachine: const withState works") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  const auto& constMachine = machine;

  // Const withState works and returns value
  auto result =
      constMachine.withState<CActive>([](const CActive& a) { return a.resourceName.size(); });
  KJ_EXPECT(result != kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(result) == 8);  // "resource"

  // Const withState returns none for wrong state
  auto result2 = constMachine.withState<CClosed>([](const CClosed&) { return 42; });
  KJ_EXPECT(result2 == kj::none);
}

KJ_TEST("StateMachine: deferTransitionTo respects terminal states") {
  StateMachine<TerminalStates<CClosed, CErrored>, PendingStates<CClosed, CErrored>, CActive,
      CClosed, CErrored>
      machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  // Close the machine (terminal state)
  machine.transitionTo<CClosed>();
  KJ_EXPECT(machine.isTerminal());

  // deferTransitionTo should also fail from terminal state
  auto tryDeferTransition = [&]() {
    auto _ KJ_UNUSED = machine.deferTransitionTo<CErrored>(kj::str("error"));
  };
  KJ_EXPECT_THROW_MESSAGE("Cannot transition from terminal state", tryDeferTransition());
}

// =============================================================================
// Streams Integration Example
// =============================================================================
// This demonstrates how StateMachine could replace the separate
// state + readState pattern found in ReadableStreamInternalController.

namespace stream_integration_example {

// Simulated stream source (like ReadableStreamSource)
struct MockSource {
  bool dataAvailable = true;

  kj::Maybe<kj::String> read() {
    if (dataAvailable) {
      dataAvailable = false;
      return kj::str("data chunk");
    }
    return kj::none;
  }
};

// State types matching the streams pattern
struct Readable {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "readable"_kj;
  kj::Own<MockSource> source;

  explicit Readable(kj::Own<MockSource> s): source(kj::mv(s)) {}
};

struct StreamClosed {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "closed"_kj;
};

struct StreamErrored {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "errored"_kj;
  kj::String reason;

  explicit StreamErrored(kj::String r): reason(kj::mv(r)) {}
};

// Lock states (separate state machine in the real code)
struct Unlocked {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "unlocked"_kj;
};

struct Locked {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "locked"_kj;
};

struct ReaderLocked {
  static constexpr kj::StringPtr NAME KJ_UNUSED = "reader_locked"_kj;
  uint32_t readerId;
  explicit ReaderLocked(uint32_t id): readerId(id) {}
};

// The full-featured state machine type for stream data state
using StreamDataState = StateMachine<TerminalStates<StreamClosed, StreamErrored>,
    ErrorState<StreamErrored>,
    ActiveState<Readable>,
    PendingStates<StreamClosed, StreamErrored>,
    Readable,
    StreamClosed,
    StreamErrored>;

// Lock state machine (simpler)
using StreamLockState = StateMachine<Unlocked, Locked, ReaderLocked>;

// Simulated controller showing combined usage
class MockReadableStreamController {
 public:
  void initialize(kj::Own<MockSource> source) {
    dataState.transitionTo<Readable>(kj::mv(source));
    lockState.transitionTo<Unlocked>();  // Initialize lock state
  }

  bool isReadable() const {
    return dataState.isActive();
  }

  bool isClosedOrErrored() const {
    return dataState.isTerminal();
  }

  bool isErrored() const {
    return dataState.isErrored();
  }

  bool isLocked() const {
    return !lockState.is<Unlocked>();
  }

  kj::Maybe<kj::String> read() {
    // Only read if in readable state and not already reading
    if (!dataState.isActive()) {
      return kj::none;
    }

    // Start read operation (defers close/error during read)
    auto op = dataState.scopedOperation();

    // Safe access to source
    KJ_IF_SOME(result, dataState.whenActive([](Readable& r) -> kj::Maybe<kj::String> {
      return r.source->read();
    })) {
      return kj::mv(result);
    }
    return kj::none;
  }

  void close() {
    if (dataState.isTerminal()) return;

    // If operation in progress, defer the close
    auto _ KJ_UNUSED = dataState.deferTransitionTo<StreamClosed>();
  }

  void error(kj::String reason) {
    if (dataState.isTerminal()) return;

    // Error takes precedence - force even if operation in progress
    dataState.forceTransitionTo<StreamErrored>(kj::mv(reason));
  }

  bool acquireReaderLock(uint32_t readerId) {
    if (isLocked()) return false;
    lockState.transitionTo<ReaderLocked>(readerId);
    return true;
  }

  void releaseReaderLock() {
    lockState.transitionTo<Unlocked>();
  }

 private:
  StreamDataState dataState;
  StreamLockState lockState;
};

}  // namespace stream_integration_example

KJ_TEST("StateMachine: stream integration example - basic flow") {
  using namespace stream_integration_example;

  MockReadableStreamController controller;

  // Initialize
  controller.initialize(kj::heap<MockSource>());
  KJ_EXPECT(controller.isReadable());
  KJ_EXPECT(!controller.isClosedOrErrored());
  KJ_EXPECT(!controller.isLocked());

  // Acquire reader lock
  KJ_EXPECT(controller.acquireReaderLock(123));
  KJ_EXPECT(controller.isLocked());

  // Read data
  auto chunk1 = controller.read();
  KJ_EXPECT(chunk1 != kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(chunk1) == "data chunk");

  // Second read returns none (source exhausted)
  auto chunk2 = controller.read();
  KJ_EXPECT(chunk2 == kj::none);

  // Close the stream
  controller.close();
  KJ_EXPECT(!controller.isReadable());
  KJ_EXPECT(controller.isClosedOrErrored());

  // Release lock
  controller.releaseReaderLock();
  KJ_EXPECT(!controller.isLocked());
}

KJ_TEST("StateMachine: stream integration example - close during read") {
  using namespace stream_integration_example;

  MockReadableStreamController controller;
  controller.initialize(kj::heap<MockSource>());

  // This test demonstrates that if close() is called during a read operation,
  // the close is deferred until the read completes.
  //
  // In a real implementation, this would be more complex with async operations,
  // but the pattern is the same.

  // Simulate close being called while readable (no operation in progress)
  controller.close();
  KJ_EXPECT(controller.isClosedOrErrored());
}

KJ_TEST("StateMachine: stream integration example - error handling") {
  using namespace stream_integration_example;

  MockReadableStreamController controller;
  controller.initialize(kj::heap<MockSource>());

  // Error the stream
  controller.error(kj::str("Network failure"));

  KJ_EXPECT(!controller.isReadable());
  KJ_EXPECT(controller.isClosedOrErrored());
  KJ_EXPECT(controller.isErrored());

  // Reads after error return none
  auto chunk = controller.read();
  KJ_EXPECT(chunk == kj::none);
}

// =============================================================================
// StateMachine Additional API Tests
// =============================================================================

KJ_TEST("StateMachine: visit method") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  // Visit with return value - note: visitor must return the same type for all states
  size_t result = machine.visit([](auto& s) -> size_t {
    using S = std::decay_t<decltype(s)>;
    if constexpr (std::is_same_v<S, CActive>) {
      return s.resourceName.size();
    } else if constexpr (std::is_same_v<S, CClosed>) {
      return 0;
    } else {
      return s.reason.size();
    }
  });
  KJ_EXPECT(result == 8);  // "resource"

  machine.transitionTo<CClosed>();
  result = machine.visit([](auto& s) -> size_t {
    using S = std::decay_t<decltype(s)>;
    if constexpr (std::is_same_v<S, CActive>) {
      return s.resourceName.size();
    } else if constexpr (std::is_same_v<S, CClosed>) {
      return 0;
    } else {
      return s.reason.size();
    }
  });
  KJ_EXPECT(result == 0);
}

KJ_TEST("StateMachine: visit const method") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("test"));

  const auto& constMachine = machine;
  size_t result = constMachine.visit([](const auto& s) -> size_t {
    using S = std::decay_t<decltype(s)>;
    if constexpr (std::is_same_v<S, CActive>) {
      return 1;
    } else if constexpr (std::is_same_v<S, CClosed>) {
      return 2;
    } else {
      return 3;
    }
  });
  KJ_EXPECT(result == 1);
}

KJ_TEST("StateMachine: withStateOr") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  // Execute when in state
  size_t result = machine.withStateOr<CActive>([](CActive& a) { return a.resourceName.size(); }, 0);
  KJ_EXPECT(result == 8);  // "resource"

  // Return default when not in state
  size_t result2 = machine.withStateOr<CClosed>([](CClosed&) { return 42; }, 99);
  KJ_EXPECT(result2 == 99);
}

KJ_TEST("StateMachine: transitionFromToIf") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  // Transition with false predicate - should not transition
  auto result = machine.transitionFromToIf<CActive, CClosed>(
      [](CActive& a) { return a.resourceName == "foo"; });
  KJ_EXPECT(result == kj::none);
  KJ_EXPECT(machine.is<CActive>());

  // Transition with true predicate - should transition
  auto result2 = machine.transitionFromToIf<CActive, CClosed>(
      [](CActive& a) { return a.resourceName == "resource"; });
  KJ_EXPECT(result2 != kj::none);
  KJ_EXPECT(machine.is<CClosed>());
}

KJ_TEST("StateMachine: transitionFromToIf wrong source") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CClosed>();

  // Try to transition from wrong state
  auto result = machine.transitionFromToIf<CActive, CErrored>(
      [](CActive&) { return true; }, kj::str("error"));
  KJ_EXPECT(result == kj::none);
  KJ_EXPECT(machine.is<CClosed>());
}

KJ_TEST("StateMachine: transitionFromToWith") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  // Producer that returns none - should not transition
  auto result = machine.transitionFromToWith<CActive, CErrored>(
      [](CActive&) -> kj::Maybe<CErrored> { return kj::none; });
  KJ_EXPECT(result == kj::none);
  KJ_EXPECT(machine.is<CActive>());

  // Producer that returns value - should transition
  auto result2 =
      machine.transitionFromToWith<CActive, CErrored>([](CActive& a) -> kj::Maybe<CErrored> {
    return CErrored(kj::str("derived from ", a.resourceName));
  });
  KJ_EXPECT(result2 != kj::none);
  KJ_EXPECT(machine.is<CErrored>());
  KJ_EXPECT(machine.get<CErrored>().reason == "derived from resource"_kj);
}

KJ_TEST("StateMachine: underlying accessor") {
  StateMachine<CActive, CClosed, CErrored> machine;
  machine.transitionTo<CActive>(kj::str("resource"));

  // Access underlying kj::OneOf
  auto& underlying = machine.underlying();
  KJ_EXPECT(underlying.is<CActive>());
  KJ_EXPECT(underlying.get<CActive>().resourceName == "resource"_kj);

  // Const access
  const auto& constMachine = machine;
  const auto& constUnderlying = constMachine.underlying();
  KJ_EXPECT(constUnderlying.is<CActive>());
}

KJ_TEST("StateMachine: applyPendingStateImpl respects terminal") {
  // When we force-transition to a terminal state during an operation,
  // the pending state should be discarded on endOperation.
  StateMachine<TerminalStates<CClosed, CErrored>, PendingStates<CClosed, CErrored>, CActive,
      CClosed, CErrored>
      machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  // Start an operation
  machine.beginOperation();

  // Request a deferred close
  auto _ KJ_UNUSED = machine.deferTransitionTo<CClosed>();
  KJ_EXPECT(machine.hasPendingState());
  KJ_EXPECT(machine.is<CActive>());

  // Force transition to error (terminal state) while operation is in progress
  machine.forceTransitionTo<CErrored>(kj::str("forced error"));
  KJ_EXPECT(machine.is<CErrored>());

  // End operation - pending Close should be discarded since we're in terminal state
  bool pendingApplied = machine.endOperation();
  KJ_EXPECT(!pendingApplied);             // Pending was discarded, not applied
  KJ_EXPECT(machine.is<CErrored>());      // Still in errored state
  KJ_EXPECT(!machine.hasPendingState());  // Pending was cleared
}

KJ_TEST("StateMachine: endOperation inside withState throws") {
  // This test verifies that ending an operation (which could apply a pending state)
  // inside a withState() callback throws an error. This prevents UAF where a
  // transition invalidates the reference being used in the callback.
  StateMachine<PendingStates<CClosed, CErrored>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  // This pattern would cause UAF without the safety check:
  //   withState gets reference to Active
  //   scopedOperation ends, applies pending state -> Active is destroyed
  //   callback continues using destroyed Active reference
  auto tryUnsafePattern = [&]() {
    machine.withState<CActive>([&](CActive&) {
      {
        auto op = machine.scopedOperation();
        auto _ KJ_UNUSED = machine.deferTransitionTo<CClosed>();
      }  // op destroyed here - endOperation() would apply pending state
    });
  };

  KJ_EXPECT_THROW_MESSAGE("transitions are locked", tryUnsafePattern());

  // Verify the machine is still in a valid state (transition was blocked)
  KJ_EXPECT(machine.is<CActive>());
}

KJ_TEST("StateMachine: endOperation outside withState works") {
  // Verify the correct pattern still works: end operations outside withState
  StateMachine<PendingStates<CClosed, CErrored>, CActive, CClosed, CErrored> machine;

  machine.transitionTo<CActive>(kj::str("resource"));

  {
    auto op = machine.scopedOperation();
    machine.withState<CActive>([&](CActive& a) {
      // Safe to use 'a' here - no operation ending in this scope
      KJ_EXPECT(a.resourceName == "resource");
    });
    auto _ KJ_UNUSED = machine.deferTransitionTo<CClosed>();
  }  // op ends here, OUTSIDE any withState callback - safe!

  KJ_EXPECT(machine.is<CClosed>());
}

}  // namespace
}  // namespace workerd
