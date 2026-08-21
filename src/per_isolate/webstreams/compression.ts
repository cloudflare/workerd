'use strict';

// CompressionStream and DecompressionStream — Compression Streams spec
// pairs implemented over the synchronous C++ codec handle produced by
// the flag-gated CompressionStream.newCodec static (captured below
// BEFORE main.ts replaces the global with the class defined here, so
// user code never observes it).
//
// ARCHITECTURE (see the compression design notes): the codec core is
// the C++ CodecStage (api/compression.h) — eager on push, buffering its
// own output. The pair is a JS writable sink feeding the handle plus a
// QUEUED byte-capable readable (BYOB served from the queue) that the
// sink's drains enqueue into. (The pipeline-optimization effort hosts
// this readable on the NATIVE backend for sink-end fusion; on this
// substrate it is a queued byte stream, per the design's E1 sequencing
// resolution.)
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
//   - BYTE-CAPABLE READABLE: legacy parity — the C++ pair's readable
//     accepts BYOB readers, so this one does too (WHATWG describes a
//     default stream here).

import type {
  ReadableStream as ReadableStreamType,
  WritableStream as WritableStreamType,
} from './types';

const {
  DataViewPrototypeGetBuffer,
  ObjectDefineProperties,
  SymbolToStringTag,
  TypeError,
  TypedArrayPrototypeGetBuffer,
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
} = require('webstreams/readable');
const {
  WritableStream,
  WritableStreamDefaultController,
} = require('webstreams/writable');

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
}

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
// excluding anything SharedArrayBuffer-backed (per Web IDL, [AllowShared] is
// not granted here; WPT pins the rejection). Captured getters are used for
// the view's buffer — prototype accessors are user-patchable.
function isValidChunk(chunk: unknown): boolean {
  if (isArrayBuffer(chunk)) return true;
  if (isSharedArrayBuffer(chunk)) return false;
  if (!isArrayBufferView(chunk)) return false;
  const buffer = isDataView(chunk)
    ? DataViewPrototypeGetBuffer(chunk)
    : TypedArrayPrototypeGetBuffer(chunk);
  return !isSharedArrayBuffer(buffer);
}

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

  // Codec failure (corrupt input on write; strict end checks on close):
  // error the readable side — the writable errors via the sink throw
  // itself. Mirrors the legacy implementation's cancelInternal, which
  // rejected pending reads and errored the state machine on any codec
  // exception.
  const failBoth = (reason: unknown): void => {
    byteControllerError(readableController, reason);
  };

  // Drains all buffered stage output into the readable's queue. The
  // enqueue is unconditional: every call site runs either right after a
  // successful codec step (stream readable) or is unreachable once the
  // pair has failed or been canceled (the errored/canceled writable
  // rejects writes before the sink hooks run).
  const drainStage = (): void => {
    const available = handle.available();
    if (available <= 0) return;
    const out = new Uint8Array(available);
    handle.pullInto(out);
    byteControllerEnqueue(readableController, out);
  };

  const writable = new WritableStream({
    start: (c: object): void => {
      writableController = c;
    },
    write: (chunk: unknown): void => {
      if (!isValidChunk(chunk)) {
        // An invalid chunk errors BOTH sides, matching the legacy
        // implementation (any write failure errored the whole pair) —
        // without this the readable side would hang on its pending
        // pull.
        const err = new TypeError(
          'The provided value is not of type (ArrayBuffer or ArrayBufferView)'
        );
        failBoth(err);
        throw err;
      }
      // EAGER: the codec consumes the chunk synchronously (the caller's
      // buffer is never retained); a codec error throws HERE, rejecting
      // the write — the spec's transform-time error timing. The throw
      // errors the writable via the sink machinery; the readable is
      // errored explicitly, mirroring the legacy cancelInternal path.
      try {
        handle.push(chunk as ArrayBuffer | ArrayBufferView);
      } catch (e) {
        // Deliver output the codec produced before the error point (e.g. the
        // final valid bytes preceding trailing junk) to any pending read, then
        // error. The WPT-pinned order: output first, error on later reads.
        drainStage();
        failBoth(e);
        throw e;
      }
      // Move any produced output to the readable immediately (writes
      // never wait for reads — legacy-parity settlement; the queue
      // buffers).
      drainStage();
    },
    close: (): void => {
      // Z_FINISH plus the strict-mode end checks; a throw rejects the
      // close (the spec's flush-time error timing) with the same
      // both-sides error propagation as write above.
      try {
        handle.end();
      } catch (e) {
        drainStage();
        failBoth(e);
        throw e;
      }
      // Deliver the flush tail, then close (buffered bytes are served
      // to remaining reads before the close lands — queued byte-stream
      // semantics).
      drainStage();
      byteControllerClose(readableController);
    },
    abort: (reason: unknown): void => {
      byteControllerError(readableController, reason);
    },
  });

  // The readable half: a queued byte stream (BYOB-capable) whose queue
  // the sink drains into. highWaterMark 0 documents that production is
  // write-driven; the eager pushes enqueue regardless of desiredSize
  // (unbounded buffering, exactly like the legacy pair).
  const readable = new ReadableStream(
    {
      type: 'bytes',
      start: (c: object): void => {
        readableController = c;
      },
      cancel: (reason: unknown): void => {
        // Reader-side cancel tears down the write side, mirroring the
        // legacy adapter's cancel → abortWrite path. Erroring a
        // closed/errored writable is a spec no-op, so no state check is
        // needed.
        if (writableController !== undefined) {
          writableControllerError(writableController, reason);
        }
      },
    },
    { highWaterMark: 0 }
  );

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
