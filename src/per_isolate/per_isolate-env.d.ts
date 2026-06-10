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
