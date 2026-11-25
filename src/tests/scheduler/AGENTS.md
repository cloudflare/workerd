# scheduler

An informal specification of the `scheduler` global and its `wait()` as
implemented in workerd, kept in lockstep with the test suite in this
directory. **The tests are the normative artifact.** The cells run the same
modules (`scheduler-modules.capnp`):

| Cell | Implementation | Flags |
| --- | --- | --- |
| `scheduler-cpp` | `Scheduler` in `api/basics.{h,c++}` | those the variant's date gives, so `set_tostring_tag` differs between `@` and `@all-compat-flags` |
| `scheduler-opt-in-cpp` | the same | also `set_tostring_tag` at either date, `spec_compliant_property_attributes` and `immutable_api_prototypes` (`--experimental`) |
| `scheduler-ts`, `scheduler-opt-in-ts` | `src/per_isolate/scheduler.ts`, under the `TS_SCHEDULER` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. The C++ cells have no `@all-autogates`
variant, since enabling every autogate enables `TS_SCHEDULER`.

Every cell has two services: the suite (`main.js`), and a worker of its own
(`first-read.js`) whose top level replaces built-ins before the first read
of `scheduler`, which makes the object, and which then makes the changes to
the global that cannot be undone.

`//src/workerd/api/tests:startup-snapshot-scheduler-test` holds the object
across a startup snapshot; its `@all-autogates` variant runs the TypeScript
implementation through the snapshot pipeline.

## Core semantics

- **The global:** `scheduler` is a writable, enumerable, configurable own
  data property of the global, whatever the flags, made on its first read
  and the same object from then on. User code can assign or delete it;
  nothing makes another (`shape.js`, `first-read.js`).
- **The object:** it has no own properties and inherits from a prototype
  that holds `wait`, `constructor` and JSG's marks (`Symbol.toStringTag`
  under `set_tostring_tag`, the internal-class marker, the inspect hook), in
  that order. It is not serializable.
- **The constructor:** reachable only as `scheduler.constructor`: a function
  named `Scheduler` of length 0, whose `prototype` is writable unless
  `immutable_api_prototypes`. Constructed, called or reached from a
  subclass, it throws `TypeError: Illegal constructor`, whose stack starts
  at the caller.
- **`wait(delay, options)`:** a function named `wait` that is not a
  constructor, of length 1 under `spec_compliant_property_attributes` and 0
  without (`shape.js`). In order (`wait.js`):
  1. A receiver other than the scheduler throws the "Illegal invocation"
     `TypeError`. Every later failure but an abort rejects the promise under
     `capture_async_api_throws`, and is thrown without.
  2. No arguments fail with a `TypeError`. Otherwise `delay` is converted
     to a number; what the conversion throws is the failure.
  3. `options` that are neither `undefined`, `null` nor an object fail with
     a `TypeError`. `signal` is read from them once; a value other than
     `undefined`, an `AbortSignal` or an object inheriting from one fails
     with a `TypeError`.
  4. Outside of a request the call fails with the runtime's "Disallowed
     operation called within global scope" `Error`.
  5. A signal that has aborted rejects the promise. Otherwise the promise
     resolves with `undefined` after `delay` milliseconds on the request's
     timers, among those of `setTimeout()`, or rejects when the signal
     aborts first, which clears the timer.
- **The rejection of an abort:** the reason crosses the runtime's event
  loop. An error arrives as a new error of the same type and message, a
  primitive as an `Error` whose message describes it, and the rejection
  comes after the `'abort'` event and the microtasks queued by then.
- **Loading:** the TypeScript implementation loads at the first read of the
  global (`src/per_isolate/main.ts`), so a Worker that does not use it does
  not pay for it.
- **Pollution:** `wait()` calls or reads nothing user code can replace
  (`wait.js`), nor does making the object, but for what loading the
  TypeScript implementation calls (ledger row 3, `first-read.js`).

## Ledger

Where the TypeScript implementation differs from the C++. Each row is pinned
on both sides with a `usingTsImpl` branch.

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | `Function.prototype.toString()` of `scheduler.constructor` and of `wait`, and the error messages that quote it, such as that of assigning the constructor's `prototype` under `immutable_api_prototypes` | `function Scheduler() { [native code] }`, `function wait() { [native code] }` | the function's source, which V8 abbreviates in a message | `shape.js` |
| 2 | Position of `scheduler` among the global's own properties, which `for...in` over the global follows | where `global-scope.h` registers it, before `performance` | after the properties JSG registers | `shape.js` |
| 3 | User code that replaced the global `Object.defineProperty` before the first read of `scheduler` | not called | called as the module loads, with the module's initial `exports` object (empty, and used by nothing else), `'__esModule'` and `{ value: true }`; what it throws fails that read, and the next read loads the module again | `first-read.js` |
| 4 | Stack of the errors of the delay's conversion and of a `signal` that is no `AbortSignal` | starts at the caller | starts at the implementation's `wait()`, then the caller | `wait.js` |
