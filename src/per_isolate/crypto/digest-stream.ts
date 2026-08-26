'use strict';

// DigestStream — a non-standard workerd extension that hashes everything
// written to it and exposes the result as a promise. It is a real subclass of
// WritableStream, which is the reason it is implemented here: the C++ version
// subclasses the C++ WritableStream, so under the
// `typescript_implemented_streams` flag it would inherit from the wrong class.
//
// The hashing is done by a native context obtained from
// utils.createDigestContext(); this module owns only the stream semantics and
// the state machine. See DigestContextHandle in
// src/workerd/api/crypto/crypto.h for the native side.
//
// STATE MACHINE
//
// The digest state is tracked separately from the parent WritableStream's state
// and does not always agree with it. dispose() errors the digest without
// touching the WritableStream, so a disposed stream is still `writable` and
// still hands out a writer; the first write then fails from the sink. The three
// digest states are:
//
//   ready   — accepting writes; the native context is live.
//   closed  — close() ran, digest resolved, native context finalized.
//   errored — abort() or dispose() ran, digest rejected with `storedError`.
//
// Writes in `closed` resolve without hashing, and writes in `errored` re-reject
// with the stored error. Neither is reachable through a well-behaved
// WritableStream, which rejects such writes before the sink sees them, but the
// arms exist so the sink can never hash into a finalized context.
//
// NO COPY
//
// Chunks are handed to the native context as-is. Unlike
// IdentityTransformStream, which copies because it enqueues chunks for a reader
// to observe later, the digest consumes bytes synchronously inside update() and
// never retains them, so copying would be pure overhead. The consequence is
// that SharedArrayBuffer-backed views are read in place, and a concurrent write
// from another thread races the hash — as it does in C++.
//
// STRINGS
//
// String chunks are forwarded to the native context rather than encoded here.
// A lone surrogate has no UTF-8 encoding, and the default is to hash it as
// WTF-8 (U+D800 becomes ED A0 80) — which TextEncoder cannot produce, since it
// always substitutes U+FFFD. Callers who want the substitution opt in with
// `{toWellFormed: true}`, and the choice is fixed when the native context is
// created. Routing both implementations through the same native conversion is
// what keeps their digests byte-identical under either setting.
//
// This is also why update() returns a byte count — a string's UTF-8 length is
// not observable from JavaScript. Both encodings happen to agree on it, since a
// lone surrogate and U+FFFD are both three bytes.

import type {
  UnderlyingSink,
  WritableStream as WritableStreamType,
} from '../webstreams/types';

const {
  ArrayBufferPrototypeByteLengthGet,
  BigInt,
  DataViewPrototypeGetByteLength,
  ObjectDefineProperties,
  PromiseWithResolvers,
  SymbolDispose,
  SymbolToStringTag,
  TypeError,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetSymbolToStringTag,
} = primordials;

const {
  isArrayBuffer,
  isArrayBufferView,
  markPromiseHandled,
  createDigestContext,
} = utils;

const { WritableStream } = require('webstreams/writable') as {
  WritableStream: typeof WritableStreamType;
};

type Chunk = ArrayBuffer | ArrayBufferView | string;

type DigestState = {
  // Cleared once the digest is finalized or discarded, so a stale sink can
  // never reach a context that native code has already consumed.
  context: DigestContext | undefined;
  promise: Promise<ArrayBuffer>;
  resolve: (value: ArrayBuffer) => void;
  reject: (reason?: unknown) => void;
  state: 'ready' | 'closed' | 'errored';
  storedError: unknown;
  // Tracked as a number rather than a bigint to keep writes allocation-free;
  // converted at the getter. Exact to 2^53 bytes, far beyond any worker.
  bytesWritten: number;
};

function isActualObject(value: unknown): boolean {
  return value != null && typeof value === 'object';
}

function byteLengthOf(chunk: ArrayBuffer | ArrayBufferView): number {
  if (isArrayBuffer(chunk)) {
    return ArrayBufferPrototypeByteLengthGet(chunk);
  }
  // A view: either a TypedArray or a DataView. The tag getter returns the
  // internal [[TypedArrayName]], and undefined for a DataView.
  if (TypedArrayPrototypeGetSymbolToStringTag(chunk) !== undefined) {
    return TypedArrayPrototypeGetByteLength(chunk as Uint8Array);
  }
  return DataViewPrototypeGetByteLength(chunk as DataView);
}

// Collapses the WebCrypto-style `string | { name }` algorithm parameter to a
// name. Unrecognized names are not rejected here — createDigestContext() throws
// for those, which is what keeps the error type and message identical to C++.
function normalizeAlgorithmName(algorithm: unknown): string {
  if (typeof algorithm === 'string') return algorithm;
  if (algorithm === null || algorithm === undefined) {
    throw new TypeError(
      'DigestStream requires a digest algorithm name, or an object with a ' +
        'name property.'
    );
  }
  if (typeof algorithm === 'object') {
    // A user-provided object, so ordinary property access and coercion are the
    // correct behavior here. A missing name coerces to "undefined" and then
    // fails the algorithm lookup, exactly as it does in C++.
    return `${(algorithm as { name?: unknown }).name}`;
  }
  return `${algorithm as { toString(): string }}`;
}

// Mirrors how JSG unwraps `jsg::Optional<DigestStream::Options>`: undefined and
// null give an empty bag, any object has its field read with ToBoolean, and a
// primitive is a TypeError. Arrays and functions are objects here, as they are
// to v8::Value::IsObject(), so they are accepted and simply have no field. The
// message reproduces the one JSG generates so both implementations report the
// same thing for the same mistake.
function readToWellFormed(options: unknown): boolean {
  if (options === undefined || options === null) return false;
  if (typeof options !== 'object' && typeof options !== 'function') {
    throw new TypeError(
      "Failed to construct 'DigestStream': constructor parameter 2 is not of " +
        "type 'Options'."
    );
  }
  // A user-provided bag, so ordinary property access and ToBoolean are correct.
  return !!(options as { toWellFormed?: unknown }).toWellFormed;
}

function createDigestState(algorithm: unknown, options: unknown): DigestState {
  // JSG unwraps constructor arguments left to right, so a bad algorithm must be
  // reported before a bad option bag.
  const name = normalizeAlgorithmName(algorithm);
  const context = createDigestContext(name, readToWellFormed(options));
  const { promise, resolve, reject } = PromiseWithResolvers() as {
    promise: Promise<ArrayBuffer>;
    resolve: (value: ArrayBuffer) => void;
    reject: (reason?: unknown) => void;
  };
  // Reading `digest` is optional — a stream may be used only for bytesWritten,
  // or abandoned entirely — and an abort is already surfaced through the
  // writer's ready/closed promises. An unobserved rejection here is therefore
  // not worth reporting. Marked before returning, so no rejecting path can run
  // first. Derived promises are unaffected: `stream.digest.then(f)` with no
  // catch still reports, so a real consumer mistake stays visible.
  markPromiseHandled(promise);
  return {
    context,
    promise,
    resolve,
    reject,
    state: 'ready',
    storedError: undefined,
    bytesWritten: 0,
  };
}

function digestWrite(state: DigestState, chunk: unknown): void {
  const isBytes = isArrayBuffer(chunk) || isArrayBufferView(chunk);
  if (!isBytes && typeof chunk !== 'string') {
    // A bare SharedArrayBuffer lands here: V8 does not report it as an
    // ArrayBuffer. A view onto one is accepted, as in C++.
    throw new TypeError(
      'DigestStream is a byte stream but received an object of ' +
        'non-ArrayBuffer/ArrayBufferView/string type on its writable side.'
    );
  }

  // Zero-length writes short-circuit ahead of the state check, so they neither
  // observe a finalized context nor move the byte count. An empty string is
  // always zero UTF-8 bytes, so the length test is sound for both input kinds.
  if (isBytes) {
    if (byteLengthOf(chunk as ArrayBuffer | ArrayBufferView) === 0) return;
  } else if ((chunk as string).length === 0) {
    return;
  }

  if (state.state === 'closed') return;
  if (state.state === 'errored') throw state.storedError;

  const context = state.context;
  if (context === undefined) {
    throw new TypeError('The digest context has already been finalized.');
  }
  state.bytesWritten += context.update(chunk as Chunk);
}

function digestClose(state: DigestState): void {
  if (state.state === 'closed') return;
  if (state.state === 'errored') throw state.storedError;

  const context = state.context;
  if (context === undefined) {
    throw new TypeError('The digest context has already been finalized.');
  }
  // Nothing further can arrive, so anything the encoder held back is now final.
  // digest() would fold this in regardless; calling flush() first is what makes
  // those bytes show up in bytesWritten.
  state.bytesWritten += context.flush();
  state.context = undefined;
  state.state = 'closed';
  state.resolve(context.digest());
}

function digestError(state: DigestState, reason: unknown): void {
  // Already closed or errored: nothing to do, matching the C++ abort().
  if (state.state !== 'ready') return;
  state.context = undefined;
  state.state = 'errored';
  state.storedError = reason;
  state.reject(reason);
}

class DigestStream extends WritableStream<ArrayBuffer | ArrayBufferView> {
  #state: DigestState;

  constructor(
    algorithm: string | { name: string },
    options?: { toWellFormed?: boolean }
  ) {
    // The state is built before super() because the sink closures below capture
    // it — they cannot reference `this`, which is in its temporal dead zone
    // until super() returns. An unrecognized algorithm therefore also throws
    // before any stream exists, as in C++.
    const state = createDigestState(algorithm, options);
    const sink: UnderlyingSink<ArrayBuffer | ArrayBufferView> = {
      write(chunk: ArrayBuffer | ArrayBufferView): void {
        digestWrite(state, chunk);
      },
      close(): void {
        digestClose(state);
      },
      abort(reason?: unknown): void {
        digestError(state, reason);
      },
    };
    super(sink);
    this.#state = state;
  }

  // Returns the identical promise object on every access, matching the C++
  // jsg::MemoizedIdentity.
  get digest(): Promise<ArrayBuffer> {
    if (!isActualObject(this) || !(#state in this)) {
      throw new TypeError('Illegal invocation');
    }
    return this.#state.promise;
  }

  get bytesWritten(): bigint {
    if (!isActualObject(this) || !(#state in this)) {
      throw new TypeError('Illegal invocation');
    }
    return BigInt(this.#state.bytesWritten);
  }

  [SymbolDispose](): void {
    if (!isActualObject(this) || !(#state in this)) {
      throw new TypeError('Illegal invocation');
    }
    // Errors the digest but deliberately leaves the WritableStream alone, so
    // the stream stays writable and the failure surfaces from the first write.
    // Idempotent: digestError() ignores non-ready states.
    digestError(this.#state, new TypeError('The DigestStream was disposed.'));
  }
}

ObjectDefineProperties(DigestStream.prototype, {
  __proto__: null,
  digest: { __proto__: null, enumerable: true },
  bytesWritten: { __proto__: null, enumerable: true },
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'DigestStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

module.exports = {
  DigestStream,
};
