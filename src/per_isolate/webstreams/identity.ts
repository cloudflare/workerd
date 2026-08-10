'use strict';

// IdentityTransformStream and FixedLengthStream — non-standard
// byte-capable identity transforms.
//
// IdentityTransformStream is semantically equivalent to
// `new TransformStream()` (no-op, elided) except:
//   - The readable side is a BYTE STREAM (BYOB, min/atLeast, draining,
//     extraction — all inherited from ReadableByteStreamController).
//   - The writable side only accepts BYTES (ArrayBuffer, ArrayBufferView,
//     SharedArrayBuffer) and STRINGS (→ UTF-8 via TextEncoder).
//     Everything else: TypeError.
//   - All writes COPY the data (never transfer/detach the input buffer).
//     SAB input forces this anyway; uniform copy avoids a behavioral
//     split and matches legacy parity with the old C++ ITS.
//   - Zero-length writes are accepted as NO-OPS: the write resolves
//     immediately without touching the readable queue (no zero-length
//     chunk enqueued, no backpressure interaction, no pull).
//
// RENDEZVOUS BACKPRESSURE MODEL
//
// This implementation uses a rendezvous pattern matching the original
// C++ IdentityTransformStream behavior: backpressure starts ENABLED
// (#backpressure = true) and the readable side uses highWaterMark: 0.
// A write will block in sinkWrite's `while (#backpressure)` loop
// until the readable side's pull callback fires (triggered by a
// reader.read() call), which clears backpressure. This means
// writer.write() will NOT resolve until a corresponding reader.read()
// is issued — callers must not `await writer.write()` before starting
// a read, or the result is a deadlock.
//
// Correct usage:
//   const readPromise = reader.read();  // triggers pull → clears bp
//   await writer.write(chunk);          // now proceeds
//   const { value } = await readPromise;
//
// Deadlock:
//   await writer.write(chunk);  // blocks forever — no read pending
//   reader.read();              // never reached
//
// FixedLengthStream extends IdentityTransformStream with an
// `expectedLength` that flows through to the readable byte controller,
// giving `new Response(fixedLengthStream.readable)` its Content-Length
// header via the existing expectedLength machinery.
//
// Not a subclass of the standard TransformStream: the byte-stream
// readable, the write-side validation, and the copy-not-transfer
// semantics make it a distinct class with its own wiring.

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  QueuingStrategy,
  ReadableStream as ReadableStreamType,
  WritableStream as WritableStreamType,
} from './types';

const {
  ArrayBuffer,
  ArrayBufferPrototypeByteLengthGet,
  BigInt,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  PromiseWithResolvers,
  RangeError,
  Symbol,
  SymbolToStringTag,
  TextEncoder,
  TextEncoderEncode,
  TypeError,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetSymbolToStringTag,
  TypedArrayPrototypeSet,
  Uint8Array,
  uncurryThis,
} = primordials;

const {
  isArrayBufferView,
  isArrayBuffer,
  isSharedArrayBuffer,
  markPromiseHandled,
} = utils;

const {
  ReadableStream,
  ReadableByteStreamController,
} = require('webstreams/readable');
const {
  WritableStream,
  WritableStreamDefaultController,
  internalsForPipe: writableInternals,
} = require('webstreams/writable');

const writableControllerError = uncurryThis(
  WritableStreamDefaultController.prototype.error
) as (controller: object, reason: unknown) => void;

function isActualObject(value: unknown) {
  return value != null && typeof value === 'object';
}

// --- Bootstrap captures (byte controller methods + TextEncoder) ----------

const byteControllerEnqueue = uncurryThis(
  ReadableByteStreamController.prototype.enqueue
) as (controller: object, chunk: ArrayBufferView) => void;
const byteControllerClose = uncurryThis(
  ReadableByteStreamController.prototype.close
) as (controller: object) => void;
const byteControllerError = uncurryThis(
  ReadableByteStreamController.prototype.error
) as (controller: object, reason: unknown) => void;
const byteControllerDesiredSizeGet = (() => {
  const desc = ObjectGetOwnPropertyDescriptor(
    ReadableByteStreamController.prototype,
    'desiredSize'
  );
  if (desc === undefined || desc.get === undefined) {
    throw new TypeError(
      "Expected accessor property 'desiredSize' on prototype"
    );
  }
  return uncurryThis(desc.get);
})() as (controller: object) => number | null;

// TextEncoder instance for string → UTF-8 conversion.
const textEncoderInstance = new TextEncoder();

// ---------------------------------------------------------------------------
// Chunk validation and copy
//
// Returns a COPIED Uint8Array for byte inputs, a TextEncoder result for
// strings, or undefined for zero-length inputs (write no-op). Throws
// TypeError for anything else. Never detaches the input buffer.

function validateAndCopyChunk(chunk: unknown): Uint8Array | undefined {
  if (typeof chunk === 'string') {
    if (chunk.length === 0) return undefined;
    return TextEncoderEncode(textEncoderInstance, chunk);
  }
  if (isArrayBuffer(chunk) || isSharedArrayBuffer(chunk)) {
    // Wrap in Uint8Array for a uniform code path — Uint8Array accepts
    // both ArrayBuffer and SharedArrayBuffer, and its byteLength getter
    // works on both (unlike ArrayBuffer.prototype.byteLength, which
    // throws on SAB receivers).
    const src = new Uint8Array(chunk as ArrayBuffer);
    const byteLength = TypedArrayPrototypeGetByteLength(src) as number;
    if (byteLength === 0) return undefined;
    const copy = new Uint8Array(new ArrayBuffer(byteLength));
    TypedArrayPrototypeSet(copy, src);
    return copy;
  }
  if (isArrayBufferView(chunk)) {
    const isDataView =
      TypedArrayPrototypeGetSymbolToStringTag(chunk) === undefined;
    const byteOffset = (
      isDataView
        ? DataViewPrototypeGetByteOffset(chunk)
        : TypedArrayPrototypeGetByteOffset(chunk)
    ) as number;
    const byteLength = (
      isDataView
        ? DataViewPrototypeGetByteLength(chunk)
        : TypedArrayPrototypeGetByteLength(chunk)
    ) as number;
    if (byteLength === 0) return undefined;
    const buffer = (
      isDataView
        ? DataViewPrototypeGetBuffer(chunk)
        : TypedArrayPrototypeGetBuffer(chunk)
    ) as ArrayBuffer;
    const copy = new Uint8Array(new ArrayBuffer(byteLength));
    TypedArrayPrototypeSet(
      copy,
      new Uint8Array(buffer, byteOffset, byteLength)
    );
    return copy;
  }
  throw new TypeError(
    'IdentityTransformStream: chunk must be a BufferSource or string'
  );
}

// Compute the byte size of a chunk for WritableStream queue tracking.
// Used ONLY as the `size` strategy callback when highWaterMark is
// specified, feeding queueTotalSize which drives desiredSize and
// writer.ready — purely advisory backpressure signaling. It does NOT
// affect data correctness, the FLS byte budget (#remaining uses actual
// byte lengths from the copied chunk), or Content-Length.
//
// Our WritableStreamDefaultController dequeues AFTER the write algorithm
// completes (writable.ts #processWrite), so in-flight bytes stay counted
// in queueTotalSize — matching the C++ model where all pipeline bytes
// (in-flight + queued) are tracked until fully consumed.
//
// For strings, uses str.length * 3 as a conservative upper-bound
// estimate (max UTF-8 bytes per UTF-16 code unit) to avoid a redundant
// TextEncoder.encode — the actual encode happens once in
// validateAndCopyChunk. The overcount is relatively harmless: since this
// only affects backpressure, overestimating just means the writable side
// signals backpressure slightly earlier than strictly necessary.
function byteSize(chunk: unknown): number {
  if (typeof chunk === 'string') {
    return (chunk as string).length * 3;
  }
  if (isArrayBuffer(chunk)) {
    return ArrayBufferPrototypeByteLengthGet(chunk as ArrayBuffer);
  }
  if (isSharedArrayBuffer(chunk)) {
    // SharedArrayBuffer.prototype.byteLength getter is separate from
    // ArrayBuffer's; use Uint8Array wrapper for the uncommon SAB case.
    return TypedArrayPrototypeGetByteLength(
      new Uint8Array(chunk as unknown as ArrayBuffer)
    ) as number;
  }
  if (isArrayBufferView(chunk)) {
    const isDataView =
      TypedArrayPrototypeGetSymbolToStringTag(chunk) === undefined;
    return (
      isDataView
        ? DataViewPrototypeGetByteLength(chunk)
        : TypedArrayPrototypeGetByteLength(chunk)
    ) as number;
  }
  // Invalid chunk type — validateAndCopyChunk will throw TypeError.
  return 1;
}

let assertIsIdentityTransformStream: (self: IdentityTransformStream) => void;

// ---------------------------------------------------------------------------

const kPrivateSymbol: symbol = Symbol('private');

class IdentityTransformStream {
  #readable: ReadableStreamType<Uint8Array>;
  #writable: WritableStreamType<unknown>;
  #readableController: object | undefined;
  #writableController: object | undefined;
  // Backpressure starts ENABLED — part of the rendezvous pattern.
  // Writes block until a reader pull clears this flag.
  #backpressure: boolean = true;
  #backpressureChange: PromiseWithResolversType<void>;
  // Byte budget for FixedLengthStream enforcement. undefined for plain
  // IdentityTransformStream; set to expectedLength for FixedLengthStream.
  // Decremented on each write; overwrite/underwrite errors match C++
  // (identity-transform-stream.c++ tryReadInternal). Stored as bigint
  // to preserve precision for the full uint64_t range.
  #remaining: bigint | undefined;

  static {
    assertIsIdentityTransformStream = function (self) {
      if (!isActualObject(self) || !(#readable in self))
        throw new TypeError('Illegal invocation');
    };
  }

  #setBackpressure(backpressure: boolean): void {
    this.#backpressureChange.resolve();
    const replacement =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    markPromiseHandled(replacement.promise);
    this.#backpressureChange = replacement;
    this.#backpressure = backpressure;
  }

  #errorWritableAndUnblockWrite(reason: unknown): void {
    const wc = this.#writableController;
    if (wc !== undefined) {
      writableControllerError(wc, reason);
    }
    if (this.#backpressure) {
      this.#setBackpressure(false);
    }
  }

  constructor(writableStrategy?: QueuingStrategy<unknown>);
  // Internal: called by FixedLengthStream.
  constructor(
    internal: symbol,
    expectedLength: bigint | number,
    writableStrategy?: QueuingStrategy<unknown>
  );
  constructor(
    writableStrategyOrInternal?: QueuingStrategy<unknown> | symbol,
    internalExpectedLength?: bigint | number,
    internalWritableStrategy?: QueuingStrategy<unknown>
  ) {
    // External: new IdentityTransformStream() or
    // new IdentityTransformStream(writableStrategy).
    // Internal (from FixedLengthStream): new ITS(kPrivateSymbol, len, strategy?).
    let writableStrategy: QueuingStrategy<unknown> | undefined;
    let expectedLength: bigint | number | undefined;
    if (writableStrategyOrInternal === kPrivateSymbol) {
      expectedLength = internalExpectedLength;
      writableStrategy = internalWritableStrategy;
    } else {
      writableStrategy = writableStrategyOrInternal as
        | QueuingStrategy<unknown>
        | undefined;
    }
    writableStrategy ??= {} as QueuingStrategy<unknown>;

    // Initialize byte budget for FixedLengthStream enforcement.
    // Stored as bigint to cover the full uint64_t range without
    // precision loss.
    if (expectedLength !== undefined) {
      this.#remaining =
        typeof expectedLength === 'bigint'
          ? expectedLength
          : BigInt(expectedLength);
    }

    // When highWaterMark is explicitly provided, switch to byte-length
    // sizing so that desiredSize tracks bytes rather than chunk count,
    // matching the C++ WritableStreamInternalController which uses
    // adjustWriteBufferSize with actual byte lengths.
    if (writableStrategy.highWaterMark !== undefined) {
      writableStrategy = {
        highWaterMark: writableStrategy.highWaterMark,
        size: byteSize,
      };
    }

    const initialBackpressureChange =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    markPromiseHandled(initialBackpressureChange.promise);
    this.#backpressureChange = initialBackpressureChange;

    // --- Writable side (byte-only ingress) ---
    const sinkWrite = async (chunk: unknown): Promise<void> => {
      const copied = validateAndCopyChunk(chunk);
      if (copied === undefined) return; // zero-length no-op

      // FixedLengthStream overwrite enforcement (matches C++
      // identity-transform-stream.c++ tryReadInternal overwrite check).
      if (this.#remaining !== undefined) {
        const len = BigInt(TypedArrayPrototypeGetByteLength(copied) as number);
        if (len > this.#remaining) {
          const err = new RangeError(
            'Attempt to write too many bytes through a FixedLengthStream.'
          );
          const rc = this.#readableController;
          if (rc !== undefined) byteControllerError(rc, err);
          throw err;
        }
        this.#remaining -= len;
      }

      // RENDEZVOUS: block here until a reader.read() triggers pull,
      // which sets #backpressure = false. See file-level comment.
      while (this.#backpressure) {
        await this.#backpressureChange.promise;
        const state = writableInternals.getState(this.#writable);
        if (state === 'erroring' || state === 'errored') {
          throw writableInternals.getStoredError(this.#writable);
        }
      }
      const rc = this.#readableController as object;
      byteControllerEnqueue(rc, copied);
      const desiredSize = byteControllerDesiredSizeGet(rc);
      const backpressure = desiredSize !== null && desiredSize <= 0;
      if (backpressure !== this.#backpressure) {
        this.#setBackpressure(backpressure);
      }
    };
    // FixedLengthStream underwrite enforcement (matches C++
    // identity-transform-stream.c++ tryReadInternal underwrite check).
    // Not called on abort — sinkAbort fires instead, naturally
    // skipping the underwrite check (matching C++ behavior).
    const sinkClose = (): void => {
      if (this.#remaining !== undefined && this.#remaining > 0n) {
        const err = new RangeError(
          'FixedLengthStream did not see all expected bytes before close().'
        );
        const rc = this.#readableController;
        if (rc !== undefined) byteControllerError(rc, err);
        throw err;
      }
      const rc = this.#readableController;
      if (rc !== undefined) byteControllerClose(rc);
    };
    const sinkAbort = (reason: unknown): void => {
      const rc = this.#readableController;
      if (rc !== undefined) byteControllerError(rc, reason);
    };

    this.#writable = new WritableStream(
      {
        start: (c: object) => {
          this.#writableController = c;
        },
        write: sinkWrite,
        close: sinkClose,
        abort: sinkAbort,
      },
      writableStrategy
    );

    // --- Readable side (byte stream, BYOB capable) ---
    // RENDEZVOUS: pull is called when a reader.read() needs data.
    // Clearing backpressure here unblocks the pending sinkWrite.
    const sourcePull = (): Promise<void> => {
      this.#setBackpressure(false);
      return this.#backpressureChange.promise;
    };
    const sourceCancel = (reason: unknown): void => {
      this.#errorWritableAndUnblockWrite(reason);
    };

    const byteSource: Record<string, unknown> = {
      type: 'bytes',
      start: (c: object) => {
        this.#readableController = c;
      },
      pull: sourcePull,
      cancel: sourceCancel,
    };
    if (expectedLength !== undefined) {
      byteSource.expectedLength = expectedLength;
    }

    // highWaterMark: 0 ensures pull is not called eagerly — it fires
    // only when a reader.read() is pending, enforcing the rendezvous.
    this.#readable = new ReadableStream(byteSource, {
      highWaterMark: 0,
    });
  }

  get readable(): ReadableStreamType<Uint8Array> {
    assertIsIdentityTransformStream(this);
    return this.#readable;
  }

  get writable(): WritableStreamType<unknown> {
    assertIsIdentityTransformStream(this);
    return this.#writable;
  }
}

// Maximum expectedLength: uint64_t max. Content-Length is carried as
// uint64_t through the C++/KJ HTTP layer, so values beyond this are
// not representable.
const MAX_UINT64 = 0xffff_ffff_ffff_ffffn;

class FixedLengthStream extends IdentityTransformStream {
  constructor(
    expectedLength: bigint | number,
    writableStrategy?: QueuingStrategy<unknown>
  ) {
    if (
      typeof expectedLength !== 'number' &&
      typeof expectedLength !== 'bigint'
    ) {
      throw new TypeError(
        'FixedLengthStream expected length must be a number or bigint.'
      );
    }
    // BigInt() conversion rejects NaN and fractions (RangeError) naturally.
    const bigLen =
      typeof expectedLength === 'bigint'
        ? expectedLength
        : BigInt(expectedLength);
    if (bigLen < 0n || bigLen > MAX_UINT64) {
      throw new RangeError(
        'FixedLengthStream requires a non-negative expected length ' +
          'that fits in a uint64.'
      );
    }
    //
    // Cap highWaterMark at expectedLength, matching C++ behavior
    // (identity-transform-stream.c++ FixedLengthStream::constructor): buffering more than the
    // total expected output is pointless.
    if (
      writableStrategy !== undefined &&
      writableStrategy.highWaterMark !== undefined
    ) {
      const numExpected =
        typeof expectedLength === 'bigint'
          ? Number(expectedLength)
          : expectedLength;
      const hwm = writableStrategy.highWaterMark;
      writableStrategy = {
        highWaterMark: hwm < numExpected ? hwm : numExpected,
      };
    }
    super(kPrivateSymbol, expectedLength, writableStrategy);
  }
}

const kEnumerable = { __proto__: null, enumerable: true };

ObjectDefineProperties(IdentityTransformStream.prototype, {
  __proto__: null,
  readable: kEnumerable,
  writable: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'IdentityTransformStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

ObjectDefineProperties(FixedLengthStream.prototype, {
  __proto__: null,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'FixedLengthStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

module.exports = {
  IdentityTransformStream,
  FixedLengthStream,
};
