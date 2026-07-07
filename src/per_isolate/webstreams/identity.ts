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
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  PromiseWithResolvers,
  Symbol,
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

// ---------------------------------------------------------------------------

const kPrivateSymbol: symbol = Symbol('private');

class IdentityTransformStream {
  #readable: ReadableStreamType<Uint8Array>;
  #writable: WritableStreamType<unknown>;
  #readableController: object | undefined;
  #writableController: object | undefined;
  #backpressure: boolean = true;
  #backpressureChange: PromiseWithResolversType<void>;

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
  // Internal: called by FixedLengthStream with (kPrivateSymbol, length).
  constructor(internal: symbol, expectedLength: bigint | number);
  constructor(
    writableStrategyOrInternal?: QueuingStrategy<unknown> | symbol,
    internalExpectedLength?: bigint | number
  ) {
    // External: new IdentityTransformStream() or
    // new IdentityTransformStream(writableStrategy).
    // Internal (from FixedLengthStream): new ITS(kPrivateSymbol, length).
    let writableStrategy: QueuingStrategy<unknown> | undefined;
    let expectedLength: bigint | number | undefined;
    if (writableStrategyOrInternal === kPrivateSymbol) {
      expectedLength = internalExpectedLength;
    } else {
      writableStrategy = writableStrategyOrInternal as
        | QueuingStrategy<unknown>
        | undefined;
    }
    writableStrategy ??= {} as QueuingStrategy<unknown>;

    const initialBackpressureChange =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    markPromiseHandled(initialBackpressureChange.promise);
    this.#backpressureChange = initialBackpressureChange;

    // --- Writable side (byte-only ingress) ---
    const sinkWrite = async (chunk: unknown): Promise<void> => {
      const copied = validateAndCopyChunk(chunk);
      if (copied === undefined) return; // zero-length no-op
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
    const sinkClose = (): void => {
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

    this.#readable = new ReadableStream(byteSource, {
      highWaterMark: 0,
    });
  }

  get readable(): ReadableStreamType<Uint8Array> {
    if (!(#readable in this)) throw new TypeError('Illegal invocation');
    return this.#readable;
  }

  get writable(): WritableStreamType<unknown> {
    if (!(#writable in this)) throw new TypeError('Illegal invocation');
    return this.#writable;
  }
}

class FixedLengthStream extends IdentityTransformStream {
  constructor(
    expectedLength: bigint | number,
    _writableStrategy?: QueuingStrategy<unknown>
  ) {
    // Validation of expectedLength is handled by the
    // ReadableByteStreamController (normalizeExpectedLength) at the
    // readable-side construction. writableStrategy is accepted for
    // signature parity but currently unused (ITS defaults apply).
    super(kPrivateSymbol, expectedLength);
  }
}

ObjectDefineProperties(IdentityTransformStream.prototype, {
  readable: { enumerable: true },
  writable: { enumerable: true },
});

module.exports = {
  IdentityTransformStream,
  FixedLengthStream,
};
