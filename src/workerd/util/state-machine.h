// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// State Machine Abstraction built on kj::OneOf.
// TODO(later): If this proves useful, consider moving it into kj itself as there
// are no workerd-specific dependencies.
//
// Entire implementation was Claude-generated initially.
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
// THE PROBLEM: Use-After-Free (UAF)
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
// 1. TRANSITION LOCKING via withState()/whenActive():
//
//    state.withState<Readable>([](Readable& r) {
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
//    TerminalStateMachine/ComposableStateMachine with TerminalStates<> will
//    throw if you attempt this, catching the bug immediately.
//
// 4. SEMANTIC HELPERS:
//
//    Instead of: state.is<kj::Exception>() || state.is<Closed>()
//    Write:      state.isTerminal()  or  state.isInactive()
//
//    Instead of: KJ_IF_SOME(e, state.tryGet<kj::Exception>()) { ... }
//    Write:      KJ_IF_SOME(e, state.tryGetError()) { ... }
//
// WHEN TO USE WHICH:
//
//   - New code: Use ComposableStateMachine with appropriate specs
//   - Simple state tracking: StateMachine<States...> is fine
//   - Resource lifecycle (streams, handles): Use TerminalStates + PendingStates
//   - Migrating existing code: See MIGRATION GUIDE section below
//
// =============================================================================
// AVAILABLE CLASSES
// =============================================================================
//
// This header provides several state machine classes:
//
// 1. StateMachine<States...>
//    - Basic state machine, thin wrapper over kj::OneOf
//    - Provides transition locking, safe access patterns, visitor support
//    - Movable but NOT copyable (for consistency with other state machine types)
//
// 2. TerminalStateMachine<TerminalStates<...>, States...>
//    - Enforces that terminal states cannot be transitioned FROM
//
// 3. ErrorableStateMachine<ErrorState, States...>
//    - Adds isErrored(), tryGetError(), getError() helpers
//
// 4. ResourceStateMachine<Active, Closed, Errored>
//    - Specialized for the common Active/Closed/Errored pattern
//
// 5. ComposableStateMachine<Specs..., States...>
//    - RECOMMENDED for new code
//    - Combines any subset of features via spec types
//    - Not copyable (to support pending state semantics)
//
// =============================================================================
// COMPOSABLE STATE MACHINE
// =============================================================================
//
// ComposableStateMachine supports composable features via spec types:
//
//   // Simple (no specs)
//   ComposableStateMachine<Idle, Running, Done> basic;
//
//   // With terminal state enforcement
//   ComposableStateMachine<TerminalStates<Done>, Idle, Running, Done> withTerminal;
//
//   // With error extraction helpers
//   ComposableStateMachine<ErrorState<Errored>, Active, Closed, Errored> withError;
//
//   // With deferred transitions
//   ComposableStateMachine<PendingStates<Closed, Errored>, Active, Closed, Errored> withDefer;
//
//   // Full-featured (combine any specs)
//   ComposableStateMachine<
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
//                              Enables: isErrored(), tryGetError(), getError()
//   - ActiveState<T>         - Designates the active/working state type
//                              Enables: isActive(), isInactive(), whenActive(), tryGetActive()
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
//      machine.withState<Active>([](Active& a) {
//        // machine.transitionTo<Closed>();  // Would fail - locked!
//        a.resource->read();  // Safe - Active cannot be destroyed
//      });
//
// 2. DEBUG ASSERTIONS: In debug builds, the machine tracks active references
//    and asserts if a transition occurs while references exist.
//
// 3. SAFE ACCESS PATTERNS: Prefer withState() and whenActive() over get()
//    to ensure references don't outlive their validity.
//
// UNSAFE PATTERNS TO AVOID:
//
//   // DON'T: Store references from get() across transitions
//   Active& active = machine.get<Active>();
//   machine.transitionTo<Closed>();  // active is now dangling!
//
//   // DO: Use withState() for safe scoped access
//   machine.withState<Active>([](Active& a) {
//     // a is guaranteed valid for the duration of the callback
//   });
//
//   // DON'T: Transition inside a callback (will fail if locked)
//   machine.withState<Active>([&](Active& a) {
//     machine.transitionTo<Closed>();  // Fails!
//   });
//
//   // DO: Return a value and transition after
//   auto result = machine.withState<Active>([](Active& a) {
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
//   // RECOMMENDED: Use withState() for safe scoped access
//   state.withState<Readable>([](Readable& r) {
//     r.source->read();  // Safe - transitions blocked during callback
//   });
//
//   // Or with a return value
//   auto size = state.withState<Readable>([](Readable& r) {
//     return r.source->size();
//   });  // Returns kj::Maybe<size_t>
//
// Stream-like state machine (common pattern in workerd):
//
//   ComposableStateMachine<
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
//   KJ_IF_SOME(err, state.tryGetError()) { ... }
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
// Note: If using ErrorState<T>, T must be a struct (not a type alias).
//
//   // Won't work with ErrorState:
//   using Errored = jsg::Value;
//
//   // Works with ErrorState:
//   struct Errored {
//     static constexpr kj::StringPtr NAME = "Errored"_kj;
//     jsg::Value reason;
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
//   ComposableStateMachine<
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
//   state.withState<Readable>([](Readable& r) {
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
#include <kj/function.h>
#include <kj/one-of.h>
#include <kj/string.h>
#include <kj/tuple.h>

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

// Check if type is empty (no non-static data members)
template <typename T>
inline constexpr bool isEmptyState = std::is_empty_v<T>;

// Default transition policy: all transitions allowed
struct AllowAllTransitions {
  template <typename From, typename To>
  static constexpr bool isAllowed() {
    return true;
  }
};

// Strict transition policy: no transitions allowed by default
struct DenyAllTransitions {
  template <typename From, typename To>
  static constexpr bool isAllowed() {
    return false;
  }
};

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

// Marker type to specify the error state (enables isErrored(), tryGetError(), etc.)
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
struct FilterStates;

template <>
struct FilterStates<> {
  using Type = std::tuple<>;
};

template <typename First, typename... Rest>
struct FilterStates<First, Rest...> {
  using RestFiltered = typename FilterStates<Rest...>::Type;
  using Type = std::conditional_t<isSpec<First>,
      RestFiltered,
      decltype(std::tuple_cat(kj::instance<std::tuple<First>>(), kj::instance<RestFiltered>()))>;
};

template <typename... Ts>
using FilterStatesT = typename FilterStates<Ts...>::Type;

// Convert tuple to kj::OneOf
template <typename Tuple>
struct TupleToOneOf;

template <typename... Ts>
struct TupleToOneOf<std::tuple<Ts...>> {
  using Type = kj::OneOf<Ts...>;
};

template <typename Tuple>
using TupleToOneOfT = typename TupleToOneOf<Tuple>::Type;

// Find a specific spec in a list
template <template <typename...> class SpecTemplate, typename... Ts>
struct FindSpec {
  using Type = void;  // Not found
};

template <template <typename...> class SpecTemplate, typename First, typename... Rest>
struct FindSpec<SpecTemplate, First, Rest...> {
  using Type = std::conditional_t<isTerminalStatesSpec<First> &&
          kj::isSameType<SpecTemplate<>, TerminalStates<>>(),
      First,
      std::conditional_t<isPendingStatesSpec<First> &&
              kj::isSameType<SpecTemplate<>, PendingStates<>>(),
          First,
          typename FindSpec<SpecTemplate, Rest...>::Type>>;
};

// Find ErrorState spec
template <typename... Ts>
struct FindErrorStateSpec {
  using Type = void;
};

template <typename First, typename... Rest>
struct FindErrorStateSpec<First, Rest...> {
  using Type = std::
      conditional_t<isErrorStateSpec<First>, First, typename FindErrorStateSpec<Rest...>::Type>;
};

// Find ActiveState spec
template <typename... Ts>
struct FindActiveStateSpec {
  using Type = void;
};

template <typename First, typename... Rest>
struct FindActiveStateSpec<First, Rest...> {
  using Type = std::
      conditional_t<isActiveStateSpec<First>, First, typename FindActiveStateSpec<Rest...>::Type>;
};

// Find TerminalStates spec
template <typename... Ts>
struct FindTerminalStatesSpec {
  using Type = void;
};

template <typename First, typename... Rest>
struct FindTerminalStatesSpec<First, Rest...> {
  using Type = std::conditional_t<isTerminalStatesSpec<First>,
      First,
      typename FindTerminalStatesSpec<Rest...>::Type>;
};

// Find PendingStates spec
template <typename... Ts>
struct FindPendingStatesSpec {
  using Type = void;
};

template <typename First, typename... Rest>
struct FindPendingStatesSpec<First, Rest...> {
  using Type = std::conditional_t<isPendingStatesSpec<First>,
      First,
      typename FindPendingStatesSpec<Rest...>::Type>;
};

// Helper to get state tuple size
template <typename Tuple>
struct TupleSize;

template <typename... Ts>
struct TupleSize<std::tuple<Ts...>> {
  static constexpr size_t value = sizeof...(Ts);
};

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
struct ExtractSpecType {
  using Type = typename Spec::Type;
};

template <>
struct ExtractSpecType<void> {
  using Type = PlaceholderType;
};

template <typename Spec>
using ExtractSpecTypeT = typename ExtractSpecType<Spec>::Type;

// Helper to validate that a type is in a tuple (for spec validation)
template <typename T, typename Tuple>
struct ValidateTypeInTuple {
  static_assert(isInTuple<T, Tuple>,
      "Spec type parameter must be one of the state types in the state machine");
  static constexpr bool valid = true;
};

// Count how many times a spec type appears in a list
template <template <typename...> class SpecTemplate, typename... Ts>
struct CountSpec {
  static constexpr size_t value = 0;
};

template <template <typename...> class SpecTemplate, typename First, typename... Rest>
struct CountSpec<SpecTemplate, First, Rest...> {
  static constexpr bool isMatch = []() {
    if constexpr (isTerminalStatesSpec<First>) {
      return kj::isSameType<SpecTemplate<>, TerminalStates<>>();
    } else if constexpr (isPendingStatesSpec<First>) {
      return kj::isSameType<SpecTemplate<>, PendingStates<>>();
    } else {
      return false;
    }
  }();
  static constexpr size_t value = (isMatch ? 1 : 0) + CountSpec<SpecTemplate, Rest...>::value;
};

// Count ErrorState specs
template <typename... Ts>
struct CountErrorStateSpec {
  static constexpr size_t value = 0;
};

template <typename First, typename... Rest>
struct CountErrorStateSpec<First, Rest...> {
  static constexpr size_t value =
      (isErrorStateSpec<First> ? 1 : 0) + CountErrorStateSpec<Rest...>::value;
};

// Count ActiveState specs
template <typename... Ts>
struct CountActiveStateSpec {
  static constexpr size_t value = 0;
};

template <typename First, typename... Rest>
struct CountActiveStateSpec<First, Rest...> {
  static constexpr size_t value =
      (isActiveStateSpec<First> ? 1 : 0) + CountActiveStateSpec<Rest...>::value;
};

// Count TerminalStates specs
template <typename... Ts>
struct CountTerminalStatesSpec {
  static constexpr size_t value = 0;
};

template <typename First, typename... Rest>
struct CountTerminalStatesSpec<First, Rest...> {
  static constexpr size_t value =
      (isTerminalStatesSpec<First> ? 1 : 0) + CountTerminalStatesSpec<Rest...>::value;
};

// Count PendingStates specs
template <typename... Ts>
struct CountPendingStatesSpec {
  static constexpr size_t value = 0;
};

template <typename First, typename... Rest>
struct CountPendingStatesSpec<First, Rest...> {
  static constexpr size_t value =
      (isPendingStatesSpec<First> ? 1 : 0) + CountPendingStatesSpec<Rest...>::value;
};

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
template <typename Machine>
class TransitionLock {
 public:
  explicit TransitionLock(Machine& m): machine(m) {
    machine.lockTransitions();
  }

  ~TransitionLock() {
    machine.unlockTransitions();
  }

  TransitionLock(const TransitionLock&) = delete;
  TransitionLock& operator=(const TransitionLock&) = delete;
  TransitionLock(TransitionLock&&) = delete;
  TransitionLock& operator=(TransitionLock&&) = delete;

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

// =============================================================================
// Base State Machine
// =============================================================================

// Base state machine without transition validation.
// This is a thin wrapper around kj::OneOf that provides a more intention-revealing API.
//
// MEMORY SAFETY:
// - Use withState() for safe scoped access to state data
// - Avoid storing references from get() across potential transitions
// - In debug builds, transitions while references are held will assert
template <typename... States>
class StateMachine {
 public:
  using StateUnion = kj::OneOf<States...>;
  static constexpr size_t STATE_COUNT = sizeof...(States);

  // Default constructor: state is uninitialized (tag == 0)
  StateMachine() = default;

  // Destructor checks for outstanding locks in debug builds
  ~StateMachine() {
    KJ_DASSERT(transitionLockCount == 0, "StateMachine destroyed while transition locks are held");
  }

  // Move operations - both source and destination must not have locks held
  StateMachine(StateMachine&& other) noexcept: state(kj::mv(other.state)), transitionLockCount(0) {
    KJ_DASSERT(other.transitionLockCount == 0,
        "Cannot move from StateMachine while transition locks are held");
  }

  StateMachine& operator=(StateMachine&& other) noexcept {
    KJ_DASSERT(transitionLockCount == 0,
        "Cannot move-assign to StateMachine while transition locks are held");
    KJ_DASSERT(other.transitionLockCount == 0,
        "Cannot move from StateMachine while transition locks are held");
    state = kj::mv(other.state);
    return *this;
  }

  // Copying is disabled for consistency with other state machine types
  // and to avoid confusion about transitionLockCount semantics.
  StateMachine(const StateMachine&) = delete;
  StateMachine& operator=(const StateMachine&) = delete;

  // Initialize with a specific state
  template <typename S, typename... Args>
  explicit StateMachine(kj::Badge<S>, Args&&... args)
    requires(_::isOneOf<S, States...>)
  {
    state.template init<S>(kj::fwd<Args>(args)...);
  }

  // Factory function for clearer initialization
  template <typename S, typename... Args>
  static StateMachine create(Args&&... args)
    requires(_::isOneOf<S, States...>)
  {
    StateMachine m;
    m.state.template init<S>(kj::fwd<Args>(args)...);
    return m;
  }

  // ---------------------------------------------------------------------------
  // Transition Locking
  // ---------------------------------------------------------------------------

  // Check if transitions are currently locked
  bool isTransitionLocked() const {
    return transitionLockCount > 0;
  }

  // Lock transitions (called by TransitionLock RAII guard)
  void lockTransitions() {
    ++transitionLockCount;
  }

  // Unlock transitions (called by TransitionLock RAII guard)
  void unlockTransitions() {
    KJ_DASSERT(transitionLockCount > 0, "Transition lock underflow");
    --transitionLockCount;
  }

  // Get a RAII lock that prevents transitions while in scope
  TransitionLock<StateMachine> acquireTransitionLock() {
    return TransitionLock<StateMachine>(*this);
  }

  // ---------------------------------------------------------------------------
  // State Queries
  // ---------------------------------------------------------------------------

  // Check if the machine is in a specific state.
  // Returns false if uninitialized.
  template <typename S>
  bool is() const
    requires(_::isOneOf<S, States...>)
  {
    return state.template is<S>();
  }

  // Check if the machine is in any of the specified states.
  // Returns false if uninitialized.
  template <typename... Ss>
  bool isAnyOf() const
    requires((_::isOneOf<Ss, States...>) && ...)
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
  // State Access
  // ---------------------------------------------------------------------------
  //
  // WARNING: get() and tryGet() return UNLOCKED references. The returned
  // reference becomes invalid if the state machine transitions. Prefer
  // withState() for safe access that locks transitions during the callback.
  //
  // UNSAFE PATTERN:
  //   Active& a = machine.get<Active>();
  //   someFunction();  // If this transitions the machine...
  //   a.doSomething(); // ...this is use-after-free!
  //
  // SAFE PATTERN:
  //   machine.withState<Active>([](Active& a) {
  //     a.doSomething();  // Transitions blocked during callback
  //   });

  // Get reference to current state data.
  // Throws if not in the specified state or if uninitialized.
  //
  // WARNING: The returned reference is NOT protected by a transition lock.
  // It becomes dangling if the machine transitions before you're done using it.
  // Prefer withState() for safe access.
  template <typename S>
  S& get() KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    requireInitialized();
    KJ_REQUIRE(is<S>(), "State machine is not in the expected state");
    return state.template get<S>();
  }

  template <typename S>
  const S& get() const KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    requireInitialized();
    KJ_REQUIRE(is<S>(), "State machine is not in the expected state");
    return state.template get<S>();
  }

  // Try to get reference to current state data.
  // Returns kj::none if not in the specified state.
  //
  // WARNING: The returned reference is NOT protected by a transition lock.
  // It becomes dangling if the machine transitions before you're done using it.
  // Prefer withState() for safe access.
  template <typename S>
  kj::Maybe<S&> tryGet() KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    return state.template tryGet<S>();
  }

  template <typename S>
  kj::Maybe<const S&> tryGet() const KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    return state.template tryGet<S>();
  }

  // ---------------------------------------------------------------------------
  // Safe State Access (RECOMMENDED)
  // ---------------------------------------------------------------------------

  // Execute a function with the current state, locking transitions.
  // This is the SAFEST way to access state data as it prevents
  // use-after-free by blocking transitions during the callback.
  //
  // Usage:
  //   machine.withState<Active>([](Active& a) {
  //     a.resource->doSomething();  // Safe!
  //   });
  //
  // Returns the function's result wrapped in Maybe (none if not in state).
  // For void functions, returns true if executed, false if not in state.
  //
  // WARNING: Do NOT store or escape the reference passed to the callback!
  // The reference is only valid during the callback. Storing it for later
  // use defeats the purpose of the transition lock:
  //
  //   Active* escaped;
  //   machine.withState<Active>([&](Active& a) {
  //     escaped = &a;  // BAD: escaping the reference!
  //   });
  //   escaped->foo();  // UAF if machine has transitioned!
  template <typename S, typename Func>
  auto withState(
      Func&& func) -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<S&>()))>,
                       bool,
                       kj::Maybe<decltype(func(kj::instance<S&>()))>>
    requires(_::isOneOf<S, States...>)
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

  template <typename S, typename Func>
  auto withState(Func&& func) const
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<const S&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<const S&>()))>>
    requires(_::isOneOf<S, States...>)
  {
    if (!is<S>()) {
      if constexpr (std::is_void_v<decltype(func(kj::instance<const S&>()))>) {
        return false;
      } else {
        return kj::none;
      }
    }

    // Note: Lock count is mutable, so we can lock even on const
    ++transitionLockCount;
    KJ_DEFER(--transitionLockCount);
    if constexpr (std::is_void_v<decltype(func(kj::instance<const S&>()))>) {
      func(state.template get<S>());
      return true;
    } else {
      return func(state.template get<S>());
    }
  }

  // Execute with state, providing a default value if not in that state.
  template <typename S, typename Func, typename Default>
  auto withStateOr(Func&& func, Default&& defaultValue) -> decltype(func(kj::instance<S&>()))
    requires(_::isOneOf<S, States...>)
  {
    if (!is<S>()) {
      return kj::fwd<Default>(defaultValue);
    }

    auto lock = acquireTransitionLock();
    return func(state.template get<S>());
  }

  // ---------------------------------------------------------------------------
  // State Transitions
  // ---------------------------------------------------------------------------

  // Transition to a new state, constructing it in-place.
  // Returns a reference to the new state.
  //
  // WARNING: The returned reference becomes invalid on the next transition!
  // Do NOT store this reference. If you need to work with the new state,
  // either use the reference immediately inline, or use withState() after:
  //
  //   // OK: Use immediately inline
  //   machine.transitionTo<Active>(...).startOperation();
  //
  //   // OK: Use withState() for extended access
  //   machine.transitionTo<Active>(...);
  //   machine.withState<Active>([](Active& a) { ... });
  //
  //   // BAD: Storing the reference
  //   Active& a = machine.transitionTo<Active>(...);
  //   someFunction();  // If this transitions machine...
  //   a.foo();         // ...this is UAF!
  template <typename S, typename... Args>
  S& transitionTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    requireUnlocked();
    return state.template init<S>(kj::fwd<Args>(args)...);
  }

  // Transition to a new state only if currently in a specific state.
  // Returns kj::Maybe<To&> - none if the precondition state wasn't met.
  //
  // WARNING: The returned reference becomes invalid on the next transition!
  template <typename From, typename To, typename... Args>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<From, States...>) && (_::isOneOf<To, States...>)
  {
    requireUnlocked();
    if (is<From>()) {
      return state.template init<To>(kj::fwd<Args>(args)...);
    }
    return kj::none;
  }

  // ---------------------------------------------------------------------------
  // Conditional State Transitions
  // ---------------------------------------------------------------------------

  // Transition from one state to another only if a predicate is satisfied.
  // The predicate receives a reference to the current From state and should return bool.
  //
  // Returns kj::Maybe<To&>:
  //   - none if not in From state OR predicate returned false
  //   - reference to new To state if transition occurred
  //
  // THREAD SAFETY NOTE: This method is designed for single-threaded use only.
  // The predicate is evaluated while transitions are locked, ensuring the state
  // doesn't change during predicate evaluation. However, there is a brief window
  // between predicate evaluation and transition where the lock is released
  // (necessary because transitions require unlocked state). In single-threaded
  // code this is safe since no other code runs in that window.
  //
  // Usage:
  //   state.transitionFromToIf<Reading, Done>(
  //       [](Reading& r) { return r.bytesRemaining == 0; },
  //       doneArgs...);
  template <typename From, typename To, typename Predicate, typename... Args>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromToIf(
      Predicate&& predicate, Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<From, States...>) && (_::isOneOf<To, States...>)
  {
    requireUnlocked();

    if (!is<From>()) {
      return kj::none;
    }

    // Lock while evaluating predicate to ensure atomicity
    bool shouldTransition;
    {
      auto lock = acquireTransitionLock();
      shouldTransition = predicate(state.template get<From>());
    }

    if (shouldTransition) {
      return state.template init<To>(kj::fwd<Args>(args)...);
    }
    return kj::none;
  }

  // Transition with predicate that also produces the arguments for the new state.
  // The predicate returns kj::Maybe<To> - none means don't transition.
  //
  // Usage:
  //   state.transitionFromToWith<Reading, Done>(
  //       [](Reading& r) -> kj::Maybe<Done> {
  //         if (r.bytesRemaining == 0) {
  //           return Done { r.totalBytes };
  //         }
  //         return kj::none;
  //       });
  template <typename From, typename To, typename Producer>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromToWith(Producer&& producer) KJ_LIFETIMEBOUND
    requires(_::isOneOf<From, States...>) && (_::isOneOf<To, States...>)
  {
    requireUnlocked();

    if (!is<From>()) {
      return kj::none;
    }

    // Lock while evaluating producer to ensure atomicity
    kj::Maybe<To> maybeNewState;
    {
      auto lock = acquireTransitionLock();
      maybeNewState = producer(state.template get<From>());
    }

    KJ_IF_SOME(newState, maybeNewState) {
      return state.template init<To>(kj::mv(newState));
    }
    return kj::none;
  }

  // ---------------------------------------------------------------------------
  // State Introspection
  // ---------------------------------------------------------------------------

  // Get the name of the current state (requires states to have NAME member)
  kj::StringPtr currentStateName() const {
    if (!isInitialized()) {
      return "(uninitialized)"_kj;
    }
    kj::StringPtr result = "(unknown)"_kj;
    visit([&result](const auto& s) {
      using S = kj::Decay<decltype(s)>;
      result = _::getStateName<S>();
    });
    return result;
  }

  // ---------------------------------------------------------------------------
  // Visitor Pattern
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
    return visitImpl(kj::fwd<Visitor>(visitor), std::index_sequence_for<States...>{});
  }

  template <typename Visitor>
  decltype(auto) visit(Visitor&& visitor) const {
    return visitConstImpl(kj::fwd<Visitor>(visitor), std::index_sequence_for<States...>{});
  }

  // ---------------------------------------------------------------------------
  // Interop
  // ---------------------------------------------------------------------------

  // Access the underlying kj::OneOf for interop with existing code
  StateUnion& underlying() KJ_LIFETIMEBOUND {
    return state;
  }
  const StateUnion& underlying() const KJ_LIFETIMEBOUND {
    return state;
  }

  // For use with KJ_SWITCH_ONEOF
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

 protected:
  StateUnion state;
  mutable uint32_t transitionLockCount = 0;

  // Check that transitions are allowed (not locked)
  void requireUnlocked() const {
    KJ_REQUIRE(transitionLockCount == 0,
        "Cannot transition state machine while transitions are locked. "
        "This usually means you're trying to transition inside a withState() callback.");
  }

  // Visitor implementation using index sequence
  template <typename Visitor, size_t... Is>
  decltype(auto) visitImpl(Visitor&& visitor, std::index_sequence<Is...>) {
    KJ_REQUIRE(isInitialized(), "Cannot visit uninitialized state machine");

    using ReturnType = std::common_type_t<decltype(visitor(kj::instance<States&>()))...>;

    if constexpr (std::is_void_v<ReturnType>) {
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, std::tuple<States...>>;
        if (state.template is<S>()) {
          visitor(state.template get<S>());
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
    } else {
      ReturnType result{};
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, std::tuple<States...>>;
        if (state.template is<S>()) {
          result = visitor(state.template get<S>());
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
      return result;
    }
  }

  // Helper for visit() - const version
  template <typename Visitor, size_t... Is>
  decltype(auto) visitConstImpl(Visitor&& visitor, std::index_sequence<Is...>) const {
    KJ_REQUIRE(isInitialized(), "Cannot visit uninitialized state machine");

    using ReturnType = std::common_type_t<decltype(visitor(kj::instance<const States&>()))...>;

    if constexpr (std::is_void_v<ReturnType>) {
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, std::tuple<States...>>;
        if (state.template is<S>()) {
          visitor(state.template get<S>());
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
    } else {
      ReturnType result{};
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, std::tuple<States...>>;
        if (state.template is<S>()) {
          result = visitor(state.template get<S>());
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
      return result;
    }
  }
};

// =============================================================================
// Terminal State Machine
// =============================================================================

// A state machine that enforces terminal states - once in a terminal state,
// no further transitions are allowed.
//
// Usage:
//   // Specify terminal states as first template arguments, then all states
//   TerminalStateMachine<
//       TerminalStates<Closed, Errored>,  // Terminal states
//       Active, Closed, Errored           // All states
//   > state;
//
//   state.transitionTo<Active>(...);
//   state.transitionTo<Closed>();
//   state.transitionTo<Active>(...);  // KJ_REQUIRE fails!
//
// This catches bugs where code accidentally transitions out of terminal states.

template <typename TerminalSpec, typename... States>
class TerminalStateMachine: public StateMachine<States...> {
  using Base = StateMachine<States...>;

 public:
  using Base::Base;
  using Base::currentStateName;
  using Base::get;
  using Base::is;
  using Base::isAnyOf;
  using Base::tryGet;

  // Check if currently in a terminal state
  bool isTerminal() const {
    return TerminalSpec::isTerminal(*this);
  }

  // Transition that enforces terminal state rules
  template <typename S, typename... Args>
  S& transitionTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    this->requireUnlocked();
    KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state. Current state is terminal.");
    return this->state.template init<S>(kj::fwd<Args>(args)...);
  }

  // Force transition even from terminal state.
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
  template <typename S, typename... Args>
  S& forceTransitionTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, States...>)
  {
    this->requireUnlocked();
    return this->state.template init<S>(kj::fwd<Args>(args)...);
  }

  // Transition from a specific state (also enforces terminal)
  template <typename From, typename To, typename... Args>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<From, States...>) && (_::isOneOf<To, States...>)
  {
    this->requireUnlocked();
    if (this->template is<From>()) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
      return this->state.template init<To>(kj::fwd<Args>(args)...);
    }
    return kj::none;
  }
};

// =============================================================================
// Errorable State Machine
// =============================================================================

// A state machine with built-in support for error states.
// Reduces boilerplate for the common pattern of extracting errors.
//
// Usage:
//   ErrorableStateMachine<Readable, Closed, Errored> state;
//
//   // Instead of:
//   //   KJ_IF_SOME(errored, state.tryGet<Errored>()) { ... }
//   // You can write:
//   KJ_IF_SOME(errored, state.tryGetError()) { ... }
//
//   if (state.isErrored()) { ... }

template <typename ErrorState, typename... AllStates>
class ErrorableStateMachine: public StateMachine<AllStates...> {
  using Base = StateMachine<AllStates...>;
  static_assert(_::isOneOf<ErrorState, AllStates...>, "ErrorState must be one of the state types");

 public:
  using Base::Base;
  using Base::get;
  using Base::is;
  using Base::transitionTo;
  using Base::tryGet;

  // Check if in errored state
  bool isErrored() const {
    return this->template is<ErrorState>();
  }

  // Get the error state if currently errored
  kj::Maybe<ErrorState&> tryGetError() KJ_LIFETIMEBOUND {
    return this->template tryGet<ErrorState>();
  }

  kj::Maybe<const ErrorState&> tryGetError() const KJ_LIFETIMEBOUND {
    return this->template tryGet<ErrorState>();
  }

  // Get the error state, asserting we are errored
  ErrorState& getError() KJ_LIFETIMEBOUND {
    return this->template get<ErrorState>();
  }

  const ErrorState& getError() const KJ_LIFETIMEBOUND {
    return this->template get<ErrorState>();
  }
};

// =============================================================================
// Resource State Machine
// =============================================================================

// A state machine for managing resources with active/closed/errored lifecycle.
// This is the most common pattern in streams: one "active" state holds a resource,
// and terminal states indicate the resource is no longer available.
//
// Usage:
//   struct Active { kj::Own<Source> source; };
//   struct Closed { static constexpr kj::StringPtr NAME = "closed"_kj; };
//   struct Errored { jsg::Value error; static constexpr kj::StringPtr NAME = "errored"_kj; };
//
//   ResourceStateMachine<Active, Closed, Errored> state;
//
//   // Check resource availability
//   if (state.isActive()) { ... }
//   if (state.isTerminated()) { ... }  // closed OR errored
//
//   // Get the active resource
//   KJ_IF_SOME(active, state.tryGetActive()) {
//     active.source->read(...);
//   }
//
//   // Execute only if active
//   state.whenActive([](Active& a) {
//     a.source->doSomething();
//   });

template <typename ActiveState, typename ClosedState, typename ErrorState>
class ResourceStateMachine: public StateMachine<ActiveState, ClosedState, ErrorState> {
  using Base = StateMachine<ActiveState, ClosedState, ErrorState>;

 public:
  using Base::Base;
  using Base::get;
  using Base::is;
  using Base::tryGet;

  // ---------------------------------------------------------------------------
  // Resource State Queries
  // ---------------------------------------------------------------------------

  // Is the resource still active/usable?
  bool isActive() const {
    return this->template is<ActiveState>();
  }

  // Is the resource closed normally?
  bool isClosed() const {
    return this->template is<ClosedState>();
  }

  // Is the resource in an error state?
  bool isErrored() const {
    return this->template is<ErrorState>();
  }

  // Is the resource terminated (closed or errored)?
  bool isTerminated() const {
    return isClosed() || isErrored();
  }

  // Alias for isTerminated (matches streams API naming)
  bool isClosedOrErrored() const {
    return isTerminated();
  }

  // ---------------------------------------------------------------------------
  // Resource Access
  // ---------------------------------------------------------------------------

  // Get the active state if available
  kj::Maybe<ActiveState&> tryGetActive() KJ_LIFETIMEBOUND {
    return this->template tryGet<ActiveState>();
  }

  kj::Maybe<const ActiveState&> tryGetActive() const KJ_LIFETIMEBOUND {
    return this->template tryGet<ActiveState>();
  }

  // Get the error state if errored
  kj::Maybe<ErrorState&> tryGetError() KJ_LIFETIMEBOUND {
    return this->template tryGet<ErrorState>();
  }

  kj::Maybe<const ErrorState&> tryGetError() const KJ_LIFETIMEBOUND {
    return this->template tryGet<ErrorState>();
  }

  // ---------------------------------------------------------------------------
  // Resource Operations (with transition locking for safety)
  // ---------------------------------------------------------------------------

  // Execute a function only if in active state.
  // LOCKS TRANSITIONS during callback execution to prevent use-after-free.
  // Returns the function's result, or kj::none if not active.
  // For void functions, returns true if executed, false if not active.
  template <typename Func>
  auto whenActive(Func&& func)
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<ActiveState&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<ActiveState&>()))>> {
    if (!isActive()) {
      if constexpr (std::is_void_v<decltype(func(kj::instance<ActiveState&>()))>) {
        return false;
      } else {
        return kj::none;
      }
    }

    auto lock = this->acquireTransitionLock();
    auto& active = this->state.template get<ActiveState>();
    if constexpr (std::is_void_v<decltype(func(kj::instance<ActiveState&>()))>) {
      func(active);
      return true;
    } else {
      return func(active);
    }
  }

  template <typename Func>
  auto whenActive(Func&& func) const
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<const ActiveState&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<const ActiveState&>()))>> {
    if (!isActive()) {
      if constexpr (std::is_void_v<decltype(func(kj::instance<const ActiveState&>()))>) {
        return false;
      } else {
        return kj::none;
      }
    }

    ++this->transitionLockCount;
    KJ_DEFER(--this->transitionLockCount);
    const auto& active = this->state.template get<ActiveState>();
    if constexpr (std::is_void_v<decltype(func(kj::instance<const ActiveState&>()))>) {
      func(active);
      return true;
    } else {
      return func(active);
    }
  }

  // Execute a function if active, or return a default value.
  // LOCKS TRANSITIONS during callback execution.
  template <typename Func, typename Default>
  auto whenActiveOr(
      Func&& func, Default&& defaultValue) -> decltype(func(kj::instance<ActiveState&>())) {
    if (!isActive()) {
      return kj::fwd<Default>(defaultValue);
    }

    auto lock = this->acquireTransitionLock();
    auto& active = this->state.template get<ActiveState>();
    return func(active);
  }

  // ---------------------------------------------------------------------------
  // State Transitions with Semantics
  // ---------------------------------------------------------------------------

  // Close the resource (transition to closed state)
  template <typename... Args>
  ClosedState& close(Args&&... args) KJ_LIFETIMEBOUND {
    this->requireUnlocked();
    KJ_REQUIRE(!isTerminated(), "Resource is already terminated");
    return this->state.template init<ClosedState>(kj::fwd<Args>(args)...);
  }

  // Error the resource (transition to error state)
  template <typename... Args>
  ErrorState& error(Args&&... args) KJ_LIFETIMEBOUND {
    this->requireUnlocked();
    KJ_REQUIRE(!isTerminated(), "Resource is already terminated");
    return this->state.template init<ErrorState>(kj::fwd<Args>(args)...);
  }

  // Close even if already terminated (for cleanup scenarios).
  //
  // WARNING: Bypasses terminal state protection. See forceTransitionTo() docs
  // for when this is appropriate vs. suspicious.
  template <typename... Args>
  ClosedState& forceClose(Args&&... args) KJ_LIFETIMEBOUND {
    this->requireUnlocked();
    return this->state.template init<ClosedState>(kj::fwd<Args>(args)...);
  }

  // Error even if already terminated (for cleanup scenarios).
  //
  // WARNING: Bypasses terminal state protection. See forceTransitionTo() docs
  // for when this is appropriate vs. suspicious.
  template <typename... Args>
  ErrorState& forceError(Args&&... args) KJ_LIFETIMEBOUND {
    this->requireUnlocked();
    return this->state.template init<ErrorState>(kj::fwd<Args>(args)...);
  }

  // Generic transition (for backward compatibility)
  template <typename S, typename... Args>
  S& transitionTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<S, ActiveState, ClosedState, ErrorState>)
  {
    this->requireUnlocked();
    return this->state.template init<S>(kj::fwd<Args>(args)...);
  }
};

// =============================================================================
// Validated State Machine
// =============================================================================

// A state machine with compile-time transition validation.
// The TransitionPolicy must have a static method:
//   template <typename From, typename To>
//   static constexpr bool isAllowed();
//
// Invalid transitions will cause a compile-time error.

template <typename TransitionPolicy, typename... States>
class ValidatedStateMachine: public StateMachine<States...> {
  using Base = StateMachine<States...>;

 public:
  using Base::Base;
  using Base::get;
  using Base::is;
  using Base::tryGet;

  // Unvalidated transition (same as base)
  template <typename To, typename... Args>
  To& transitionTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<To, States...>)
  {
    this->requireUnlocked();
    return this->state.template init<To>(kj::fwd<Args>(args)...);
  }

  // Validated transition from a specific state.
  // Compile-time error if From -> To is not allowed by the policy.
  template <typename From, typename To, typename... Args>
  To& checkedTransitionFromTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<From, States...>) && (_::isOneOf<To, States...>) &&
      (TransitionPolicy::template isAllowed<From, To>())
  {
    this->requireUnlocked();
    KJ_REQUIRE(this->template is<From>(),
        "State machine transition precondition failed: not in expected state");
    return this->state.template init<To>(kj::fwd<Args>(args)...);
  }

  // Try validated transition - returns none if not in From state.
  template <typename From, typename To, typename... Args>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> tryCheckedTransitionFromTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<From, States...>) && (_::isOneOf<To, States...>) &&
      (TransitionPolicy::template isAllowed<From, To>())
  {
    this->requireUnlocked();
    if (this->template is<From>()) {
      return this->state.template init<To>(kj::fwd<Args>(args)...);
    }
    return kj::none;
  }
};

// =============================================================================
// Observable State Machine
// =============================================================================

// A state machine that can notify observers of state changes.
// Useful for debugging, logging, or triggering side effects.

template <typename... States>
class ObservableStateMachine: public StateMachine<States...> {
  using Base = StateMachine<States...>;

 public:
  using TransitionCallback = kj::Function<void(kj::StringPtr fromState, kj::StringPtr toState)>;

  ObservableStateMachine() = default;

  // Set a callback to be invoked on any state transition
  void onTransition(TransitionCallback callback) {
    transitionCallback = kj::mv(callback);
  }

  template <typename To, typename... Args>
  To& transitionTo(Args&&... args) KJ_LIFETIMEBOUND
    requires(_::isOneOf<To, States...>)
  {
    this->requireUnlocked();
    kj::StringPtr fromName = this->currentStateName();
    auto& result = this->state.template init<To>(kj::fwd<Args>(args)...);
    KJ_IF_SOME(cb, transitionCallback) {
      cb(fromName, _::getStateName<To>());
    }
    return result;
  }

 private:
  kj::Maybe<TransitionCallback> transitionCallback;
};

// =============================================================================
// Deferrable State Machine
// =============================================================================

// A state machine that supports pending/deferred state transitions.
//
// This is useful when:
// - An operation is in progress (e.g., a read)
// - A terminal state change is requested (e.g., close/error)
// - The actual transition should be deferred until the operation completes
//
// The machine tracks a "pending state" separately from the current state.
// When the blocking condition clears, call applyPendingState() to complete
// the deferred transition.
//
// Usage:
//   DeferrableStateMachine<
//       PendingStates<Closed, Errored>,  // States that can be pending
//       Active, Closed, Errored          // All states
//   > state;
//
//   state.transitionTo<Active>();
//   state.beginOperation();  // Mark that an operation is in progress
//
//   // This will defer the transition since an operation is in progress
//   state.deferTransitionTo<Closed>();
//
//   KJ_EXPECT(state.is<Active>());           // Still Active!
//   KJ_EXPECT(state.hasPendingState());      // But Close is pending
//   KJ_EXPECT(state.pendingStateIs<Closed>()); // Specifically, Closed
//
//   state.endOperation();  // Mark operation complete
//   // If no more operations, pending state is automatically applied
//   KJ_EXPECT(state.is<Closed>());           // Now Closed!
//
// The pending state can also be checked to modify behavior:
//   if (state.hasPendingState()) {
//     // Don't start new operations, we're shutting down
//   }

template <typename PendingSpec, typename... States>
class DeferrableStateMachine: public StateMachine<States...> {
  using Base = StateMachine<States...>;
  using PendingUnion = kj::OneOf<States...>;

 public:
  using Base::Base;
  using Base::currentStateName;
  using Base::get;
  using Base::is;
  using Base::isAnyOf;
  using Base::tryGet;

  // ---------------------------------------------------------------------------
  // Operation Tracking
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

  // Mark that an operation is beginning. While operations are in progress,
  // certain transitions will be deferred rather than applied immediately.
  // Prefer scopedOperation() for automatic cleanup.
  void beginOperation() {
    ++operationCount;
  }

  // Mark that an operation has completed. If no more operations are pending
  // and there's a deferred state transition, it will be applied.
  // Returns true if a pending state was applied.
  // Prefer scopedOperation() for automatic cleanup.
  KJ_WARN_UNUSED_RESULT bool endOperation() {
    KJ_REQUIRE(operationCount > 0, "endOperation() called without matching beginOperation()");
    --operationCount;

    if (operationCount == 0 && pendingState != nullptr) {
      applyPendingStateImpl();
      return true;
    }
    return false;
  }

  // Check if any operations are in progress
  bool hasOperationInProgress() const {
    return operationCount > 0;
  }

  // Get the count of in-progress operations
  uint32_t operationCountValue() const {
    return operationCount;
  }

  // RAII guard for operation tracking.
  //
  // EXCEPTION SAFETY: If endOperation() triggers a pending state transition
  // and the state constructor throws, the exception will propagate from the
  // destructor. This is generally acceptable since state machine corruption
  // is unrecoverable, but be aware when using this in exception-sensitive code.
  class OperationScope {
   public:
    explicit OperationScope(DeferrableStateMachine& m): machine(m) {
      machine.beginOperation();
    }
    ~OperationScope() noexcept(false) {
      // Note: endOperation() may throw if pending state constructor throws.
      // We mark this noexcept(false) to be explicit about this.
      // In destructor we don't need to check whether a pending state was applied.
      auto applied KJ_UNUSED = machine.endOperation();
    }

    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
    OperationScope(OperationScope&&) = delete;
    OperationScope& operator=(OperationScope&&) = delete;

   private:
    DeferrableStateMachine& machine;
  };

  // Get an RAII scope for an operation
  OperationScope scopedOperation() {
    return OperationScope(*this);
  }

  // ---------------------------------------------------------------------------
  // Pending State Management
  // ---------------------------------------------------------------------------

  // Check if there's a pending state transition
  bool hasPendingState() const {
    return !(pendingState == nullptr);
  }

  // Check if a specific state is pending
  template <typename S>
  bool pendingStateIs() const
    requires(PendingSpec::template contains<S>)
  {
    return pendingState.template is<S>();
  }

  // Check if the pending state is any of the specified states
  template <typename... Ss>
  bool pendingStateIsAnyOf() const
    requires((PendingSpec::template contains<Ss>) && ...)
  {
    return (pendingState.template is<Ss>() || ...);
  }

  // Get the pending state if it matches the specified type
  template <typename S>
  kj::Maybe<S&> tryGetPendingState() KJ_LIFETIMEBOUND
    requires(PendingSpec::template contains<S>)
  {
    return pendingState.template tryGet<S>();
  }

  template <typename S>
  kj::Maybe<const S&> tryGetPendingState() const KJ_LIFETIMEBOUND
    requires(PendingSpec::template contains<S>)
  {
    return pendingState.template tryGet<S>();
  }

  // Get the name of the pending state (or "(none)" if no pending state)
  kj::StringPtr pendingStateName() const {
    if (pendingState == nullptr) {
      return "(none)"_kj;
    }
    kj::StringPtr result = "(unknown)"_kj;
    // Visit the pending state to get its name
    visitPendingState([&result]<typename S>(const S&) { result = _::getStateName<S>(); });
    return result;
  }

  // Clear any pending state without applying it
  void clearPendingState() {
    pendingState = PendingUnion();
  }

  // Manually apply the pending state (if any).
  // Usually called when all blocking operations complete.
  // Returns true if a pending state was applied.
  KJ_WARN_UNUSED_RESULT bool applyPendingState() {
    if (pendingState == nullptr) {
      return false;
    }
    applyPendingStateImpl();
    return true;
  }

  // ---------------------------------------------------------------------------
  // Deferred Transitions
  // ---------------------------------------------------------------------------

  // Request a transition that will be deferred if operations are in progress.
  // - If no operations in progress: transition happens immediately
  // - If operations in progress: state is stored as pending
  //
  // Only states marked in PendingStates<> can be deferred.
  // Returns true if transition happened immediately, false if deferred.
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
  template <typename S, typename... Args>
  KJ_WARN_UNUSED_RESULT bool deferTransitionTo(Args&&... args)
    requires(PendingSpec::template contains<S>)
  {
    this->requireUnlocked();

    if (operationCount == 0) {
      // No operations in progress, transition immediately
      this->state.template init<S>(kj::fwd<Args>(args)...);
      return true;
    } else {
      // Defer the transition - only store if not already pending (first wins)
      if (pendingState == nullptr) {
        pendingState.template init<S>(kj::fwd<Args>(args)...);
      }
      return false;
    }
  }

  // Request a deferred transition only if currently in a specific state.
  // Returns:
  //   - kj::none if not in From state
  //   - true if transition happened immediately
  //   - false if transition was deferred
  template <typename From, typename To, typename... Args>
  KJ_WARN_UNUSED_RESULT kj::Maybe<bool> deferTransitionFromTo(Args&&... args)
    requires(_::isOneOf<From, States...>) && (PendingSpec::template contains<To>)
  {
    this->requireUnlocked();

    if (!this->template is<From>()) {
      return kj::none;
    }

    return deferTransitionTo<To>(kj::fwd<Args>(args)...);
  }

  // ---------------------------------------------------------------------------
  // Combined State Queries
  // ---------------------------------------------------------------------------

  // Check if the machine is in state S OR has S pending.
  // Useful for "is closed or closing" type checks.
  template <typename S>
  bool isOrPending() const
    requires(_::isOneOf<S, States...>)
  {
    if (this->template is<S>()) {
      return true;
    }
    if constexpr (PendingSpec::template contains<S>) {
      return pendingState.template is<S>();
    }
    return false;
  }

  // Check if any of the specified states are current OR pending
  template <typename... Ss>
  bool isAnyOfOrPending() const {
    return (isOrPending<Ss>() || ...);
  }

  // Get the "effective" state name - pending state if any, otherwise current
  kj::StringPtr effectiveStateName() const {
    if (hasPendingState()) {
      return pendingStateName();
    }
    return this->currentStateName();
  }

 private:
  PendingUnion pendingState;
  uint32_t operationCount = 0;

  void applyPendingStateImpl() {
    // Applying a pending state is a transition, so we must not be locked.
    // This prevents UAF when endOperation() is called inside a withState() callback.
    this->requireUnlocked();

    // Move pending state to current state
    visitPendingState([this]<typename S>(S& s) { this->state.template init<S>(kj::mv(s)); });
    pendingState = PendingUnion();
  }

  template <typename Visitor>
  void visitPendingState(Visitor&& visitor) const {
    visitPendingStateImpl(kj::fwd<Visitor>(visitor), std::index_sequence_for<States...>{});
  }

  template <typename Visitor>
  void visitPendingState(Visitor&& visitor) {
    visitPendingStateImpl(kj::fwd<Visitor>(visitor), std::index_sequence_for<States...>{});
  }

  template <typename Visitor, size_t... Is>
  void visitPendingStateImpl(Visitor&& visitor, std::index_sequence<Is...>) const {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, std::tuple<States...>>;
      if (pendingState.template is<S>()) {
        visitor.template operator()<S>(pendingState.template get<S>());
        return true;
      }
      return false;
    };

    (tryVisit.template operator()<Is>() || ...);
  }

  template <typename Visitor, size_t... Is>
  void visitPendingStateImpl(Visitor&& visitor, std::index_sequence<Is...>) {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, std::tuple<States...>>;
      if (pendingState.template is<S>()) {
        visitor.template operator()<S>(pendingState.template get<S>());
        return true;
      }
      return false;
    };

    (tryVisit.template operator()<Is>() || ...);
  }
};

// =============================================================================
// Common State Types
// =============================================================================

// Pre-defined state types with proper NAME members for introspection.

namespace states {

// Empty state with name - used when no data is needed
struct Empty {
  static constexpr kj::StringPtr NAME = "empty"_kj;
};

// Closed state - commonly used in streams
struct Closed {
  static constexpr kj::StringPtr NAME = "closed"_kj;
};

// Unlocked state - for lock state machines
struct Unlocked {
  static constexpr kj::StringPtr NAME = "unlocked"_kj;
};

// Locked state - for lock state machines
struct Locked {
  static constexpr kj::StringPtr NAME = "locked"_kj;
};

// Generic error state template
template <typename ErrorType>
struct Errored {
  static constexpr kj::StringPtr NAME = "errored"_kj;

  ErrorType error;

  explicit Errored(ErrorType e): error(kj::mv(e)) {}
};

// Initial state - for reader/writer attachment
struct Initial {
  static constexpr kj::StringPtr NAME = "initial"_kj;
};

// Released state - for reader/writer release
struct Released {
  static constexpr kj::StringPtr NAME = "released"_kj;
};

}  // namespace states

// =============================================================================
// Transition Policy Helpers
// =============================================================================

namespace transitions {

// Policy that allows all transitions
using AllowAll = _::AllowAllTransitions;

// Policy that denies all transitions (base for custom policies)
using DenyAll = _::DenyAllTransitions;

// Helper to define individual transitions
template <typename From, typename To>
struct Transition {
  using FromType = From;
  using ToType = To;
};

// Policy from a list of allowed transitions
template <typename... Transitions>
struct TransitionList {
  template <typename From, typename To>
  static constexpr bool isAllowed() {
    return (isMatch<From, To, Transitions>() || ...);
  }

 private:
  template <typename From, typename To, typename T>
  static constexpr bool isMatch() {
    return kj::isSameType<From, typename T::FromType>() && kj::isSameType<To, typename T::ToType>();
  }
};

// Linear policy - only allows transitions to the next state in sequence
template <typename... States>
struct LinearPolicy {
  template <typename From, typename To>
  static constexpr bool isAllowed() {
    return isAllowedImpl<0, From, To, States...>();
  }

 private:
  template <size_t Index,
      typename From,
      typename To,
      typename Current,
      typename Next,
      typename... Rest>
  static constexpr bool isAllowedImpl() {
    if constexpr (kj::isSameType<From, Current>() && kj::isSameType<To, Next>()) {
      return true;
    } else {
      return isAllowedImpl<Index + 1, From, To, Next, Rest...>();
    }
  }

  template <size_t Index, typename From, typename To, typename Current>
  static constexpr bool isAllowedImpl() {
    return false;
  }
};

// Terminal policy - allows transitions TO terminal states from any state,
// but not FROM terminal states
template <typename... TerminalStates>
struct TerminalPolicy {
  template <typename From, typename To>
  static constexpr bool isAllowed() {
    // Cannot transition FROM a terminal state
    if constexpr ((_::isOneOf<From, TerminalStates...>)) {
      return false;
    }
    return true;
  }
};

}  // namespace transitions

// =============================================================================
// Utility Functions
// =============================================================================

// Helper to require a specific state, throwing a typed error if not.
template <typename S, typename... States>
S& requireState(
    StateMachine<States...>& machine KJ_LIFETIMEBOUND, kj::StringPtr message = nullptr) {
  if (message == nullptr) {
    message = "State machine is not in the expected state"_kj;
  }
  KJ_REQUIRE(machine.template is<S>(), message);
  return machine.template get<S>();
}

template <typename S, typename... States>
const S& requireState(
    const StateMachine<States...>& machine KJ_LIFETIMEBOUND, kj::StringPtr message = nullptr) {
  if (message == nullptr) {
    message = "State machine is not in the expected state"_kj;
  }
  KJ_REQUIRE(machine.template is<S>(), message);
  return machine.template get<S>();
}

// Helper for common pattern: do something if in state, return default otherwise.
// LOCKS TRANSITIONS during callback to prevent use-after-free.
template <typename S, typename Machine, typename Func, typename Default>
auto ifInState(Machine& machine,
    Func&& func,
    Default&& defaultValue) -> decltype(func(machine.template get<S>())) {
  if (!machine.template is<S>()) {
    return kj::fwd<Default>(defaultValue);
  }
  auto lock = machine.acquireTransitionLock();
  return func(machine.template get<S>());
}

// =============================================================================
// Composable State Machine
// =============================================================================

// A unified state machine that supports all features via spec types.
// Features are conditionally enabled based on which specs are provided.
//
// Usage:
//   // Simple (no specs)
//   ComposableStateMachine<Idle, Running, Done> simple;
//
//   // With terminal states
//   ComposableStateMachine<TerminalStates<Done>, Idle, Running, Done> withTerminal;
//
//   // Full-featured (stream pattern)
//   ComposableStateMachine<
//       TerminalStates<Closed, Errored>,
//       ErrorState<Errored>,
//       ActiveState<Readable>,
//       PendingStates<Closed, Errored>,
//       Readable, Closed, Errored
//   > stream;
//
// All features from separate classes are available when their spec is provided:
//   - TerminalStates<...> -> isTerminal(), enforces no transitions from terminal
//   - ErrorState<T> -> isErrored(), tryGetError(), getError()
//   - ActiveState<T> -> isActive(), isInactive(), whenActive(), tryGetActive()
//   - PendingStates<...> -> beginOperation(), endOperation(), deferTransitionTo(), etc.

template <typename... Args>
class ComposableStateMachine {
 public:
  // Extract specs from Args
  using TerminalSpec = typename _::FindTerminalStatesSpec<Args...>::Type;
  using ErrorSpec = typename _::FindErrorStateSpec<Args...>::Type;
  using ActiveSpec = typename _::FindActiveStateSpec<Args...>::Type;
  using PendingSpec = typename _::FindPendingStatesSpec<Args...>::Type;

  // Filter out specs to get actual states
  using StatesTuple = _::FilterStatesT<Args...>;
  using StateUnion = _::TupleToOneOfT<StatesTuple>;
  static constexpr size_t STATE_COUNT = _::TupleSize<StatesTuple>::value;

  // Feature detection
  static constexpr bool HAS_TERMINAL = !std::is_void_v<TerminalSpec>;
  static constexpr bool HAS_ERROR = !std::is_void_v<ErrorSpec>;
  static constexpr bool HAS_ACTIVE = !std::is_void_v<ActiveSpec>;
  static constexpr bool HAS_PENDING = !std::is_void_v<PendingSpec>;

  // Get the error state type (PlaceholderType if not specified)
  // Uses helper to avoid accessing ::Type on void
  using ErrorStateType = _::ExtractSpecTypeT<ErrorSpec>;
  using ActiveStateType = _::ExtractSpecTypeT<ActiveSpec>;

 private:
  // ==========================================================================
  // Compile-time validation
  // ==========================================================================

  // Detect duplicate specs
  static_assert(_::CountTerminalStatesSpec<Args...>::value <= 1,
      "Multiple TerminalStates<...> specs provided. Only one is allowed.");
  static_assert(_::CountErrorStateSpec<Args...>::value <= 1,
      "Multiple ErrorState<...> specs provided. Only one is allowed.");
  static_assert(_::CountActiveStateSpec<Args...>::value <= 1,
      "Multiple ActiveState<...> specs provided. Only one is allowed.");
  static_assert(_::CountPendingStatesSpec<Args...>::value <= 1,
      "Multiple PendingStates<...> specs provided. Only one is allowed.");

  // Validate that spec types reference actual states
  static constexpr bool validateErrorSpec() {
    if constexpr (HAS_ERROR) {
      static_assert(_::isInTuple<ErrorStateType, StatesTuple>,
          "ErrorState<T> must reference a type that is one of the state machine's states");
    }
    return true;
  }

  static constexpr bool validateActiveSpec() {
    if constexpr (HAS_ACTIVE) {
      static_assert(_::isInTuple<ActiveStateType, StatesTuple>,
          "ActiveState<T> must reference a type that is one of the state machine's states");
    }
    return true;
  }

  static constexpr bool validateTerminalSpec() {
    if constexpr (HAS_TERMINAL) {
      static_assert(_::ValidateTerminalSpec<StatesTuple, TerminalSpec>::valid,
          "All types in TerminalStates<...> must be actual state types");
    }
    return true;
  }

  static constexpr bool validatePendingSpec() {
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

  // Default constructor: state is uninitialized
  ComposableStateMachine() = default;

  // Destructor checks for outstanding locks
  ~ComposableStateMachine() {
    KJ_DASSERT(transitionLockCount == 0,
        "ComposableStateMachine destroyed while transition locks are held");
  }

  // Move operations - both source and destination must not have locks held
  ComposableStateMachine(ComposableStateMachine&& other) noexcept
      : state(kj::mv(other.state)),
        transitionLockCount(0) {
    KJ_DASSERT(other.transitionLockCount == 0,
        "Cannot move from ComposableStateMachine while transition locks are held");
    if constexpr (HAS_PENDING) {
      operationCount = other.operationCount;
      pendingState = kj::mv(other.pendingState);
      other.operationCount = 0;
    }
  }

  ComposableStateMachine& operator=(ComposableStateMachine&& other) noexcept {
    KJ_DASSERT(transitionLockCount == 0,
        "Cannot move-assign to ComposableStateMachine while transition locks are held");
    KJ_DASSERT(other.transitionLockCount == 0,
        "Cannot move from ComposableStateMachine while transition locks are held");
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
  ComposableStateMachine(const ComposableStateMachine&) = delete;
  ComposableStateMachine& operator=(const ComposableStateMachine&) = delete;

  // Factory function for clearer initialization
  template <typename S, typename... TArgs>
  static ComposableStateMachine create(TArgs&&... args)
    requires(_::isInTuple<S, StatesTuple>)
  {
    ComposableStateMachine m;
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

  template <typename S>
  S& get() KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    requireInitialized();
    KJ_REQUIRE(is<S>(), "State machine is not in the expected state");
    return state.template get<S>();
  }

  template <typename S>
  const S& get() const KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    requireInitialized();
    KJ_REQUIRE(is<S>(), "State machine is not in the expected state");
    return state.template get<S>();
  }

  template <typename S>
  kj::Maybe<S&> tryGet() KJ_LIFETIMEBOUND
    requires(_::isInTuple<S, StatesTuple>)
  {
    return state.template tryGet<S>();
  }

  template <typename S>
  kj::Maybe<const S&> tryGet() const KJ_LIFETIMEBOUND
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

  TransitionLock<ComposableStateMachine> acquireTransitionLock() {
    return TransitionLock<ComposableStateMachine>(*this);
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
  auto withState(
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
  auto withState(Func&& func) const
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

  // Execute with state, providing a default value if not in that state.
  template <typename S, typename Func, typename Default>
  auto withStateOr(Func&& func, Default&& defaultValue) -> decltype(func(kj::instance<S&>()))
    requires(_::isInTuple<S, StatesTuple>)
  {
    if (!is<S>()) {
      return kj::fwd<Default>(defaultValue);
    }

    auto lock = acquireTransitionLock();
    return func(state.template get<S>());
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
    if constexpr (HAS_TERMINAL) {
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

  template <typename From, typename To, typename... TArgs>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromTo(TArgs&&... args) KJ_LIFETIMEBOUND
    requires(_::isInTuple<From, StatesTuple>) && (_::isInTuple<To, StatesTuple>)
  {
    requireUnlocked();
    if (!is<From>()) {
      return kj::none;
    }
    if constexpr (HAS_TERMINAL) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
    }
    if constexpr (HAS_PENDING) {
      clearPendingState();
    }
    return state.template init<To>(kj::fwd<TArgs>(args)...);
  }

  // ---------------------------------------------------------------------------
  // Conditional State Transitions (always available)
  // ---------------------------------------------------------------------------

  // Transition from one state to another only if a predicate is satisfied.
  // The predicate receives a reference to the current From state and should return bool.
  //
  // Returns kj::Maybe<To&>:
  //   - none if not in From state OR predicate returned false
  //   - reference to new To state if transition occurred
  //
  // IMPORTANT: The predicate is evaluated while transitions are locked, but the
  // transition arguments (args...) are captured BEFORE the predicate runs.
  // This means the args must not depend on state that could change.
  //
  // Usage:
  //   state.transitionFromToIf<Reading, Done>(
  //       [](Reading& r) { return r.bytesRemaining == 0; },
  //       doneArgs...);
  template <typename From, typename To, typename Predicate, typename... TArgs>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromToIf(
      Predicate&& predicate, TArgs&&... args) KJ_LIFETIMEBOUND
    requires(_::isInTuple<From, StatesTuple>) && (_::isInTuple<To, StatesTuple>)
  {
    requireUnlocked();

    if (!is<From>()) {
      return kj::none;
    }

    if constexpr (HAS_TERMINAL) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
    }

    // Capture args into a tuple before locking, then evaluate predicate and
    // transition while locked. This ensures the entire operation is atomic.
    auto argsTuple = kj::tuple(kj::fwd<TArgs>(args)...);

    ++transitionLockCount;
    bool shouldTransition = predicate(state.template get<From>());

    if (shouldTransition) {
      // Unlock before transition (transition will check lock count)
      --transitionLockCount;
      if constexpr (HAS_PENDING) {
        clearPendingState();
      }
      return kj::apply([this](auto&&... capturedArgs) -> To& {
        return state.template init<To>(kj::fwd<decltype(capturedArgs)>(capturedArgs)...);
      }, kj::mv(argsTuple));
    }
    --transitionLockCount;
    return kj::none;
  }

  // Transition with predicate that also produces the arguments for the new state.
  // The producer returns kj::Maybe<To> - none means don't transition.
  // The producer is evaluated while transitions are locked, and the transition
  // happens immediately after (still locked) if the producer returns a value.
  //
  // Usage:
  //   state.transitionFromToWith<Reading, Done>(
  //       [](Reading& r) -> kj::Maybe<Done> {
  //         if (r.bytesRemaining == 0) {
  //           return Done { r.totalBytes };
  //         }
  //         return kj::none;
  //       });
  template <typename From, typename To, typename Producer>
  KJ_WARN_UNUSED_RESULT kj::Maybe<To&> transitionFromToWith(Producer&& producer) KJ_LIFETIMEBOUND
    requires(_::isInTuple<From, StatesTuple>) && (_::isInTuple<To, StatesTuple>)
  {
    requireUnlocked();

    if (!is<From>()) {
      return kj::none;
    }

    if constexpr (HAS_TERMINAL) {
      KJ_REQUIRE(!isTerminal(), "Cannot transition from terminal state");
    }

    // Evaluate producer while locked. If it returns a value, transition
    // immediately (unlock first since init requires unlocked).
    ++transitionLockCount;
    kj::Maybe<To> maybeNewState = producer(state.template get<From>());
    --transitionLockCount;

    KJ_IF_SOME(newState, maybeNewState) {
      if constexpr (HAS_PENDING) {
        clearPendingState();
      }
      return state.template init<To>(kj::mv(newState));
    }
    return kj::none;
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
  // Terminal State Features (enabled when TerminalStates<...> is provided)
  // ---------------------------------------------------------------------------

  // Check if currently in a terminal state (no further transitions allowed).
  bool isTerminal() const
    requires(HAS_TERMINAL)
  {
    return TerminalSpec::isTerminal(*this);
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
  // WARNING: This returns an UNLOCKED reference - same risks as get()/tryGet().
  // Error states are typically terminal so the risk is lower, but the reference
  // can still dangle if forceTransitionTo() is used.
  kj::Maybe<ErrorStateType&> tryGetError() KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return tryGet<ErrorStateType>();
  }

  kj::Maybe<const ErrorStateType&> tryGetError() const KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return tryGet<ErrorStateType>();
  }

  // Get the error state, asserting we are errored.
  ErrorStateType& getError() KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return get<ErrorStateType>();
  }

  const ErrorStateType& getError() const KJ_LIFETIMEBOUND
    requires(HAS_ERROR)
  {
    return get<ErrorStateType>();
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
  // WARNING: This returns an UNLOCKED reference - same risks as get()/tryGet().
  // The reference can dangle if the machine transitions. Prefer whenActive()
  // for safe access with locked transitions.
  kj::Maybe<ActiveStateType&> tryGetActive() KJ_LIFETIMEBOUND
    requires(HAS_ACTIVE)
  {
    return tryGet<ActiveStateType>();
  }

  kj::Maybe<const ActiveStateType&> tryGetActive() const KJ_LIFETIMEBOUND
    requires(HAS_ACTIVE)
  {
    return tryGet<ActiveStateType>();
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
    return withState<ActiveStateType>(kj::fwd<Func>(func));
  }

  template <typename Func>
  auto whenActive(Func&& func) const
      -> std::conditional_t<std::is_void_v<decltype(func(kj::instance<const ActiveStateType&>()))>,
          bool,
          kj::Maybe<decltype(func(kj::instance<const ActiveStateType&>()))>>
    requires(HAS_ACTIVE)
  {
    return withState<ActiveStateType>(kj::fwd<Func>(func));
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

  // Get the count of in-progress operations.
  uint32_t operationCountValue() const
    requires(HAS_PENDING)
  {
    return operationCount;
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
  template <typename S>
  kj::Maybe<S&> tryGetPendingState() KJ_LIFETIMEBOUND
    requires(HAS_PENDING) && (PendingSpec::template contains<S>)
  {
    return pendingState.template tryGet<S>();
  }

  template <typename S>
  kj::Maybe<const S&> tryGetPendingState() const KJ_LIFETIMEBOUND
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
    if constexpr (HAS_TERMINAL) {
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
    visitPendingStateNames([&result]<typename S>(const S&) { result = _::getStateName<S>(); });
    return result;
  }

  // RAII guard for operation tracking.
  //
  // EXCEPTION SAFETY: If endOperation() triggers a pending state transition
  // and the state constructor throws, the exception will propagate from the
  // destructor. This is generally acceptable since state machine corruption
  // is unrecoverable, but be aware when using this in exception-sensitive code.
  class OperationScope {
   public:
    explicit OperationScope(ComposableStateMachine& m): machine(m) {
      machine.beginOperation();
    }

    ~OperationScope() noexcept(false) {
      // Note: endOperation() may throw if pending state constructor throws.
      // We mark this noexcept(false) to be explicit about this.
      // In destructor we don't need to check whether a pending state was applied.
      auto applied KJ_UNUSED = machine.endOperation();
    }

    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
    OperationScope(OperationScope&&) = delete;
    OperationScope& operator=(OperationScope&&) = delete;

   private:
    ComposableStateMachine& machine;
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
  // - Migrating existing code to use ComposableStateMachine
  // - Implementing new patterns that the state machine doesn't support yet
  // - Interfacing with APIs that expect kj::OneOf directly
  //
  // STRONGLY PREFER: withState(), transitionTo(), and other type-safe methods.
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
  // For safe access, use withState() instead:
  //
  //   machine.withState<Active>([](Active& active) {
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
  StateUnion state;
  mutable uint32_t transitionLockCount = 0;

  // Pending state support (only allocated when HAS_PENDING is true)
  // Using _::Empty instead of char for proper [[no_unique_address]] optimization
  [[no_unique_address]] std::conditional_t<HAS_PENDING, StateUnion, _::Empty> pendingState{};
  [[no_unique_address]] std::conditional_t<HAS_PENDING, uint32_t, _::Empty> operationCount{};

  void requireUnlocked() const {
    KJ_REQUIRE(transitionLockCount == 0,
        "Cannot transition state machine while transitions are locked. "
        "This usually means you're trying to transition inside a withState() callback.");
  }

  // Helper to visit state names (using index sequence)
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
        return true;
      }
      return false;
    };
    (tryVisit.template operator()<Is>() || ...);
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
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
    } else {
      ReturnType result{};
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, StatesTuple>;
        if (state.template is<S>()) {
          result = visitor(state.template get<S>());
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
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
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
    } else {
      ReturnType result{};
      auto tryVisit = [&]<size_t I>() {
        using S = std::tuple_element_t<I, StatesTuple>;
        if (state.template is<S>()) {
          result = visitor(state.template get<S>());
          return true;
        }
        return false;
      };
      (tryVisit.template operator()<Is>() || ...);
      return result;
    }
  }

  void applyPendingStateImpl()
    requires(HAS_PENDING)
  {
    // Applying a pending state is a transition, so we must not be locked.
    // This prevents UAF when endOperation() is called inside a withState() callback:
    //
    //   machine.withState<Active>([&](Active& a) {
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
    if constexpr (HAS_TERMINAL) {
      if (TerminalSpec::isTerminal(*this)) {
        // Already in terminal state, discard the pending state
        pendingState = StateUnion();
        return;
      }
    }

    visitPendingStateNames([this]<typename S>(S& s) { this->state.template init<S>(kj::mv(s)); });
    pendingState = StateUnion();
  }

  template <typename Visitor>
  void visitPendingStateNames(Visitor&& visitor) const
    requires(HAS_PENDING)
  {
    visitPendingStateNamesImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor>
  void visitPendingStateNames(Visitor&& visitor)
    requires(HAS_PENDING)
  {
    visitPendingStateNamesImpl(kj::fwd<Visitor>(visitor), std::make_index_sequence<STATE_COUNT>{});
  }

  template <typename Visitor, size_t... Is>
  void visitPendingStateNamesImpl(Visitor&& visitor, std::index_sequence<Is...>) const
    requires(HAS_PENDING)
  {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (pendingState.template is<S>()) {
        visitor.template operator()<S>(pendingState.template get<S>());
        return true;
      }
      return false;
    };
    (tryVisit.template operator()<Is>() || ...);
  }

  template <typename Visitor, size_t... Is>
  void visitPendingStateNamesImpl(Visitor&& visitor, std::index_sequence<Is...>)
    requires(HAS_PENDING)
  {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (pendingState.template is<S>()) {
        visitor.template operator()<S>(pendingState.template get<S>());
        return true;
      }
      return false;
    };
    (tryVisit.template operator()<Is>() || ...);
  }

  // Helper for visitForGc - visits the current state if the visitor can handle it
  template <typename Visitor, size_t... Is>
  void visitForGcImpl(Visitor& visitor, std::index_sequence<Is...>) {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (state.template is<S>()) {
        // Only call visit if the visitor can handle this type
        if constexpr (requires { visitor.visit(state.template get<S>()); }) {
          visitor.visit(state.template get<S>());
        }
        return true;
      }
      return false;
    };
    (tryVisit.template operator()<Is>() || ...);
  }

  template <typename Visitor, size_t... Is>
  void visitForGcImpl(Visitor& visitor, std::index_sequence<Is...>) const {
    auto tryVisit = [&]<size_t I>() {
      using S = std::tuple_element_t<I, StatesTuple>;
      if (state.template is<S>()) {
        // Only call visit if the visitor can handle this type
        if constexpr (requires { visitor.visit(state.template get<S>()); }) {
          visitor.visit(state.template get<S>());
        }
        return true;
      }
      return false;
    };
    (tryVisit.template operator()<Is>() || ...);
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
//   struct Readable {
//     static constexpr kj::StringPtr NAME = "readable"_kj;
//     kj::Own<ReadableStreamSource> source;
//   };
//
//   struct Closed {
//     static constexpr kj::StringPtr NAME = "closed"_kj;
//   };
//
//   struct Errored {
//     static constexpr kj::StringPtr NAME = "errored"_kj;
//     jsg::Value error;
//     auto getHandle(jsg::Lock& js) { return error.getHandle(js); }
//   };
//
//   // Use ResourceStateMachine for automatic active/closed/errored handling
//   ResourceStateMachine<Readable, Closed, Errored> state;
//
//   // Initialize
//   state.transitionTo<Readable>(kj::mv(source));
//
//   // Check state
//   if (state.isActive()) { ... }
//   if (state.isClosedOrErrored()) { ... }
//
//   // RECOMMENDED: Use whenActive() for safe access (transitions locked)
//   state.whenActive([](Readable& r) {
//     r.source->doSomething();  // Safe - transitions blocked during callback
//   });
//
//   // whenActive() can return values
//   auto result = state.whenActive([](Readable& r) {
//     return r.source->read();  // Returns kj::Maybe of the result
//   });
//
//   // CAUTION: tryGetActive() does NOT lock - use carefully
//   KJ_IF_SOME(readable, state.tryGetActive()) {
//     // Don't transition in this scope or readable becomes dangling!
//     readable.source->read(...);
//   }
//
//   // Semantic transitions
//   state.close();                      // -> Closed
//   state.error(Errored{js.v8Ref(x)});  // -> Errored
//
// Example 2: Terminal State Enforcement
// -------------------------------------
//
//   TerminalStateMachine<
//       TerminalStates<Closed, Errored>,  // Mark terminal states
//       Readable, Closed, Errored         // All states
//   > state;
//
//   state.transitionTo<Readable>(...);
//
//   // This works
//   state.transitionTo<Closed>();
//
//   // This throws! Cannot leave terminal state
//   state.transitionTo<Readable>(...);  // KJ_REQUIRE fails
//
//   // For cleanup, use forceTransitionTo
//   state.forceTransitionTo<Readable>(...);  // Allowed
//
// Example 3: Error Extraction
// ---------------------------
//
//   ErrorableStateMachine<Readable, Closed, Errored> state;
//
//   // Old pattern (verbose):
//   KJ_IF_SOME(errored, state.tryGet<Errored>()) {
//     return errored.getHandle(js);
//   }
//
//   // New pattern (cleaner):
//   KJ_IF_SOME(errored, state.tryGetError()) {
//     return errored.getHandle(js);
//   }
//
//   // Or simply:
//   if (state.isErrored()) {
//     auto& err = state.getError();
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
// Example 5: Observable State Machine for Debugging
// -------------------------------------------------
//
//   ObservableStateMachine<Idle, Running, Done> state;
//
//   state.onTransition([](kj::StringPtr from, kj::StringPtr to) {
//     KJ_LOG(INFO, "State transition", from, "->", to);
//   });
//
//   state.transitionTo<Idle>();    // Logs: (uninitialized) -> idle
//   state.transitionTo<Running>(); // Logs: idle -> running
//   state.transitionTo<Done>();    // Logs: running -> done
//
// Example 6: Lock State Machine
// -----------------------------
//
//   struct ReaderLocked {
//     static constexpr kj::StringPtr NAME = "reader_locked"_kj;
//     jsg::PromiseResolverPair<void> closedPromise;
//   };
//
//   using LockState = TerminalStateMachine<
//       states::Unlocked, states::Locked, ReaderLocked>
//       ::WithTerminal<>;  // No terminal states - locks can always be released
//
//   LockState lockState;
//   lockState.transitionTo<states::Unlocked>();
//
//   // Acquire lock
//   if (lockState.is<states::Unlocked>()) {
//     lockState.transitionTo<ReaderLocked>(kj::mv(promise));
//   }
//
//   // Release lock
//   lockState.transitionTo<states::Unlocked>();
//
// Example 7: Compile-Time Validated Transitions
// ---------------------------------------------
//
//   using MyTransitions = transitions::TransitionList<
//     transitions::Transition<Idle, Running>,
//     transitions::Transition<Running, Paused>,
//     transitions::Transition<Running, Done>,
//     transitions::Transition<Paused, Running>,
//     transitions::Transition<Paused, Done>
//   >;
//
//   ValidatedStateMachine<MyTransitions, Idle, Running, Paused, Done> state;
//
//   state.transitionTo<Idle>();
//
//   // This compiles - Idle -> Running is allowed
//   state.checkedTransitionFromTo<Idle, Running>();
//
//   // This would NOT compile - Running -> Idle is not in the list
//   // state.checkedTransitionFromTo<Running, Idle>();  // Compile error!
//
// Example 8: Safe State Access with withState()
// ----------------------------------------------
//
//   StateMachine<Active, Paused, Done> state;
//   state.transitionTo<Active>();
//
//   // SAFE: withState() locks transitions during callback
//   auto result = state.withState<Active>([](Active& a) {
//     return a.computeResult();  // a is guaranteed valid
//   });  // Returns kj::Maybe<ResultType>
//
//   // Handle result after callback (transitions now allowed)
//   KJ_IF_SOME(r, result) {
//     state.transitionTo<Done>(kj::mv(r));
//   }
//
//   // withStateOr() for default values
//   auto value = state.withStateOr<Active>(
//       [](Active& a) { return a.getValue(); },
//       defaultValue);
//
//   // UNSAFE patterns to avoid:
//   //
//   //   Active& a = state.get<Active>();  // Reference not locked!
//   //   state.transitionTo<Done>();       // a is now dangling!
//   //   a.doSomething();                  // USE-AFTER-FREE!
//   //
//   //   state.withState<Active>([&](Active& a) {
//   //     state.transitionTo<Done>();     // FAILS - transitions locked!
//   //   });
//
// Example 9: Using with KJ_SWITCH_ONEOF
// -------------------------------------
//
//   // StateMachine works directly with KJ_SWITCH_ONEOF
//   ResourceStateMachine<Readable, Closed, Errored> state;
//
//   // NOTE: KJ_SWITCH_ONEOF does NOT lock transitions.
//   // Only use for read-only operations or when you control all code paths.
//   KJ_SWITCH_ONEOF(state) {
//     KJ_CASE_ONEOF(readable, Readable) {
//       // Don't call code that might transition the state!
//       readable.source->read(...);
//     }
//     KJ_CASE_ONEOF(closed, Closed) {
//       // Handle closed
//     }
//     KJ_CASE_ONEOF(errored, Errored) {
//       // Handle error
//     }
//   }
//
// Example 10: Visitor Pattern
// ---------------------------
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
// Example 11: Manual Transition Locking
// -------------------------------------
//
//   StateMachine<Active, Paused, Done> state;
//   state.transitionTo<Active>();
//
//   // For complex operations that need multiple state accesses
//   {
//     auto lock = state.acquireTransitionLock();
//
//     // All transitions blocked while lock is held
//     auto& active = state.get<Active>();
//     active.doStep1();
//     active.doStep2();
//     active.doStep3();
//
//   }  // lock released, transitions now allowed
//
//   state.transitionTo<Done>();
//
// Example 12: Conditional Transitions
// ------------------------------------
//
//   struct Reading {
//     size_t bytesRemaining;
//     size_t totalBytes;
//   };
//   struct Done { size_t totalBytesRead; };
//
//   StateMachine<Idle, Reading, Done> state;
//   state.transitionTo<Reading>(...);
//
//   // Transition only if predicate is satisfied (atomic check + transition)
//   state.transitionFromToIf<Reading, Done>(
//       [](Reading& r) { return r.bytesRemaining == 0; },
//       totalBytes);  // Args for Done constructor
//
//   // Or use transitionFromToWith when the new state depends on the old:
//   state.transitionFromToWith<Reading, Done>(
//       [](Reading& r) -> kj::Maybe<Done> {
//         if (r.bytesRemaining == 0) {
//           return Done{r.totalBytes};  // Produce the new state
//         }
//         return kj::none;  // Don't transition
//       });
//
// Example 13: Deferred State Transitions
// --------------------------------------
//
//   // For deferring close/error until pending operations complete
//   DeferrableStateMachine<
//       PendingStates<Closed, Errored>,  // States that can be deferred
//       Active, Closed, Errored          // All states
//   > state;
//
//   state.transitionTo<Active>();
//
//   // Start a read operation
//   state.beginOperation();  // Or: auto scope = state.scopedOperation();
//
//   // Close is requested, but we're mid-operation - defer it
//   state.deferTransitionTo<Closed>();
//
//   KJ_EXPECT(state.is<Active>());         // Still active!
//   KJ_EXPECT(state.hasPendingState());    // Close is pending
//   KJ_EXPECT(state.isOrPending<Closed>()); // "Is closed or closing"
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
// =============================================================================
// STREAMS INTEGRATION GUIDE
// =============================================================================
//
// The following shows how existing streams code could use these utilities:
//
// ReadableStreamInternalController:
// ---------------------------------
//   // Current:
//   kj::OneOf<StreamStates::Closed, StreamStates::Errored, Readable> state;
//
//   // With utility:
//   ResourceStateMachine<Readable, StreamStates::Closed, StreamStates::Errored> state;
//
//   // Benefits:
//   // - isClosedOrErrored() built-in
//   // - whenActive() for safe resource access with transition locking
//   // - close()/error() with terminal enforcement
//
//   // Safe read pattern:
//   state.whenActive([&](Readable& r) {
//     return r.source->read(js, ...);  // Transitions blocked
//   });
//
// ReaderImpl:
// -----------
//   // Current:
//   kj::OneOf<Initial, Attached, StreamStates::Closed, Released> state;
//
//   // With utility:
//   TerminalStateMachine<
//       TerminalStates<StreamStates::Closed, Released>,
//       Initial, Attached, StreamStates::Closed, Released> state;
//
//   // Benefits:
//   // - Cannot accidentally transition out of Closed/Released
//   // - currentStateName() for debugging
//   // - withState() for safe access to Attached state
//
// ReadableSourceKjAdapter::Active:
// ---------------------------------
//   // Current (complex 6-state machine):
//   kj::OneOf<Idle, Readable, Reading, Done, Canceling, Canceled> state;
//
//   // With utility:
//   TerminalStateMachine<
//       TerminalStates<Canceled>,
//       Idle, Readable, Reading, Done, Canceling, Canceled> state;
//
//   // Benefits:
//   // - Cannot transition out of Canceled state
//   // - Clear documentation of terminal vs non-terminal states
//   // - Transition locking prevents use-after-free in callbacks
//
// ReadableStreamJsController (deferred close pattern):
// ----------------------------------------------------
//   // Current:
//   kj::Maybe<kj::OneOf<Closed, Errored>> maybePendingState;
//   size_t pendingReadCount = 0;
//
//   void doClose(jsg::Lock& js) {
//     if (isReadPending()) {
//       setPendingState(Closed{});
//     } else {
//       state.init<Closed>();
//     }
//   }
//
//   // With utility:
//   DeferrableStateMachine<
//       PendingStates<Closed, Errored>,
//       Active, Closed, Errored> state;
//
//   void doClose(jsg::Lock& js) {
//     state.deferTransitionTo<Closed>();  // Handles pending automatically
//   }
//
//   jsg::Promise<ReadResult> read(jsg::Lock& js) {
//     auto scope = state.scopedOperation();  // Track pending read
//     // ... do read ...
//   }  // Pending close applied when scope ends
//
//   bool isClosedOrErrored() const {
//     return state.isAnyOfOrPending<Closed, Errored>();
//   }
//
