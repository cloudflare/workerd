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
// references resistant to prototype pollution.
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
const ObjectPrototype = Object.prototype;
const FunctionPrototype = Function.prototype;
const { call, bind } = FunctionPrototype;

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
const AggregateErrorCtor = AggregateError;
const ArrayCtor = Array;
const ArrayBufferCtor = ArrayBuffer;
const BigIntCtor = BigInt;
const DataViewCtor = DataView;
const ErrorCtor = Error;
const FinalizationRegistryCtor = FinalizationRegistry;
const FunctionCtor = Function;
const MapCtor = Map;
const ObjectCtor = Object;
const PromiseCtor = Promise;
const RangeErrorCtor = RangeError;
const RegExpCtor = RegExp;
const SetCtor = Set;
const SymbolCtor = Symbol;
const TypeErrorCtor = TypeError;
const Uint8ArrayCtor = Uint8Array;
const WeakMapCtor = WeakMap;
const WeakRefCtor = WeakRef;
const WeakSetCtor = WeakSet;

// --- Selective captures: add as bootstrap scripts need them ---
// Do NOT capture exhaustively. Only add entries that are actually used
// by per-isolate bootstrap scripts.

// Function
const FunctionPrototypeBind = uncurryThis(FunctionPrototype.bind);

// Object
const ObjectCreate = Object.create;
const ObjectDefineProperty = Object.defineProperty;
const ObjectDefineProperties = Object.defineProperties;
const ObjectFreeze = Object.freeze;
const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
const ObjectKeys = Object.keys;
const ObjectGetPrototypeOf = Object.getPrototypeOf;
const ObjectSetPrototypeOf = Object.setPrototypeOf;

// %AsyncIteratorPrototype% — not directly exposed as a global.
// Reached via: async function*(){} → .prototype → [[Prototype]] → [[Prototype]]
const AsyncIteratorPrototype = ObjectGetPrototypeOf(
  ObjectGetPrototypeOf(async function* () {}.prototype)
);

// Promise
const PromisePrototypeThen = uncurryThis(Promise.prototype.then);
const PromisePrototypeCatch = uncurryThis(Promise.prototype.catch);
const PromisePrototypeFinally = uncurryThis(Promise.prototype.finally);
class SafePromise<T> extends PromiseCtor<T> {
  constructor(
    executor: (
      resolve: (value: T | PromiseLike<T>) => void,
      reject: (reason?: unknown) => void
    ) => void
  ) {
    super(executor);
  }
  override then<TResult1 = T, TResult2 = never>(
    onfulfilled?:
      | ((value: T) => TResult1 | PromiseLike<TResult1>)
      | undefined
      | null,
    onrejected?:
      | ((reason: any) => TResult2 | PromiseLike<TResult2>)
      | undefined
      | null
  ): SafePromise<TResult1 | TResult2> {
    // The captured PromisePrototypeThen calls the original
    // Promise.prototype.then, which uses SpeciesConstructor to create the
    // result promise. Our Symbol.species override (below) ensures the result
    // is a SafePromise, so the cast is truthful.
    return PromisePrototypeThen(this, onfulfilled, onrejected) as SafePromise<
      TResult1 | TResult2
    >;
  }
  override catch<TResult = never>(
    onrejected?:
      | ((reason: any) => TResult | PromiseLike<TResult>)
      | undefined
      | null
  ): SafePromise<T | TResult> {
    return PromisePrototypeCatch(this, onrejected) as SafePromise<T | TResult>;
  }
  override finally(
    onfinally?: (() => void) | undefined | null
  ): SafePromise<T> {
    return PromisePrototypeFinally(this, onfinally) as SafePromise<T>;
  }
}

// --- SafePromise hardening (must follow class definition) ---

// Pin Symbol.species as an OWN data property on SafePromise so that the
// original Promise.prototype.then (called by the overridden then/catch/
// finally) creates SafePromise instances for result promises. Because this
// is an own property, it shadows any pollution of Promise[Symbol.species].
ObjectDefineProperty(SafePromise, SymbolCtor.species, {
  value: SafePromise,
  configurable: false,
  writable: false,
});

// Rebind inherited static methods so they use SafePromise as the constructor
// receiver. Each call captures the CURRENT value of PromiseCtor.X (the
// original, since this runs before any user code) and binds it to
// SafePromise using the captured Function.prototype.bind. Even if
// Promise.all (etc.) is later replaced on the global, SafePromise.all
// still calls the original and returns SafePromise instances.
ObjectDefineProperty(SafePromise, 'resolve', {
  value: FunctionPrototypeBind(PromiseCtor.resolve, SafePromise),
  writable: false,
  configurable: false,
});
ObjectDefineProperty(SafePromise, 'reject', {
  value: FunctionPrototypeBind(PromiseCtor.reject, SafePromise),
  writable: false,
  configurable: false,
});
ObjectDefineProperty(SafePromise, 'all', {
  value: FunctionPrototypeBind(PromiseCtor.all, SafePromise),
  writable: false,
  configurable: false,
});
ObjectDefineProperty(SafePromise, 'allSettled', {
  value: FunctionPrototypeBind(PromiseCtor.allSettled, SafePromise),
  writable: false,
  configurable: false,
});
ObjectDefineProperty(SafePromise, 'any', {
  value: FunctionPrototypeBind(PromiseCtor.any, SafePromise),
  writable: false,
  configurable: false,
});
ObjectDefineProperty(SafePromise, 'race', {
  value: FunctionPrototypeBind(PromiseCtor.race, SafePromise),
  writable: false,
  configurable: false,
});
ObjectDefineProperty(SafePromise, 'withResolvers', {
  value: FunctionPrototypeBind(PromiseCtor.withResolvers, SafePromise),
  writable: false,
  configurable: false,
});

// Regular-Promise static methods (captured before user code runs).
// Use these for user-facing promises. SafePromise.resolve/reject/withResolvers
// remain available for internal-only promises that need species protection.
const PromiseResolve = FunctionPrototypeBind(PromiseCtor.resolve, PromiseCtor);
const PromiseReject = FunctionPrototypeBind(PromiseCtor.reject, PromiseCtor);
const PromiseWithResolvers = FunctionPrototypeBind(
  PromiseCtor.withResolvers,
  PromiseCtor
);

// Array
const ArrayIsArray = Array.isArray;
const ArrayFrom = Array.from;
const ArrayPrototypeForEach = uncurryThis(Array.prototype.forEach);
const ArrayPrototypeMap = uncurryThis(Array.prototype.map);
const ArrayPrototypeFilter = uncurryThis(Array.prototype.filter);
const ArrayPrototypeIncludes = uncurryThis(Array.prototype.includes);
const ArrayPrototypeIndexOf = uncurryThis(Array.prototype.indexOf);
const ArrayPrototypePush = uncurryThis(Array.prototype.push);
const ArrayPrototypeShift = uncurryThis(Array.prototype.shift);
const ArrayPrototypeSlice = uncurryThis(Array.prototype.slice);
const ArrayPrototypeSplice = uncurryThis(Array.prototype.splice);

// Map
const MapPrototypeGet = uncurryThis(Map.prototype.get);
const MapPrototypeSet = uncurryThis(Map.prototype.set);
const MapPrototypeHas = uncurryThis(Map.prototype.has);
const MapPrototypeDelete = uncurryThis(Map.prototype.delete);
const MapPrototypeForEach = uncurryThis(Map.prototype.forEach);
const MapPrototypeEntries = uncurryThis(Map.prototype.entries);
const MapPrototypeKeys = uncurryThis(Map.prototype.keys);
const MapPrototypeValues = uncurryThis(Map.prototype.values);
const MapPrototypeClear = uncurryThis(Map.prototype.clear);

// Set
const SetPrototypeAdd = uncurryThis(Set.prototype.add);
const SetPrototypeHas = uncurryThis(Set.prototype.has);
const SetPrototypeDelete = uncurryThis(Set.prototype.delete);
const SetPrototypeForEach = uncurryThis(Set.prototype.forEach);
const SetPrototypeValues = uncurryThis(Set.prototype.values);
const SetPrototypeClear = uncurryThis(Set.prototype.clear);
const SetPrototypeEntries = uncurryThis(Set.prototype.entries);

// WeakMap
const WeakMapPrototypeGet = uncurryThis(WeakMap.prototype.get);
const WeakMapPrototypeSet = uncurryThis(WeakMap.prototype.set);
const WeakMapPrototypeHas = uncurryThis(WeakMap.prototype.has);
const WeakMapPrototypeDelete = uncurryThis(WeakMap.prototype.delete);

// WeakSet
const WeakSetPrototypeAdd = uncurryThis(WeakSet.prototype.add);
const WeakSetPrototypeHas = uncurryThis(WeakSet.prototype.has);
const WeakSetPrototypeDelete = uncurryThis(WeakSet.prototype.delete);

// String
const StringPrototypeCharCodeAt = uncurryThis(String.prototype.charCodeAt);
const StringPrototypeSlice = uncurryThis(String.prototype.slice);
const StringPrototypeStartsWith = uncurryThis(String.prototype.startsWith);

// Symbol
const SymbolIterator = Symbol.iterator;
const SymbolAsyncIterator = Symbol.asyncIterator;
const SymbolToStringTag = Symbol.toStringTag;

// WeakRef / FinalizationRegistry
// These globals exist during bootstrap — their deletion from the global is
// deferred until after bootstrap completes (see runPerIsolateBootstrap) —
// but they are hidden from user code unless the enable_weak_ref compat flag
// is set. The captures here keep working after the globals are deleted.
const WeakRefPrototypeDeref = uncurryThis(WeakRef.prototype.deref);
const FinalizationRegistryPrototypeRegister = uncurryThis(
  FinalizationRegistry.prototype.register
);
const FinalizationRegistryPrototypeUnregister = uncurryThis(
  FinalizationRegistry.prototype.unregister
);

// Reflect
const ReflectConstruct = Reflect.construct;

// Math / Number (statics — plain value captures)
const MathMax = Math.max;
const MathMin = Math.min;
const NumberIsFinite = Number.isFinite;
const NumberIsNaN = Number.isNaN;

// JSON
const JSONParse = JSON.parse;
const JSONStringify = JSON.stringify;

// Helper: capture a spec-mandated prototype accessor getter, with a
// validation guard instead of non-null assertions. These accessors are
// guaranteed by the ECMAScript spec — the guard is a diagnostic safety
// net, not a recoverable branch.
function getProtoGetter<T>(proto: object, name: string | symbol): T {
  const desc = ObjectGetOwnPropertyDescriptor(proto, name);
  if (desc === undefined || desc.get === undefined) {
    throw new TypeError(
      `Expected accessor property '${String(name)}' on prototype`
    );
  }
  return uncurryThis(desc.get) as T;
}

// Map / Set prototype accessor captures (size is a getter, not a data property)
const MapPrototypeSizeGet = getProtoGetter<(map: Map<any, any>) => number>(
  Map.prototype,
  'size'
);
const SetPrototypeSizeGet = getProtoGetter<(set: Set<any>) => number>(
  Set.prototype,
  'size'
);

// %MapIteratorPrototype%.next — shared by entries(), keys(), values().
// Reached by spinning up a throwaway iterator to get the prototype.
const MapIteratorPrototypeNext: (iter: any) => IteratorResult<any> =
  uncurryThis(
    (ObjectGetPrototypeOf(MapPrototypeEntries(new MapCtor())) as any).next
  );

// %SetIteratorPrototype%.next — shared by entries(), keys(), values().
const SetIteratorPrototypeNext: (iter: any) => IteratorResult<any> =
  uncurryThis(
    (ObjectGetPrototypeOf(SetPrototypeValues(new SetCtor())) as any).next
  );

// ArrayBuffer
const ArrayBufferPrototypeSlice = uncurryThis(ArrayBuffer.prototype.slice);
const ArrayBufferPrototypeTransfer = uncurryThis(
  ArrayBuffer.prototype.transfer
);
const ArrayBufferPrototypeByteLengthGet = getProtoGetter<
  (buffer: ArrayBuffer) => number
>(ArrayBuffer.prototype, 'byteLength');
const ArrayBufferPrototypeDetachedGet = getProtoGetter<
  (buffer: ArrayBuffer) => boolean
>(ArrayBuffer.prototype, 'detached');

// TypedArray — instance methods live on %TypedArray%.prototype, the shared
// prototype of all typed array types. Capturing via Uint8Array.prototype
// reaches the same (inherited) functions.
const TypedArrayPrototypeSet = uncurryThis(Uint8Array.prototype.set);
const TypedArrayPrototypeSlice = uncurryThis(Uint8Array.prototype.slice);
const TypedArrayPrototypeSubarray = uncurryThis(Uint8Array.prototype.subarray);

// %TypedArray%.prototype — the shared prototype holding the metadata
// accessors for every typed array type.
const TypedArrayPrototype = ObjectGetPrototypeOf(Uint8Array.prototype);

// %TypedArray%.prototype[Symbol.toStringTag] is an accessor that reads the
// internal [[TypedArrayName]] slot: returns e.g. "Uint8Array" for typed
// arrays and undefined for anything else (including DataView, whose own
// toStringTag is a plain data property). This is the pollution-proof way to
// identify a view's REAL type — never use view.constructor, which is
// user-controllable via an own property.
const TypedArrayPrototypeGetSymbolToStringTag = getProtoGetter<
  (value: unknown) => string | undefined
>(TypedArrayPrototype, SymbolToStringTag);

// Metadata getter captures — view.buffer / view.byteOffset / view.byteLength
// / view.length are PROTOTYPE ACCESSORS, patchable by user code. Internal
// reads of view metadata at trust boundaries must go through these (or the
// DataView equivalents below), never through property access on the view.
const TypedArrayPrototypeGetBuffer = getProtoGetter<
  (view: ArrayBufferView) => ArrayBuffer
>(TypedArrayPrototype, 'buffer');
const TypedArrayPrototypeGetByteOffset = getProtoGetter<
  (view: ArrayBufferView) => number
>(TypedArrayPrototype, 'byteOffset');
const TypedArrayPrototypeGetByteLength = getProtoGetter<
  (view: ArrayBufferView) => number
>(TypedArrayPrototype, 'byteLength');
const TypedArrayPrototypeGetLength = getProtoGetter<
  (view: ArrayBufferView) => number
>(TypedArrayPrototype, 'length');

// DataView metadata getters. NOTE: these brand-check their receiver (they
// throw TypeError for non-DataView objects), which doubles as validation in
// code paths that have already excluded typed arrays.
const DataViewPrototypeGetBuffer = getProtoGetter<
  (view: DataView) => ArrayBuffer
>(DataView.prototype, 'buffer');
const DataViewPrototypeGetByteOffset = getProtoGetter<
  (view: DataView) => number
>(DataView.prototype, 'byteOffset');
const DataViewPrototypeGetByteLength = getProtoGetter<
  (view: DataView) => number
>(DataView.prototype, 'byteLength');

// Captured constructors for every typed array type, keyed by the name
// returned by TypedArrayPrototypeGetSymbolToStringTag. Null prototype so
// lookups can't be confused via Object.prototype pollution. DataView is not
// in this map (the getter returns undefined for it) — detect it separately
// and use the DataView capture above.
// Float16Array is enabled unconditionally via --js-float16array (jsg
// setup.c++), so a plain capture is safe.
const TypedArrayCtorByName = ObjectFreeze(
  ObjectSetPrototypeOf(
    {
      Int8Array,
      Uint8Array,
      Uint8ClampedArray,
      Int16Array,
      Uint16Array,
      Int32Array,
      Uint32Array,
      Float16Array,
      Float32Array,
      Float64Array,
      BigInt64Array,
      BigUint64Array,
    },
    null
  )
);

// --- Safe types: wrappers that use captured methods internally ---
// These are safe to use with normal method-call syntax (map.get(k))
// because they override every method to dispatch through the captured
// primordials, not the (potentially polluted) prototype chain.

// Safe iterator wrappers — hold a real iterator internally and call the
// captured .next(), so iteration is resistant to prototype pollution of
// %MapIteratorPrototype% / %SetIteratorPrototype%.
class SafeMapIterator<T> {
  #inner: Iterator<T>;
  constructor(inner: Iterator<T>) {
    this.#inner = inner;
  }
  next(): IteratorResult<T> {
    return MapIteratorPrototypeNext(this.#inner);
  }
  [SymbolIterator](): SafeMapIterator<T> {
    return this;
  }
}

class SafeSetIterator<T> {
  #inner: Iterator<T>;
  constructor(inner: Iterator<T>) {
    this.#inner = inner;
  }
  next(): IteratorResult<T> {
    return SetIteratorPrototypeNext(this.#inner);
  }
  [SymbolIterator](): SafeSetIterator<T> {
    return this;
  }
}

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
  override clear(): void {
    MapPrototypeClear(this);
  }
  override forEach(
    cb: (value: V, key: K, map: Map<K, V>) => void,
    thisArg?: any
  ): void {
    MapPrototypeForEach(this, cb, thisArg);
  }
  override get size(): number {
    return MapPrototypeSizeGet(this);
  }
  override entries(): MapIterator<[K, V]> {
    return new SafeMapIterator(
      MapPrototypeEntries(this)
    ) as unknown as MapIterator<[K, V]>;
  }
  override keys(): MapIterator<K> {
    return new SafeMapIterator(
      MapPrototypeKeys(this)
    ) as unknown as MapIterator<K>;
  }
  override values(): MapIterator<V> {
    return new SafeMapIterator(
      MapPrototypeValues(this)
    ) as unknown as MapIterator<V>;
  }
  [SymbolIterator](): MapIterator<[K, V]> {
    return new SafeMapIterator(
      MapPrototypeEntries(this)
    ) as unknown as MapIterator<[K, V]>;
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
  override clear(): void {
    SetPrototypeClear(this);
  }
  override forEach(
    cb: (value: T, value2: T, set: Set<T>) => void,
    thisArg?: any
  ): void {
    SetPrototypeForEach(this, cb, thisArg);
  }
  override get size(): number {
    return SetPrototypeSizeGet(this);
  }
  override entries(): SetIterator<[T, T]> {
    return new SafeSetIterator(
      SetPrototypeEntries(this)
    ) as unknown as SetIterator<[T, T]>;
  }
  // Per spec, Set.prototype.keys === Set.prototype.values (ECMA-262 §24.2.5.5),
  // so delegating to SetPrototypeValues is correct.
  override keys(): SetIterator<T> {
    return new SafeSetIterator(
      SetPrototypeValues(this)
    ) as unknown as SetIterator<T>;
  }
  override values(): SetIterator<T> {
    return new SafeSetIterator(
      SetPrototypeValues(this)
    ) as unknown as SetIterator<T>;
  }
  [SymbolIterator](): SetIterator<T> {
    return new SafeSetIterator(
      SetPrototypeValues(this)
    ) as unknown as SetIterator<T>;
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
  override delete(key: K): boolean {
    return WeakMapPrototypeDelete(this, key);
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
  override delete(value: T): boolean {
    return WeakSetPrototypeDelete(this, value);
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

// Runtime built-ins that we also need to capture
function captureJsgGetter<T>(proto: object, name: string): T {
  const get = ObjectGetOwnPropertyDescriptor(proto, name)?.get;
  if (get !== undefined) {
    return uncurryThis(get) as T;
  }
  return ((instance: Record<string, unknown>) =>
    instance[name]) as unknown as T;
}

const AbortControllerCtor = globalThis.AbortController;
const AbortControllerAbort = uncurryThis(
  AbortControllerCtor.prototype.abort
) as (controller: AbortController, reason?: unknown) => void;
const AbortControllerSignalGet = captureJsgGetter<
  (controller: AbortController) => AbortSignal
>(AbortControllerCtor.prototype, 'signal');

const AbortSignalCtor = globalThis.AbortSignal;
const AbortSignalAbortedGet = captureJsgGetter<
  (signal: AbortSignal) => boolean
>(AbortSignalCtor.prototype, 'aborted');
const AbortSignalReasonGet = captureJsgGetter<(signal: AbortSignal) => unknown>(
  AbortSignalCtor.prototype,
  'reason'
);

const EventTargetCtor = globalThis.EventTarget;
const EventTargetAddEventListener = uncurryThis(
  EventTargetCtor.prototype.addEventListener
) as (target: object, type: string, listener: () => void) => void;
const EventTargetRemoveEventListener = uncurryThis(
  EventTargetCtor.prototype.removeEventListener
) as (target: object, type: string, listener: () => void) => void;

const TextDecoderCtor = globalThis.TextDecoder;
const TextEncoderCtor = globalThis.TextEncoder;

const TextEncoderEncode = uncurryThis(TextEncoderCtor.prototype.encode) as (
  encoder: TextEncoder,
  input: string
) => Uint8Array;

const TextDecoderDecode = uncurryThis(TextDecoderCtor.prototype.decode) as (
  decoder: TextDecoder,
  input?: BufferSource,
  options?: { stream?: boolean }
) => string;

// TextDecoder readonly properties use the JSG layout trap: prototype
// accessors under modern compat, own data properties under old dates.
const TextDecoderEncodingGet = captureJsgGetter<
  (decoder: TextDecoder) => string
>(TextDecoderCtor.prototype, 'encoding');
const TextDecoderFatalGet = captureJsgGetter<(decoder: TextDecoder) => boolean>(
  TextDecoderCtor.prototype,
  'fatal'
);
const TextDecoderIgnoreBOMGet = captureJsgGetter<
  (decoder: TextDecoder) => boolean
>(TextDecoderCtor.prototype, 'ignoreBOM');

// Freeze the exports object to prevent accidental mutation by bootstrap scripts.
// Uses the captured ObjectFreeze, not the potentially-polluted global.
module.exports = ObjectFreeze({
  // Helpers — exported so other scripts can capture ad-hoc
  uncurryThis,
  applyBind,

  // Global types
  AbortController: AbortControllerCtor,
  AbortSignal: AbortSignalCtor,
  AggregateError: AggregateErrorCtor,
  Array: ArrayCtor,
  ArrayBuffer: ArrayBufferCtor,
  BigInt: BigIntCtor,
  DataView: DataViewCtor,
  Error: ErrorCtor,
  FinalizationRegistry: FinalizationRegistryCtor,
  Function: FunctionCtor,
  Map: MapCtor,
  Object: ObjectCtor,
  Promise: PromiseCtor,
  RangeError: RangeErrorCtor,
  RegExp: RegExpCtor,
  Set: SetCtor,
  Symbol: SymbolCtor,
  TextDecoder: TextDecoderCtor,
  TextEncoder: TextEncoderCtor,
  TypeError: TypeErrorCtor,
  Uint8Array: Uint8ArrayCtor,
  WeakMap: WeakMapCtor,
  WeakRef: WeakRefCtor,
  WeakSet: WeakSetCtor,

  // Function
  FunctionPrototype,
  FunctionPrototypeBind,

  // Object
  ObjectPrototype,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  ObjectKeys,
  ObjectGetPrototypeOf,
  ObjectSetPrototypeOf,

  // %AsyncIteratorPrototype%
  AsyncIteratorPrototype,

  // Promise — regular Promise (for user-facing promises)
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  // Promise — captured prototype methods
  PromisePrototypeThen,
  PromisePrototypeCatch,
  PromisePrototypeFinally,
  // Promise — SafePromise (for internal-only promises needing species protection)
  SafePromise,

  // Array
  ArrayIsArray,
  ArrayFrom,
  ArrayPrototypeForEach,
  ArrayPrototypeMap,
  ArrayPrototypeFilter,
  ArrayPrototypeIncludes,
  ArrayPrototypeIndexOf,
  ArrayPrototypePush,
  ArrayPrototypeShift,
  ArrayPrototypeSlice,
  ArrayPrototypeSplice,

  // Map
  MapPrototypeGet,
  MapPrototypeSet,
  MapPrototypeHas,
  MapPrototypeDelete,
  MapPrototypeForEach,
  MapPrototypeEntries,
  MapPrototypeKeys,
  MapPrototypeValues,
  MapPrototypeClear,
  MapPrototypeSizeGet,
  MapIteratorPrototypeNext,

  // Set
  SetPrototypeAdd,
  SetPrototypeHas,
  SetPrototypeDelete,
  SetPrototypeForEach,
  SetPrototypeValues,
  SetPrototypeClear,
  SetPrototypeEntries,
  SetPrototypeSizeGet,
  SetIteratorPrototypeNext,

  // WeakMap
  WeakMapPrototypeGet,
  WeakMapPrototypeSet,
  WeakMapPrototypeHas,
  WeakMapPrototypeDelete,

  // WeakRef / FinalizationRegistry
  WeakRefPrototypeDeref,
  FinalizationRegistryPrototypeRegister,
  FinalizationRegistryPrototypeUnregister,

  // WeakSet
  WeakSetPrototypeAdd,
  WeakSetPrototypeHas,
  WeakSetPrototypeDelete,

  // Reflect
  ReflectConstruct,

  // Math / Number
  MathMax,
  MathMin,
  NumberIsFinite,
  NumberIsNaN,

  // ArrayBuffer
  ArrayBufferPrototypeSlice,
  ArrayBufferPrototypeTransfer,
  ArrayBufferPrototypeByteLengthGet,
  ArrayBufferPrototypeDetachedGet,

  // TypedArray
  TypedArrayPrototypeSet,
  TypedArrayPrototypeSlice,
  TypedArrayPrototypeSubarray,
  TypedArrayPrototypeGetSymbolToStringTag,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetLength,
  TypedArrayCtorByName,

  // DataView
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteOffset,
  DataViewPrototypeGetByteLength,

  // String
  StringPrototypeCharCodeAt,
  StringPrototypeSlice,
  StringPrototypeStartsWith,

  // Symbol
  SymbolAsyncIterator,
  SymbolIterator,
  SymbolToStringTag,

  // JSON
  JSONParse,
  JSONStringify,

  // EventTarget
  EventTargetAddEventListener,
  EventTargetRemoveEventListener,

  // AbortController
  AbortControllerCtor,
  AbortControllerAbort,
  AbortControllerSignalGet,
  AbortSignalAbortedGet,
  AbortSignalReasonGet,

  // TextDecoder/TextEncoder
  TextDecoderEncodingGet,
  TextDecoderFatalGet,
  TextDecoderIgnoreBOMGet,
  TextEncoderEncode,
  TextDecoderDecode,

  // Safe types — use normal method syntax, resistant to prototype pollution
  SafeMap,
  SafeSet,
  SafeWeakMap,
  SafeWeakSet,
  SafeArrayIterator,
});
