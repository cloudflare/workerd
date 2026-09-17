'use strict';

// CompressionStream and DecompressionStream — Compression Streams spec
// pairs implemented over the synchronous C++ codec handle minted by
// utils.newCompressionCodec (api/compression.h CompressionCodec).
//
// ARCHITECTURE: the codec core is the C++ CodecStage — eager on push,
// buffering its own output. The pair is a JS writable sink feeding the
// handle plus a QUEUED byte-capable readable that takes the buffered
// output in bounded pieces (see DELIVERY below).
//
// SEMANTICS:
//   - EAGER PUSH: write(chunk) runs the codec synchronously; corrupt
//     input rejects the WRITE and a strict-mode incomplete stream
//     rejects the CLOSE — the spec's transform()/flush() error timing.
//   - LEGACY-PARITY SETTLEMENT: writes settle as soon as the codec
//     consumed the chunk, without waiting for reads — matching the C++
//     implementation this replaces (which had no write backpressure),
//     not the standard TransformStream's one-chunk lookahead. The
//     divergence is deliberate and carried forward.
//   - DELIVERY: output waits in the stage buffer (C++ memory) and moves
//     into the readable one read at a time: a BYOB read's view is filled
//     in place, a default read gets a piece of at most kPieceSize. A
//     write's output is never materialized in JS all at once, so peak
//     memory is the output itself, not twice it, and no chunk exceeds
//     kPieceSize.
//   - BYTE-CAPABLE READABLE: legacy parity — the C++ pair's readable
//     accepts BYOB readers, so this one does too (WHATWG describes a
//     default stream here).

import type {
  ReadableStream as ReadableStreamType,
  WritableStream as WritableStreamType,
} from './types';
import type {
  RingBuffer as RingBufferType,
  RingBufferConstructor,
} from './ring-buffer';

const {
  ArrayBufferPrototypeByteLengthGet,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  MathMin,
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  SymbolToStringTag,
  TypeError,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeSet,
  Uint8Array,
  uncurryThis,
} = primordials;

const { isArrayBuffer, isArrayBufferView, isSharedArrayBuffer, isDataView } =
  utils;

// Captured for primordials discipline — ToString coercion per spec.
const StringCoerce = String;

const {
  ReadableStream,
  ReadableByteStreamController,
  ReadableStreamBYOBRequest,
  internalsForTransform: readableInternals,
} = require('webstreams/readable');
const {
  WritableStream,
  WritableStreamDefaultController,
  internalsForPipe: writableInternals,
} = require('webstreams/writable');
const { RingBuffer } = require('webstreams/ring-buffer') as {
  RingBuffer: RingBufferConstructor;
};

// --- Bootstrap captures ---------------------------------------------------

const writableControllerError = uncurryThis(
  WritableStreamDefaultController.prototype.error
) as (controller: object, reason: unknown) => void;

const byteControllerEnqueue = uncurryThis(
  ReadableByteStreamController.prototype.enqueue
) as (controller: object, chunk: ArrayBufferView) => void;
const byteControllerClose = uncurryThis(
  ReadableByteStreamController.prototype.close
) as (controller: object) => void;
const byteControllerError = uncurryThis(
  ReadableByteStreamController.prototype.error
) as (controller: object, reason: unknown) => void;

function captureGetter(
  prototype: object,
  name: string
): (self: object) => unknown {
  const desc = ObjectGetOwnPropertyDescriptor(prototype, name);
  if (desc === undefined || desc.get === undefined) {
    throw new TypeError(`Expected accessor property '${name}' on prototype`);
  }
  return uncurryThis(desc.get) as (self: object) => unknown;
}
const byteControllerByobRequestGet = captureGetter(
  ReadableByteStreamController.prototype,
  'byobRequest'
) as (controller: object) => object | null;
const byobRequestViewGet = captureGetter(
  ReadableStreamBYOBRequest.prototype,
  'view'
) as (request: object) => Uint8Array | null;
const byobRequestRespond = uncurryThis(
  ReadableStreamBYOBRequest.prototype.respond
) as (request: object, bytesWritten: number) => void;

// The synchronous codec handle produced by utils.newCompressionCodec: an
// internal JSG resource (CompressionCodec in api/compression.h). Its methods
// live on a per-isolate JSG prototype that user code can never reach — the
// handle instances are module-private and the type is registered as neither a
// global nor a nested type — so plain method calls are pollution-safe here
// (the same reachability argument as the #-brand internals).
interface CodecHandle {
  push(chunk: ArrayBuffer | ArrayBufferView): void;
  end(): void;
  pullInto(view: ArrayBufferView): number;
  available(): number;
  clear(): void;
}

// The largest chunk a default read receives.
const kPieceSize = 64 * 1024;

// The C++ codec factory, injected through the bootstrap's utils pseudo-global
// (never present on globalThis or any user-visible surface).
const newCodec = utils.newCompressionCodec as (
  mode: 'compress' | 'decompress',
  format: string
) => CodecHandle;

function isActualObject(value: unknown): boolean {
  return value != null && typeof value === 'object';
}

// True for BufferSource chunks the codec accepts: ArrayBuffers and views,
// excluding anything SharedArrayBuffer-backed. The Compression Streams
// spec takes a Web IDL BufferSource without [AllowShared], so shared
// memory is a TypeError (WPT compression-bad-chunks pins the rejection).
// This is a standard API, so the spec shape wins over parity with the
// identity streams, which accept shared views by copying. Captured getters
// are used for the view's buffer — prototype accessors are user-patchable.
function isValidChunk(chunk: unknown): boolean {
  if (isArrayBuffer(chunk)) return true;
  if (isSharedArrayBuffer(chunk)) return false;
  if (!isArrayBufferView(chunk)) return false;
  const buffer = isDataView(chunk)
    ? DataViewPrototypeGetBuffer(chunk)
    : TypedArrayPrototypeGetBuffer(chunk);
  return !isSharedArrayBuffer(buffer);
}

// Validates a chunk and copies its CURRENT bytes. Runs synchronously
// inside writer.write() (via the strategy size callback), so resizing,
// detaching, or mutating the buffer after write() returns cannot change
// what the codec consumes — matching the C++ implementation, whose adapter
// copies inside write() for exactly these hazards. Detached or
// out-of-bounds inputs report zero length through the captured getters and
// copy as empty (a codec no-op).
function snapshotChunk(chunk: unknown): Uint8Array {
  if (!isValidChunk(chunk)) {
    throw new TypeError(
      'The provided value is not of type (ArrayBuffer or ArrayBufferView)'
    );
  }
  let buffer: ArrayBuffer;
  let byteOffset: number;
  let byteLength: number;
  if (isArrayBuffer(chunk)) {
    buffer = chunk as ArrayBuffer;
    byteOffset = 0;
    byteLength = ArrayBufferPrototypeByteLengthGet(chunk) as number;
  } else if (isDataView(chunk)) {
    buffer = DataViewPrototypeGetBuffer(chunk) as ArrayBuffer;
    byteOffset = DataViewPrototypeGetByteOffset(chunk) as number;
    byteLength = DataViewPrototypeGetByteLength(chunk) as number;
  } else {
    const view = chunk as ArrayBufferView;
    buffer = TypedArrayPrototypeGetBuffer(view) as ArrayBuffer;
    byteOffset = TypedArrayPrototypeGetByteOffset(view) as number;
    byteLength = TypedArrayPrototypeGetByteLength(view) as number;
  }
  const copy = new Uint8Array(byteLength);
  if (byteLength > 0) {
    TypedArrayPrototypeSet(
      copy,
      new Uint8Array(buffer, byteOffset, byteLength),
      0
    );
  }
  return copy;
}

type SnapshotEntry =
  { ok: true; copied: Uint8Array } | { ok: false; error: unknown };

interface CodecPair {
  readable: ReadableStreamType<Uint8Array>;
  writable: WritableStreamType<unknown>;
}

function createCodecPair(
  mode: 'compress' | 'decompress',
  format: unknown
): CodecPair {
  // Spec: format is ToString-coerced, then validated — the handle
  // factory performs the validation with the same TypeError message as
  // the legacy constructor.
  const formatString = StringCoerce(format);
  const handle = newCodec(mode, formatString);

  let writableController: object | undefined;
  let readableController: object;
  // handle.end() has run: the readable closes once its output is delivered.
  let ended = false;
  // The readable is closed or errored, or its reader cancelled: nothing
  // more is delivered.
  let finished = false;
  // A pull ran and no piece has been delivered since: a read is waiting,
  // and the sink delivers to it as soon as the codec produces output.
  let demandUnmet = false;

  // Codec failure (corrupt input on write; strict end checks on close):
  // error the readable side — the writable errors via the sink throw
  // itself. Mirrors the legacy implementation's cancelInternal, which
  // rejected pending reads and errored the state machine on any codec
  // exception.
  const failBoth = (reason: unknown): void => {
    finished = true;
    // Queued writes are discarded by the erroring writable without sink
    // steps; drop their snapshots with them.
    snapshots.clear();
    handle.clear();
    byteControllerError(readableController, reason);
  };

  // Moves one piece of buffered output into the readable: into the BYOB
  // request's view when a BYOB read is waiting, else as a chunk of at most
  // kPieceSize. Precondition: output is available.
  const deliverPiece = (available: number): void => {
    const request = byteControllerByobRequestGet(readableController);
    if (request !== null) {
      const view = byobRequestViewGet(request) as Uint8Array;
      byobRequestRespond(request, handle.pullInto(view));
      return;
    }
    const out = new Uint8Array(MathMin(available, kPieceSize));
    handle.pullInto(out);
    byteControllerEnqueue(readableController, out);
  };

  // Delivers one piece if a read is waiting for one and output is
  // available, then closes the readable once the codec has ended and its
  // output is all delivered. Each enqueue/respond re-enters pull() while
  // reads are still waiting, so the controller drives the loop for as long
  // as there is demand.
  const deliver = (): void => {
    if (finished) return;
    if (demandUnmet) {
      const available = handle.available();
      if (available > 0) {
        demandUnmet = false;
        deliverPiece(available);
      }
    }
    if (!finished && ended && handle.available() <= 0) {
      finished = true;
      byteControllerClose(readableController);
    }
  };

  // A codec error rejects the write or close. A piece of the output the
  // codec produced before the error point (e.g. the final valid bytes
  // preceding trailing junk) is delivered first: a waiting read receives it
  // (whether or not the readable has started pulling), and erroring drops
  // it otherwise — the WPT-pinned order: output first, error on later
  // reads.
  const failCodec = (e: unknown): never => {
    const available = handle.available();
    if (available > 0) {
      demandUnmet = false;
      deliverPiece(available);
    }
    failBoth(e);
    throw e;
  };

  // Chunk snapshots taken synchronously inside writer.write() by the
  // strategy size callback; the sink consumes them in FIFO order. A
  // validation failure is recorded rather than thrown so that earlier
  // queued valid writes still deliver before the error surfaces at its
  // turn.
  //
  // The writer machinery runs size() BEFORE its state checks, so a write
  // against a closing/errored stream would copy and then reject without a
  // sink step to shift the entry. Doomed writes are skipped without
  // copying (see willAcceptWrite in writable.ts for the coupling
  // invariant); terminal transitions clear any entries whose queued
  // writes the machinery discards.
  const snapshots: RingBufferType<SnapshotEntry> = new RingBuffer();
  let writableRef: object | undefined;
  const sizeAndSnapshot = (chunk: unknown): number => {
    if (
      writableRef === undefined ||
      !writableInternals.willAcceptWrite(writableRef)
    ) {
      return 1;
    }
    try {
      snapshots.push({ ok: true, copied: snapshotChunk(chunk) });
    } catch (error) {
      snapshots.push({ ok: false, error });
    }
    return 1;
  };

  const writable = new WritableStream(
    {
      __proto__: null,
      start: (c: object): void => {
        writableController = c;
      },
      write: (): void => {
        const entry = snapshots.shift();
        if (entry === undefined) {
          throw new TypeError(
            'Compression streams internal error: snapshot queue desync'
          );
        }
        if (!entry.ok) {
          // An invalid chunk errors BOTH sides — the spec's transform-time
          // TypeError, which TransformStreamError propagates to the readable
          // and the writable alike (WPT bad-chunks pins the read rejecting
          // too). Without failBoth the readable would hang on its pending
          // pull. This deliberately differs from the identity streams'
          // per-write rejection: CompressionStream is a standard API.
          failBoth(entry.error);
          throw entry.error;
        }
        // EAGER: the codec consumes the snapshot; a codec error throws
        // HERE, rejecting the write — the spec's transform-time error
        // timing. The throw errors the writable via the sink machinery;
        // the readable is errored explicitly, mirroring the legacy
        // cancelInternal path.
        try {
          handle.push(entry.copied);
        } catch (e) {
          failCodec(e);
        }
        // Writes never wait for reads (legacy-parity settlement): the
        // output stays in the stage until the readable takes it.
        deliver();
      },
      close: (): void => {
        // Z_FINISH plus the strict-mode end checks; a throw rejects the
        // close (the spec's flush-time error timing) with the same
        // both-sides error propagation as write above.
        try {
          handle.end();
        } catch (e) {
          failCodec(e);
        }
        ended = true;
        deliver();
      },
      abort: (reason: unknown): void => {
        failBoth(reason);
      },
    },
    { __proto__: null, size: sizeAndSnapshot }
  );
  writableRef = writable;

  // The readable half: a queued byte stream (BYOB-capable) fed from the
  // stage buffer. Nothing is queued ahead of demand (highWaterMark 0):
  // pull() runs only for a waiting read, which takes its piece straight
  // from the stage.
  const readable = new ReadableStream(
    {
      __proto__: null,
      type: 'bytes',
      start: (c: object): void => {
        readableController = c;
      },
      pull: (): void => {
        demandUnmet = true;
        deliver();
      },
      cancel: (reason: unknown): void => {
        // Reader-side cancel tears down the write side, mirroring the
        // legacy adapter's cancel → abortWrite path. Erroring a
        // closed/errored writable is a spec no-op, so no state check is
        // needed.
        finished = true;
        snapshots.clear();
        handle.clear();
        if (writableController !== undefined) {
          writableControllerError(writableController, reason);
        }
      },
    },
    { __proto__: null, highWaterMark: 0 }
  );

  // The Node.js interop hook errors one half without running the sink's
  // abort or the source's cancel; error the other half too.
  const errorPair = (reason: unknown): void => {
    failBoth(reason);
    if (writableController !== undefined) {
      writableControllerError(writableController, reason);
    }
  };
  writableInternals.setInteropErrorHook(writable, errorPair);
  readableInternals.setInteropErrorHook(readable, errorPair);

  return {
    readable: readable as ReadableStreamType<Uint8Array>,
    writable: writable as WritableStreamType<unknown>,
  };
}

let assertIsCompressionStream: (self: CompressionStream) => void;
let assertIsDecompressionStream: (self: DecompressionStream) => void;

class CompressionStream {
  #pair: CodecPair;

  static {
    assertIsCompressionStream = function (self: CompressionStream) {
      if (!isActualObject(self) || !(#pair in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor(format: unknown) {
    this.#pair = createCodecPair('compress', format);
  }

  get readable(): ReadableStreamType<Uint8Array> {
    assertIsCompressionStream(this);
    return this.#pair.readable;
  }

  get writable(): WritableStreamType<unknown> {
    assertIsCompressionStream(this);
    return this.#pair.writable;
  }
}

class DecompressionStream {
  #pair: CodecPair;

  static {
    assertIsDecompressionStream = function (self: DecompressionStream) {
      if (!isActualObject(self) || !(#pair in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor(format: unknown) {
    this.#pair = createCodecPair('decompress', format);
  }

  get readable(): ReadableStreamType<Uint8Array> {
    assertIsDecompressionStream(this);
    return this.#pair.readable;
  }

  get writable(): WritableStreamType<unknown> {
    assertIsDecompressionStream(this);
    return this.#pair.writable;
  }
}

const kEnumerable = { __proto__: null, enumerable: true };

ObjectDefineProperties(CompressionStream.prototype, {
  __proto__: null,
  readable: kEnumerable,
  writable: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'CompressionStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

ObjectDefineProperties(DecompressionStream.prototype, {
  __proto__: null,
  readable: kEnumerable,
  writable: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'DecompressionStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

module.exports = {
  CompressionStream,
  DecompressionStream,
};
