# src/per_isolate/performance/

TypeScript implementations of `Performance.prototype.now` and the
`Performance.prototype.timeOrigin` getter. Parent directory conventions
(primordials discipline, private-brand dispatch, no `instanceof`) apply; see
`src/per_isolate/AGENTS.md`.

## SCOPE AND GATING

The class itself is the JSG `Performance` (`src/workerd/api/performance.h`),
and so is the lazily created `globalThis.performance` instance. `now.ts`
replaces just two of its prototype members.

The install is gated by the `typescript-performance-now` autogate
(`util/autogate.h`), not by a compat flag, so every worker in a process with
the gate on gets it. That works only because the swap is meant to be
unobservable, apart from the divergences listed below. Keep it that way, or
put it behind a flag.

When the autogate is removed, the JSG members go with it, and `install()`
defines the members itself instead of copying their shape (see the TODO on
`install()` in `now.ts`).

## FILE MAP

| File     | Role                                                        |
| -------- | ----------------------------------------------------------- |
| `now.ts` | `now` and the `timeOrigin` getter, plus their `install()` |

`main.ts` calls `install(utils.getPerformancePrototype())` when the autogate
is on.

## THE C++ CONTRACT

The native half is declared in `src/workerd/api/performance-bootstrap.h`,
implemented in `performance.c++`, and wired into `utils` in
`src/workerd/api/per-isolate-bootstrap.c++`. `createUtilsObject()` builds
these natives only when the autogate is on, and otherwise leaves them off
`utils`, so with the gate off the bootstrap neither depends on `Performance`
nor pays for the template and tag-range lookups:

- **`utils.performanceNow()`** calls `Performance::now()`. The clock stays
  native for two reasons: it is the IoContext's Spectre-safe time
  (`dateNow()`, which is 0 outside a request), and `now()` reports a
  `markPerfEvent("performance_now")` to the isolate limit enforcer. Calling a
  captured `Date.now` from TS would lose the perf event.
- **`utils.requirePerformance()`** does nothing. It exists for its receiver
  check, for the `timeOrigin` getter, which is the constant 0 and needs
  nothing else native.
- **The receiver is `this`, and V8 checks it.** Both natives are created with a
  `v8::Signature` for Performance's `FunctionTemplate`
  (`jsg::Lock::getTemplateFor<T>()`), which is how JSG binds its own methods.
  `now.ts` calls them with the Performance as `this` through `uncurryThis`, and
  V8 rejects any other receiver before the callback runs, on the slow path and
  the fast path alike. That is the exact check JSG members get: the receiver
  must itself be a Performance wrapper, so `Object.create(performance)` is
  rejected. The error is V8's "Illegal invocation", which
  `patches/v8/0012-*` rewords to match JSG's `kIllegalInvocation`. The
  callbacks then read the C++ object straight out of the wrapper with
  `unwrapFromShimInRangeOrAbort`, as JSG's dispatch does. The tag range it
  needs (`jsg::Lock::getWrappableTagRangeFor<T>()`) is looked up once per
  context and travels as `performanceNow`'s function data. Never pass the
  receiver as an argument instead: that bypasses the signature.
- **One crossing per member, as a fast-API call.** `performanceNow` and
  `requirePerformance` are each the only JS->C++ crossing of their member, and
  both are registered with a `v8::CFunction`, so optimized callers reach them
  without the slow callback. They return primitives and touch the JS heap only
  to throw, which fast calls may do through `liftKj`. Keep anything that
  allocates or runs JS off this path.
- **`utils.getPerformancePrototype()`** returns `Performance.prototype`.
  `globalThis.Performance` exists only under `enable_global_performance_classes`
  or `enable_nodejs_perf_hooks_module`. Reading
  `Object.getPrototypeOf(globalThis.performance)` instead would allocate the
  lazy singleton at context creation.

## SHAPE PARITY

`install()` copies each original descriptor's attributes, and copies `name` and
`length` from each original function. Both depend on
`spec_compliant_property_attributes`: the JSG getter is named `get timeOrigin`
when it is on and `""` when it is off. Replacing the members on the prototype
reaches every route to them: `globalThis.performance`, `node:perf_hooks`'
`performance` and `Performance`, and the `Performance` global.

Both implementations run `src/workerd/api/tests/performance-now-test.js`
unmodified. `performance-now-test.wd-test` runs it against JSG, and its
`@all-autogates` variant against TS. `performance-now-ts-test.wd-test` turns
on the `typescript-performance-now` autogate, so all of its variants run
against TS. Each config covers the default compat date, the
`spec_compliant_property_attributes` attributes, and the perf-hooks flag.
`optimizedCalls` optimizes callers with `%OptimizeFunctionOnNextCall` (the
configs set `--allow-natives-syntax`, and the natives are compiled through an
`unsafeEval` binding). The callers read `p.now()` and `p.timeOrigin`, as user
code does: for a method bound to a signature, TurboFan takes the fast-API path
only when it knows the receiver's map, which the property load supplies. A
caller written as `now.call(receiver)` never reaches the fast path, for JSG
methods either.
`typeScriptIsInstalled`, enabled by the `expectTypeScript` binding, is the
canary that the install took effect. Without it the suite would also pass
against JSG.

## INTENTIONAL DIVERGENCE

- `Function.prototype.toString()` returns the TS source rather than
  `function now() { [native code] }`. The canary relies on this.
- The JSG `timeOrigin` getter is a plain `FunctionTemplate`, so it has a
  `prototype` property and `new` reaches its receiver check ("Illegal
  invocation"). The TS getter has no `prototype`, and `new` throws "is not a
  constructor". Both are TypeErrors.

## ANTI-PATTERNS

- **NEVER** read the clock in TS (`Date.now`, a captured primordial). Go
  through `utils.performanceNow()`, so the perf event is reported.
- **NEVER** hard-code descriptor attributes, `name` or `length` while the JSG
  originals exist. Copy them from the originals.
- **NEVER** brand-check in C++ with `TypeHandler::tryUnwrap()`. It searches
  the prototype chain, so it accepts `Object.create(performance)`, and it costs
  a type-map lookup plus a refcounted `Ref` on every call. Use the signature.

`src/workerd/io/bench-timer-clock` measures `performance.now()` both ways:
`clockReadsWithBlockedTimers` (JSG) and
`clockReadsWithBlockedTimersTypeScript` (TypeScript, under the autogate). It makes 10k
reads on a fresh isolate per iteration, so it includes the time before
TurboFan optimizes the TypeScript member, which the JSG method does not need.
Raise `READS` to compare optimized code.
