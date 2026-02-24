// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// MSVC uses a different attribute name for no_unique_address
#if _MSC_VER
#define WD_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define WD_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// State Machine Abstraction built on kj::OneOf.
// TODO(later): If this proves useful, consider moving it into kj itself as there
// are no workerd-specific dependencies.
//
// Entire implementation was Claude-generated initially.
//
// Most of the detailed doc comments here are largely intended to be used by agents
// and tooling. Human readers may prefer to just skip to the actual code.
//
// This header provides utilities for building type-safe state machines using kj::OneOf.
// It addresses common patterns found throughout the workerd codebase with improvements
// that provide tangible benefits over raw kj::OneOf usage.
//
// =============================================================================
// WHY USE THIS INSTEAD OF RAW kj::OneOf?
// =============================================================================
//
// Throughout workerd, we use kj::OneOf as a state machine to track the lifecycle
// of streams, readers, writers, and other resources. A typical pattern looks like:
//
//   kj::OneOf<Readable, Closed, kj::Exception> state;
//
//   void read() {
//     KJ_SWITCH_ONEOF(state) {
//       KJ_CASE_ONEOF(readable, Readable) {
//         auto data = readable.source->read();  // Get reference to state
//         processData(data);                    // Call some function...
//         readable.source->advance();           // Use reference again - UAF!
//       }
//       KJ_CASE_ONEOF(closed, Closed) { ... }
//       KJ_CASE_ONEOF(err, kj::Exception) { ... }
//     }
//   }
//
// THE PROBLEM: Use-After-Free (UAF) from unsound state-transitions
//
// The `readable` reference points into the kj::OneOf's internal storage. If ANY
// code path between obtaining that reference and using it triggers a state
// transition (even indirectly through callbacks, promise continuations, or
// nested calls), the reference becomes dangling:
//
//   KJ_CASE_ONEOF(readable, Readable) {
//     readable.source->read();    // This might call back into our code...
//                                 // ...which might call close()...
//                                 // ...which does state.init<Closed>()
//     readable.buffer.size();     // UAF! readable is now destroyed
//   }
//
// This is particularly insidious because:
//   1. The bug may not manifest in simple tests
//   2. It depends on complex callback chains that are hard to reason about
//   3. It causes memory corruption that may crash much later
//   4. ASAN/valgrind may not catch it if the memory is quickly reused
//
// HOW StateMachine HELPS:
//
// 1. TRANSITION LOCKING via whenState()/whenActive():
//
//    state.whenState<Readable>([](Readable& r) {
//      r.source->read();         // If this tries to transition...
//      r.buffer.size();          // ...it throws instead of UAF
//    });
//
//    The callback holds a "transition lock" - any attempt to transition the
//    state machine while the lock is held will throw an exception instead of
//    silently corrupting memory. This turns silent UAF into a loud, debuggable
//    failure.
//
// 2. DEFERRED TRANSITIONS for async operations:
//
//    When code legitimately needs to transition during an operation (e.g.,
//    a read discovers EOF and needs to close), use deferred transitions:
//
//    {
//      auto op = state.scopedOperation();
//      state.whenActive([&](Readable& r) {
//        if (r.source->atEof()) {
//          state.deferTransitionTo<Closed>();  // Queued, not immediate
//        }
//      });
//    }  // Transition happens here, after callback completes safely
//
// 3. TERMINAL STATE ENFORCEMENT:
//
//    Once a stream is Closed or Errored, it should never transition back to
//    Readable. Raw kj::OneOf allows this silently:
//
//      state.init<Closed>();
//      state.init<Readable>(...);  // Oops - zombie stream!
//
//    StateMachine with TerminalStates<> will throw if you attempt this,
//    catching the bug immediately.
//
// 4. SEMANTIC HELPERS:
//
//    Instead of: state.is<kj::Exception>() || state.is<Closed>()
//    Write:      state.isTerminal()  or  state.isInactive()
//
//    Instead of: KJ_IF_SOME(e, state.tryGetUnsafe<kj::Exception>()) { ... }
//    Write:      KJ_IF_SOME(e, state.tryGetErrorUnsafe()) { ... }
//
// WHEN TO USE:
//
//   - Simple state tracking: StateMachine<States...> is fine
//   - Resource lifecycle (streams, handles): Use TerminalStates + PendingStates
//   - Migrating existing code: See MIGRATION GUIDE section below
//
// =============================================================================
// STATE MACHINE
// =============================================================================
//
// StateMachine supports composable features via spec types:
//
//   // Simple (no specs)
//   StateMachine<Idle, Running, Done> basic;
//
//   // With terminal state enforcement
//   StateMachine<TerminalStates<Done>, Idle, Running, Done> withTerminal;
//
//   // With error extraction helpers
//   StateMachine<ErrorState<Errored>, Active, Closed, Errored> withError;
//
//   // With deferred transitions
//   StateMachine<PendingStates<Closed, Errored>, Active, Closed, Errored> withDefer;
//
//   // Full-featured (combine any specs)
//   StateMachine<
//       TerminalStates<Closed, Errored>,
//       ErrorState<Errored>,
//       ActiveState<Active>,
//       PendingStates<Closed, Errored>,
//       Active, Closed, Errored
//   > fullyFeatured;
//
// Available spec types:
//   - TerminalStates<Ts...>  - States that cannot be transitioned FROM
//                              Enables: isTerminal()
//   - ErrorState<T>          - Designates the error state type
//                              Enables: isErrored(), tryGetErrorUnsafe(), getErrorUnsafe()
//   - ActiveState<T>         - Designates the active/working state type
//                              Enables: isActive(), isInactive(), whenActive(), whenActiveOr(),
//                                       tryGetActiveUnsafe(), requireActiveUnsafe()
//   - PendingStates<Ts...>   - States that can be deferred during operations
//                              Enables: beginOperation(), endOperation(), deferTransitionTo(), etc.
//
// NAMING CONVENTIONS:
//   - isTerminal()  = current state is in TerminalStates (enforces no outgoing transitions)
//   - isInactive()  = current state is NOT the ActiveState (semantic "done" state)
//
// =============================================================================
// MEMORY SAFETY
// =============================================================================
//
// THREAD SAFETY: State machines are NOT thread-safe. All operations on a
// single state machine instance must be performed from the same thread.
// If you need concurrent access, use external synchronization.
//
// This utility provides protections against common memory safety issues:
//
// 1. TRANSITION LOCKING: The state machine can be locked during callbacks to
//    prevent transitions that would invalidate references:
//
//      machine.whenState<Active>([](Active& a) {
//        // machine.transitionTo<Closed>();  // Would fail - locked!
//        a.resource->read();  // Safe - Active cannot be destroyed
//      });
//
// 2. TRANSITION LOCK ENFORCEMENT: The machine tracks active transition locks
//    and throws if a transition is attempted while locks are held.
//
// 3. SAFE ACCESS PATTERNS: Prefer whenState() and whenActive() over get()
//    to ensure references don't outlive their validity.
//
// UNSAFE PATTERNS TO AVOID:
//
//   // DON'T: Store references from getUnsafe() across transitions
//   Active& active = machine.getUnsafe<Active>();
//   machine.transitionTo<Closed>();  // active is now dangling!
//
//   // DO: Use whenState() for safe scoped access
//   machine.whenState<Active>([](Active& a) {
//     // a is guaranteed valid for the duration of the callback
//   });
//
//   // DON'T: Transition inside a callback (will fail if locked)
//   machine.whenState<Active>([&](Active& a) {
//     machine.transitionTo<Closed>();  // Fails!
//   });
//
//   // DO: Return a value and transition after
//   auto result = machine.whenState<Active>([](Active& a) {
//     return a.computeSomething();
//   });
//   machine.transitionTo<Closed>();
//
// =============================================================================
// QUICK START
// =============================================================================
//
// Define your state types (add NAME for introspection):
//
//   struct Readable {
//     static constexpr kj::StringPtr NAME = "readable"_kj;
//     kj::Own<Source> source;
//   };
//   struct Closed { static constexpr kj::StringPtr NAME = "closed"_kj; };
//   struct Errored {
//     static constexpr kj::StringPtr NAME = "errored"_kj;
//     jsg::Value error;
//   };
//
// Basic state machine with safe access:
//
//   StateMachine<Readable, Closed, Errored> state;
//   state.transitionTo<Readable>(...);
//
//   // RECOMMENDED: Use whenState() for safe scoped access
//   state.whenState<Readable>([](Readable& r) {
//     r.source->read();  // Safe - transitions blocked during callback
//   });
//
//   // Or with a return value
//   auto size = state.whenState<Readable>([](Readable& r) {
//     return r.source->size();
//   });  // Returns kj::Maybe<size_t>
//
// Stream-like state machine (common pattern in workerd):
//
//   StateMachine<
//       TerminalStates<Closed, Errored>,
//       ErrorState<Errored>,
//       ActiveState<Readable>,
//       PendingStates<Closed, Errored>,
//       Readable, Closed, Errored
//   > state;
//
//   state.transitionTo<Readable>(...);
//
//   // Safe access with whenActive()
//   state.whenActive([](Readable& r) {
//     r.source->doSomething();  // Transitions blocked
//   });
//
//   // Error checking
//   if (state.isErrored()) { ... }
//   KJ_IF_SOME(err, state.tryGetErrorUnsafe()) { ... }
//
//   // Deferred transitions during operations
//   state.beginOperation();
//   state.deferTransitionTo<Closed>();  // Deferred until operation ends
//   state.endOperation();               // Now transitions to Closed
//
//   // Terminal enforcement
//   state.transitionTo<Closed>();
//   state.transitionTo<Readable>(...);  // FAILS - can't leave terminal state
//
// =============================================================================
// MIGRATION GUIDE: From kj::OneOf to StateMachine
// =============================================================================
//
// This section describes how to migrate existing kj::OneOf state management
// to use these StateMachine utilities.
//
// STEP 1: Add NAME constants to state types
// -----------------------------------------
// StateMachine provides currentStateName() for debugging. Add NAME to states:
//
//   // Before:
//   struct Closed {};
//
//   // After:
//   struct Closed {
//     static constexpr kj::StringPtr NAME = "Closed"_kj;
//   };
//
// STEP 2: Replace kj::OneOf with appropriate StateMachine
// --------------------------------------------------------
//
//   // Before:
//   kj::OneOf<Closed, Errored, Readable> state;
//
//   // After (basic):
//   StateMachine<Closed, Errored, Readable> state;
//
//   // After (with features):
//   StateMachine<
//       TerminalStates<Closed, Errored>,
//       ErrorState<Errored>,
//       ActiveState<Readable>,
//       Closed, Errored, Readable
//   > state;
//
// STEP 3: Update state assignments to use transitionTo()
// ------------------------------------------------------
//
//   // Before:
//   state = Closed{};
//   state = Errored{kj::mv(error)};
//
//   // After:
//   state.transitionTo<Closed>();
//   state.transitionTo<Errored>(kj::mv(error));
//
// STEP 4: Update state checks
// ---------------------------
//
//   // Before:
//   if (state.is<Closed>() || state.is<Errored>()) { ... }
//   if (state.is<Errored>()) { ... }
//
//   // After (with ActiveState<Readable>):
//   if (state.isInactive()) { ... }  // Not in active state
//
//   // After (with ErrorState<Errored>):
//   if (state.isErrored()) { ... }
//
// STEP 5: Replace unsafe get() with safe access patterns
// ------------------------------------------------------
//
//   // Before (unsafe - reference may dangle if callback transitions):
//   KJ_SWITCH_ONEOF(state) {
//     KJ_CASE_ONEOF(readable, Readable) {
//       readable.source->read();  // May be unsafe
//     }
//   }
//
//   // After (safe - transitions blocked during callback):
//   state.whenActive([](Readable& r) {
//     r.source->read();  // Safe
//   });
//
//   // Or for specific state:
//   state.whenState<Readable>([](Readable& r) {
//     r.source->read();
//   });
//
// STEP 6: Replace manual deferred-transition bookkeeping
// ------------------------------------------------------
// If you have code that tracks pending operations and defers close/error:
//
//   // Before:
//   bool closing = false;
//   int pendingOps = 0;
//
//   void startOp() { pendingOps++; }
//   void endOp() {
//     if (--pendingOps == 0 && closing) doClose();
//   }
//   void close() {
//     if (pendingOps > 0) { closing = true; return; }
//     doClose();
//   }
//
//   // After (with PendingStates<Closed>):
//   void startOp() { state.beginOperation(); }
//   void endOp() { state.endOperation(); }  // Auto-applies pending
//   void close() { state.deferTransitionTo<Closed>(); }
//
//   // Or with RAII:
//   void doWork() {
//     auto op = state.scopedOperation();
//     // ... work ...
//   }  // endOperation() called automatically
//
// STEP 7: Update visitForGc
// -------------------------
//
//   // Before:
//   void visitForGc(jsg::GcVisitor& visitor) {
//     KJ_SWITCH_ONEOF(state) {
//       KJ_CASE_ONEOF(e, Errored) { visitor.visit(e.reason); }
//       // ...
//     }
//   }
//
//   // After:
//   void visitForGc(jsg::GcVisitor& visitor) {
//     state.visitForGc(visitor);  // Visits all GC-able states automatically
//   }
//
// STEP 8: KJ_SWITCH_ONEOF still works
// -----------------------------------
// If you need to keep KJ_SWITCH_ONEOF for complex logic:
//
//   KJ_SWITCH_ONEOF(state.underlying()) {
//     KJ_CASE_ONEOF(r, Readable) { ... }
//     KJ_CASE_ONEOF(c, Closed) { ... }
//     KJ_CASE_ONEOF(e, Errored) { ... }
//   }
//
// Or use the visitor pattern:
//
//   state.visit([](auto& s) {
//     using S = kj::Decay<decltype(s)>;
//     if constexpr (kj::isSameType<S, Readable>()) { ... }
//     else if constexpr (kj::isSameType<S, Closed>()) { ... }
//     else { ... }
//   });
//
// =============================================================================

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/one-of.h>
#include <kj/string.h>

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

namespace workerd {

// =============================================================================
// Type Traits and Helpers
// =============================================================================

namespace _ {  // private

// Helper to check if a type is in a parameter pack
template <typename T, typename... Ts>
inline constexpr bool isOneOf = false;

template <typename T, typename First, typename... Rest>
inline constexpr bool isOneOf<T, First, Rest...> =
    kj::isSameType<T, First>() || isOneOf<T, Rest...>;

// Concept: type has a static NAME member of type kj::StringPtr
template <typename T>
concept HasStateName = requires {
  { T::NAME } -> std::convertible_to<kj::StringPtr>;
};

// Get state name, using NAME if available, otherwise a placeholder
template <typename T>
constexpr kj::StringPtr getStateName() {
  if constexpr (HasStateName<T>) {
    return T::NAME;
  } else {
    return "(unnamed)"_kj;
  }
}

}  // namespace _

// =============================================================================
// Spec Types for Composable Features
// =============================================================================

// Marker type to specify terminal states (cannot transition FROM these)
template <typename... Ts>
struct TerminalStates {
  template <typename T>
  static constexpr bool contains = _::isOneOf<T, Ts...>;

  template <typename Machine>
  static bool isTerminal(const Machine& machine) {
    return (machine.template is<Ts>() || ...);
  }
};

// Marker type to specify the error state (enables isErrored(), tryGetErrorUnsafe(), etc.)
// Note: Error states are implicitly terminal - you cannot transition out of an error state
// using normal transitions. Use forceTransitionTo() if you need to reset from an error.
template <typename T>
struct ErrorState {
  using Type = T;
};

// Marker type to specify the active state (enables isActive(), whenActive(), etc.)
template <typename T>
struct ActiveState {
  using Type = T;
};

// Marker type to specify which states can be pending/deferred
template <typename... Ts>
struct PendingStates {
  template <typename T>
  static constexpr bool contains = _::isOneOf<T, Ts...>;
};

// =============================================================================
// Spec Detection Traits
// =============================================================================

namespace _ {  // private

// Helper to detect template instantiations
template <typename T, template <typename...> class Template>
inline constexpr bool isInstanceOf = false;

template <template <typename...> class Template, typename... Args>
inline constexpr bool isInstanceOf<Template<Args...>, Template> = true;

// Spec detection using template matching
template <typename T>
inline constexpr bool isTerminalStatesSpec = isInstanceOf<T, TerminalStates>;

template <typename T>
inline constexpr bool isErrorStateSpec = isInstanceOf<T, ErrorState>;

template <typename T>
inline constexpr bool isActiveStateSpec = isInstanceOf<T, ActiveState>;

template <typename T>
inline constexpr bool isPendingStatesSpec = isInstanceOf<T, PendingStates>;

// Check if a type is any spec type
template <typename T>
inline constexpr bool isSpec = isTerminalStatesSpec<T> || isErrorStateSpec<T> ||
    isActiveStateSpec<T> || isPendingStatesSpec<T>;

// Filter out specs from a type list, keeping only actual states
template <typename... Ts>
struct FilterStates_;

template <>
struct FilterStates_<> {
  using Type = std::tuple<>;
};

template <typename First, typename... Rest>
struct FilterStates_<First, Rest...> {
  using RestFiltered = FilterStates_<Rest...>::Type;
  using Type = std::conditional_t<isSpec<First>,
      RestFiltered,
      decltype(std::tuple_cat(kj::instance<std::tuple<First>>(), kj::instance<RestFiltered>()))>;
};

template <typename... Ts>
using FilterStates = FilterStates_<Ts...>::Type;

// Convert tuple to kj::OneOf
template <typename Tuple>
struct TupleToOneOf_;

template <typename... Ts>
struct TupleToOneOf_<std::tuple<Ts...>> {
  using Type = kj::OneOf<Ts...>;
};

template <typename Tuple>
using TupleToOneOf = TupleToOneOf_<Tuple>::Type;

// Generic spec finder - finds the first type matching a predicate
template <template <typename> class Pred, typename... Ts>
struct FindSpecWhere {
  using Type = void;  // Not found
};

template <template <typename> class Pred, typename First, typename... Rest>
struct FindSpecWhere<Pred, First, Rest...> {
  using Type =
      std::conditional_t<Pred<First>::value, First, typename FindSpecWhere<Pred, Rest...>::Type>;
};

// Predicate wrappers for each spec type
template <typename T>
struct IsErrorStateSpec {
  static constexpr bool value = isErrorStateSpec<T>;
};
template <typename T>
struct IsActiveStateSpec {
  static constexpr bool value = isActiveStateSpec<T>;
};
template <typename T>
struct IsTerminalStatesSpec {
  static constexpr bool value = isTerminalStatesSpec<T>;
};
template <typename T>
struct IsPendingStatesSpec {
  static constexpr bool value = isPendingStatesSpec<T>;
};

// Convenient aliases for finding each spec type
template <typename... Ts>
using FindErrorStateSpec = FindSpecWhere<IsErrorStateSpec, Ts...>;
template <typename... Ts>
using FindActiveStateSpec = FindSpecWhere<IsActiveStateSpec, Ts...>;
template <typename... Ts>
using FindTerminalStatesSpec = FindSpecWhere<IsTerminalStatesSpec, Ts...>;
template <typename... Ts>
using FindPendingStatesSpec = FindSpecWhere<IsPendingStatesSpec, Ts...>;

// Check if a type is in a tuple (type list)
template <typename T, typename Tuple>
inline constexpr bool isInTuple = false;

template <typename T, typename... Ts>
inline constexpr bool isInTuple<T, std::tuple<Ts...>> = (kj::isSameType<T, Ts>() || ...);

// Placeholder type used when a feature is disabled
// This is needed because C++ doesn't allow references to void
struct PlaceholderType {};

// Empty struct for [[no_unique_address]] optimization
// Unlike char, this can actually be zero-sized when used with [[no_unique_address]]
struct Empty {};

// Helper to extract ::Type from a spec, or PlaceholderType if spec is void
template <typename Spec>
struct ExtractSpecType_ {
  using Type = Spec::Type;
};

template <>
struct ExtractSpecType_<void> {
  using Type = PlaceholderType;
};

template <typename Spec>
using ExtractSpecType = ExtractSpecType_<Spec>::Type;

// Generic spec counter using fold expression
template <template <typename> class Pred, typename... Ts>
inline constexpr size_t countSpecsWhere = ((Pred<Ts>::value ? 1 : 0) + ... + 0);

// Convenient aliases for counting each spec type
template <typename... Ts>
inline constexpr size_t countErrorStateSpecs = countSpecsWhere<IsErrorStateSpec, Ts...>;
template <typename... Ts>
inline constexpr size_t countActiveStateSpecs = countSpecsWhere<IsActiveStateSpec, Ts...>;
template <typename... Ts>
inline constexpr size_t countTerminalStatesSpecs = countSpecsWhere<IsTerminalStatesSpec, Ts...>;
template <typename... Ts>
inline constexpr size_t countPendingStatesSpecs = countSpecsWhere<IsPendingStatesSpec, Ts...>;

// Validate that all types in a TerminalStates spec are actual state types
template <typename StatesTuple, typename... TerminalTs>
struct ValidateTerminalStates {
  static constexpr bool allValid = (isInTuple<TerminalTs, StatesTuple> && ...);
  static_assert(allValid || sizeof...(TerminalTs) == 0,
      "All types in TerminalStates<...> must be actual state types in the state machine");
};

// Validate that all types in a PendingStates spec are actual state types
template <typename StatesTuple, typename... PendingTs>
struct ValidatePendingStates {
  static constexpr bool allValid = (isInTuple<PendingTs, StatesTuple> && ...);
  static_assert(allValid || sizeof...(PendingTs) == 0,
      "All types in PendingStates<...> must be actual state types in the state machine");
};

// Helper to extract types from TerminalStates for validation
template <typename StatesTuple, typename TerminalSpec>
struct ValidateTerminalSpec {
  static constexpr bool valid = true;  // Default: no terminal spec, nothing to validate
};

template <typename StatesTuple, typename... Ts>
struct ValidateTerminalSpec<StatesTuple, TerminalStates<Ts...>> {
  static constexpr bool valid = ValidateTerminalStates<StatesTuple, Ts...>::allValid;
};

// Helper to extract types from PendingStates for validation
template <typename StatesTuple, typename PendingSpec>
struct ValidatePendingSpec {
  static constexpr bool valid = true;  // Default: no pending spec, nothing to validate
};

template <typename StatesTuple, typename... Ts>
struct ValidatePendingSpec<StatesTuple, PendingStates<Ts...>> {
  static constexpr bool valid = ValidatePendingStates<StatesTuple, Ts...>::allValid;
};

}  // namespace _

// =============================================================================
// Transition Lock
// =============================================================================

// RAII guard that prevents state transitions while in scope.
// This is used to ensure references to state data remain valid.
//
// LIFETIME REQUIREMENTS:
// The TransitionLock holds a reference to the state machine. The state machine
// MUST outlive the TransitionLock. Destroying the state machine while a
// TransitionLock exists will result in undefined behavior (use-after-free).
//
// CORRECT USAGE:
//   {
//     auto lock = machine.acquireTransitionLock();
//     // ... use state data safely ...
//   }  // lock destroyed, then machine can be safely destroyed
//
// INCORRECT USAGE:
//   auto lock = machine.acquireTransitionLock();
//   machine = StateMachine{};  // BUG: lock still holds reference to old machine!
//
// TODO(someday): Consider adding tryGet<S>() and get<S>() accessor methods to provide
// safe state access while locked. This would enable patterns like:
//
//   auto lock = state.acquireTransitionLock();
//   KJ_IF_SOME(open, lock.tryGet<Open>()) { ... }
//
// Could also explore a WD_IF_STATE macro for KJ_IF_SOME-style ergonomics. If we add
// accessors here, we may want to support deferred transitions (queued until lock
// release), but this raises design questions about conditional transitions.
//
// The relationship between TransitionLock (for safe state access) and OperationScope
// (for pending operation tracking with deferred transitions) also needs clarification.
template <typename Machine>
class TransitionLock {
 public:
  explicit TransitionLock(Machine& m): machine(m) {
    machine.lockTransitions();
  }

  ~TransitionLock() {
    machine.unlockTransitions();
  }

  KJ_DISALLOW_COPY_AND_MOVE(TransitionLock);

 private:
  Machine& machine;
};

// =============================================================================
// State Name Trait
// =============================================================================

// Add NAME to your state types for introspection support:
//
//   struct Closed {
//     static constexpr kj::StringPtr NAME = "closed"_kj;
//   };
//
//   struct Errored {
//     jsg::Value error;
//     static constexpr kj::StringPtr NAME = "errored"_kj;
//   };

// Forward declaration
template <typename... Args>
class StateMachine;

// =============================================================================
// State Machine
// =============================================================================

// A unified state machine that supports all features via spec types.
// Features are conditionally enabled based on which specs are provided.
//
// Usage:
//   // Simple (no specs)
//   StateMachine<Idle, Running, Done> simple;
//
//   // With terminal states
//   StateMachine<TerminalStates<Done>, Idle, Running, Done> withTerminal;
//
//   // Full-featured (stream pattern)
//   StateMachine<
//       TerminalStates<Closed, Errored>,
//       ErrorState<Errored>,
//       ActiveState<Readable>,
//       PendingStates<Closed, Errored>,
//       Readable, Closed, Errored
//   > stream;
//
// All features from separate classes are available when their spec is provided:
//   - TerminalStates<...> -> isTerminal(), enforces no transitions from terminal
//   - ErrorState<T> -> isErrored(), tryGetErrorUnsafe(), getErrorUnsafe()
//   - ActiveState<T> -> isActive(), isInactive(), whenActive(), tryGetActiveUnsafe()
//   - PendingStates<...> -> beginOperation(), endOperation(), deferTransitionTo(), etc.

template <typename... Args>
class StateMachine {
 public:
  // Extract specs from Args
  using TerminalSpec = _::FindTerminalStatesSpec<Args...>::Type;
  using ErrorSpec = _::FindErrorStateSpec<Args...>::Type;
  using ActiveSpec = _::FindActiveStateSpec<Args...>::Type;
  using PendingSpec = _::FindPendingStatesSpec<Args...>::Type;

  // Filter out specs to get actual states
  using StatesTuple = _::FilterStates<Args...>;
  using StateUnion = _::TupleToOneOf<StatesTuple>;
  static constexpr size_t STATE_COUNT = std::tuple_size_v<StatesTuple>;

  // Feature detection
  static constexpr bool HAS_TERMINAL = !std::is_void_v<TerminalSpec>;
  static constexpr bool HAS_ERROR = !std::is_void_v<ErrorSpec>;
  static constexpr bool HAS_ACTIVE = !std::is_void_v<ActiveSpec>;
  static constexpr bool HAS_PENDING = !std::is_void_v<PendingSpec>;

  // Get the error state type (PlaceholderType if not specified)
  // Uses helper to avoid accessing ::Type on void
  using ErrorStateType = _::ExtractSpecType<ErrorSpec>;
  using ActiveStateType = _::ExtractSpecType<ActiveSpec>;

 private:
  // ==========================================================================
  // Compile-time validation
  // ==========================================================================

  // Detect duplicate specs
  static_assert(_::countTerminalStatesSpecs<Args...> <= 1,
      "Multiple TerminalStates<...> specs provided. Only one is allowed.");
  static_assert(_::countErrorStateSpecs<Args...> <= 1,
      "Multiple ErrorState<...> specs provided. Only one is allowed.");
  static_assert(_::countActiveStateSpecs<Args...> <= 1,
      "Multiple ActiveState<...> specs provided. Only one is allowed.");
  static_assert(_::countPendingStatesSpecs<Args...> <= 1,
      "Multiple PendingStates<...> specs provided. Only one is allowed.");

  // Validate that spec types reference actual states
  static consteval bool validateErrorSpec() {
    if constexpr (HAS_ERROR) {
      static_assert(_::isInTuple<ErrorStateType, StatesTuple>,
          "ErrorState<T> must reference a type that is one of the state machine's states");
    }
    return true;
  }

  static consteval bool validateActiveSpec() {
    if constexpr (HAS_ACTIVE) {
      static_assert(_::isInTuple<ActiveStateType, StatesTuple>,
          "ActiveState<T> must reference a type that is one of the state machine's states");
    }
    return true;
  }

  static consteval bool validateTerminalSpec() {
    if constexpr (HAS_TERMINAL) {
      static_assert(_::ValidateTerminalSpec<StatesTuple, TerminalSpec>::valid,
          "All types in TerminalStates<...> must be actual state types");
    }
    return true;
  }

  static consteval bool validatePendingSpec() {
    if constexpr (HAS_PENDING) {
      static_assert(_::ValidatePendingSpec<StatesTuple, PendingSpec>::valid,
          "All types in PendingStates<...> must be actual state types");
    }
    return true;
  }

  // Force validation at class instantiation time
  static_assert(validateErrorSpec(), "ErrorState validation failed");
  static_assert(validateActiveSpec(), "ActiveState validation failed");
  static_assert(validateTerminalSpec(), "TerminalStates validation failed");
  static_assert(validatePendingSpec(), "PendingStates validation failed");

 public:
  // ==========================================================================
  // Constructors and assignment
  // ==========================================================================

  // Default constructor is private - use StateMachine::create<State>(...) instead.
  // This ensures all state machines are properly initialized.

  // Destructor checks for outstanding locks
  ~StateMachine() {
    KJ_DASSERT(transitionLockCount == 0, "StateMachine destroyed while transition locks are held");
  }

  // Move operations - both source and destination must not have locks held
  StateMachine(StateMachine&& other) noexcept: state(kj::mv(other.state)), transitionLockCount(0) {
    KJ_DASSERT(other.transitionLockCount == 0,
        "Cannot move from StateMachine while transition locks are held");
    if constexpr (HAS_PENDING) {
      operationCount = other.operationCount;
      pendingState = kj::mv(other.pendingState);
      other.operationCount = 0;
    }
  }

  StateMachine& operator=(StateMachine&& other) noexcept {
    KJ_DASSERT(transitionLockCount == 0,
        "Cannot move-assign to StateMachine while transition locks are held");
    KJ_DASSERT(other.transitionLockCount == 0,
        "Cannot move from StateMachine while transition locks are held");
    state = kj::mv(other.state);
    if constexpr (HAS_PENDING) {
      operationCount = other.operationCount;
      pendingState = kj::mv(other.pendingState);
      other.operationCount = 0;
    }
    return *this;
  }

  // State machines are generally not copyable - they're owned by classes
  // that typically aren't copyable either (e.g., stream controllers).
  KJ_DISALLOW_COPY(StateMachine);

  // Factory function for clearer initialization
  template <typename S, typename... TArgs>
  static StateMachine create(TArgs&&... args)
    requires(_::isInTuple<S, StatesTuple>)
  {
    StateMachine m;
    m.state.template init<S>(kj::fwd<TArgs>(args)...);
    return m;
  }

  // ---------------------------------------------------------------------------
  // Core State Queries (always available)
  // ---------------------------------------------------------------------------

  template <typename S>
  bool is() const
    requires(_::isInTuple<S, StatesTuple>)
  {
    return state.template is<S>();
  }

  template <typename... Ss>
  bool isAnyOf() const
    requires((_::isInTuple<Ss, StatesTuple>) && ...)
  {
    return (is<Ss>() || ...);
  }

  // Check if the machine is initialized (not in the null state).
  // Call transitionTo<>() to initialize the state machine.
  bool isInitialized() const {
    return !(state == nullptr);
  }

  // Assert that the machine is initialized, with a clear error message.
  void requireInitialized() const {
    KJ_REQUIRE(isInitialized(),
        "State machine used before initialization. Call transitionTo<InitialState>() first.");
  }

  // ---------------------------------------------------------------------------
  // Core State Access (always available)
  // ---------------------------------------------------------------------------
  //
  // NAMING CONVENTION: Methods with "Unsafe" suffix return raw references to
  // state data without any protection against use-after-free. These references
  // can dangle if a state transition occurs while the reference is held.
  //
  // The "Unsafe" suffix serves as a visual warning at every call site,
  // encouraging developers to:
  //   1. Use safe alternatives (whenState(), whenActive()) when possible
  //   2. Carefully audit code paths that could trigger transitions
  //   3. Keep the reference's lifetime as short as possible
  //
  // Safe alternatives:
  //   - whenState<S>(callback)  - Locks transitions during callback
  //   - whenActive(callback)    - Locks transitions, only runs if active
  //   - acquireTransitionLock() - RAII lock for manual control

  template <typename S>
  S& getUnsafe() KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    requireInitialized();
    KJ_REQUIRE(is<S>(), "State machine is not in the expected state");
    return state.template get<S>();
  }

  template <typename S>
  const S& getUnsafe() const KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    requireInitialized();
    KJ_REQUIRE(is<S>(), "State machine is not in the expected state");
    return state.template get<S>();
  }

  template <typename S>
  kj::Maybe<S&> tryGetUnsafe() KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    return state.template tryGet<S>();
  }

  template <typename S>
  kj::Maybe<const S&> tryGetUnsafe() const KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    return state.template tryGet<S>();
  }

  // ---------------------------------------------------------------------------
  // Transition Locking (always available)
  // ---------------------------------------------------------------------------

  bool isTransitionLocked() const {
    return transitionLockCount > 0;
  }

  void lockTransitions() {
    ++transitionLockCount;
  }

  void unlockTransitions() {
    KJ_DASSERT(transitionLockCount > 0, "Transition lock underflow");
    --transitionLockCount;
  }

  TransitionLock<StateMachine> acquireTransitionLock() {
    return TransitionLock<StateMachine>(*this);
  }

  // ---------------------------------------------------------------------------
  // Safe State Access with Locking (always available)
  // ---------------------------------------------------------------------------

  // Execute a function with the current state, locking transitions.
  // This is the SAFEST way to access state data as it prevents
  // use-after-free by blocking transitions during the callback.
  //
  // Returns the function's result wrapped in Maybe (none if not in state).
  // For void functions, returns true if executed, false if not in state.
  template <typename S, typename Func>
  auto whenState(
      Func&& func) -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<S&>()))>,
                       bool,
                       kj::Maybe<decltype(func(kj::instance<S&>()))>>
    requires(_::isInTuple<S, StatesTuple>)
  {
    if (!is<S>()) {
      if constexpr (std::is_void_v<decltype(func(kj::instance<S&>()))>) {
        return false;
      } else {
        return kj::none;
      }
    }

    auto lock = acquireTransitionLock();
    if constexpr (std::is_void_v<decltype(func(kj::instance<S&>()))>) {
      func(state.template get<S>());
      return true;
    } else {
      return func(state.template get<S>());
    }
  }

  // Const version for read-only access
  template <typename S, typename Func>
  auto whenState(Func&& func) const
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<const S&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<const S&>()))>>
    requires(_::isInTuple<S, StatesTuple>)
  {
    if (!is<S>()) {
      if constexpr (std::is_void_v<decltype(func(kj::instance<const S&>()))>) {
        return false;
      } else {
        return kj::none;
      }
    }

    // Note: We still acquire the lock for consistency, even though const
    // methods shouldn't transition. This catches bugs where someone
    // tries to transition through a captured non-const reference.
    ++transitionLockCount;
    KJ_DEFER(--transitionLockCount);
    if constexpr (std::is_void_v<decltype(func(kj::instance<const S&>()))>) {
      func(state.template get<S>());
      return true;
    } else {
      return func(state.template get<S>());
    }
  }

  // ---------------------------------------------------------------------------
  // Visitor Pattern (always available)
  // ---------------------------------------------------------------------------

  // Visit the current state with a generic lambda.
  // The lambda must be able to accept any state type.
  //
  // Usage:
  //   state.visit([](auto& s) {
  //     // s is a reference to the current state
  //   });
  //
  // Or with explicit type handling:
  //   state.visit([](auto& s) {
  //     using S = kj::Decay<decltype(s)>;
  //     if constexpr (kj::isSameType<S, Readable>()) { ... }
  //   });
  template <typename Visitor>
  decltype(auto) visit(Visitor&& visitor) {
    return visitImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor>
  decltype(auto) visit(Visitor&& visitor) const {
    return visitConstImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  // ---------------------------------------------------------------------------
  // State Transitions (always available, but terminal-aware if spec provided)
  // ---------------------------------------------------------------------------

  template <typename S, typename... TArgs>
  S& transitionTo(TArgs&&... args) KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    requireUnlocked();
    if constexpr (HAS_TERMINAL || HAS_ERROR) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
    }
    if constexpr (HAS_PENDING) {
      clearPendingState();
    }
    return state.template init<S>(kj::fwd<TArgs>(args)...);
  }

  // Force transition bypassing terminal state protection.
  //
  // WARNING: This bypasses terminal state protection! Use sparingly and only
  // for legitimate cleanup/reset scenarios. If you find yourself using this
  // frequently, reconsider whether your state should actually be terminal.
  //
  // Legitimate uses:
  //   - Resetting a state machine for reuse
  //   - Cleanup during destruction
  //   - Test fixtures
  //
  // Suspicious uses (reconsider your design):
  //   - Regular business logic transitions
  //   - "Retry" or "restart" operations
  template <typename S, typename... TArgs>
  S& forceTransitionTo(TArgs&&... args) KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    requireUnlocked();
    if constexpr (HAS_PENDING) {
      clearPendingState();
    }
    return state.template init<S>(kj::fwd<TArgs>(args)...);
  }

  // Conditionally transition from one state to another.
  // If the current state is From, transitions to To and returns a reference to the new state.
  // If the current state is NOT From, does nothing and returns kj::none.
  // This is useful for atomic "check and transition" operations.
  template <typename From, typename To, typename... TArgs>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromTo(TArgs&&... args) KJ_LIFETIMEBOUND
    requires(_::isInTuple<From, StatesTuple>) && (_::isInTuple<To, StatesTuple>)
  {
    requireUnlocked();
    if (!is<From>()) {
      return kj::none;
    }
    if constexpr (HAS_TERMINAL || HAS_ERROR) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
    }
    if constexpr (HAS_PENDING) {
      clearPendingState();
    }
    return state.template init<To>(kj::fwd<TArgs>(args)...);
  }

  // ---------------------------------------------------------------------------
  // State Introspection (always available)
  // ---------------------------------------------------------------------------

  kj::StringPtr currentStateName() const {
    kj::StringPtr result = "(uninitialized)"_kj;
    visitStateNames([&result]<typename S>(const S&) { result = _::getStateName<S>(); });
    return result;
  }

  // ---------------------------------------------------------------------------
  // Terminal State Features (enabled when TerminalStates<...> or ErrorState<T> is provided)
  // ---------------------------------------------------------------------------

  // Check if currently in a terminal state (no further transitions allowed).
  // Note: Error states are implicitly terminal - you cannot transition out of an error state.
  bool isTerminal() const
    requires(HAS_TERMINAL || HAS_ERROR)
  {
    bool terminal = false;
    if constexpr (HAS_TERMINAL) {
      terminal = TerminalSpec::isTerminal(*this);
    }
    if constexpr (HAS_ERROR) {
      terminal = terminal || is<ErrorStateType>();
    }
    return terminal;
  }

  // ---------------------------------------------------------------------------
  // Error State Features (enabled when ErrorState<T> is provided)
  // ---------------------------------------------------------------------------

  // Check if currently in the error state.
  bool isErrored() const
    requires(HAS_ERROR)
  {
    return is<ErrorStateType>();
  }

  // Get the error state if currently errored.
  //
  // WARNING: Returns an UNLOCKED reference - can dangle if the machine transitions.
  kj::Maybe<ErrorStateType&> tryGetErrorUnsafe() KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return tryGetUnsafe<ErrorStateType>();
  }

  kj::Maybe<const ErrorStateType&> tryGetErrorUnsafe() const KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return tryGetUnsafe<ErrorStateType>();
  }

  // Get the error state, asserting we are errored.
  //
  // WARNING: Returns an UNLOCKED reference - can dangle if the machine transitions.
  ErrorStateType& getErrorUnsafe() KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return getUnsafe<ErrorStateType>();
  }

  const ErrorStateType& getErrorUnsafe() const KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return getUnsafe<ErrorStateType>();
  }

  // ---------------------------------------------------------------------------
  // Active State Features (enabled when ActiveState<T> is provided)
  // ---------------------------------------------------------------------------

  // Check if currently in the active state.
  bool isActive() const
    requires(HAS_ACTIVE)
  {
    return is<ActiveStateType>();
  }

  // Returns true if not in the active state (i.e., closed, errored, or any non-active state).
  // Note: This is different from isTerminal() which checks if transitions are blocked.
  bool isInactive() const
    requires(HAS_ACTIVE)
  {
    return !isActive();
  }

  // Get the active state if currently active.
  //
  // WARNING: Returns an UNLOCKED reference - can dangle if the machine transitions.
  // Prefer whenActive() for safe access with locked transitions.
  kj::Maybe<ActiveStateType&> tryGetActiveUnsafe() KJ_LIFETIMEBOUND
    requires(HAS_ACTIVE)
  {
    return tryGetUnsafe<ActiveStateType>();
  }

  kj::Maybe<const ActiveStateType&> tryGetActiveUnsafe() const KJ_LIFETIMEBOUND
    requires(HAS_ACTIVE)
  {
    return tryGetUnsafe<ActiveStateType>();
  }

  // Get the active state, throwing KJ_REQUIRE if not active.
  //
  // WARNING: Returns an UNLOCKED reference - can dangle if the machine transitions.
  ActiveStateType& requireActiveUnsafe(kj::StringPtr message = nullptr) KJ_LIFETIMEBOUND
    requires(HAS_ACTIVE)
  {
    if (message == nullptr) {
      message = "State machine is not in the active state"_kj;
    }
    KJ_REQUIRE(isActive(), message);
    return state.template get<ActiveStateType>();
  }

  const ActiveStateType& requireActiveUnsafe(kj::StringPtr message = nullptr) const KJ_LIFETIMEBOUND
    requires(HAS_ACTIVE)
  {
    if (message == nullptr) {
      message = "State machine is not in the active state"_kj;
    }
    KJ_REQUIRE(isActive(), message);
    return state.template get<ActiveStateType>();
  }

  // Execute a function only if in the active state.
  // LOCKS TRANSITIONS during callback execution to prevent use-after-free.
  // Returns the function's result wrapped in Maybe, or none if not active.
  // For void functions, returns true if executed, false if not active.
  template <typename Func>
  auto whenActive(Func&& func)
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<ActiveStateType&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<ActiveStateType&>()))>>
    requires(HAS_ACTIVE)
  {
    return whenState<ActiveStateType>(kj::fwd<Func>(func));
  }

  template <typename Func>
  auto whenActive(Func&& func) const
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<const ActiveStateType&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<const ActiveStateType&>()))>>
    requires(HAS_ACTIVE)
  {
    return whenState<ActiveStateType>(kj::fwd<Func>(func));
  }

  // Execute a function if active, or return a default value.
  // LOCKS TRANSITIONS during callback execution.
  template <typename Func, typename Default>
  auto whenActiveOr(
      Func&& func, Default&& defaultValue) -> decltype(func(kj::instance<ActiveStateType&>()))
    requires(HAS_ACTIVE)
  {
    if (!isActive()) {
      return kj::fwd<Default>(defaultValue);
    }
    auto lock = acquireTransitionLock();
    return func(state.template get<ActiveStateType>());
  }

  template <typename Func, typename Default>
  auto whenActiveOr(Func&& func,
      Default&& defaultValue) const -> decltype(func(kj::instance<const ActiveStateType&>()))
    requires(HAS_ACTIVE)
  {
    if (!isActive()) {
      return kj::fwd<Default>(defaultValue);
    }
    ++transitionLockCount;
    KJ_DEFER(--transitionLockCount);
    return func(state.template get<ActiveStateType>());
  }

  // ---------------------------------------------------------------------------
  // Pending State Features (enabled when PendingStates<...> is provided)
  // ---------------------------------------------------------------------------
  //
  // RECOMMENDATION: Prefer scopedOperation() RAII guard over manual
  // beginOperation()/endOperation() calls. Manual calls are error-prone:
  //
  //   void badExample() {
  //     machine.beginOperation();
  //     if (condition) return;  // BUG: leaks operation count!
  //     machine.endOperation();
  //   }
  //
  //   void goodExample() {
  //     auto op = machine.scopedOperation();
  //     if (condition) return;  // OK: destructor calls endOperation()
  //   }
  //
  //   void exampleWithEarlyEnd() {
  //     auto op = machine.scopedOperation();
  //     // ... do work ...
  //     if (op.end()) {  // End early and check if transition occurred
  //       // A pending state was applied
  //     }
  //   }  // destructor is now a no-op
  //
  // Manual beginOperation()/endOperation() may still be appropriate when:
  //   - You need different exception handling (e.g., clearPendingState() before endOperation())
  //   - You need to conditionally execute callbacks after the pending state is applied

  // Mark that an operation is starting. While operations are in progress,
  // certain transitions (via deferTransitionTo) will be deferred rather than
  // applied immediately. Prefer scopedOperation() for automatic cleanup.
  void beginOperation()
    requires(HAS_PENDING)
  {
    ++operationCount;
  }

  // Mark that an operation has completed. If no more operations are pending
  // and there's a deferred state transition, it will be applied.
  // Returns true if a pending state was applied.
  // Prefer scopedOperation() for automatic cleanup.
  KJ_WARN_UNUSED_RESULT bool endOperation()
    requires(HAS_PENDING)
  {
    KJ_REQUIRE(operationCount > 0, "endOperation() called without matching beginOperation()");
    --operationCount;

    if (operationCount == 0 && hasPendingState()) {
      applyPendingStateImpl();
      return true;
    }
    return false;
  }

  // Check if any operations are currently in progress.
  bool hasOperationInProgress() const
    requires(HAS_PENDING)
  {
    return operationCount > 0;
  }

  // Check if there's a pending state transition waiting to be applied.
  bool hasPendingState() const
    requires(HAS_PENDING)
  {
    return !(pendingState == nullptr);
  }

  // Check if a specific state is pending.
  template <typename S>
  bool pendingStateIs() const
    requires(HAS_PENDING) && (PendingSpec::template contains<S>)
  {
    return pendingState.template is<S>();
  }

  // Get the pending state if it matches the specified type.
  //
  // WARNING: Returns an UNLOCKED reference - can dangle if the pending state is applied.
  template <typename S>
  kj::Maybe<S&> tryGetPendingStateUnsafe() KJ_LIFETIMEBOUND
    requires(HAS_PENDING) && (PendingSpec::template contains<S>)
  {
    return pendingState.template tryGet<S>();
  }

  template <typename S>
  kj::Maybe<const S&> tryGetPendingStateUnsafe() const KJ_LIFETIMEBOUND
    requires(HAS_PENDING) && (PendingSpec::template contains<S>)
  {
    return pendingState.template tryGet<S>();
  }

  // Clear any pending state without applying it.
  void clearPendingState()
    requires(HAS_PENDING)
  {
    pendingState = StateUnion();
  }

  // Transition to a pending state. If no operation is in progress, the
  // transition happens immediately. Otherwise, it's deferred until all
  // operations complete.
  //
  // Returns true if the transition happened immediately, false if deferred.
  //
  // IMPORTANT: First-wins semantics! If a pending state is already set, this
  // call is SILENTLY IGNORED. The first deferred transition wins:
  //
  //   machine.beginOperation();
  //   machine.deferTransitionTo<Closed>();   // This one wins
  //   machine.deferTransitionTo<Errored>(e); // IGNORED - Closed already pending!
  //   machine.endOperation();                // Transitions to Closed, not Errored
  //
  // If you need error to take precedence over close, you must either:
  //   1. Use forceTransitionTo<Errored>() which bypasses deferral, or
  //   2. Check hasPendingState() before deferring, or
  //   3. Use clearPendingState() first to override
  template <typename S, typename... TArgs>
  KJ_WARN_UNUSED_RESULT bool deferTransitionTo(TArgs&&... args)
    requires(HAS_PENDING) && (PendingSpec::template contains<S>)
  {
    requireUnlocked();

    // Check terminal state if applicable (same as transitionTo)
    if constexpr (HAS_TERMINAL || HAS_ERROR) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
    }

    if (operationCount == 0) {
      // No operation in progress, transition immediately
      state.template init<S>(kj::fwd<TArgs>(args)...);
      return true;
    } else {
      // Operation in progress, defer the transition (first wins)
      if (pendingState == nullptr) {
        pendingState.template init<S>(kj::fwd<TArgs>(args)...);
      }
      return false;
    }
  }

  // Check if the machine is in state S OR has S pending.
  // Useful for "is closed or closing" type checks.
  template <typename S>
  bool isOrPending() const
    requires(HAS_PENDING) && (_::isInTuple<S, StatesTuple>)
  {
    if (is<S>()) {
      return true;
    }
    if constexpr (PendingSpec::template contains<S>) {
      return pendingState.template is<S>();
    }
    return false;
  }

  // Get the name of the pending state (or "(none)" if no pending state).
  kj::StringPtr pendingStateName() const
    requires(HAS_PENDING)
  {
    if (pendingState == nullptr) {
      return "(none)"_kj;
    }
    kj::StringPtr result = "(unknown)"_kj;
    visitPendingStates([&result]<typename S>(const S&) { result = _::getStateName<S>(); });
    return result;
  }

  // RAII guard for operation tracking.
  //
  // EXCEPTION SAFETY: If endOperation() triggers a pending state transition
  // and the state constructor throws, the exception will propagate from the
  // destructor. This is generally acceptable since state machine corruption
  // is unrecoverable, but be aware when using this in exception-sensitive code.
  //
  // TODO(maybe): Currently, OperationScope does not check for transition locks at
  // construction time - it only throws when endOperation() tries to apply a pending
  // state while locks are held. This allows legitimate interleaved patterns like:
  // start operation -> acquire lock -> read state -> release lock -> end operation.
  // However, if TransitionLock and OperationScope become the only public APIs for
  // mutating their respective counts (i.e., beginOperation()/endOperation() and
  // lockTransitions()/unlockTransitions() are made private or removed), it might be
  // reasonable to throw at construction time, making the error easier to diagnose.
  class OperationScope {
   public:
    explicit OperationScope(StateMachine& m): machine(&m) {
      m.beginOperation();
    }

    ~OperationScope() noexcept(false) {
      // Note: endOperation() may throw if pending state constructor throws.
      // We mark this noexcept(false) to be explicit about this.
      KJ_IF_SOME(m, machine) {
        auto applied KJ_UNUSED = m.endOperation();
      }
    }

    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
    OperationScope(OperationScope&&) = delete;
    OperationScope& operator=(OperationScope&&) = delete;

    // End the operation early, returning whether a pending state was applied.
    // After calling end(), the destructor becomes a no-op.
    // Similar to kj::Locked<T>::unlock().
    KJ_WARN_UNUSED_RESULT bool end() {
      KJ_IF_SOME(m, machine) {
        machine = kj::none;
        return m.endOperation();
      }
      return false;
    }

   private:
    kj::Maybe<StateMachine&> machine;
  };

  OperationScope scopedOperation()
    requires(HAS_PENDING)
  {
    return OperationScope(*this);
  }

  // ---------------------------------------------------------------------------
  // GC Visitation Support
  // ---------------------------------------------------------------------------

  // Visit the current state for garbage collection.
  // The visitor should have a visit() method that accepts references to
  // GC-visitable types (like jsg::GcVisitor).
  //
  // Usage in a class with a state machine member:
  //   void visitForGc(jsg::GcVisitor& visitor) {
  //     state.visitForGc(visitor);
  //   }
  //
  // The visitor's visit() method will be called with the current state.
  // If the state type doesn't support GC visitation, the visit() call
  // will be a no-op (assuming the visitor handles non-visitable types).
  template <typename Visitor>
  void visitForGc(Visitor& visitor) {
    visitForGcImpl(visitor, std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor>
  void visitForGc(Visitor& visitor) const {
    visitForGcImpl(visitor, std::make_index_sequence<STATE_COUNT>{});
  }

  // ---------------------------------------------------------------------------
  // Interop (use sparingly - bypasses safety features)
  // ---------------------------------------------------------------------------

  // Access the underlying kj::OneOf for interop with existing code.
  //
  // WARNING: Use this sparingly! The returned reference bypasses ALL safety
  // features of the state machine:
  //   - No transition locking (references can dangle)
  //   - No terminal state enforcement
  //   - No pending state handling
  //   - Modifications won't trigger pending state application
  //
  // This is primarily useful for:
  // - Migrating existing code to use StateMachine
  // - Implementing new patterns that the state machine doesn't support yet
  // - Interfacing with APIs that expect kj::OneOf directly
  //
  // STRONGLY PREFER: whenState(), transitionTo(), and other type-safe methods.
  // TODO(later): Revisit whether these should be kept.
  StateUnion& underlying() KJ_LIFETIMEBOUND {
    return state;
  }
  const StateUnion& underlying() const KJ_LIFETIMEBOUND {
    return state;
  }

  // For use with KJ_SWITCH_ONEOF.
  //
  // WARNING: KJ_SWITCH_ONEOF does NOT acquire a transition lock! References
  // obtained inside KJ_CASE_ONEOF blocks can become dangling if any code
  // in that block triggers a state transition:
  //
  //   KJ_SWITCH_ONEOF(machine) {
  //     KJ_CASE_ONEOF(active, Active) {
  //       someFunction();  // If this transitions machine...
  //       active.foo();    // ...this is UAF!
  //     }
  //   }
  //
  // For safe access, use whenState() instead:
  //
  //   machine.whenState<Active>([](Active& active) {
  //     someFunction();  // If this tries to transition, it throws
  //     active.foo();    // Safe - transitions are locked
  //   });
  auto _switchSubject() & {
    requireInitialized();
    return state._switchSubject();
  }
  auto _switchSubject() const& {
    requireInitialized();
    return state._switchSubject();
  }
  auto _switchSubject() && {
    requireInitialized();
    return kj::mv(state)._switchSubject();
  }

 private:
  // Private default constructor - use create<State>() factory function instead.
  // Making this private ensures state machines are always initialized.
  StateMachine() = default;

  StateUnion state;

  // Counter for detecting illegal transitions from within whenState()/whenActiveOr() callbacks.
  // Marked mutable because const methods use it for internal bookkeeping while not changing
  // the logical state (i.e., which state the machine is in). This class is NOT thread-safe;
  // callers are responsible for synchronization if needed. The const qualifier on methods
  // indicates "does not transition the state machine", not "thread-safe".
  mutable uint32_t transitionLockCount = 0;

  // Pending state support (only allocated when HAS_PENDING is true)
  // Using _::Empty instead of char for proper [[no_unique_address]] optimization
  WD_NO_UNIQUE_ADDRESS std::conditional_t<HAS_PENDING, StateUnion, _::Empty> pendingState{};
  WD_NO_UNIQUE_ADDRESS std::conditional_t<HAS_PENDING, uint32_t, _::Empty> operationCount{};

  void requireUnlocked() const {
    KJ_REQUIRE(transitionLockCount == 0,
        "Cannot transition state machine while transitions are locked. "
        "This usually means you're trying to transition inside a whenState() callback.");
  }

  // Helper for currentStateName()
  template <typename Visitor>
  void visitStateNames(Visitor&& visitor) const {
    visitStateNamesImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor, size_t... Is>
  void visitStateNamesImpl(Visitor&& visitor, std::index_sequence<Is...>) const {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (state.template is<S>()) {
        visitor.template operator()<S>(state.template get<S>());
      }
    };
    (tryVisit.template operator()<Is>(), ...);
  }

  // Helper for visit() - non-const version
  template <typename Visitor, size_t... Is>
  decltype(auto) visitImpl(Visitor&& visitor, std::index_sequence<Is...>) {
    KJ_REQUIRE(isInitialized(), "Cannot visit uninitialized state machine");

    // Use common_type to handle visitors that return compatible but different types
    using ReturnType = std::common_type_t<decltype(visitor(
        kj::instance<std::tuple_element_t<Is, StatesTuple>&>()))...>;

    if constexpr (std::is_void_v<ReturnType>) {
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, StatesTuple>;
        if (state.template is<S>()) {
          visitor(state.template get<S>());
        }
      };
      (tryVisit.template operator()<Is>(), ...);
    } else {
      ReturnType result{};
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, StatesTuple>;
        if (state.template is<S>()) {
          result = visitor(state.template get<S>());
        }
      };
      (tryVisit.template operator()<Is>(), ...);
      return result;
    }
  }

  // Helper for visit() - const version
  template <typename Visitor, size_t... Is>
  decltype(auto) visitConstImpl(Visitor&& visitor, std::index_sequence<Is...>) const {
    KJ_REQUIRE(isInitialized(), "Cannot visit uninitialized state machine");

    using ReturnType = std::common_type_t<decltype(visitor(
        kj::instance<const std::tuple_element_t<Is, StatesTuple>&>()))...>;

    if constexpr (std::is_void_v<ReturnType>) {
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, StatesTuple>;
        if (state.template is<S>()) {
          visitor(state.template get<S>());
        }
      };
      (tryVisit.template operator()<Is>(), ...);
    } else {
      ReturnType result{};
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, StatesTuple>;
        if (state.template is<S>()) {
          result = visitor(state.template get<S>());
        }
      };
      (tryVisit.template operator()<Is>(), ...);
      return result;
    }
  }

  void applyPendingStateImpl()
    requires(HAS_PENDING)
  {
    // Applying a pending state is a transition, so we must not be locked.
    // This prevents UAF when endOperation() is called inside a whenState() callback:
    //
    //   machine.whenState<Active>([&](Active& a) {
    //     {
    //       auto op = machine.scopedOperation();
    //       machine.deferTransitionTo<Closed>();
    //     }  // op destroyed here - would transition while 'a' is still in use!
    //     a.doSomething();  // UAF if transition happened above
    //   });
    //
    // With this check, the above code will throw instead of causing UAF.
    requireUnlocked();

    // Check terminal state if applicable - don't apply pending state if we're
    // already in a terminal state (this can happen if a forceTransitionTo was
    // used to reach a terminal state while an operation was in progress).
    if constexpr (HAS_TERMINAL || HAS_ERROR) {
      if (isTerminal()) {
        // Already in terminal state, discard the pending state
        pendingState = StateUnion();
        return;
      }
    }

    visitPendingStates([this]<typename S>(S& s) { this->state.template init<S>(kj::mv(s)); });
    pendingState = StateUnion();
  }

  template <typename Visitor>
  void visitPendingStates(Visitor&& visitor) const
    requires(HAS_PENDING)
  {
    visitPendingStatesImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor>
  void visitPendingStates(Visitor&& visitor)
    requires(HAS_PENDING)
  {
    visitPendingStatesImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor, size_t... Is>
  void visitPendingStatesImpl(Visitor&& visitor, std::index_sequence<Is...>) const
    requires(HAS_PENDING)
  {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (pendingState.template is<S>()) {
        visitor.template operator()<S>(pendingState.template get<S>());
      }
    };
    (tryVisit.template operator()<Is>(), ...);
  }

  template <typename Visitor, size_t... Is>
  void visitPendingStatesImpl(Visitor&& visitor, std::index_sequence<Is...>)
    requires(HAS_PENDING)
  {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (pendingState.template is<S>()) {
        visitor.template operator()<S>(pendingState.template get<S>());
      }
    };
    (tryVisit.template operator()<Is>(), ...);
  }

  // TODO(later): If we decide to ever move state-machine.h into kj, then the visitForGc
  // details will need to be revisited since those are specific to workerd.
  // The reasons we can't support this in the regular visit() public API are:
  // * Need to support uninitialized states
  // * Need to support visitors which don't implement overloads for all state types
  // * Need to support visitors with visit() functions instead of operator()
  // Points 1 and 2 could perhaps be encapsulated in a public API named something like weakVisit(),
  // and point 3 could be taken care of by saying "Your visitor must have either a visit() or
  // operator(), but not both."
  // For now, tho, we will just keep this here and we can revisit later.

  // Helper for visitForGc - visits the current state if the visitor can handle it
  template <typename Visitor, size_t... Is>
  void visitForGcImpl(Visitor& visitor, std::index_sequence<Is...>) {
    auto tryVisit = [&]<size_t I>(StateUnion& s) {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (s.template is<S>()) {
        // Only call visit if the visitor can handle this type
        if constexpr (requires { visitor.visit(s.template get<S>()); }) {
          visitor.visit(s.template get<S>());
        }
      }
    };
    (tryVisit.template operator()<Is>(state), ...);
    // Also visit pending state if present
    if constexpr (HAS_PENDING) {
      if (hasPendingState()) {
        (tryVisit.template operator()<Is>(pendingState), ...);
      }
    }
  }

  template <typename Visitor, size_t... Is>
  void visitForGcImpl(Visitor& visitor, std::index_sequence<Is...>) const {
    auto tryVisit = [&]<size_t I>(const StateUnion& s) {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (s.template is<S>()) {
        // Only call visit if the visitor can handle this type
        if constexpr (requires { visitor.visit(s.template get<S>()); }) {
          visitor.visit(s.template get<S>());
        }
      }
    };
    (tryVisit.template operator()<Is>(state), ...);
    // Also visit pending state if present
    if constexpr (HAS_PENDING) {
      if (hasPendingState()) {
        (tryVisit.template operator()<Is>(pendingState), ...);
      }
    }
  }
};

}  // namespace workerd

// =============================================================================
// DETAILED USAGE EXAMPLES
// =============================================================================
//
// Example 1: Basic Resource State Machine (Streams Pattern)
// ---------------------------------------------------------
//
//   struct Open {
//     static constexpr kj::StringPtr NAME = "open"_kj;
//     kj::Own<kj::AsyncInputStream> stream;
//   };
//
//   struct Closed {
//     static constexpr kj::StringPtr NAME = "closed"_kj;
//   };
//
//   // Full-featured stream state machine (actual pattern used in streams code)
//   using StreamState = StateMachine<
//       TerminalStates<Closed, kj::Exception>,  // Cannot transition out of these
//       ErrorState<kj::Exception>,              // Enables tryGetErrorUnsafe(), isErrored()
//       ActiveState<Open>,                      // Enables tryGetActiveUnsafe(), isActive()
//       Open, Closed, kj::Exception>;
//
//   StreamState state;
//   state.transitionTo<Open>(kj::mv(stream));
//
//   // Check state
//   if (state.isActive()) { ... }
//   if (state.isTerminal()) { ... }  // Closed or errored
//
//   // COMMON PATTERN: tryGetActiveUnsafe() with KJ_IF_SOME
//   // This is the most frequently used pattern in actual streams code.
//   // It works well with early returns and coroutines.
//   KJ_IF_SOME(open, state.tryGetActiveUnsafe()) {
//     // CAUTION: Don't transition state in this scope!
//     co_return co_await open.stream->read(buffer);
//   }
//
//   // ALTERNATIVE: whenActive() for safe access (transitions locked)
//   // Use when the callback might indirectly trigger state transitions.
//   state.whenActive([](Open& open) {
//     open.stream->doSomething();  // Safe - transitions blocked
//   });
//
//   // Error checking
//   KJ_IF_SOME(exception, state.tryGetErrorUnsafe()) {
//     kj::throwFatalException(kj::cp(exception));
//   }
//
// Example 2: Terminal State Enforcement
// -------------------------------------
//
//   StateMachine<
//       TerminalStates<Closed, kj::Exception>,
//       Open, Closed, kj::Exception
//   > state;
//
//   state.transitionTo<Open>(...);
//
//   // This works
//   state.transitionTo<Closed>();
//
//   // This throws! Cannot leave terminal state
//   state.transitionTo<Open>(...);  // KJ_REQUIRE fails
//
//   // For cleanup/reset, use forceTransitionTo
//   state.forceTransitionTo<Open>(...);  // Bypasses terminal check
//
// Example 3: Error State Helpers
// ------------------------------
//
//   StateMachine<ErrorState<kj::Exception>, Open, Closed, kj::Exception> state;
//
//   // Old pattern (verbose):
//   KJ_IF_SOME(err, state.tryGetUnsafe<kj::Exception>()) {
//     kj::throwFatalException(kj::cp(err));
//   }
//
//   // New pattern (cleaner):
//   KJ_IF_SOME(err, state.tryGetErrorUnsafe()) {
//     kj::throwFatalException(kj::cp(err));
//   }
//
//   // Or check first:
//   if (state.isErrored()) {
//     auto& err = state.getErrorUnsafe();
//   }
//
// Example 4: State Introspection for Debugging
// --------------------------------------------
//
//   struct Active { static constexpr kj::StringPtr NAME = "active"_kj; };
//   struct Paused { static constexpr kj::StringPtr NAME = "paused"_kj; };
//   struct Done { static constexpr kj::StringPtr NAME = "done"_kj; };
//
//   StateMachine<Active, Paused, Done> state;
//   state.transitionTo<Active>();
//
//   // Get current state name for logging/debugging
//   kj::StringPtr name = state.currentStateName();  // "active"
//
//   // Use in inspectState for JS visibility
//   jsg::JsString inspectState(jsg::Lock& js) {
//     return js.strIntern(state.currentStateName());
//   }
//
// Example 5: Lock State Machine (no terminal states)
// --------------------------------------------------
//
//   struct ReaderLocked {
//     static constexpr kj::StringPtr NAME = "reader_locked"_kj;
//   };
//   struct Unlocked {
//     static constexpr kj::StringPtr NAME = "unlocked"_kj;
//   };
//   struct Locked {
//     static constexpr kj::StringPtr NAME = "locked"_kj;
//   };
//
//   // No TerminalStates - locks can always be released
//   using LockState = StateMachine<Unlocked, Locked, ReaderLocked>;
//
//   LockState lockState;
//   lockState.transitionTo<Unlocked>();
//
//   // Acquire lock
//   if (lockState.is<Unlocked>()) {
//     lockState.transitionTo<ReaderLocked>();
//   }
//
//   // Release lock - always allowed
//   lockState.transitionTo<Unlocked>();
//
// Example 6: Safe State Access with whenState()
// ---------------------------------------------
//
//   StateMachine<Active, Paused, Done> state;
//   state.transitionTo<Active>();
//
//   // SAFE: whenState() locks transitions during callback
//   auto result = state.whenState<Active>([](Active& a) {
//     return a.computeResult();  // a is guaranteed valid
//   });  // Returns kj::Maybe<ResultType>
//
//   // Handle result after callback (transitions now allowed)
//   KJ_IF_SOME(r, result) {
//     state.transitionTo<Done>(kj::mv(r));
//   }
//
//   // whenActiveOr() provides a default for non-active states
//   size_t count = state.whenActiveOr(
//       [](Active& a) { return a.itemCount; },
//       size_t{0});  // Default if not active
//
// Example 7: Manual Transition Locking
// ------------------------------------
//
//   StateMachine<Active, Paused, Done> state;
//   state.transitionTo<Active>();
//
//   // For complex operations that need multiple state accesses
//   {
//     auto lock = state.acquireTransitionLock();
//
//     // All transitions blocked while lock is held
//     auto& active = state.getUnsafe<Active>();
//     active.doStep1();
//     active.doStep2();
//     active.doStep3();
//
//   }  // lock released, transitions now allowed
//
//   state.transitionTo<Done>();
//
// Example 8: Deferred State Transitions
// -------------------------------------
//
//   // For deferring close/error until pending operations complete
//   StateMachine<
//       TerminalStates<Closed, Errored>,
//       PendingStates<Closed, Errored>,  // States that can be deferred
//       Active, Closed, Errored
//   > state;
//
//   state.transitionTo<Active>();
//
//   // Start an operation
//   state.beginOperation();  // Or: auto scope = state.scopedOperation();
//
//   // Close is requested, but we're mid-operation - defer it
//   state.deferTransitionTo<Closed>();
//
//   KJ_EXPECT(state.is<Active>());         // Still active!
//   KJ_EXPECT(state.hasPendingState());    // Close is pending
//
//   // Complete the operation - pending state is auto-applied
//   state.endOperation();
//   KJ_EXPECT(state.is<Closed>());         // Now closed!
//
//   // Common pattern for streams:
//   void doRead(jsg::Lock& js) {
//     auto scope = state.scopedOperation();  // RAII operation tracking
//
//     if (state.hasPendingState()) {
//       // Don't start new work, we're shutting down
//       return;
//     }
//
//     // ... do the read ...
//   }  // Operation ends, pending state applied if any
//
// Example 9: Visitor Pattern
// --------------------------
//
//   StateMachine<Active, Paused, Done> state;
//
//   // Generic visitor (does NOT lock transitions)
//   state.visit([](auto& s) {
//     using S = kj::Decay<decltype(s)>;
//     if constexpr (kj::isSameType<S, Active>()) {
//       // Handle active
//     } else if constexpr (kj::isSameType<S, Paused>()) {
//       // Handle paused
//     } else {
//       // Handle done
//     }
//   });
//
// =============================================================================
// ACTUAL USAGE PATTERNS FROM STREAMS CODE
// =============================================================================
//
// The streams code uses StateMachine extensively. Here are the actual patterns:
//
// Common state machine declaration:
// ---------------------------------
//   using StreamState = StateMachine<
//       TerminalStates<Closed, kj::Exception>,
//       ErrorState<kj::Exception>,
//       ActiveState<Open>,
//       Open, Closed, kj::Exception>;
//
// Most common access pattern (tryGetActiveUnsafe + KJ_IF_SOME):
// -------------------------------------------------------------
//   // This pattern is used 100+ times in streams code because it works
//   // well with coroutines and early returns.
//   KJ_IF_SOME(open, state.tryGetActiveUnsafe()) {
//     co_return co_await open.stream->read(buffer);
//   }
//   // Falls through if not in active state
//
// Error checking pattern:
// -----------------------
//   KJ_IF_SOME(exception, state.tryGetErrorUnsafe()) {
//     output.abort(kj::cp(exception));
//     kj::throwFatalException(kj::cp(exception));
//   }
//
// Simple state checks:
// --------------------
//   if (state.is<Closed>()) { co_return 0; }
//   if (state.isActive()) { ... }
//   if (state.isTerminal()) { ... }
//
// whenActiveOr for default values:
// --------------------------------
//   return state.whenActiveOr(
//       [](Queue& q) { return q.getConsumerCount(); },
//       size_t{0});
//
