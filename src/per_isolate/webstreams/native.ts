'use strict';

// The NATIVE consumer backend: streams whose data is produced by a C++
// (JSG/KJ) source rather than a JS underlying source feeding the shared
// queue. See native-stream-integration.md §10 for the fence conventions
// separating this backend from the queued one (queue.ts).
//
// THE C++/JS CONTRACT (phase 3; implemented on the C++ side by
// ReadableStreamNativeSource in src/workerd/api/js-readable-stream.{h,c++},
// which must conform; JS mocks in tests exercise the conduit
// independently of it):
//   - A native underlying source is an ordinary object carrying the
//     kNativeSource marker as an OWN property whose value is the marker
//     symbol itself. Marker presence ⇒ native ⇒ BYTE CAPABLE (there is
//     no separate byte flag).
//   - The source implements the standard UnderlyingSource shape:
//     pull(controller) and cancel(reason) as regular methods ARE the
//     hooks, driven by the standard pull algorithm (pulling/pullAgain
//     serialization). `start` is IGNORED (assumed no-op). `type` and
//     `autoAllocateChunkSize` are forbidden. Additional hooks (tee) are
//     additional regular methods — safe because the native source object
//     is never exposed to user code.
//   - pull(controller) discriminates the read mode via the controller:
//     controller.byobRequest !== null ⇒ BYOB read — fill the request's
//     view and call respond(bytesWritten), honoring `atLeast` (the
//     read's minimum, in bytes) and `elementSize` (the read view's
//     element size, in bytes): bytesWritten must be a whole number of
//     elements, so the source keeps a trailing partial element for the
//     next pull, and one left at EOF errors the stream with a TypeError
//     (as closing a queued byte stream mid-element does). The view and
//     atLeast are whole elements already. respondWithNewView() is
//     deliberately omitted: the native source writes into consumer-provided
//     memory (KJ tryRead semantics); buffer-swapping has no C++ analogue,
//     and the default-read enqueue() path already covers source-allocated
//     buffers.
//     byobRequest === null ⇒ default read — the source allocates its OWN
//     buffer and calls controller.enqueue(view). The view is never empty:
//     as with respond(0), an empty answer would mean end-of-stream, for
//     which close() is the verb, so enqueue() refuses one (TypeError).
//   - The source calls enqueue-or-respond AT MOST ONCE per pull
//     invocation and never pushes outside pull. desiredSize is exposed
//     but the native source never consults it.
//   - The source hands over memory it never touches again: each
//     enqueue()d chunk is a fresh buffer, never a SharedArrayBuffer, and
//     the source stops writing to byobRequest.view once it responds.
//     The conduit relies on this rather than checking it: it neither
//     refuses a shared chunk nor transfers what it delivers, so the
//     source's view of a responded read stays attached. The queued
//     controller does both, because its source is user code.
//   - PER-PULL CANCELLATION: pull receives an extension argument
//     pull(controller, signal) — an AbortSignal that fires when the
//     consumer abandons the read (releaseLock, cancel, tee). The source
//     MUST check signal.aborted synchronously immediately before
//     enqueue/respond; if aborted, it retains the bytes and redelivers
//     on the next pull. The controller REFUSES delivery on an aborted
//     signal as a backstop. This eliminates JS-side buffering for the
//     release-mid-pull race: race buffering lives SOURCE-SIDE; the JS
//     conduit is uniformly bufferless. A failure is kept the same way:
//     the conduit takes no rejection from an aborted pull, so a source
//     whose read fails rejects every later pull with that failure, and
//     its tee() gives two branches that fail with it.
//     FUTURE OPTIMIZATION: reuse a single AbortController per conduit
//     (reset between pulls) rather than allocating one per pull.
//   - MIN-READ / UNDER-DELIVERY CONTRACT (KJ tryRead semantics): the
//     respond is the source's COMPLETE answer for the read. Any internal
//     accumulation toward `atLeast` happens inside the native source;
//     the conduit never re-pulls an unsatisfied read. Responding with
//     fewer bytes than `atLeast` IMPLICITLY SIGNALS CLOSURE: the stream
//     closes, the partial fill commits as { done: false, value:
//     partialView }, and every subsequent read returns EOF (BYOB reads
//     get a zero-length view over their transferred buffer). This is the
//     C++-parity readAtLeast tail shape — the below-minimum
//     tail is never fused with the done flag — and it matches the
//     queued backend's deferred end-of-data settlement, so consumers
//     observe the same tail on both backends. The legacy C++ streams
//     differ only in settling the tail before the stream closes, which
//     happens at their next read. (Only an explicit same-turn
//     respond(0)-after-close on the QUEUED backend produces the spec's
//     fused { done: true, value: partial } fold; the native conduit has
//     no respond(0) — close() is the EOF verb.)
//   - Close is otherwise a fused close-commit: controller.close() may
//     follow a respond() in the same pull turn — deliberately divergent
//     from the queued backend's drain-then-sentinel model.
//   - tee(): returns a PAIR of new native source objects, leaving the
//     original source closed. The stream layer wraps each branch in a
//     fresh ReadableStream; branches are fully independent (no composite
//     cancel). tee() may arrive while an aborted pull's read is still
//     running (the reader was released mid-pull); both branches must then
//     deliver the bytes that read produces, ahead of anything later.
//   - expectedLength (non-standard extension, optional): the TOTAL bytes
//     the source promises to produce, a uint64 (the C++ source answers a
//     bigint), read once at construction and converted to a bigint by
//     normalizeExpectedLength (common.ts).
//     undefined = unknown (C++ uses chunked encoding). The conduit
//     enforces the exact-total contract: overflow at delivery and
//     underflow at source-initiated close (including min-read
//     under-delivery landing short) are RangeError violations.
//     Consumer-initiated cancel/tee/extraction are exempt. C++ reads the
//     property directly off the extracted source object.
//
// NATIVE-BACKEND INVARIANTS (binding on changes to this file; the queued
// backend in queue.ts has a DIFFERENT set — do not port logic across the
// fence without checking both):
//   - There is NO JS-side queue AND NO JS-SIDE OVERFLOW BUFFER: every
//     read becomes demand on the unified request FIFO, satisfied
//     directly by the source inside pull. Race buffering (the
//     release-mid-pull case) lives SOURCE-SIDE via the abort signal
//     contract — the source stashes and redelivers; the conduit never
//     holds data. Nothing here may assume entries, cursors, positions,
//     or the CLOSE_SENTINEL exist.
//   - JSG owns the source lifetime: no WeakRef owner tracking and no
//     FinalizationRegistry backstop (the queued backend needs both).
//   - The controller façade is consumed ONLY by the native source; it is
//     never handed to user code.
//   - Stream-level state transitions cross the module fence through the
//     injected NativeStreamHooks (readableStreamClose/Error on the far
//     side) — never by touching stream internals from here.
//   - The stream transitions before the reads it ends settle: every
//     close or error the conduit makes calls its hook first, then
//     settles the reads (spec ReadableStreamClose/ReadableStreamError:
//     reader.closed first), so reader.closed and the C++ EOF signal
//     precede the last read. (A cancel is closed by the stream layer
//     before it reaches the conduit.)
//   - A COMPLETION LATCH prevents busy-loop re-pulls: a pull that
//     settles without delivering, closing, or erroring while requests
//     are still pending errors the stream. Aborted pulls are exempt.
//
// Nothing in this module is ever exposed to user code.

import type {
  ByteStreamConsumer as ByteStreamConsumerType,
  PendingRead,
  PullIntoDescriptor,
  createReadResult as createReadResultType,
} from './queue';
import type {
  PromiseWithResolvers as PromiseWithResolversType,
  ReadableStreamReadResult,
} from './types';
import type {
  RingBuffer as RingBufferType,
  RingBufferConstructor,
} from './ring-buffer';
import type { CommonHelpers } from './common';
import type { ViewExtentHelpers } from './view-extent';

const {
  AbortController,
  AbortControllerAbort,
  AbortControllerSignalGet,
  AbortSignalAbortedGet,
  BigInt,
  NumberIsNaN,
  ObjectGetOwnPropertyDescriptor,
  ObjectGetPrototypeOf,
  ObjectPrototype,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  PromisePrototypeThen,
  RangeError,
  TypeError,
  TypedArrayPrototypeGetByteLength,
  Uint8Array,
  uncurryThis,
} = primordials;

const { isArrayBufferView, isUint8Array, markPromiseHandled } = utils;

const { isActualObject, normalizeExpectedLength, convertCallback } =
  require('webstreams/common') as CommonHelpers;
const { viewByteExtent } =
  require('webstreams/view-extent') as ViewExtentHelpers;
const { RingBuffer } = require('webstreams/ring-buffer') as {
  RingBuffer: RingBufferConstructor;
};
const { createReadResult } = require('webstreams/queue') as {
  createReadResult: typeof createReadResultType;
};

// ---------------------------------------------------------------------------
// The construction-time handshake

// A native underlying source carries this symbol as an own property whose
// value is the symbol itself. The value is currently meaningless beyond
// the identity check (reserved for future capability data).
const kNativeSource: symbol = utils.getApiSymbol('kNativeSource');

// Whether `object` carries `marker` (kNativeSource or kNativeSink) as its
// own; `kind` names the object in the error. Throws on a malformed marker
// — by the time the marker key is present at all, this is a contract
// violation on the native/mock side, never a user-input condition.
//
// Hardening: OWN-property read via descriptor — a marker symbol planted
// on Object.prototype must not convert every plain source or sink into a
// native one, and a hostile accessor at the marker is never invoked (we
// read desc.value; accessor-defined markers are ignored).
function hasOwnMarker(
  object: object,
  marker: symbol,
  kind: 'source' | 'sink'
): boolean {
  const desc = ObjectGetOwnPropertyDescriptor(object, marker) as
    PropertyDescriptor | undefined;
  const value = desc?.value as unknown;
  if (value === undefined) return false;
  if (value !== marker) {
    throw new TypeError(`Invalid native stream ${kind} marker`);
  }
  return true;
}

// --- Writable-side markers (same conventions as the readable side) ---

// A native underlying sink carries this symbol as an own property whose
// value is the symbol itself. Detection uses own-property descriptor
// read (inherited/accessor markers ignored). Unlike the source side,
// native sinks need no new backend machinery — the standard
// WritableStream drives them via start/write/close/abort. The marker
// exists for pipe dispatch (is the dest native?) and extraction (hand
// the sink to C++ for the native+native fast path). Two extension hooks
// beyond the standard sink surface:
//   - pipeFrom(source, options): the native+native pipe fast path,
//     called by the pipe dispatch when both ends are native-backed.
//   - detach(): called by detachWritableStream just before it drops its
//     reference, releasing the C++ sink immediately (and cancelling a
//     write in flight) when the underlying connection is taken over
//     (e.g. Socket startTls). Returns the error the stream's pending
//     writes reject with.
const kNativeSink: symbol = utils.getApiSymbol('kNativeSink');

// Extraction marker for native-backed WritableStream instances. Mirrors
// kExtractNativeSource: own, non-enumerable property, shared extractor
// function, one-shot via locked precondition. Writable has no
// "disturbed" concept — locked is the only precondition.
const kExtractNativeSink: symbol = utils.getApiSymbol('kExtractNativeSink');

function isNativeUnderlyingSink(sink: object): boolean {
  return hasOwnMarker(sink, kNativeSink, 'sink');
}

// The JS-to-C++ extraction marker. Installed on native-backed
// ReadableStream instances as an own, non-enumerable, non-writable,
// non-configurable property whose value is a shared extractor function.
// The C++ TypeWrapper detects this property's presence to classify the
// stream: present -> extract the native source for a pure C++ data
// path; absent -> acquire a DrainingReader instead. The marker is KEPT
// after extraction (presence means "native-backed", not "extractable");
// the extractor's locked/disturbed preconditions make it one-shot. The
// symbol comes from the runtime's API-symbol registry, unreachable from
// user code.
const kExtractNativeSource: symbol = utils.getApiSymbol('kExtractNativeSource');

// Returns true if `source` is native-marked; false for ordinary (queued)
// sources.
function isNativeUnderlyingSource(source: object): boolean {
  return hasOwnMarker(source, kNativeSource, 'source');
}

// Stream-level transitions injected by readable.ts at construction (the
// stream's private state lives across the module fence). Both are
// state-guarded on the far side, so calling them redundantly is safe.
export interface NativeStreamHooks {
  closeStream: () => void;
  errorStream: (reason: unknown) => void;
}

// ---------------------------------------------------------------------------
// The unified request FIFO
//
// Default reads and BYOB reads share ONE queue so results resolve in
// submission order. The head request's kind is what the controller façade
// reflects to the source: byobRequest !== null ⇔ the head is a BYOB read.

interface NativeDefaultRequest {
  kind: 'default';
  read: PendingRead<Uint8Array>;
}

interface NativeByobRequestEntry {
  kind: 'byob';
  desc: PullIntoDescriptor;
}

type NativeRequest = NativeDefaultRequest | NativeByobRequestEntry;

// What settles a request: its pending read, or the pull-into descriptor of
// its BYOB read.
function settlerOf(
  request: NativeRequest
): PendingRead<Uint8Array> | PullIntoDescriptor {
  return request.kind === 'default' ? request.read : request.desc;
}

// Cross-class accessors (assigned in static blocks).
let getRequestDesc: (
  request: NativeReadableStreamBYOBRequest
) => PullIntoDescriptor;
let isNativeController: (
  value: unknown
) => value is NativeReadableStreamController;
let getControllerConduit: (
  controller: NativeReadableStreamController
) => NativePullConduit;

// ---------------------------------------------------------------------------
// The BYOB request façade
//
// Wraps the head pull-into descriptor for the native source's consumption
// during pull. Mirrors a subset of the global ReadableStreamBYOBRequest
// (view/atLeast/respond, plus the elementSize extension — respondWithNewView
// is deliberately omitted; see the contract header) but is a distinct,
// module-private class: it is only ever handed to the native source, never
// to user code, so it needs no global registration or brand hardening
// beyond its private fields.
// Invalidated automatically once its descriptor is no longer the head
// request (view/atLeast/elementSize report null; responds throw).

let assertIsNativeReadableStreamBYOBRequest: (
  self: NativeReadableStreamBYOBRequest
) => void;

class NativeReadableStreamBYOBRequest {
  #conduit: NativePullConduit;
  #desc: PullIntoDescriptor;

  static {
    getRequestDesc = (request) => request.#desc;

    assertIsNativeReadableStreamBYOBRequest = function (
      self: NativeReadableStreamBYOBRequest
    ): void {
      if (!(#desc in self)) throw new TypeError('Illegal invocation');
    };
  }

  constructor(conduit: NativePullConduit, desc: PullIntoDescriptor) {
    this.#conduit = conduit;
    this.#desc = desc;
  }

  get view(): Uint8Array | null {
    assertIsNativeReadableStreamBYOBRequest(this);
    if (!this.#conduit.isHeadDesc(this.#desc)) return null;
    const desc = this.#desc;
    return new Uint8Array(
      desc.buffer,
      desc.byteOffset + desc.bytesFilled,
      desc.byteLength - desc.bytesFilled
    );
  }

  // Non-standard workerd extension (mirrors the global BYOBRequest): the
  // minimum number of BYTES the read requires. Delivering fewer is the
  // under-delivery EOF signal (see the min-read contract in the module
  // header). The subtraction is defensive: a live descriptor always has
  // bytesFilled === 0 under the once-per-read contract.
  get atLeast(): number | null {
    assertIsNativeReadableStreamBYOBRequest(this);
    if (!this.#conduit.isHeadDesc(this.#desc)) return null;
    const desc = this.#desc;
    const remaining = desc.minimumFill - desc.bytesFilled;
    return remaining > 0 ? remaining : 0;
  }

  // The read view's element size in bytes; every respond covers whole
  // elements (see the contract header).
  get elementSize(): number | null {
    assertIsNativeReadableStreamBYOBRequest(this);
    if (!this.#conduit.isHeadDesc(this.#desc)) return null;
    return this.#desc.elementSize;
  }

  respond(bytesWritten: number): void {
    assertIsNativeReadableStreamBYOBRequest(this);
    this.#conduit.respondToDesc(this.#desc, bytesWritten);
  }
}

// ---------------------------------------------------------------------------
// The controller façade
//
// Occupies the stream's #controller slot and is passed to the native
// source's pull(controller) per the standard pull algorithm. Consumed
// ONLY by the native source — never handed to user code — but the methods
// brand-check anyway, matching the file-wide convention.

let assertIsNativeReadableStreamController: (
  self: NativeReadableStreamController
) => void;

class NativeReadableStreamController {
  #conduit: NativePullConduit;

  static {
    isNativeController = (
      value: unknown
    ): value is NativeReadableStreamController => {
      return isActualObject(value) && #conduit in value;
    };

    getControllerConduit = (controller) => controller.#conduit;

    assertIsNativeReadableStreamController = function (
      self: NativeReadableStreamController
    ): void {
      if (!(#conduit in self)) throw new TypeError('Illegal invocation');
    };
  }

  constructor(conduit: NativePullConduit) {
    this.#conduit = conduit;
  }

  // Mode discrimination for the native source: non-null ⇔ the request at
  // the head of the FIFO is a BYOB read.
  get byobRequest(): NativeReadableStreamBYOBRequest | null {
    assertIsNativeReadableStreamController(this);
    return this.#conduit.getByobRequest();
  }

  // Exposed for shape parity with the standard controller. The native
  // source contractually never consults it: pacing is purely demand-
  // driven (effective high-water mark of zero; strategy is ignored).
  get desiredSize(): number | null {
    assertIsNativeReadableStreamController(this);
    return this.#conduit.desiredSize;
  }

  enqueue(chunk: ArrayBufferView): void {
    assertIsNativeReadableStreamController(this);
    this.#conduit.enqueue(chunk);
  }

  close(): void {
    assertIsNativeReadableStreamController(this);
    this.#conduit.closeFromSource();
  }

  error(reason?: unknown): void {
    assertIsNativeReadableStreamController(this);
    this.#conduit.errorFromSource(reason);
  }
}

// ---------------------------------------------------------------------------
// The native pull conduit (the consumer-fence implementation)

type PullAlgorithm = (
  controller: NativeReadableStreamController,
  signal: AbortSignal
) => Promise<void>;
type CancelAlgorithm = (reason: unknown) => Promise<void>;

interface NativeAlgorithms {
  pullAlgorithm: PullAlgorithm | undefined;
  cancelAlgorithm: CancelAlgorithm | undefined;
  teeAlgorithm: (() => unknown) | undefined;
  // Normalized expectedLength (undefined = unknown). The conduit
  // enforces the exact-total contract.
  expectedLength: bigint | undefined;
}

class NativePullConduit implements ByteStreamConsumerType {
  #requests: RingBufferType<NativeRequest> = new RingBuffer();
  // 'closed' covers source-close, stream cancel, and tee (the contract
  // leaves a teed-away original source closed).
  #status: 'active' | 'closed' | 'errored' = 'active';
  #storedError: unknown;
  // Set by a successful enqueue/respond, cleared at pull start. Used for
  // the once-per-pull delivery check AND the completion latch: a pull
  // that settles without delivering, closing, or erroring while requests
  // are still pending is a broken source — the latch errors the stream
  // rather than busy-looping the re-pull. Aborted pulls are exempt.
  #deliveredThisPull: boolean = false;
  // The AbortController of the pull in flight, or undefined when none is;
  // its presence serializes pulls. An aborted pull keeps it until the
  // pull's promise settles, so the next pull waits for that.
  #pullAbortController: AbortController | undefined;
  #pullAlgorithm: PullAlgorithm | undefined;
  #cancelAlgorithm: CancelAlgorithm | undefined;
  #teeAlgorithm: (() => unknown) | undefined;
  #hooks: NativeStreamHooks;
  #controller: NativeReadableStreamController;
  #byobRequestCache: NativeReadableStreamBYOBRequest | null = null;
  // Back-reference to the original source object, kept for extraction
  // (the JS-to-C++ path returns this object so C++ can unwrap its
  // backing class). Set once at construction and never cleared: after
  // detachForExtraction the conduit is terminal (#status 'closed'), so
  // retaining the reference is harmless and keeps the field non-optional.
  #source: object;
  // The exact-total byte contract (undefined = unknown/chunked). The
  // source must deliver exactly this many bytes over its lifetime:
  // overflow at delivery and underflow at source-initiated close are
  // RangeError violations. Consumer-initiated cancel/tee/extraction are
  // exempt (the source didn't break its promise; the consumer left).
  #expectedLength: bigint | undefined;
  #bytesDelivered: bigint = 0n;

  constructor(
    source: object,
    algorithms: NativeAlgorithms,
    hooks: NativeStreamHooks
  ) {
    this.#source = source;
    this.#expectedLength = algorithms.expectedLength;
    this.#pullAlgorithm = algorithms.pullAlgorithm;
    this.#cancelAlgorithm = algorithms.cancelAlgorithm;
    this.#teeAlgorithm = algorithms.teeAlgorithm;
    this.#hooks = hooks;
    this.#controller = new NativeReadableStreamController(this);
  }

  // The façade the stream holds as its controller.
  get controller(): NativeReadableStreamController {
    return this.#controller;
  }

  // --- StreamConsumer surface ----------------------------------------------

  // Native sources are always pull-based (async); synchronous reads are
  // never available.
  tryReadSync(
    _reader: object
  ): ReadableStreamReadResult<Uint8Array> | undefined {
    return undefined;
  }

  read(reader: object): Promise<ReadableStreamReadResult<Uint8Array>> {
    const { promise, resolve, reject } =
      PromiseWithResolvers() as PromiseWithResolversType<
        ReadableStreamReadResult<Uint8Array>
      >;
    // The pull prompt arrives via controllerPullIfNeeded from the reader
    // layer (spec PullSteps ordering), not here.
    this.submitRead({ resolve, reject, reader });
    return promise;
  }

  submitRead(request: PendingRead<Uint8Array>): void {
    if (this.#status === 'errored') {
      request.reject(this.#storedError);
      return;
    }
    if (this.#status === 'closed') {
      request.resolve(createReadResult(undefined, true));
      return;
    }
    this.#requests.push({
      kind: 'default',
      read: request,
    } satisfies NativeDefaultRequest);
  }

  drain(_maxSize?: number): { chunks: Uint8Array[]; done: boolean } {
    // Nothing is ever buffered on the JS side; the draining reader falls
    // back to read() for the wait-for-data case.
    return { chunks: [], done: this.#status === 'closed' };
  }

  cancelReadsForReader(reader: object, reason: unknown): void {
    const requests = this.#takeRequests();
    for (let i = 0; i < requests.length; i++) {
      const request = requests.get(i) as NativeRequest;
      const settler = settlerOf(request);
      if (settler.reader === reader) {
        settler.reject(reason);
      } else {
        this.#requests.push(request);
      }
    }
    // ABORT SITE: if the released reader's reads were the only demand and
    // a pull is in flight, abort it — the source contractually holds any
    // bytes and redelivers on the next pull.
    if (this.#requests.length === 0) this.#abortCurrentPull(reason);
  }

  errorAllReads(reason: unknown): void {
    const requests = this.#takeRequests();
    for (let i = 0; i < requests.length; i++) {
      settlerOf(requests.get(i) as NativeRequest).reject(reason);
    }
  }

  resolveAllReadsAsDone(): void {
    const requests = this.#takeRequests();
    for (let i = 0; i < requests.length; i++) {
      settlerOf(requests.get(i) as NativeRequest).resolve(
        createReadResult(undefined, true)
      );
    }
  }

  // Detaches the queued requests for a caller about to settle them, so
  // nothing a settlement prompts can change the queue being settled, and
  // drops the byobRequest minted for the old head.
  #takeRequests(): RingBufferType<NativeRequest> {
    const requests = this.#requests;
    this.#requests = new RingBuffer();
    this.#byobRequestCache = null;
    return requests;
  }

  get hasPendingRead(): boolean {
    return this.#requests.length > 0;
  }

  fulfillFirstPendingRead(value: Uint8Array): void {
    const pending = this.#requests.shift() as NativeDefaultRequest;
    pending.read.resolve(createReadResult(value, false));
  }

  cancelStream(
    reason: unknown,
    decideSourceCancel: (isLastConsumer: boolean) => Promise<void>
  ): Promise<void> {
    // Sole consumer by construction (native tee produces independent
    // streams, never shared consumers), so this conduit is always the
    // last one. Settle outstanding reads as done BEFORE the source-cancel
    // decision, per the StreamConsumer contract ordering.
    this.resolveAllReadsAsDone();
    // ABORT SITE: abort-then-cancel ordering. The cancel reason flows
    // through to signal.reason so the source can distinguish cancel from
    // release if it cares.
    this.#abortCurrentPull(reason);
    if (this.#status === 'active') this.#status = 'closed';
    return decideSourceCancel(true);
  }

  // --- ByteStreamConsumer surface ------------------------------------------

  readBYOB(
    desc: PullIntoDescriptor
  ): Promise<ReadableStreamReadResult<ArrayBufferView>> {
    if (this.#status === 'errored') {
      desc.reject(this.#storedError);
      return desc.promise;
    }
    if (this.#status === 'closed') {
      // Closed-stream BYOB semantics: hand the (transferred) buffer back
      // via a zero-length view, done: true.
      desc.resolve(
        createReadResult(
          new desc.viewCtor(desc.buffer, desc.byteOffset, 0),
          true
        )
      );
      return desc.promise;
    }
    this.#requests.push({
      kind: 'byob',
      desc,
    } satisfies NativeByobRequestEntry);
    return desc.promise;
  }

  get headPullInto(): PullIntoDescriptor | undefined {
    const head = this.#requests.peek();
    return head !== undefined && head.kind === 'byob' ? head.desc : undefined;
  }

  get pendingPullIntoView(): Uint8Array | undefined {
    const head = this.#requests.peek();
    if (head === undefined || head.kind !== 'byob') return undefined;
    const desc = head.desc;
    return new Uint8Array(
      desc.buffer,
      desc.byteOffset + desc.bytesFilled,
      desc.byteLength - desc.bytesFilled
    );
  }

  respondBYOB(bytesWritten: number): void {
    const head = this.#requests.peek();
    if (head === undefined || head.kind !== 'byob') {
      throw new TypeError('No pending BYOB read to respond to');
    }
    this.respondToDesc(head.desc, bytesWritten);
  }

  commitPullIntosOnClose(): void {
    // The fused close-commit happens in closeFromSource(); once the
    // conduit has left the active state there is nothing left to commit.
  }

  shiftAutoAllocateDescriptor(): PullIntoDescriptor | undefined {
    // Native conduit doesn't use autoAllocateChunkSize.
    return undefined;
  }

  // --- Pull pacing -----------------------------------------------------------

  // Pulls are serialized as the spec's [[pulling]] flag serializes them:
  // at most one is in flight (#pullAbortController), and a read made
  // meanwhile waits for it to settle. Demand-driven: pull only while
  // requests are pending (effective high-water mark of zero), and
  // re-evaluate after each pull settles for the REMAINING requests, which
  // covers what the spec's [[pullAgain]] records. A single request never
  // spans pulls (the respond is the complete answer for its read — see
  // the min-read contract).
  //
  // PER-PULL CANCELLATION: each pull invocation gets a fresh
  // AbortController whose signal is passed as pull(controller, signal).
  // The source contractually checks signal.aborted immediately before
  // enqueue/respond; the controller backstop refuses delivery on an
  // aborted signal. An aborted pull's settlement is not the source's
  // answer to anything: the source held its data for the next pull, so a
  // fulfilled one skips the completion latch, and a rejected one (kj
  // cancellation of its read) does not error a healthy stream.
  pullIfNeeded(): void {
    if (this.#status !== 'active') return;
    if (this.#requests.length === 0) return;
    const pullAlgorithm = this.#pullAlgorithm;
    if (pullAlgorithm === undefined) return;
    if (this.#pullAbortController !== undefined) return;
    const ac = new AbortController();
    this.#pullAbortController = ac;
    this.#deliveredThisPull = false;
    const signal = AbortControllerSignalGet(ac);
    markPromiseHandled(
      PromisePrototypeThen(
        pullAlgorithm(this.#controller, signal),
        () => {
          this.#pullAbortController = undefined;
          // COMPLETION LATCH: a live pull that settled without delivering,
          // closing, or erroring while requests are still pending is a
          // broken source. Error the stream to prevent a busy-loop
          // re-pull against a source that will never deliver.
          if (
            !AbortSignalAbortedGet(signal) &&
            !this.#deliveredThisPull &&
            this.#status === 'active' &&
            this.#requests.length > 0
          ) {
            this.errorFromSource(
              new TypeError(
                'native source pull settled without delivering, closing, ' +
                  'or erroring; the source appears broken'
              )
            );
            return;
          }
          this.pullIfNeeded();
        },
        (e: unknown) => {
          this.#pullAbortController = undefined;
          if (AbortSignalAbortedGet(signal)) {
            this.pullIfNeeded();
          } else {
            this.errorFromSource(e);
          }
        }
      ) as Promise<void>
    );
  }

  // Aborts the in-flight pull's signal. #pullAbortController is
  // deliberately NOT cleared: the aborted pull's promise must settle
  // before a new pull can start, keeping pulls serialized. The settle
  // handler detects the abort via signal.aborted.
  #abortCurrentPull(reason: unknown): void {
    const ac = this.#pullAbortController;
    if (ac === undefined) return;
    this.#deliveredThisPull = false;
    this.#byobRequestCache = null;
    AbortControllerAbort(ac, reason);
  }

  // --- The source-facing operations (via the controller façade) -------------

  isHeadDesc(desc: PullIntoDescriptor): boolean {
    const head = this.#requests.peek();
    return (
      this.#status === 'active' &&
      head !== undefined &&
      head.kind === 'byob' &&
      head.desc === desc
    );
  }

  getByobRequest(): NativeReadableStreamBYOBRequest | null {
    const head = this.#requests.peek();
    if (
      this.#status !== 'active' ||
      head === undefined ||
      head.kind !== 'byob'
    ) {
      return null;
    }
    const cached = this.#byobRequestCache;
    if (cached !== null && getRequestDesc(cached) === head.desc) {
      return cached;
    }
    const request = new NativeReadableStreamBYOBRequest(this, head.desc);
    this.#byobRequestCache = request;
    return request;
  }

  get desiredSize(): number | null {
    if (this.#status === 'errored') return null;
    // 0 in both the closed and readable states: there is no queue, so the
    // demand-driven model never reports positive headroom.
    return 0;
  }

  enqueue(chunk: ArrayBufferView): void {
    // A pull may legally settle after close/cancel/error/tee; a late
    // enqueue is then a no-op rather than a violation.
    if (this.#status !== 'active') return;
    if (!isArrayBufferView(chunk)) {
      throw new TypeError('enqueue requires an ArrayBufferView');
    }
    if (this.#deliveredThisPull) {
      // Contract: at most one enqueue-or-respond per pull invocation.
      // Structurally guaranteed on the native side; kept as a loud check
      // for mock authors.
      throw new TypeError(
        'the native source must deliver at most once per pull invocation'
      );
    }
    const head = this.#requests.peek();
    if (head === undefined) {
      // REFUSAL BACKSTOP: the demand was withdrawn (reader released /
      // stream cancelled) and the pull's signal was aborted. A
      // conforming source checks signal.aborted before delivery and
      // stashes the bytes for the next pull. Refusing loudly here
      // catches sources that skip the check.
      throw new TypeError(
        'delivery refused: the pull was aborted (the source must check ' +
          'signal.aborted before enqueue/respond and stash data for ' +
          'redelivery)'
      );
    }
    if (head.kind !== 'byob') {
      // Normalize to a Uint8Array over the same bytes (the conduit is
      // byte-oriented). Metadata reads go through captured getters.
      let view: Uint8Array;
      if (isUint8Array(chunk)) {
        view = chunk as Uint8Array;
      } else {
        const { buffer, byteOffset, byteLength } = viewByteExtent(chunk);
        view = new Uint8Array(buffer, byteOffset, byteLength);
      }
      const byteLength = TypedArrayPrototypeGetByteLength(view) as number;
      if (byteLength === 0) {
        throw new TypeError(
          'enqueue() of an empty chunk is not supported for native ' +
            'streams; an empty answer means end-of-stream, so call ' +
            'close() instead'
        );
      }
      // EXPECTED-LENGTH CONTRACT: overflow check before commit.
      this.#accountDelivery(byteLength);
      this.#requests.shift();
      this.#deliveredThisPull = true;
      head.read.resolve(createReadResult(view, false));
      return;
    }
    // Head is a BYOB read: the source must use the byobRequest instead.
    // (The fill could be emulated by copying, but the contract says the
    // native side discriminates via byobRequest — keep the boundary firm.)
    throw new TypeError(
      'enqueue while a BYOB read is pending; use byobRequest.respond()'
    );
  }

  respondToDesc(desc: PullIntoDescriptor, bytesWritten: number): void {
    // Source-initiated stragglers: silent discard (see enqueue comment).
    if (this.#status !== 'active') return;
    if (this.#deliveredThisPull) {
      throw new TypeError(
        'the native source must deliver at most once per pull invocation'
      );
    }
    if (!this.isHeadDesc(desc)) {
      if (this.#requests.length === 0) {
        // REFUSAL BACKSTOP: same as the enqueue case — the pull was
        // aborted, the source should have checked signal.aborted.
        throw new TypeError(
          'delivery refused: the pull was aborted (the source must check ' +
            'signal.aborted before enqueue/respond and stash data for ' +
            'redelivery)'
        );
      }
      throw new TypeError('This BYOB request has been invalidated');
    }
    const written = +bytesWritten;
    if (NumberIsNaN(written) || written % 1 !== 0 || written < 0) {
      throw new TypeError('bytesWritten must be a non-negative integer');
    }
    if (written === 0) {
      throw new TypeError(
        'respond(0) is not supported for native streams; an empty answer ' +
          'means end-of-stream, so call close() instead'
      );
    }
    const filled = desc.bytesFilled + written;
    if (filled > desc.byteLength) {
      throw new RangeError('bytesWritten exceeds the remaining view size');
    }
    if (filled % desc.elementSize !== 0) {
      // Every respond commits (see below), so every respond must account
      // a whole number of elements; the source carries any remainder.
      throw new TypeError(
        'native sources must respond in whole-element multiples of the view'
      );
    }
    // EXPECTED-LENGTH CONTRACT: overflow check before commit.
    this.#accountDelivery(written);
    desc.bytesFilled = filled;
    this.#deliveredThisPull = true;
    // MIN-READ CONTRACT: the respond is the source's COMPLETE answer for
    // this read. Delivering fewer bytes than the requested minimum
    // (atLeast) signals end-of-stream: the stream closes, the partial fill
    // commits { done: false, value: partialView }, and every subsequent
    // read returns EOF — the C++-parity readAtLeast tail shape. The
    // conduit never re-pulls an unsatisfied read — any accumulation toward
    // the minimum happens inside the native source (tryRead-style). State
    // transitions before promise resolutions.
    //
    // EXPECTED-LENGTH: under-delivery is a close signal, so the
    // exact-total contract applies — landing short of expectedLength is
    // underflow and errors the stream instead of closing it. (Landing
    // exactly on it is a legitimate close-with-delivery.)
    const endsStream = filled < desc.minimumFill;
    if (
      endsStream &&
      this.#expectedLength !== undefined &&
      this.#bytesDelivered < this.#expectedLength
    ) {
      this.errorFromSource(
        new RangeError(
          'native source closed (via min-read under-delivery) before ' +
            'producing its declared expectedLength'
        )
      );
      return;
    }
    this.#byobRequestCache = null;
    this.#requests.shift();
    const value = new desc.viewCtor(
      desc.buffer,
      desc.byteOffset,
      filled / desc.elementSize
    );
    if (!endsStream) {
      desc.resolve(createReadResult(value, false));
      return;
    }
    this.#status = 'closed';
    this.#hooks.closeStream();
    desc.resolve(createReadResult(value, false));
    this.#settleRemainingAsEof();
  }

  // The exact-total accounting: adds `byteLength` to the running total,
  // throwing RangeError on overflow (delivering past the declared
  // expectedLength). Called from both delivery paths before commit.
  // On overflow, errors the stream explicitly before throwing — the
  // throw alone only reaches the caller, which may be an external sink
  // that only errors the writable side (leaving the readable hanging).
  #accountDelivery(byteLength: number): void {
    if (this.#expectedLength === undefined) return;
    const delivered = this.#bytesDelivered + BigInt(byteLength);
    if (delivered > this.#expectedLength) {
      const e = new RangeError(
        'native source delivered more bytes than its declared expectedLength'
      );
      this.errorFromSource(e);
      throw e;
    }
    this.#bytesDelivered = delivered;
  }

  get expectedLength(): bigint | undefined {
    return this.#expectedLength;
  }

  // Resolves every queued request as end-of-stream (default reads get
  // {done, undefined}; BYOB reads get their buffer back via a zero-length
  // view). Shared by the close paths, after their closeStream hook.
  #settleRemainingAsEof(): void {
    const requests = this.#takeRequests();
    for (let i = 0; i < requests.length; i++) {
      const request = requests.get(i) as NativeRequest;
      if (request.kind === 'default') {
        request.read.resolve(createReadResult(undefined, true));
      } else {
        const desc = request.desc;
        desc.resolve(
          createReadResult(
            new desc.viewCtor(desc.buffer, desc.byteOffset, 0),
            true
          )
        );
      }
    }
  }

  closeFromSource(): void {
    if (this.#status !== 'active') return; // late settle tolerated
    // EXPECTED-LENGTH CONTRACT: source-initiated close before reaching
    // the declared total is underflow — an error, not a clean close.
    // (Consumer-initiated cancel/tee/extraction are exempt; the source
    // kept its promise, the consumer left.)
    if (
      this.#expectedLength !== undefined &&
      this.#bytesDelivered < this.#expectedLength
    ) {
      this.errorFromSource(
        new RangeError(
          'native source closed before producing its declared expectedLength'
        )
      );
      return;
    }
    this.#status = 'closed';
    // Every respond commits its read (see respondToDesc), so no read holds
    // bytes at close: the spec's check for a partially filled pull-into
    // (ReadableByteStreamControllerClose) has nothing to find here.
    this.#hooks.closeStream();
    this.#settleRemainingAsEof();
  }

  errorFromSource(reason: unknown): void {
    if (this.#status !== 'active') return; // late settle tolerated
    this.#status = 'errored';
    this.#storedError = reason;
    // The stream errors before its reads reject (spec
    // ReadableStreamError: reader.closed first).
    this.#hooks.errorStream(reason);
    this.errorAllReads(reason);
  }

  // Spec PromiseCall over the extracted cancel method; resolved if the
  // source declared none. Invoked through the controller dispatch chain
  // (the stream layer's cancel policy callback).
  cancelSource(reason: unknown): Promise<void> {
    if (this.#status === 'active') this.#status = 'closed';
    this.#byobRequestCache = null;
    const cancelAlgorithm = this.#cancelAlgorithm;
    if (cancelAlgorithm === undefined) {
      return PromiseResolve();
    }
    return cancelAlgorithm(reason);
  }

  onReaderRelease(): void {
    // The released reader's requests are rejected separately via
    // cancelReadsForReader; here we just drop any byobRequest minted for
    // a descriptor that is about to be rejected.
    this.#byobRequestCache = null;
  }

  // The native tee handoff: calls the source's tee hook, which returns a
  // pair of new native source objects and leaves the original source
  // closed. The stream layer constructs the branch streams and locks the
  // parent; this conduit becomes inert.
  teeSource(): [object, object] {
    if (this.#status !== 'active') {
      throw new TypeError('Cannot tee a closed or errored native stream');
    }
    if (this.#requests.length > 0) {
      // Unreachable through tee() (pending reads imply a locked stream,
      // and tee() rejects locked streams upstream) — kept as a guard.
      throw new TypeError('Cannot tee a native stream with pending reads');
    }
    // ABORT SITE: moot by construction (an in-flight pull at tee time
    // implies a prior release, which already aborted), but specified for
    // completeness.
    this.#abortCurrentPull(new TypeError('stream was teed'));
    const teeAlgorithm = this.#teeAlgorithm;
    if (teeAlgorithm === undefined) {
      throw new TypeError('This native stream source does not support tee');
    }
    const result = teeAlgorithm() as { 0: unknown; 1: unknown } | null;
    const branch1 = isActualObject(result)
      ? (result as { 0: unknown })[0]
      : undefined;
    const branch2 = isActualObject(result)
      ? (result as { 1: unknown })[1]
      : undefined;
    if (!isActualObject(branch1) || !isActualObject(branch2)) {
      throw new TypeError(
        'The native source tee hook must return a pair of source objects'
      );
    }
    // Per the contract the hook leaves the original source closed; mirror
    // that here so late pull settles become no-ops.
    this.#status = 'closed';
    this.#byobRequestCache = null;
    return [branch1, branch2];
  }

  // JS-to-C++ extraction: returns the original native source object and
  // terminates the conduit. The caller (readable.ts extractor) handles
  // stream-level locking/disturbing; this method handles conduit-level
  // cleanup only. Precondition: the caller validated locked/disturbed
  // already, so no in-flight pull is possible (pull requires a prior
  // read, which sets disturbed). The abort site is specified anyway.
  detachForExtraction(): object {
    const source = this.#source;
    this.#abortCurrentPull(new TypeError('stream was extracted'));
    this.#status = 'closed';
    this.#byobRequestCache = null;
    return source;
  }

  // Non-detaching source access for read-only queries from the C++ bridge
  // (the encoding-aware tryGetLength arm). The conduit's own status is
  // deliberately not consulted: the C++ source object answers from its own
  // lifecycle (a completed or extracted source reports no length).
  peekSource(): object {
    return this.#source;
  }
}

// ---------------------------------------------------------------------------
// Construction glue

// Whether the source declares `key` itself or on its own prototype. The
// C++ source's members live on its prototype, so a plain read of a member
// it lacks would reach Object.prototype.
function declaresMember(source: object, key: string): boolean {
  if (ObjectGetOwnPropertyDescriptor(source, key) !== undefined) return true;
  const proto = ObjectGetPrototypeOf(source) as object | null;
  return (
    proto !== null &&
    proto !== ObjectPrototype &&
    ObjectGetOwnPropertyDescriptor(proto, key) !== undefined
  );
}

// Extraction follows the queued controllers' conventions: properties are
// read ONCE, validated, and invoked thereafter only via captured
// uncurryThis wrappers with the source as `this` (spec
// CreateAlgorithmFromUnderlyingMethod / PromiseCall). `start` is
// deliberately NOT read: native sources have no start algorithm (assumed
// no-op per the contract; its presence is simply ignored).
function createNativeReadableStreamParts(
  source: object,
  hooks: NativeStreamHooks
): {
  controller: NativeReadableStreamController;
  conduit: NativePullConduit;
} {
  // Native sources must not look like queued sources: the reader layer's
  // autoAllocate synthesis is queued-only and relies on this prohibition,
  // and a declared type would suggest the object was built for the queued
  // byte path. Checked as declarations, since the C++ source has neither.
  if (declaresMember(source, 'autoAllocateChunkSize')) {
    throw new TypeError(
      'Native stream sources must not declare autoAllocateChunkSize'
    );
  }
  if (declaresMember(source, 'type')) {
    throw new TypeError('Native stream sources must not declare type');
  }
  const {
    cancel,
    expectedLength: rawExpectedLength,
    pull,
    tee,
  } = source as {
    cancel?: (reason: unknown) => void | PromiseLike<void>;
    expectedLength?: unknown;
    pull?: (
      controller: NativeReadableStreamController,
      signal: AbortSignal
    ) => void | PromiseLike<void>;
    tee?: () => unknown;
  };
  const cancelFn = convertCallback(cancel, 'underlyingSource.cancel');
  const pullFn = convertCallback(pull, 'underlyingSource.pull');
  const teeFn = convertCallback(tee, 'underlyingSource.tee');
  // Non-standard extension: the TOTAL bytes this source promises to
  // produce. Read once here and cached; the conduit enforces the
  // contract (overflow at delivery, underflow at source-initiated
  // close). C++ reads it directly off the extracted source object.
  const expectedLength = normalizeExpectedLength(rawExpectedLength);

  let pullAlgorithm: PullAlgorithm | undefined;
  if (pullFn !== undefined) {
    const callPull = uncurryThis(pullFn);
    pullAlgorithm = (
      controller: NativeReadableStreamController,
      signal: AbortSignal
    ) => {
      try {
        return PromiseResolve(callPull(source, controller, signal));
      } catch (e) {
        return PromiseReject(e);
      }
    };
  }
  let cancelAlgorithm: CancelAlgorithm | undefined;
  if (cancelFn !== undefined) {
    const callCancel = uncurryThis(cancelFn);
    cancelAlgorithm = (reason: unknown) => {
      try {
        return PromiseResolve(callCancel(source, reason));
      } catch (e) {
        return PromiseReject(e);
      }
    };
  }
  let teeAlgorithm: (() => unknown) | undefined;
  if (teeFn !== undefined) {
    const callTee = uncurryThis(teeFn);
    // Synchronous by contract: the branches exist upon return.
    teeAlgorithm = () => callTee(source);
  }

  const conduit = new NativePullConduit(
    source,
    { pullAlgorithm, cancelAlgorithm, teeAlgorithm, expectedLength },
    hooks
  );
  return { controller: conduit.controller, conduit };
}

// ---------------------------------------------------------------------------
// Dispatch-chain behaviors (joined to the chained controller helpers by
// readable.ts; the chain lets live across the module fence).

function nativeControllerPullIfNeeded(
  controller: NativeReadableStreamController
): void {
  getControllerConduit(controller).pullIfNeeded();
}

function nativeControllerCancelSteps(
  controller: NativeReadableStreamController,
  reason: unknown
): Promise<void> {
  return getControllerConduit(controller).cancelSource(reason);
}

// The controller's error() for internal callers (the prototype method is
// user-patchable).
function nativeControllerError(
  controller: NativeReadableStreamController,
  reason: unknown
): void {
  getControllerConduit(controller).errorFromSource(reason);
}

function nativeControllerMaybeCloseStream(
  _controller: NativeReadableStreamController
): void {
  // No deferred-close bookkeeping exists: close propagation is the fused
  // close-commit on the read path.
}

function nativeControllerOnReaderRelease(
  controller: NativeReadableStreamController
): void {
  getControllerConduit(controller).onReaderRelease();
}

function nativeControllerTeeSource(
  controller: NativeReadableStreamController
): [object, object] {
  return getControllerConduit(controller).teeSource();
}

function nativeControllerExtractSource(
  controller: NativeReadableStreamController
): object {
  return getControllerConduit(controller).detachForExtraction();
}

function nativeControllerExpectedLength(
  controller: NativeReadableStreamController
): bigint | undefined {
  return getControllerConduit(controller).expectedLength;
}

function nativeControllerPeekSource(
  controller: NativeReadableStreamController
): object {
  return getControllerConduit(controller).peekSource();
}

// Internal namespace, never re-exported to users.
const nativeStreamInternals = {
  kNativeSource,
  kExtractNativeSource,
  kNativeSink,
  kExtractNativeSink,
  isNativeUnderlyingSource,
  isNativeUnderlyingSink,
  isNativeController,
  createNativeReadableStreamParts,
  nativeControllerPullIfNeeded,
  nativeControllerCancelSteps,
  nativeControllerError,
  nativeControllerMaybeCloseStream,
  nativeControllerOnReaderRelease,
  nativeControllerTeeSource,
  nativeControllerExtractSource,
  nativeControllerExpectedLength,
  nativeControllerPeekSource,
};

// Type-only exports (fully erased at runtime — the loader sees only the
// module.exports assignment below, matching the queue.ts pattern).
// NativeStreamInternals lets the require() site in readable.ts cast the
// untyped loader result back to the real shape, preserving the brand
// predicates' type-guard narrowing.
export type { NativeReadableStreamController, NativePullConduit };
export type NativeStreamInternals = typeof nativeStreamInternals;

module.exports = { nativeStreamInternals };
