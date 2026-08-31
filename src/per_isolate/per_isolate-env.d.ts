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
  readonly SymbolDispose: typeof Symbol.dispose;

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
  isDataView(value: unknown): value is DataView;
  isPromise(value: unknown): value is Promise;
  isSharedArrayBuffer(value: unknown): value is SharedArrayBuffer;
  isUint8Array(value: unknown): value is Uint8Array;
  isAnyArrayBuffer(value: unknown): value is ArrayBuffer | SharedArrayBuffer;
  markPromiseHandled(promise: Promise): void;
  getApiSymbol(name: string): symbol;
  // The C++ compression codec factory (api/compression.h:
  // newCompressionCodecCallback), consumed by webstreams/compression.
  newCompressionCodec(mode: string, format: string): unknown;
  createDigestContext(algorithm: string, toWellFormed: boolean): DigestContext;
  // The C++ write-context factory (api/filesystem-bootstrap.h), consumed by
  // webfs/writable-file-stream. Throws the DOMException that createWritable
  // reports for an unopenable file.
  createFileSystemWriteContext(
    fileHandle: object,
    keepExistingData: boolean
  ): FileSystemWriteContext;
};

// Native incremental digest, backing the TypeScript DigestStream. Obtained only
// from utils.createDigestContext(); it has no JS-reachable constructor.
//
// Strings are passed through rather than encoded on this side because the
// default WTF-8 encoding is not expressible in JavaScript — TextEncoder always
// substitutes U+FFFD. The string encoding is fixed when the context is created;
// see DigestContextHandle in src/workerd/api/crypto/crypto.h.
declare interface DigestContext {
  // Returns the number of bytes consumed, which for a string is its UTF-8
  // length. Under toWellFormed this can be 0 for a non-empty chunk, when the
  // chunk ended with the lead half of a surrogate pair and the encoder is
  // waiting to see whether the next chunk completes it. Throws once digest()
  // has been called.
  update(chunk: ArrayBuffer | ArrayBufferView | string): number;
  // Consumes any bytes still owed at end of stream and returns how many there
  // were: 3 for a lead surrogate that was held back and never paired, else 0.
  // digest() does this too, so calling it is only needed to keep a byte count
  // accurate. Must precede digest().
  flush(): number;
  // Finalizes the digest. Throws if called more than once.
  digest(): ArrayBuffer;
}

// Native transactional write state, backing the TypeScript
// FileSystemWritableFileStream. Obtained only from
// utils.createFileSystemWriteContext(); it has no JS-reachable constructor.
//
// Writes land in a temporary file and the VFS lock is held from creation until
// commit() or discard(), so a context that is created must be finished. Every
// method resolves synchronously -- nothing here waits on I/O -- but they return
// promises because that is what the sink algorithms consume.
//
// Strings are passed through rather than encoded on this side: the file system
// writes them as WTF-8, which TextEncoder cannot produce. See
// FileSystemWriteContextHandle in src/workerd/api/filesystem.h.
declare interface FileSystemWriteContext {
  // Accepts a Blob, a BufferSource, a string, or a {type, data, position, size}
  // params object. Rejects with a TypeError once the context is finished, and
  // with a DOMException if the underlying write fails.
  write(
    data: Blob | ArrayBuffer | ArrayBufferView | string | object
  ): Promise<void>;
  // Moves the cursor, growing the file with zero bytes when past the end.
  seek(position: number): Promise<void>;
  // Resizes the file, clamping the cursor to the new size when it shrinks.
  truncate(size: number): Promise<void>;
  // Replaces the original file's contents with the temporary's and releases the
  // lock. The sink's close algorithm.
  commit(): Promise<void>;
  // Drops the written data, leaving the original intact, and releases the lock.
  // The sink's abort algorithm. Idempotent.
  discard(): void;
}
