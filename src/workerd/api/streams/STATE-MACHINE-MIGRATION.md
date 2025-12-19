# State Machine Migration Tracking

This document tracks the migration of `kj::OneOf` state machines in `api/streams` to use the new `StateMachine` utilities from `util/state-machine.h`.

Reference commit: `a990c7269` (compression.c++ conversion example)

---

## Tier 1: Very Easy (Direct Pattern Match)

These follow the exact pattern of the compression.c++ conversion (Active/Closed/Error with simple transitions):

- [x] **writable-sink.c++:169** - `WritableSinkImpl`
  - `kj::OneOf<kj::Own<kj::AsyncOutputStream>, Closed, kj::Exception>`
  - Pattern: 3-state (Active→Closed, Active→Error)
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, kj::Exception>, ErrorState<kj::Exception>, ActiveState<Open>>`

- [x] **readable-source.c++:459** - `ReadableSourceImpl`
  - `kj::OneOf<kj::Own<kj::AsyncInputStream>, Closed, kj::Exception>`
  - Pattern: Identical to writable-sink.c++
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, kj::Exception>, ErrorState<kj::Exception>, ActiveState<Open>>`

- [x] **writable-sink-adapter.h:251** - `WritableStreamSinkJsAdapter`
  - `kj::OneOf<IoOwn<Active>, Closed, kj::Exception>`
  - Pattern: Same 3-state with IoOwn wrapper
  - **DONE**: Converted to `ComposableStateMachine` with `Open` wrapper struct containing `IoOwn<Active>`

- [x] **writable-sink-adapter.h:442** - `WritableSinkKjAdapter`
  - `kj::OneOf<kj::Own<Active>, Closed, kj::Exception>`
  - Pattern: Same 3-state
  - **DONE**: Converted to `ComposableStateMachine` with `KjOpen` wrapper struct containing `kj::Own<Active>`

- [x] **readable-source-adapter.h:236** - `ReadableStreamSourceJsAdapter`
  - `kj::OneOf<IoOwn<Active>, Closed, kj::Exception>`
  - Pattern: Same 3-state with IoOwn wrapper
  - **DONE**: Converted to `ComposableStateMachine` with `Open` wrapper struct containing `IoOwn<Active>`

- [x] **readable-source-adapter.h:361** - `ReadableSourceKjAdapter`
  - `kj::OneOf<kj::Own<Active>, Closed, kj::Exception>`
  - Pattern: Same 3-state
  - **DONE**: Converted to `ComposableStateMachine` with `KjOpen` wrapper struct containing `kj::Own<Active>`

---

## Tier 2: Easy (Similar Pattern, Minor Variations)

- [x] **identity-transform-stream.c++:305** - `IdentityTransformStreamImpl`
  - `kj::OneOf<Idle, ReadRequest, WriteRequest, kj::Exception, StreamStates::Closed>`
  - 5 states with clear lifecycle
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, kj::Exception>, ErrorState<kj::Exception>, ...>` (both Closed and Exception are terminal; abort() uses forceTransitionTo for Closed→Exception)

- [x] **queue.h:296** - `QueueImpl`
  - `kj::OneOf<Ready, Closed, Errored>` (Errored = jsg::Value)
  - Note: Errored type alias needs struct wrapper for ErrorState<>
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, Errored>, ErrorState<Errored>, ActiveState<Ready>>` with `Errored` struct wrapping `jsg::Value reason`

- [x] **queue.h:562** - `ConsumerImpl`
  - `kj::OneOf<Ready, Closed, Errored>`
  - Same as QueueImpl
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, Errored>, ErrorState<Errored>, ActiveState<Ready>>` with shared `Errored` struct

---

## Tier 3: Medium (Lock State Machines)

These track lock states - different semantic pattern, may not benefit as much:

- [x] **readable.h:57** - `ReaderImpl`
  - `kj::OneOf<Initial, Attached, StreamStates::Closed, Released>`
  - Reader attachment tracking
  - Closed = stream ended naturally or errored (detach() called)
  - Released = user explicitly released the lock (releaseLock() called)
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, Released>, ActiveState<Attached>>` with `Attached` struct wrapping `jsg::Ref<ReadableStream>`

- [x] **writable.h:95** - `WritableStreamDefaultWriter`
  - `kj::OneOf<Initial, Attached, Released, StreamStates::Closed>`
  - Writer attachment tracking
  - Closed = stream ended naturally or errored (detach() called)
  - Released = user explicitly released the lock (releaseLock() called)
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, Released>, ActiveState<Attached>>` with `Attached` struct wrapping `jsg::Ref<WritableStream>`

- [x] **standard.c++:143** - `ReadableLockImpl`
  - `kj::OneOf<Locked, PipeLocked, ReaderLocked, Unlocked>`
  - Lock state (no terminal states - cyclic transitions allowed)
  - **DONE**: Converted to `ComposableStateMachine<Locked, PipeLocked, ReaderLocked, Unlocked>` (no TerminalStates since all states can transition to other states)
  - Note: Also added NAME constants to `Unlocked`, `Locked`, `ReaderLocked`, `WriterLocked` in common.h

- [x] **standard.c++:212** - `WritableLockImpl`
  - `kj::OneOf<Unlocked, Locked, WriterLocked, PipeLocked>`
  - Lock state (no terminal states - cyclic transitions allowed)
  - **DONE**: Converted to `ComposableStateMachine<Unlocked, Locked, WriterLocked, PipeLocked>`
  - Bug fix: Changed `releaseWriter()` to use `tryGet<>()` instead of `get<>()` (matching `releaseReader()` pattern)

- [x] **internal.h:149** - `ReadableStreamInternalController::readState`
  - `kj::OneOf<Unlocked, Locked, PipeLocked, ReaderLocked>`
  - Lock state (no terminal states - cyclic transitions allowed)
  - **DONE**: Converted to `ComposableStateMachine<Unlocked, Locked, PipeLocked, ReaderLocked>`
  - Note: Also added NAME constant to `PipeLocked` class

- [x] **internal.h:271** - `WritableStreamInternalController::writeState`
  - `kj::OneOf<Unlocked, Locked, PipeLocked, WriterLocked>`
  - Lock state (no terminal states - cyclic transitions allowed)
  - **DONE**: Converted to `ComposableStateMachine<Unlocked, Locked, PipeLocked, WriterLocked>`
  - Note: Also added NAME constant to `PipeLocked` struct

---

## Tier 4: Hard (Complex Multi-State with Pending State Logic)

- [x] **standard.h:221** - `ReadableImpl` (template)
  - `kj::OneOf<StreamStates::Closed, StreamStates::Errored, Queue>`
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Closed, Errored>, ActiveState<Queue>, ...>`
  - Uses `isInactive()` for checking terminal states, `transitionTo<>()` for transitions
  - Note: Added NAME constant to `StreamStates::Closed` and `StreamStates::Erroring`

- [ ] **standard.h:370** - `WritableImpl` (template)
  - `kj::OneOf<StreamStates::Closed, StreamStates::Errored, StreamStates::Erroring, Writable>`
  - 4 states including Erroring transition state

- [ ] **standard.c++:715-728** - `ReadableStreamJsController`
  - Main + pending state machines
  - Perfect candidate for PendingStates<>

- [ ] **standard.c++:848** - `WritableStreamJsController`
  - `kj::OneOf<StreamStates::Closed, StreamStates::Errored, Controller>`

- [ ] **internal.h:148** - `ReadableStreamInternalController::state`
  - `kj::OneOf<StreamStates::Closed, StreamStates::Errored, Readable>`

- [ ] **internal.h:270** - `WritableStreamInternalController::state`
  - `kj::OneOf<StreamStates::Closed, StreamStates::Errored, IoOwn<Writable>>`

- [ ] **standard.c++:2713** - Tee class state
  - `kj::OneOf<StreamStates::Closed, StreamStates::Errored, jsg::Ref<ReadableStream>>`

- [ ] **standard.c++:2843** - PumpToReader state
  - `kj::OneOf<Pumping, StreamStates::Closed, kj::Exception, jsg::Ref<ReadableStream>>`

- [x] **readable-source-adapter.c++:519** - `Active` inner class
  - `kj::OneOf<Idle, Readable, Reading, Done, Canceling, Canceled>`
  - Most complex: 6 states with multiple terminals
  - **DONE**: Converted to `ComposableStateMachine<TerminalStates<Done, Canceling, Canceled>, ...>` (no ErrorState/ActiveState since transitions are more complex)

---

## Not Candidates for Conversion

These use `kj::OneOf` as variant types, not state machines:

- `internal.h:342` - Event type variant
- `standard.c++:692` - Return type variant
- `standard.c++:2874` - Local Result type
- `readable-source-adapter.c++:573` - JsByteSource type
- `common.h:88` - Controller type variant
- `readable.h:241` - Reader variant type
- `queue.h:549` - Buffer entry type

---

## Notes

- Each conversion should be a separate commit for easy review/revert
- Run tests after each conversion: `bazel test //src/workerd/api:streams_test`
- Reference compression.c++ (commit a990c7269) for migration pattern
