// This is the FIRST script executed during per-isolate bootstrap — it runs
// before main.ts and before any other bootstrap script. Its exports are
// cached and injected as the `primordials` pseudo-global into every
// subsequent bootstrap script's scope.
//
// IMPORTANT: This script MUST NOT require() or depend on any other
// bootstrap script. It must be entirely self-contained. Any dependency
// would execute before primordials are captured, defeating their purpose.
//
// Captures built-in prototype methods and static functions as standalone
// references immune to prototype pollution.
//
// Usage in other bootstrap scripts (no require needed — it's a pseudo-global):
//
//   const { PromisePrototypeThen, uncurryThis } = primordials;
//
//   // Use captured references instead of prototype methods:
//   PromisePrototypeThen(promise, onFulfilled, onRejected);
//
//   // Or capture additional methods ad-hoc:
//   const ArrayPrototypePush = uncurryThis(Array.prototype.push);
//
// Naming convention (matches Node.js internals):
//   Instance methods:  {Type}Prototype{Method}    e.g. PromisePrototypeThen
//   Static methods:    {Type}{Method}             e.g. ObjectDefineProperty
//   Getters:           {Type}Prototype{Prop}Get   e.g. MapPrototypeSizeGet

// --- Foundation: capture call/bind FIRST, before anything else ---
const { call, bind } = Function.prototype;

// uncurryThis(Proto.method) returns (thisArg, ...args) => Proto.method.call(thisArg, ...args)
// Safe even if Function.prototype.call is later polluted.
const uncurryThis = call.bind(bind, call) as <
  T extends (...args: any[]) => any,
>(
  fn: T
) => (thisArg: ThisParameterType<T>, ...args: Parameters<T>) => ReturnType<T>;

// applyBind(Proto.method) returns (thisArg, args[]) => Proto.method.apply(thisArg, args)
const applyBind = call.bind(bind, call.bind) as <
  T extends (...args: any[]) => any,
>(
  fn: T
) => (thisArg: ThisParameterType<T>, args: Parameters<T>) => ReturnType<T>;

// --- Global types: capture constructor/namespace references ---
// If user code replaces e.g. globalThis.Promise, code using `new Promise()`
// or `Promise.all()` would get the polluted version. Capturing the originals
// here ensures bootstrap scripts can always use the real built-ins.
const ArrayCtor = Array;
const ErrorCtor = Error;
const FunctionCtor = Function;
const MapCtor = Map;
const ObjectCtor = Object;
const PromiseCtor = Promise;
const RangeErrorCtor = RangeError;
const RegExpCtor = RegExp;
const SetCtor = Set;
const SymbolCtor = Symbol;
const TypeErrorCtor = TypeError;
const WeakMapCtor = WeakMap;
const WeakSetCtor = WeakSet;

// --- Selective captures: add as bootstrap scripts need them ---
// Do NOT capture exhaustively. Only add entries that are actually used
// by per-isolate bootstrap scripts.

// Function
const FunctionPrototypeBind = uncurryThis(Function.prototype.bind);

// Object
const ObjectCreate = Object.create;
const ObjectDefineProperty = Object.defineProperty;
const ObjectDefineProperties = Object.defineProperties;
const ObjectFreeze = Object.freeze;
const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
const ObjectKeys = Object.keys;

// Promise
const PromisePrototypeThen = uncurryThis(Promise.prototype.then);
const PromisePrototypeCatch = uncurryThis(Promise.prototype.catch);
const PromisePrototypeFinally = uncurryThis(Promise.prototype.finally);
const PromiseAll = Promise.all.bind(Promise);
const PromiseResolve = Promise.resolve.bind(Promise);
const PromiseReject = Promise.reject.bind(Promise);

// Array
const ArrayIsArray = Array.isArray;
const ArrayFrom = Array.from;
const ArrayPrototypeForEach = uncurryThis(Array.prototype.forEach);
const ArrayPrototypeMap = uncurryThis(Array.prototype.map);
const ArrayPrototypeFilter = uncurryThis(Array.prototype.filter);
const ArrayPrototypeIncludes = uncurryThis(Array.prototype.includes);
const ArrayPrototypeIndexOf = uncurryThis(Array.prototype.indexOf);
const ArrayPrototypePush = uncurryThis(Array.prototype.push);
const ArrayPrototypeSlice = uncurryThis(Array.prototype.slice);

// Map
const MapPrototypeGet = uncurryThis(Map.prototype.get);
const MapPrototypeSet = uncurryThis(Map.prototype.set);
const MapPrototypeHas = uncurryThis(Map.prototype.has);
const MapPrototypeDelete = uncurryThis(Map.prototype.delete);
const MapPrototypeForEach = uncurryThis(Map.prototype.forEach);
const MapPrototypeEntries = uncurryThis(Map.prototype.entries);
const MapPrototypeKeys = uncurryThis(Map.prototype.keys);
const MapPrototypeValues = uncurryThis(Map.prototype.values);

// Set
const SetPrototypeAdd = uncurryThis(Set.prototype.add);
const SetPrototypeHas = uncurryThis(Set.prototype.has);
const SetPrototypeDelete = uncurryThis(Set.prototype.delete);
const SetPrototypeForEach = uncurryThis(Set.prototype.forEach);
const SetPrototypeValues = uncurryThis(Set.prototype.values);

// WeakMap
const WeakMapPrototypeGet = uncurryThis(WeakMap.prototype.get);
const WeakMapPrototypeSet = uncurryThis(WeakMap.prototype.set);
const WeakMapPrototypeHas = uncurryThis(WeakMap.prototype.has);

// WeakSet
const WeakSetPrototypeAdd = uncurryThis(WeakSet.prototype.add);
const WeakSetPrototypeHas = uncurryThis(WeakSet.prototype.has);

// String
const StringPrototypeSlice = uncurryThis(String.prototype.slice);
const StringPrototypeStartsWith = uncurryThis(String.prototype.startsWith);

// Symbol
const SymbolIterator = Symbol.iterator;
const SymbolToStringTag = Symbol.toStringTag;

// --- Safe types: wrappers that use captured methods internally ---
// These are safe to use with normal method-call syntax (map.get(k))
// because they override every method to dispatch through the captured
// primordials, not the (potentially polluted) prototype chain.

class SafeMap<K, V> extends MapCtor<K, V> {
  constructor(entries?: Iterable<[K, V]> | null) {
    super(entries);
  }
  override get(key: K): V | undefined {
    return MapPrototypeGet(this, key);
  }
  override set(key: K, value: V): this {
    MapPrototypeSet(this, key, value);
    return this;
  }
  override has(key: K): boolean {
    return MapPrototypeHas(this, key);
  }
  override delete(key: K): boolean {
    return MapPrototypeDelete(this, key);
  }
  override forEach(cb: (value: V, key: K, map: Map<K, V>) => void): void {
    MapPrototypeForEach(this, cb);
  }
  override entries(): MapIterator<[K, V]> {
    return MapPrototypeEntries(this);
  }
  override keys(): MapIterator<K> {
    return MapPrototypeKeys(this);
  }
  override values(): MapIterator<V> {
    return MapPrototypeValues(this);
  }
}

class SafeSet<T> extends SetCtor<T> {
  constructor(values?: Iterable<T> | null) {
    super(values);
  }
  override add(value: T): this {
    SetPrototypeAdd(this, value);
    return this;
  }
  override has(value: T): boolean {
    return SetPrototypeHas(this, value);
  }
  override delete(value: T): boolean {
    return SetPrototypeDelete(this, value);
  }
  override forEach(cb: (value: T, value2: T, set: Set<T>) => void): void {
    SetPrototypeForEach(this, cb);
  }
  override values(): SetIterator<T> {
    return SetPrototypeValues(this);
  }
}

class SafeWeakMap<K extends WeakKey, V> extends WeakMapCtor<K, V> {
  constructor(entries?: Iterable<[K, V]> | null) {
    super(entries);
  }
  override get(key: K): V | undefined {
    return WeakMapPrototypeGet(this, key);
  }
  override set(key: K, value: V): this {
    WeakMapPrototypeSet(this, key, value);
    return this;
  }
  override has(key: K): boolean {
    return WeakMapPrototypeHas(this, key);
  }
}

class SafeWeakSet<T extends WeakKey> extends WeakSetCtor<T> {
  constructor(values?: readonly T[] | null) {
    super(values);
  }
  override add(value: T): this {
    WeakSetPrototypeAdd(this, value);
    return this;
  }
  override has(value: T): boolean {
    return WeakSetPrototypeHas(this, value);
  }
}

// SafeArrayIterator: iterates an array without relying on
// Array.prototype[Symbol.iterator] which could be polluted.
class SafeArrayIterator<T> {
  #array: ArrayLike<T>;
  #index = 0;
  constructor(array: ArrayLike<T>) {
    this.#array = array;
  }
  next(): IteratorResult<T> {
    if (this.#index < this.#array.length) {
      return { value: this.#array[this.#index++] as T, done: false };
    }
    return { value: undefined, done: true } as IteratorReturnResult<undefined>;
  }
  [SymbolIterator]() {
    return this;
  }
}

// Freeze the exports object to prevent accidental mutation by bootstrap scripts.
// Uses the captured ObjectFreeze, not the potentially-polluted global.
module.exports = ObjectFreeze({
  // Helpers — exported so other scripts can capture ad-hoc
  uncurryThis,
  applyBind,

  // Global types
  Array: ArrayCtor,
  Error: ErrorCtor,
  Function: FunctionCtor,
  Map: MapCtor,
  Object: ObjectCtor,
  Promise: PromiseCtor,
  RangeError: RangeErrorCtor,
  RegExp: RegExpCtor,
  Set: SetCtor,
  Symbol: SymbolCtor,
  TypeError: TypeErrorCtor,
  WeakMap: WeakMapCtor,
  WeakSet: WeakSetCtor,

  // Function
  FunctionPrototypeBind,

  // Object
  ObjectCreate,
  ObjectDefineProperty,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  ObjectKeys,

  // Promise
  PromisePrototypeThen,
  PromisePrototypeCatch,
  PromisePrototypeFinally,
  PromiseAll,
  PromiseResolve,
  PromiseReject,

  // Array
  ArrayIsArray,
  ArrayFrom,
  ArrayPrototypeForEach,
  ArrayPrototypeMap,
  ArrayPrototypeFilter,
  ArrayPrototypeIncludes,
  ArrayPrototypeIndexOf,
  ArrayPrototypePush,
  ArrayPrototypeSlice,

  // Map
  MapPrototypeGet,
  MapPrototypeSet,
  MapPrototypeHas,
  MapPrototypeDelete,
  MapPrototypeForEach,
  MapPrototypeEntries,
  MapPrototypeKeys,
  MapPrototypeValues,

  // Set
  SetPrototypeAdd,
  SetPrototypeHas,
  SetPrototypeDelete,
  SetPrototypeForEach,
  SetPrototypeValues,

  // WeakMap
  WeakMapPrototypeGet,
  WeakMapPrototypeSet,
  WeakMapPrototypeHas,

  // WeakSet
  WeakSetPrototypeAdd,
  WeakSetPrototypeHas,

  // String
  StringPrototypeSlice,
  StringPrototypeStartsWith,

  // Symbol
  SymbolIterator,
  SymbolToStringTag,

  // Safe types — use normal method syntax, immune to prototype pollution
  SafeMap,
  SafeSet,
  SafeWeakMap,
  SafeWeakSet,
  SafeArrayIterator,
});
