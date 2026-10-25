// performance.now() and performance.timeOrigin (High Resolution Time), as a
// TypeScript facade over the native Performance object. The clock stays native:
// utils.performanceNow() reads the IoContext's Spectre-safe time, which is
// exactly what Date.now() returns, and keeps the perf-event accounting. See
// AGENTS.md in this directory for the contract and the divergences.

const {
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  TypeError,
  uncurryThis,
} = primordials;

// Both natives take the Performance as a signature-checked `this` (see "THE C++
// CONTRACT" in AGENTS.md).
const performanceNow = uncurryThis(utils.performanceNow);
const requirePerformance = uncurryThis(utils.requirePerformance);

// Object-literal methods and accessors are, like JSG members, neither
// constructors nor carriers of a `prototype` property.
const members = {
  now(this: unknown): number {
    return performanceNow(this);
  },
  get timeOrigin(): number {
    requirePerformance(this);
    // Always 0, which makes now() equivalent to Date.now(); see
    // Performance::getTimeOrigin() in src/workerd/api/performance.h.
    return 0;
  },
};

const now = members.now;
const getTimeOrigin = (
  ObjectGetOwnPropertyDescriptor(members, 'timeOrigin') as PropertyDescriptor
).get as () => number;

type AnyFunction = (...args: never[]) => unknown;

function requireDescriptor(proto: object, name: string): PropertyDescriptor {
  const descriptor = ObjectGetOwnPropertyDescriptor(proto, name);
  if (descriptor === undefined) {
    throw new TypeError(`Performance.prototype.${name} is missing`);
  }
  return descriptor;
}

// Gives `replacement` the `name` and `length` of the JSG function it replaces.
// Both depend on spec_compliant_property_attributes (a JSG getter is named
// "get timeOrigin" under it, and "" without it), so they are copied rather than
// hard-coded. Reading them is safe: no user code has run yet, and they are own
// data properties of the native function.
function matchShape(replacement: AnyFunction, original: AnyFunction): void {
  ObjectDefineProperties(replacement, {
    __proto__: null,
    name: { __proto__: null, configurable: true, value: original.name },
    length: { __proto__: null, configurable: true, value: original.length },
  });
}

// Replaces the JSG `now` and `timeOrigin` on Performance.prototype. Every
// attribute of the original descriptors is kept, so the swap is invisible to
// property introspection. Replacing them on the prototype reaches every route
// to a Performance: globalThis.performance, node:perf_hooks, and the class.
//
// TODO(cleanup): Once the typescript-performance-now autogate is fully rolled
// out and removed, the JSG `now` and `timeOrigin` are deleted, and there is
// nothing left to replace or copy the shape from. install() then has to define
// both members outright: every attribute true (as performance-now-test.js
// pins), `length` 0, and the getter's `name` "get timeOrigin" under
// compatFlags['spec_compliant_property_attributes'] and "" otherwise.
function install(proto: object): void {
  const nativeNow = requireDescriptor(proto, 'now');
  const nativeTimeOrigin = requireDescriptor(proto, 'timeOrigin');
  matchShape(now, nativeNow.value as AnyFunction);
  matchShape(getTimeOrigin, nativeTimeOrigin.get as AnyFunction);

  ObjectDefineProperties(proto, {
    __proto__: null,
    now: {
      __proto__: null,
      configurable: nativeNow.configurable,
      enumerable: nativeNow.enumerable,
      writable: nativeNow.writable,
      value: now,
    },
    timeOrigin: {
      __proto__: null,
      configurable: nativeTimeOrigin.configurable,
      enumerable: nativeTimeOrigin.enumerable,
      get: getTimeOrigin,
      set: undefined,
    },
  });
}

// Makes this file a module, so its top-level names stay out of the scope the
// other bootstrap scripts share.
export type PerformanceNowExports = { install: typeof install };

module.exports = { install } satisfies PerformanceNowExports;
