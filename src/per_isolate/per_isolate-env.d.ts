// Ambient type declarations for per-isolate bootstrap scripts.
//
// These pseudo-globals are injected via V8 context extras at runtime.
// They are NOT on globalThis and are NOT accessible to user code.

/**
 * Synchronously load a per-isolate script by specifier.
 *
 *   require('foo')           -> src/per_isolate/foo.ts
 *   require('./foo')         -> src/per_isolate/foo.ts
 *   require('utils/helpers') -> src/per_isolate/utils/helpers.ts
 *
 * Results are cached: repeated require() calls return the same exports object.
 * Circular dependencies throw a fatal error.
 */
declare function require(specifier: string): any;

/**
 * CommonJS module object. Assign to module.exports to define what
 * require() returns for this script.
 */
declare const module: { exports: any };

/** Alias for module.exports (initial value). */
declare const exports: any;

/**
 * Compatibility flags for the current worker configuration.
 * Scripts use these to conditionally install globals.
 */
declare const compatFlags: {
  readonly [key: string]: boolean;
};

/**
 * Autogate flags for the current process. Unlike compatFlags (per-worker),
 * autogates are process-wide and used for gradual rollout of risky changes.
 * Gate names use kebab-case (e.g., "v8-fast-api", "rust-backed-node-dns").
 */
declare const autogates: {
  readonly [key: string]: boolean;
};

/**
 * Captured built-in prototype methods and constructors, immune to
 * prototype pollution. Loaded before main.ts and injected automatically
 * into every bootstrap script's scope.
 *
 * See primordials.ts for the full list of captures.
 */
declare const primordials: {
  // Well-known symbols must be typed precisely so computed properties
  // using them (e.g., [SymbolAsyncIterator]) satisfy interface constraints.
  // NOTE: destructuring widens unique symbol → symbol. Files that need the
  // precise type for indexing should use primordials.SymbolXxx directly.
  readonly SymbolIterator: typeof Symbol.iterator;
  readonly SymbolAsyncIterator: typeof Symbol.asyncIterator;
  readonly SymbolToStringTag: typeof Symbol.toStringTag;

  // uncurryThis accepts Function (the typeof-narrowed type) in addition to
  // properly typed callables, so callers don't need to cast after a
  // `typeof fn === 'function'` guard.
  readonly uncurryThis: <T extends ((...args: any[]) => any) | Function>(
    fn: T
  ) => T extends (...args: any[]) => any
    ? (thisArg: ThisParameterType<T>, ...args: Parameters<T>) => ReturnType<T>
    : (...args: any[]) => any;

  // Everything else is loosely typed — add specific entries as needed.
  readonly [key: string]: any;
};

declare const utils: {
  isArrayBuffer(value: unknown): value is ArrayBuffer;
  isArrayBufferView(value: unknown): value is ArrayBufferView;
  isPromise(value: unknown): value is Promise;
  isSharedArrayBuffer(value: unknown): value is SharedArrayBuffer;
  isUint8Array(value: unknown): value is Uint8Array;
  isAnyArrayBuffer(value: unknown): value is ArrayBuffer | SharedArrayBuffer;
  markPromiseHandled(promise: Promise): void;
  getApiSymbol(name: string): symbol;
};
