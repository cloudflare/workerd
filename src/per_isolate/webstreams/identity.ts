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
//   - All writes COPY the data (never transfer/detach the input buffer),
//     and the copy is taken SYNCHRONOUSLY inside writer.write() — via the
//     strategy size callback, the one hook the writable machinery runs
//     before control returns to the caller — so resizing or detaching the
//     buffer after write() cannot change or destroy what gets delivered.
//     SAB input forces copying anyway; uniform copy avoids a behavioral
//     split and matches legacy parity with the old C++ ITS (whose
//     processChunk copies inside write() for exactly these hazards).
//   - Zero-length writes are accepted as NO-OPS: the write resolves
//     immediately without touching the readable queue (no zero-length
//     chunk enqueued, no backpressure interaction, no pull).
//
// See /src/tests/streams/identity/AGENTS.md for the IdentityTransformStream
// and FixedLengthStream specification.
//
// RENDEZVOUS MODEL
//
// A write's promise settles only once reads have CONSUMED its bytes, not
// when they are queued. Nothing moves to the readable side until a read
// asks: the readable uses highWaterMark 0, so its pull fires only while a
// read is pending. Each pull hands over everything already written, in
// order (#deliver): the rest of the write the sink is on, then the writes
// queued behind it, whose bytes were snapshotted when they were written
// (#snapshots). A direct BYOB read takes as much as fits in its view,
// across writes, through its byobRequest; a read with a minimum waits for
// it, then takes everything available, up to its view. Otherwise (a
// default read, or any read on a tee branch) each write's bytes are
// enqueued as one chunk, in a batch that notifies the readers once, so a
// branch's BYOB read spans them and a default read takes one write. A pull
// that finds nothing written parks until a write arrives (#parkedPull).
//
// The write the sink is on settles once the SLOWEST consumer (the sole
// reader, or the slowest tee branch) has read its last byte, tracked
// through the readable queue's consumption notification
// (#checkSettlement), so its bytes stay counted in the writer's
// desiredSize until then. A starved consumer (a pull that finds nothing
// more) releases a fully delivered write, as a pending read overrides
// backpressure in the queue's pull rule: an idle tee branch buffers rather
// than stalling the writer. A write read before the writable reaches it
// settles when its sink step runs.
//
// This deliberately differs from the C++ IdentityTransformStream, which
// also settles writes on consumption but answers a BYOB read with the
// bytes of one write at most, stops a read with a minimum once it is met,
// and settles a write once one tee branch has read it (the identity
// suite's ledger #22 and #23).
//
// writer.write() therefore does not resolve until reads consume it —
// callers must not `await writer.write()` before starting a read, or the
// result is a deadlock. Aborting the writable or cancelling the readable
// also wakes the waiting write, which then rejects.
//
// Correct usage:
//   const readPromise = reader.read();  // pull parks, waiting for data
//   await writer.write(chunk);          // delivered to the read
//   const { value } = await readPromise;
//
// Deadlock:
//   await writer.write(chunk);  // waits forever — no read pending
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
import type {
  RingBuffer as RingBufferType,
  RingBufferConstructor,
} from './ring-buffer';

const {
  ArrayBuffer,
  ArrayPrototypePush,
  ArrayBufferPrototypeByteLengthGet,
  BigInt,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  Number,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  PromisePrototypeThen,
  PromiseResolve,
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

const { isArrayBufferView, isArrayBuffer, isSharedArrayBuffer } = utils;

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

const writableControllerError = uncurryThis(
  WritableStreamDefaultController.prototype.error
) as (controller: object, reason: unknown) => void;

function isActualObject(value: unknown) {
  return value != null && typeof value === 'object';
}

// --- Bootstrap captures (byte controller methods + TextEncoder) ----------

const byteControllerClose = uncurryThis(
  ReadableByteStreamController.prototype.close
) as (controller: object) => void;
const byteControllerError = uncurryThis(
  ReadableByteStreamController.prototype.error
) as (controller: object, reason: unknown) => void;
function captureGetter(proto: object, name: string): (self: object) => unknown {
  const desc = ObjectGetOwnPropertyDescriptor(proto, name);
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
const byteControllerDesiredSizeGet = captureGetter(
  ReadableByteStreamController.prototype,
  'desiredSize'
) as (controller: object) => number | null;
const byobRequestRespond = uncurryThis(
  ReadableStreamBYOBRequest.prototype.respond
) as (request: object, bytesWritten: number) => void;

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
// Used by the always-installed size callback (sizeAndSnapshot in the
// constructor) when an explicit highWaterMark selects byte accounting,
// feeding queueTotalSize which drives desiredSize and
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
  // byteSize runs only on chunks validateAndCopyChunk has already
  // accepted (sizeAndSnapshot validates before sizing), so anything that
  // is not a string or (Shared)ArrayBuffer is an ArrayBufferView.
  const isDataView =
    TypedArrayPrototypeGetSymbolToStringTag(chunk) === undefined;
  return (
    isDataView
      ? DataViewPrototypeGetByteLength(chunk)
      : TypedArrayPrototypeGetByteLength(chunk)
  ) as number;
}

let assertIsIdentityTransformStream: (self: IdentityTransformStream) => void;

// ---------------------------------------------------------------------------

const kPrivateSymbol: symbol = Symbol('private');

const kEmptyStrategy = ObjectFreeze({
  __proto__: null,
}) as QueuingStrategy<unknown>;

// A write accepted by the writable, snapshotted in its size() callback (see
// sizeAndSnapshot). Entries are tagged with an own `ok` data property rather
// than discriminated with an `in` check so that a polluted Object.prototype
// cannot forge or mask the discriminant.
interface AcceptedWrite {
  ok: true;
  // The write's bytes; undefined for a zero-length write.
  copied: Uint8Array | undefined;
  // How many of them have been handed to the readable side.
  delivered: number;
  // Whether they are counted against a FixedLengthStream's budget.
  budgeted: boolean;
  // The offset, in the delivered byte stream, just past the write's last
  // byte; -1 until all of it has been delivered.
  end: number;
}
type SnapshotEntry = AcceptedWrite | { ok: false; error: unknown };
// The write whose sink step is in flight, with its promise.
interface InFlightWrite {
  entry: AcceptedWrite;
  pending: PromiseWithResolversType<void>;
}

class IdentityTransformStream {
  #readable: ReadableStreamType<Uint8Array>;
  #writable: WritableStreamType<unknown>;
  #readableController: object | undefined;
  #writableController: object | undefined;
  // Every accepted write whose sink step has not begun, in order (see
  // sizeAndSnapshot). Their bytes are already written, so a read may take
  // them before the writable hands the write to the sink.
  #snapshots: RingBufferType<SnapshotEntry> = new RingBuffer();
  // The write whose sink step is in flight, with its promise: resolved once
  // the slowest consumer has read past its last byte, rejected when the
  // stream errors.
  #current: InFlightWrite | undefined;
  // A pull that found nothing written; settled once data arrives or the
  // stream ends.
  #parkedPull: PromiseWithResolversType<void> | undefined;
  // Bytes handed to the readable side so far.
  #deliveredTotal: number = 0;
  #settlementCheckScheduled: boolean = false;
  #deliveryScheduled: boolean = false;
  // True while #deliver answers a read; it checks settlement itself once
  // the read is answered.
  #delivering: boolean = false;
  // Byte budget for FixedLengthStream enforcement. undefined for plain
  // IdentityTransformStream; set to expectedLength for FixedLengthStream.
  // Decremented as each write is delivered (or reaches the sink);
  // overwrite/underwrite errors match C++
  // (identity-transform-stream.c++ tryReadInternal). Stored as bigint
  // to preserve precision for the full uint64_t range.
  #remaining: bigint | undefined;

  static {
    assertIsIdentityTransformStream = function (self) {
      if (!isActualObject(self) || !(#readable in self))
        throw new TypeError('Illegal invocation');
    };
  }

  // Bytes every consumer has read: those delivered, less the slowest
  // consumer's backlog (the readable's high-water mark is 0, so its
  // desiredSize is minus the largest backlog among the cursors).
  #consumedBySlowest(): number {
    const rc = this.#readableController;
    if (rc === undefined) return 0;
    const desiredSize = byteControllerDesiredSizeGet(rc);
    const backlog = desiredSize === null || desiredSize >= 0 ? 0 : -desiredSize;
    return this.#deliveredTotal - backlog;
  }

  // A consumer is starved: it has a pending read and has taken everything
  // delivered (the pull found nothing more). As the queue's pull rule lets
  // a pending read on any cursor override backpressure, a starved consumer
  // releases the in-flight write once all its bytes are delivered, so an
  // idle tee branch buffers instead of stalling the writer.
  #releaseForStarvedConsumer(): void {
    const current = this.#current;
    if (current === undefined || current.entry.end < 0) return;
    if (this.#writableErroring()) return;
    this.#current = undefined;
    current.pending.resolve();
  }

  // Resolves the in-flight write once the slowest consumer has read past
  // its last byte.
  #checkSettlement(): void {
    const current = this.#current;
    if (current === undefined) return;
    const end = current.entry.end;
    if (end < 0 || end > this.#consumedBySlowest()) return;
    if (this.#writableErroring()) return;
    this.#current = undefined;
    current.pending.resolve();
  }

  // Once the writable is erroring (aborted, or the readable cancelled or
  // errored), the in-flight write is left to #unblockWrite, which rejects
  // it. A consumer's progress can still be observed in between — a
  // settlement check deferred from before, or a read answered while user
  // code aborts from inside its resolution — and by then the readable's
  // backlog no longer measures what the consumers have read.
  #writableErroring(): boolean {
    const state = writableInternals.getState(this.#writable);
    return state === 'erroring' || state === 'errored';
  }

  // The readable queue's consumption notification. It runs inside the
  // queue's walk, so the check itself waits a microtask — unless #deliver
  // is answering a read, as it checks right after.
  #onConsumption(): void {
    if (
      this.#current === undefined ||
      this.#delivering ||
      this.#settlementCheckScheduled
    )
      return;
    this.#settlementCheckScheduled = true;
    PromisePrototypeThen(PromiseResolve(), () => {
      this.#settlementCheckScheduled = false;
      this.#checkSettlement();
    });
  }

  // Rejects the in-flight write with the writable's stored error. Checked a
  // microtask later: abort() calls its hook before the writable starts
  // erroring.
  #unblockWrite(): void {
    if (this.#current === undefined) return;
    PromisePrototypeThen(PromiseResolve(), () => {
      const current = this.#current;
      if (current === undefined) return;
      const state = writableInternals.getState(this.#writable);
      if (state !== 'erroring' && state !== 'errored') return;
      this.#current = undefined;
      current.pending.reject(writableInternals.getStoredError(this.#writable));
    });
  }

  // Settles a parked pull: data has arrived, or the stream has ended.
  #releaseParkedPull(): void {
    const parked = this.#parkedPull;
    if (parked !== undefined) {
      this.#parkedPull = undefined;
      parked.resolve();
    }
  }

  // Counts a write against a FixedLengthStream's budget. False if it does
  // not fit: it is then not delivered, and its sink step errors the stream.
  #budget(entry: AcceptedWrite): boolean {
    if (entry.budgeted) return true;
    const remaining = this.#remaining;
    if (remaining !== undefined) {
      const len = BigInt(
        TypedArrayPrototypeGetByteLength(entry.copied as Uint8Array) as number
      );
      if (len > remaining) return false;
      this.#remaining = remaining - len;
    }
    entry.budgeted = true;
    return true;
  }

  // The undelivered writes, in order — the rest of the in-flight write,
  // then the writes queued behind it — up to `limit` bytes, counted against
  // a FixedLengthStream's budget as they are taken.
  #collectUndelivered(limit: number): AcceptedWrite[] {
    const segments: AcceptedWrite[] = [];
    let total = 0;
    const current = this.#current;
    if (current !== undefined && current.entry.end < 0) {
      ArrayPrototypePush(segments, current.entry);
      total += this.#undeliveredBytes(current.entry);
    }
    const snapshots = this.#snapshots;
    for (let i = 0; i < snapshots.length && total < limit; i++) {
      const entry = snapshots.get(i) as SnapshotEntry;
      if (!entry.ok || entry.copied === undefined || entry.end >= 0) continue;
      if (!this.#budget(entry)) break;
      ArrayPrototypePush(segments, entry);
      total += this.#undeliveredBytes(entry);
    }
    return segments;
  }

  #undeliveredBytes(entry: AcceptedWrite): number {
    return (
      (TypedArrayPrototypeGetByteLength(entry.copied as Uint8Array) as number) -
      entry.delivered
    );
  }

  // Hands the read that pulled everything already written, in write order
  // (see the rendezvous model above). A BYOB read with a byobRequest (a
  // direct reader) takes as much as fits in its view, across writes,
  // answered with one respond(); otherwise (a default read, or any read on
  // a tee branch) each write's remaining bytes are enqueued as one chunk,
  // in a batch that notifies the consumers once, so a branch's BYOB read
  // fills across them and a default read still takes one write. False if
  // nothing is written.
  //
  // Runs only while a pull is outstanding (from sourcePull, or for a parked
  // pull), so the controller cannot call pull again until that pull
  // settles.
  //
  // Bytes a read has taken count as read even while it waits for its
  // minimum (readAtLeast, { min }): the write they came from may settle,
  // so a writer that awaits each write can supply the rest.
  #deliver(): boolean {
    const rc = this.#readableController as object;
    const request = byteControllerByobRequestGet(rc);
    const view = request === null ? null : byobRequestViewGet(request);
    if (request !== null && view !== null) {
      const viewLength = TypedArrayPrototypeGetByteLength(view) as number;
      const segments = this.#collectUndelivered(viewLength);
      if (segments.length === 0) return false;
      const viewBuffer = TypedArrayPrototypeGetBuffer(view) as ArrayBuffer;
      const viewOffset = TypedArrayPrototypeGetByteOffset(view) as number;
      let filled = 0;
      for (let i = 0; i < segments.length && filled < viewLength; i++) {
        const entry = segments[i] as AcceptedWrite;
        const bytes = entry.copied as Uint8Array;
        const left = this.#undeliveredBytes(entry);
        const room = viewLength - filled;
        const n = left < room ? left : room;
        TypedArrayPrototypeSet(
          new Uint8Array(viewBuffer, viewOffset + filled, n),
          new Uint8Array(
            TypedArrayPrototypeGetBuffer(bytes) as ArrayBuffer,
            (TypedArrayPrototypeGetByteOffset(bytes) as number) +
              entry.delivered,
            n
          )
        );
        entry.delivered += n;
        filled += n;
        this.#deliveredTotal += n;
        if (n === left) entry.end = this.#deliveredTotal;
      }
      this.#delivering = true;
      try {
        byobRequestRespond(request, filled);
      } finally {
        this.#delivering = false;
      }
    } else {
      const segments = this.#collectUndelivered(Infinity);
      if (segments.length === 0) return false;
      const chunks: Uint8Array[] = [];
      for (let i = 0; i < segments.length; i++) {
        const entry = segments[i] as AcceptedWrite;
        const bytes = entry.copied as Uint8Array;
        const left = this.#undeliveredBytes(entry);
        ArrayPrototypePush(
          chunks,
          entry.delivered === 0
            ? bytes
            : new Uint8Array(
                TypedArrayPrototypeGetBuffer(bytes) as ArrayBuffer,
                (TypedArrayPrototypeGetByteOffset(bytes) as number) +
                  entry.delivered,
                left
              )
        );
        this.#deliveredTotal += left;
        entry.delivered += left;
        entry.end = this.#deliveredTotal;
      }
      this.#delivering = true;
      try {
        readableInternals.enqueueBytesBatch(rc, chunks);
      } finally {
        this.#delivering = false;
      }
    }
    this.#checkSettlement();
    return true;
  }

  // A parked pull takes a write as soon as it is written. The write's
  // size() callback is where it arrives, inside writer.write() and before
  // the write is queued, so the delivery waits a microtask.
  #scheduleDelivery(): void {
    if (this.#parkedPull === undefined || this.#deliveryScheduled) return;
    this.#deliveryScheduled = true;
    PromisePrototypeThen(PromiseResolve(), () => {
      this.#deliveryScheduled = false;
      if (this.#parkedPull !== undefined && this.#deliver()) {
        this.#releaseParkedPull();
      }
    });
  }

  // The stream has ended: drop the undelivered writes and settle a parked
  // pull.
  #discardPending(): void {
    this.#snapshots.clear();
    this.#releaseParkedPull();
  }

  #errorWritableAndUnblockWrite(reason: unknown): void {
    this.#discardPending();
    const wc = this.#writableController;
    if (wc !== undefined) {
      writableControllerError(wc, reason);
    }
    this.#unblockWrite();
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
        QueuingStrategy<unknown> | undefined;
    }
    writableStrategy ??= kEmptyStrategy;

    // Initialize byte budget for FixedLengthStream enforcement.
    // Stored as bigint to cover the full uint64_t range without
    // precision loss.
    if (expectedLength !== undefined) {
      this.#remaining =
        typeof expectedLength === 'bigint'
          ? expectedLength
          : BigInt(expectedLength);
    }

    // The strategy size callback is the one hook the writable machinery
    // runs SYNCHRONOUSLY inside writer.write(), so the chunk snapshot is
    // taken here — before control returns to the caller, and therefore
    // before the caller can resize or detach the buffer. sinkWrite
    // consumes the snapshots in FIFO order: the machinery calls size()
    // exactly once per write and runs the sink write algorithm for the
    // accepted ones in the same order. Until then a snapshot is already
    // written as far as reads are concerned (#deliver). The machinery runs
    // size() BEFORE its own state checks, so a write against a
    // closing/errored stream is detected and skipped without copying (see
    // willAcceptWrite in writable.ts), keeping doomed writes from growing
    // the FIFO; terminal transitions clear entries whose queued writes the
    // machinery discards.
    //
    // An INVALID chunk must not throw out of size(): the spec's
    // GetChunkSize error path errors the stream immediately, which would
    // reject earlier valid writes still sitting in the queue instead of
    // letting them deliver. The validation error is recorded in the FIFO
    // instead and thrown when its entry's turn reaches sinkWrite — so
    // errors surface in write order, exactly as when validation lived in
    // sinkWrite itself, and everything written before the bad chunk still
    // flows. (The error entry transiently counts one unit of queue size.)
    //
    // When highWaterMark is explicitly provided, the returned size is the
    // byte length so that desiredSize tracks bytes rather than chunk
    // count, matching the C++ WritableStreamInternalController which uses
    // adjustWriteBufferSize with actual byte lengths. Without an explicit
    // highWaterMark the returned size stays 1 per chunk.
    //
    // A user-supplied highWaterMark of -0 is normalized to +0 so it cannot
    // surface as a negative-zero desiredSize; the C++ implementation's
    // uint64 coercion normalizes it the same way. For a number, adding 0
    // changes nothing else; non-number values pass through untouched to
    // the writable machinery's own conversion. This also covers
    // FixedLengthStream, whose capped strategy flows through super() into
    // this read.
    let explicitHighWaterMark = writableStrategy.highWaterMark;
    if (typeof explicitHighWaterMark === 'number') {
      explicitHighWaterMark += 0;
    }
    const sizeAndSnapshot = (chunk: unknown): number => {
      // Doomed writes are skipped without copying (see willAcceptWrite in
      // writable.ts for the size()-before-state-checks coupling).
      if (!writableInternals.willAcceptWrite(this.#writable)) {
        return 1;
      }
      try {
        const copied = validateAndCopyChunk(chunk);
        // Size is computed before the push: if it ever threw, nothing
        // would have been queued and the FIFO could not desync.
        const size = explicitHighWaterMark !== undefined ? byteSize(chunk) : 1;
        this.#snapshots.push({
          __proto__: null,
          ok: true,
          copied,
          delivered: 0,
          budgeted: false,
          end: -1,
        } as AcceptedWrite);
        if (copied !== undefined) this.#scheduleDelivery();
        return size;
      } catch (error) {
        this.#snapshots.push({
          __proto__: null,
          ok: false,
          error,
        } as SnapshotEntry);
        return 1;
      }
    };
    const sinkStrategy: Record<string, unknown> =
      explicitHighWaterMark !== undefined
        ? {
            __proto__: null,
            highWaterMark: explicitHighWaterMark,
            size: sizeAndSnapshot,
          }
        : { __proto__: null, size: sizeAndSnapshot };

    // --- Writable side (byte-only ingress) ---
    const sinkWrite = (_chunk: unknown): Promise<void> | undefined => {
      // The snapshot was taken in sizeAndSnapshot when this write was
      // accepted; the raw chunk argument is deliberately unused (its
      // buffer may have been resized or detached since).
      const snapshots = this.#snapshots;
      if (snapshots.length === 0) {
        throw new TypeError(
          'IdentityTransformStream internal error: snapshot queue desync'
        );
      }
      const entry = snapshots.shift() as SnapshotEntry;
      // A recorded validation error surfaces here, at its FIFO turn, as a
      // NON-FATAL write rejection: this write's promise rejects while the
      // stream stays usable and queued writes behind it still deliver —
      // the per-write invalid-chunk contract shared with the C++ internal
      // controllers.
      if (!entry.ok) {
        throw writableInternals.nonFatalWriteRejection(entry.error);
      }
      if (entry.copied === undefined) return; // zero-length no-op

      // FixedLengthStream overwrite enforcement (matches C++
      // identity-transform-stream.c++ tryReadInternal overwrite check). A
      // write already delivered fitted the budget.
      if (!this.#budget(entry)) {
        const err = new RangeError(
          'Attempt to write too many bytes through a FixedLengthStream.'
        );
        this.#discardPending();
        const rc = this.#readableController;
        if (rc !== undefined) byteControllerError(rc, err);
        throw err;
      }

      // RENDEZVOUS: the write settles once the slowest consumer has read
      // its last byte (#checkSettlement) or a consumer is starved, or
      // rejects when the writable is aborted or the readable cancelled
      // (#unblockWrite). See file-level comment. A read may already have
      // taken it: then it settles now if the slowest consumer has read it,
      // or if a consumer is starved (a pull is parked).
      if (
        entry.end >= 0 &&
        (this.#parkedPull !== undefined ||
          entry.end <= this.#consumedBySlowest())
      ) {
        return;
      }
      const pending = PromiseWithResolvers() as PromiseWithResolversType<void>;
      this.#current = {
        __proto__: null,
        entry,
        pending,
      } as InFlightWrite;
      if (this.#parkedPull !== undefined && this.#deliver()) {
        this.#releaseParkedPull();
      }
      return pending.promise;
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
        this.#discardPending();
        const rc = this.#readableController;
        if (rc !== undefined) byteControllerError(rc, err);
        throw err;
      }
      const rc = this.#readableController;
      if (rc !== undefined) byteControllerClose(rc);
      this.#releaseParkedPull();
    };
    const sinkAbort = (reason: unknown): void => {
      this.#discardPending();
      const rc = this.#readableController;
      if (rc !== undefined) byteControllerError(rc, reason);
    };

    this.#writable = new WritableStream(
      {
        __proto__: null,
        start: (c: object) => {
          this.#writableController = c;
        },
        write: sinkWrite,
        close: sinkClose,
        abort: sinkAbort,
      },
      sinkStrategy
    );
    // abort() runs the abort steps only after the in-flight write settles,
    // but a write waiting in the rendezvous waits for reads that may never
    // come. The hook runs at the start of abort() and wakes the write; by
    // the time it resumes, the stream is erroring, so the write rejects
    // with the abort reason and the abort steps run.
    writableInternals.setAbortHook(this.#writable, () => {
      this.#unblockWrite();
    });

    // --- Readable side (byte stream, BYOB capable) ---
    // RENDEZVOUS: pull is called when a reader.read() needs data. It hands
    // over everything already written, or parks until a write arrives.
    const sourcePull = (): Promise<void> | undefined => {
      if (this.#deliver()) return undefined;
      this.#releaseForStarvedConsumer();
      const parked = PromiseWithResolvers() as PromiseWithResolversType<void>;
      this.#parkedPull = parked;
      return parked.promise;
    };
    const sourceCancel = (reason: unknown): void => {
      this.#errorWritableAndUnblockWrite(reason);
    };

    const byteSource: Record<string, unknown> = {
      __proto__: null,
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
      __proto__: null,
      highWaterMark: 0,
    });
    // A tee branch's (or the sole reader's) progress may settle the
    // in-flight write: its bytes count as read once the slowest consumer
    // has read them.
    readableInternals.setConsumptionHook(
      this.#readableController as object,
      () => {
        this.#onConsumption();
      }
    );

    // The Node.js interop hook errors one half without running sinkAbort
    // or sourceCancel; error the other half too, and wake a parked write.
    const errorPair = (reason: unknown): void => {
      const rc = this.#readableController;
      if (rc !== undefined) byteControllerError(rc, reason);
      this.#errorWritableAndUnblockWrite(reason);
    };
    writableInternals.setInteropErrorHook(this.#writable, errorPair);
    readableInternals.setInteropErrorHook(this.#readable, errorPair);
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
      // Derive the cap from the COERCED length, not the raw input: BigInt
      // conversion normalizes a -0.0 input to 0n, so Number(bigLen) is
      // always +0-or-positive and a negative zero cannot leak through the
      // min() below into the highWaterMark (and from there into the
      // writer's desiredSize).
      const numExpected = Number(bigLen);
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
