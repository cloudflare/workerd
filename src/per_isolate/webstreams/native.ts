'use strict';

// The NATIVE consumer backend: streams whose data is produced by a C++
// (JSG/KJ) source rather than a JS underlying source feeding the shared
// queue. See native-stream-integration.md §10 for the fence conventions
// separating this backend from the queued one (queue.ts).
//
// THE C++/JS CONTRACT (phase 3; currently exercised via JS mocks — the
// real C++ source arrives in a later integration step and must conform):
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
//     read's minimum, in bytes). respondWithNewView() is deliberately
//     omitted: the native source writes into consumer-provided memory
//     (KJ tryRead semantics); buffer-swapping has no C++ analogue, and
//     the default-read enqueue() path already covers source-allocated
//     buffers.
//     byobRequest === null ⇒ default read — the source allocates its OWN
//     buffer and calls controller.enqueue(view).
//   - The source calls enqueue-or-respond AT MOST ONCE per pull
//     invocation and never pushes outside pull. desiredSize is exposed
//     but the native source never consults it.
//   - PER-PULL CANCELLATION: pull receives an extension argument
//     pull(controller, signal) — an AbortSignal that fires when the
//     consumer abandons the read (releaseLock, cancel, tee). The source
//     MUST check signal.aborted synchronously immediately before
//     enqueue/respond; if aborted, it retains the bytes and redelivers
//     on the next pull. The controller REFUSES delivery on an aborted
//     signal as a backstop. This eliminates JS-side buffering for the
//     release-mid-pull race: race buffering lives SOURCE-SIDE; the JS
//     conduit is uniformly bufferless.
//     FUTURE OPTIMIZATION: reuse a single AbortController per conduit
//     (reset between pulls) rather than allocating one per pull.
//   - MIN-READ / UNDER-DELIVERY CONTRACT (KJ tryRead semantics): the
//     respond is the source's COMPLETE answer for the read. Any internal
//     accumulation toward `atLeast` happens inside the native source;
//     the conduit never re-pulls an unsatisfied read. Responding with
//     fewer bytes than `atLeast` IMPLICITLY SIGNALS CLOSURE: the partial
//     fill commits fused as { done: true, value: partialView }, the
//     stream closes, and every subsequent read returns EOF. This result
//     is deliberately INDISTINGUISHABLE from the standard's one fused
//     corner — a pending min read committed in the closed state via
//     close() + respond(0) (RespondInClosedState →
//     CommitPullIntoDescriptor, which fulfills the read-into request
//     with the partial view and done: true). The signaling MECHANISM
//     differs (a queued JS source is pulled again and must close()
//     explicitly; for a native source the under-delivery itself is the
//     signal), but consumers observe the same result shapes on both
//     backends.
//   - Close is otherwise a fused close-commit: controller.close() may
//     follow a respond() in the same pull turn — deliberately divergent
//     from the queued backend's drain-then-sentinel model.
//   - tee(): returns a PAIR of new native source objects, leaving the
//     original source closed. The stream layer wraps each branch in a
//     fresh ReadableStream; branches are fully independent (no composite
//     cancel).
//   - expectedLength (non-standard extension, optional): the TOTAL bytes
//     the source promises to produce — a non-negative bigint or integer
//     number that fits in a uint64 (normalized to bigint), read once at
//     construction.
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
//   - A COMPLETION LATCH prevents busy-loop re-pulls: a pull that
//     settles without delivering, closing, or erroring while requests
//     are still pending errors the stream. Aborted pulls are exempt.
//
// Nothing in this module is ever exposed to user code, with one TEMPORARY
// exception: streams.ts re-exports kNativeSource so mock native sources
// can be constructed from tests before the real C++ integration exists.

import type {
  ByteStreamConsumer as ByteStreamConsumerType,
  PendingRead,
  PullIntoDescriptor,
} from './queue';
import type {
  PromiseWithResolvers as PromiseWithResolversType,
  ReadableStreamReadResult,
} from './types';

const {
  AbortController,
  AbortControllerAbort,
  AbortControllerSignalGet,
  AbortSignalAbortedGet,
  ArrayPrototypePush,
  ArrayPrototypeShift,
  ArrayPrototypeSplice,
  BigInt,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  NumberIsNaN,
  ObjectGetOwnPropertyDescriptor,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  PromisePrototypeThen,
  RangeError,
  TypeError,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetSymbolToStringTag,
  Uint8Array,
  uncurryThis,
} = primordials;

const { isArrayBufferView, isUint8Array, markPromiseHandled } = utils;

// ---------------------------------------------------------------------------
// The construction-time handshake

// A native underlying source carries this symbol as an own property whose
// value is the symbol itself. The value is currently meaningless beyond
// the identity check (reserved for future capability data).
const kNativeSource: symbol = utils.getApiSymbol('kNativeSource');

// --- Writable-side markers (same conventions as the readable side) ---

// A native underlying sink carries this symbol as an own property whose
// value is the symbol itself. Detection uses own-property descriptor
// read (inherited/accessor markers ignored). Unlike the source side,
// native sinks need no new backend machinery — the standard
// WritableStream drives them via start/write/close/abort. The marker
// exists for pipe dispatch (is the dest native?) and extraction (hand
// the sink to C++ for the native+native fast path). The one extension:
// pipeFrom(source, options), the hook for the native+native pipe.
const kNativeSink: symbol = utils.getApiSymbol('kNativeSink');

// Extraction marker for native-backed WritableStream instances. Mirrors
// kExtractNativeSource: own, non-enumerable property, shared extractor
// function, one-shot via locked precondition. Writable has no
// "disturbed" concept — locked is the only precondition.
const kExtractNativeSink: symbol = utils.getApiSymbol('kExtractNativeSink');

function isNativeUnderlyingSink(sink: object): boolean {
  const desc = ObjectGetOwnPropertyDescriptor(sink, kNativeSink) as
    | PropertyDescriptor
    | undefined;
  const value = desc?.value as unknown;
  if (value === undefined) return false;
  if (value !== kNativeSink) {
    throw new TypeError('Invalid native stream sink marker');
  }
  return true;
}

// The JS-to-C++ extraction marker. Installed on native-backed
// ReadableStream instances as an own, non-enumerable, non-writable,
// non-configurable property whose value is a shared extractor function.
// The C++ TypeWrapper detects this property's presence to classify the
// stream: present -> extract the native source for a pure C++ data
// path; absent -> acquire a DrainingReader instead. The marker is KEPT
// after extraction (presence means "native-backed", not "extractable");
// the extractor's locked/disturbed preconditions make it one-shot.
// Bootstrap phase: regular symbol (temporarily exposed via streams.ts).
// Final implementation: runtime-provided private API symbol.
const kExtractNativeSource: symbol = utils.getApiSymbol('kExtractNativeSource');

function isActualObject(value: unknown): value is object {
  return value != null && typeof value === 'object';
}

// Returns true if `source` is native-marked; false for ordinary (queued)
// sources. Throws on a malformed marker — by the time the marker key is
// present at all, this is a contract violation on the native/mock side,
// never a user-input condition.
//
// Hardening: OWN-property read via descriptor — a (temporarily exposed)
// symbol planted on Object.prototype must not convert every plain source
// into a native one, and a hostile accessor at the marker is never
// invoked (we read desc.value; accessor-defined markers are ignored).
function isNativeUnderlyingSource(source: object): boolean {
  const desc = ObjectGetOwnPropertyDescriptor(source, kNativeSource) as
    | PropertyDescriptor
    | undefined;
  const value = desc?.value as unknown;
  if (value === undefined) return false;
  if (value !== kNativeSource) {
    throw new TypeError('Invalid native stream source marker');
  }
  return true;
}

// Stream-level transitions injected by readable.ts at construction (the
// stream's private state lives across the module fence). Both are
// state-guarded on the far side, so calling them redundantly is safe.
export interface NativeStreamHooks {
  closeStream: () => void;
  errorStream: (reason: unknown) => void;
}

// Maximum expectedLength: uint64 max. The value is destined for the C++
// bridge (Content-Length is carried as uint64_t through the C++/KJ HTTP
// layer), so larger totals are not representable. Mirrors the
// FixedLengthStream constructor validation in identity.ts.
const MAX_UINT64 = 0xffff_ffff_ffff_ffffn;

// Normalizes the non-standard `expectedLength` extension property: the
// TOTAL number of bytes the source promises to produce over its
// lifetime (undefined = unknown; C++ then uses chunked encoding).
// Accepts a bigint or integer number in [0, 2^64) — totals can exceed
// MAX_SAFE_INTEGER but must fit in a uint64. Shape violations (wrong
// type) are TypeErrors; value violations (non-integer, negative,
// > uint64 max) are RangeErrors, matching FixedLengthStream
// (identity.ts). A near-duplicate (pending consolidation; no uint64
// cap yet) lives in readable.ts for the queued byte controller.
function normalizeExpectedLength(value: unknown): bigint | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== 'bigint' && typeof value !== 'number') {
    throw new TypeError(
      'expectedLength must be a non-negative bigint or integer'
    );
  }
  // BigInt() conversion rejects NaN, ±Infinity, and fractions
  // (RangeError) naturally. The typeof gate above is what prevents
  // BigInt() string/object coercion (e.g. '5' → 5n) from sneaking in.
  const normalized = typeof value === 'bigint' ? value : BigInt(value);
  if (normalized < 0n || normalized > MAX_UINT64) {
    throw new RangeError(
      'expectedLength must be a non-negative integer that fits in a uint64'
    );
  }
  return normalized;
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
// (view/atLeast/respond — respondWithNewView is deliberately omitted; see
// the contract header) but is a distinct, module-private class: it is only
// ever handed to the native source, never to user code, so it needs no
// global registration or brand hardening beyond its private fields.
// Invalidated automatically once its descriptor is no longer the head
// request (view/atLeast report null; responds throw).

class NativeReadableStreamBYOBRequest {
  #conduit: NativePullConduit;
  #desc: PullIntoDescriptor;

  static {
    getRequestDesc = (request) => request.#desc;
  }

  constructor(conduit: NativePullConduit, desc: PullIntoDescriptor) {
    this.#conduit = conduit;
    this.#desc = desc;
  }

  get view(): Uint8Array | null {
    if (!(#desc in this)) throw new TypeError('Illegal invocation');
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
    if (!(#desc in this)) throw new TypeError('Illegal invocation');
    if (!this.#conduit.isHeadDesc(this.#desc)) return null;
    const desc = this.#desc;
    const remaining = desc.minimumFill - desc.bytesFilled;
    return remaining > 0 ? remaining : 0;
  }

  respond(bytesWritten: number): void {
    if (!(#desc in this)) throw new TypeError('Illegal invocation');
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

class NativeReadableStreamController {
  #conduit: NativePullConduit;

  static {
    isNativeController = (
      value: unknown
    ): value is NativeReadableStreamController => {
      return isActualObject(value) && #conduit in value;
    };

    getControllerConduit = (controller) => controller.#conduit;
  }

  constructor(conduit: NativePullConduit) {
    this.#conduit = conduit;
  }

  // Mode discrimination for the native source: non-null ⇔ the request at
  // the head of the FIFO is a BYOB read.
  get byobRequest(): NativeReadableStreamBYOBRequest | null {
    if (!(#conduit in this)) throw new TypeError('Illegal invocation');
    return this.#conduit.getByobRequest();
  }

  // Exposed for shape parity with the standard controller. The native
  // source contractually never consults it: pacing is purely demand-
  // driven (effective high-water mark of zero; strategy is ignored).
  get desiredSize(): number | null {
    if (!(#conduit in this)) throw new TypeError('Illegal invocation');
    return this.#conduit.desiredSize;
  }

  enqueue(chunk: ArrayBufferView): void {
    if (!(#conduit in this)) throw new TypeError('Illegal invocation');
    this.#conduit.enqueue(chunk);
  }

  close(): void {
    if (!(#conduit in this)) throw new TypeError('Illegal invocation');
    this.#conduit.closeFromSource();
  }

  error(reason?: unknown): void {
    if (!(#conduit in this)) throw new TypeError('Illegal invocation');
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
  #requests: NativeRequest[] = [];
  // 'closed' covers source-close, stream cancel, and tee (the contract
  // leaves a teed-away original source closed).
  #status: 'active' | 'closed' | 'errored' = 'active';
  #storedError: unknown;
  #pulling: boolean = false;
  #pullAgain: boolean = false;
  // Set by a successful enqueue/respond, cleared at pull start. Used for
  // the once-per-pull delivery check AND the completion latch: a pull
  // that settles without delivering, closing, or erroring while requests
  // are still pending is a broken source — the latch errors the stream
  // rather than busy-looping the re-pull. Aborted pulls are exempt.
  #deliveredThisPull: boolean = false;
  // The AbortController for the currently in-flight pull, or undefined
  // when no pull is active. Used as the identity key for superseded-
  // settle detection: the settle handler captures its own controller
  // reference and compares it to this field — a mismatch means the
  // pull was superseded (tee/extract). Aborted pulls keep their
  // controller reference so that #pulling serialization is maintained
  // until the aborted promise settles (signal.aborted distinguishes
  // aborted from live settlements).
  #pullAbortController: AbortController | undefined;
  #pullAlgorithm: PullAlgorithm | undefined;
  #cancelAlgorithm: CancelAlgorithm | undefined;
  #teeAlgorithm: (() => unknown) | undefined;
  #hooks: NativeStreamHooks;
  // Set immediately after construction by createNativeReadableStreamParts.
  #controller: NativeReadableStreamController | undefined;
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
  }

  attachController(controller: NativeReadableStreamController): void {
    this.#controller = controller;
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
    if (this.#status === 'errored') {
      return PromiseReject(this.#storedError) as Promise<
        ReadableStreamReadResult<Uint8Array>
      >;
    }
    if (this.#status === 'closed') {
      return PromiseResolve({ done: true, value: undefined }) as Promise<
        ReadableStreamReadResult<Uint8Array>
      >;
    }
    const withResolvers = PromiseWithResolvers() as PromiseWithResolversType<
      ReadableStreamReadResult<Uint8Array>
    >;
    ArrayPrototypePush(this.#requests, {
      kind: 'default',
      read: {
        resolve: withResolvers.resolve,
        reject: withResolvers.reject,
        reader,
      },
    } satisfies NativeDefaultRequest);
    // The pull prompt arrives via controllerPullIfNeeded from the reader
    // layer (spec PullSteps ordering), not here.
    return withResolvers.promise;
  }

  drain(_maxSize?: number): { chunks: Uint8Array[]; done: boolean } {
    // Nothing is ever buffered on the JS side; the draining reader falls
    // back to read() for the wait-for-data case.
    return { chunks: [], done: this.#status === 'closed' };
  }

  cancelReadsForReader(reader: object, reason: unknown): void {
    const kept: NativeRequest[] = [];
    const requests = this.#requests;
    for (let i = 0; i < requests.length; i++) {
      const request = requests[i] as NativeRequest;
      const owner =
        request.kind === 'default' ? request.read.reader : request.desc.reader;
      if (owner === reader) {
        if (request.kind === 'default') {
          request.read.reject(reason);
        } else {
          request.desc.reject(reason);
        }
      } else {
        ArrayPrototypePush(kept, request);
      }
    }
    this.#requests = kept;
    this.#byobRequestCache = null;
    // ABORT SITE: if the released reader's reads were the only demand and
    // a pull is in flight, abort it — the source contractually holds any
    // bytes and redelivers on the next pull.
    if (
      this.#requests.length === 0 &&
      this.#pullAbortController !== undefined
    ) {
      this.#abortCurrentPull(reason);
    }
  }

  errorAllReads(reason: unknown): void {
    const requests = this.#requests;
    this.#requests = [];
    this.#byobRequestCache = null;
    for (let i = 0; i < requests.length; i++) {
      const request = requests[i] as NativeRequest;
      if (request.kind === 'default') {
        request.read.reject(reason);
      } else {
        request.desc.reject(reason);
      }
    }
  }

  resolveAllReadsAsDone(): void {
    const requests = this.#requests;
    this.#requests = [];
    this.#byobRequestCache = null;
    for (let i = 0; i < requests.length; i++) {
      const request = requests[i] as NativeRequest;
      if (request.kind === 'default') {
        request.read.resolve({ done: true, value: undefined });
      } else {
        request.desc.resolve({ done: true, value: undefined });
      }
    }
  }

  get hasPendingRead(): boolean {
    return this.#requests.length > 0;
  }

  fulfillFirstPendingRead(value: Uint8Array): void {
    const pending = ArrayPrototypeShift(this.#requests) as NativeDefaultRequest;
    pending.read.resolve({ value, done: false });
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
      desc.resolve({
        done: true,
        value: new desc.viewCtor(desc.buffer, desc.byteOffset, 0),
      });
      return desc.promise;
    }
    ArrayPrototypePush(this.#requests, {
      kind: 'byob',
      desc,
    } satisfies NativeByobRequestEntry);
    return desc.promise;
  }

  get hasPendingPullInto(): boolean {
    const requests = this.#requests;
    for (let i = 0; i < requests.length; i++) {
      if ((requests[i] as NativeRequest).kind === 'byob') return true;
    }
    return false;
  }

  get hasPartiallyFulfilledRead(): boolean {
    const head = this.#requests[0];
    return (
      head !== undefined && head.kind === 'byob' && head.desc.bytesFilled > 0
    );
  }

  get headPullInto(): PullIntoDescriptor | undefined {
    const head = this.#requests[0];
    return head !== undefined && head.kind === 'byob' ? head.desc : undefined;
  }

  get pendingPullIntoView(): Uint8Array | undefined {
    const head = this.#requests[0];
    if (head === undefined || head.kind !== 'byob') return undefined;
    const desc = head.desc;
    return new Uint8Array(
      desc.buffer,
      desc.byteOffset + desc.bytesFilled,
      desc.byteLength - desc.bytesFilled
    );
  }

  respondBYOB(bytesWritten: number): void {
    const head = this.#requests[0];
    if (head === undefined || head.kind !== 'byob') {
      throw new TypeError('No pending BYOB read to respond to');
    }
    this.respondToDesc(head.desc, bytesWritten);
  }

  commitPullIntosOnClose(): void {
    // The fused close-commit happens in closeFromSource(); once the
    // conduit has left the active state there is nothing left to commit.
  }

  drainNoneDescriptors(): void {
    // Native conduit doesn't use releaseLock pull-into descriptors.
  }

  shiftAutoAllocateDescriptor(): PullIntoDescriptor | undefined {
    // Native conduit doesn't use autoAllocateChunkSize.
    return undefined;
  }

  // --- Pull pacing -----------------------------------------------------------

  // Standard [[pulling]]/[[pullAgain]] serialization. Demand-driven: pull
  // only while requests are pending (effective high-water mark of zero).
  // Re-evaluated after each pull settles for the REMAINING requests; a
  // single request never spans pulls (the respond is the complete answer
  // for its read — see the min-read contract).
  //
  // PER-PULL CANCELLATION: each pull invocation gets a fresh
  // AbortController whose signal is passed as pull(controller, signal).
  // The source contractually checks signal.aborted immediately before
  // enqueue/respond; the controller backstop refuses delivery on an
  // aborted signal. Settle handlers compare their captured controller
  // reference against #pullAbortController — a mismatch means the pull
  // was aborted or superseded and its settlement is inert (the rejection
  // from kj cancellation is swallowed, preventing a healthy stream from
  // erroring on a cancelled read).
  pullIfNeeded(): void {
    if (this.#status !== 'active') return;
    if (this.#requests.length === 0) return;
    const pullAlgorithm = this.#pullAlgorithm;
    if (pullAlgorithm === undefined) return;
    if (this.#pulling) {
      this.#pullAgain = true;
      return;
    }
    const controller = this.#controller;
    if (controller === undefined) return;
    this.#pulling = true;
    this.#deliveredThisPull = false;
    const ac = new AbortController();
    this.#pullAbortController = ac;
    const signal = AbortControllerSignalGet(ac);
    markPromiseHandled(
      PromisePrototypeThen(
        pullAlgorithm(controller, signal),
        () => {
          // Superseded-settle check: tee/extract may have cleared the
          // controller reference; this settlement belongs to a dead pull.
          if (this.#pullAbortController !== ac) return;
          this.#pulling = false;
          this.#pullAbortController = undefined;
          // Aborted-pull check: the pull's signal was aborted (reader
          // release, cancel, tee, extract).  The source contractually
          // held its data; skip the completion latch and just schedule
          // the next pull if demand exists.
          if (AbortSignalAbortedGet(signal)) {
            if (this.#status === 'active' && this.#requests.length > 0) {
              this.pullIfNeeded();
            }
            return;
          }
          // COMPLETION LATCH: a pull that settled without delivering,
          // closing, or erroring while requests are still pending is a
          // broken source. Error the stream to prevent a busy-loop
          // re-pull against a source that will never deliver.
          if (
            !this.#deliveredThisPull &&
            this.#status === 'active' &&
            this.#requests.length > 0
          ) {
            this.errorFromSource(
              new TypeError(
                'native source pull settled without delivering, closing, ' +
                  'or erroring — the source appears broken'
              )
            );
            return;
          }
          if (
            this.#pullAgain ||
            (this.#status === 'active' && this.#requests.length > 0)
          ) {
            this.#pullAgain = false;
            this.pullIfNeeded();
          }
        },
        (e: unknown) => {
          if (this.#pullAbortController !== ac) return; // superseded
          this.#pulling = false;
          this.#pullAbortController = undefined;
          // Aborted-pull rejection is expected (kj cancellation, etc.)
          // — not a source error.  Schedule the next pull if needed.
          if (AbortSignalAbortedGet(signal)) {
            if (this.#status === 'active' && this.#requests.length > 0) {
              this.pullIfNeeded();
            }
            return;
          }
          this.#pullAgain = false;
          this.errorFromSource(e);
        }
      ) as Promise<void>
    );
  }

  // Aborts the in-flight pull's signal.  #pulling and #pullAbortController
  // are deliberately NOT cleared — the aborted pull's promise must settle
  // before a new pull can start, maintaining the non-reentrant [[pulling]]
  // / [[pullAgain]] serialization.  The settle handler detects the abort
  // via signal.aborted and skips the completion latch.
  #abortCurrentPull(reason: unknown): void {
    const ac = this.#pullAbortController;
    if (ac === undefined) return;
    this.#pullAgain = false;
    this.#deliveredThisPull = false;
    this.#byobRequestCache = null;
    AbortControllerAbort(ac, reason);
  }

  // --- The source-facing operations (via the controller façade) -------------

  isHeadDesc(desc: PullIntoDescriptor): boolean {
    const head = this.#requests[0];
    return (
      this.#status === 'active' &&
      head !== undefined &&
      head.kind === 'byob' &&
      head.desc === desc
    );
  }

  getByobRequest(): NativeReadableStreamBYOBRequest | null {
    const head = this.#requests[0];
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
    const head = this.#requests[0];
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
        const isDataView =
          TypedArrayPrototypeGetSymbolToStringTag(chunk) === undefined;
        const buffer = (
          isDataView
            ? DataViewPrototypeGetBuffer(chunk)
            : TypedArrayPrototypeGetBuffer(chunk)
        ) as ArrayBuffer;
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
        view = new Uint8Array(buffer, byteOffset, byteLength);
      }
      // EXPECTED-LENGTH CONTRACT: overflow check before commit.
      this.#accountDelivery(TypedArrayPrototypeGetByteLength(view) as number);
      ArrayPrototypeSplice(this.#requests, 0, 1);
      this.#deliveredThisPull = true;
      head.read.resolve({ done: false, value: view });
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
      if (this.#requests[0] === undefined) {
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
          'means end-of-stream — call close() instead'
      );
    }
    const filled = desc.bytesFilled + written;
    if (filled > desc.byteLength) {
      throw new RangeError('bytesWritten exceeds the remaining view size');
    }
    if (filled % desc.elementSize !== 0) {
      // Every respond commits (see below), so every respond must account
      // a whole number of elements — there is no queue to carry a
      // remainder.
      throw new TypeError(
        'native sources must respond in whole-element multiples of the view'
      );
    }
    // EXPECTED-LENGTH CONTRACT: overflow check before commit.
    this.#accountDelivery(written);
    desc.bytesFilled = filled;
    this.#byobRequestCache = null;
    this.#deliveredThisPull = true;
    ArrayPrototypeSplice(this.#requests, 0, 1);
    const value = new desc.viewCtor(
      desc.buffer,
      desc.byteOffset,
      filled / desc.elementSize
    );
    if (filled >= desc.minimumFill) {
      desc.resolve({ done: false, value });
      return;
    }
    // MIN-READ CONTRACT: the respond is the source's COMPLETE answer for
    // this read. Delivering fewer bytes than the requested minimum
    // (atLeast) signals end-of-stream: the partial fill commits FUSED
    // with done: true, the stream closes, and every subsequent read
    // returns EOF. The conduit never re-pulls an unsatisfied read — any
    // accumulation toward the minimum happens inside the native source
    // (tryRead-style). State transitions before promise resolutions.
    //
    // EXPECTED-LENGTH: under-delivery is a close signal, so the
    // exact-total contract applies — landing short of expectedLength is
    // underflow and errors the stream instead of closing it. (Landing
    // exactly on it is a legitimate fused close.)
    if (
      this.#expectedLength !== undefined &&
      this.#bytesDelivered < this.#expectedLength
    ) {
      const error = new RangeError(
        'native source closed (via min-read under-delivery) before ' +
          'producing its declared expectedLength'
      );
      this.#status = 'errored';
      this.#storedError = error;
      desc.reject(error);
      this.errorAllReads(error);
      this.#hooks.errorStream(error);
      return;
    }
    this.#status = 'closed';
    desc.resolve({ done: true, value });
    this.#settleRemainingAsEof();
    this.#hooks.closeStream();
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
  // view). Shared by the close paths.
  #settleRemainingAsEof(): void {
    const requests = this.#requests;
    this.#requests = [];
    this.#byobRequestCache = null;
    for (let i = 0; i < requests.length; i++) {
      const request = requests[i] as NativeRequest;
      if (request.kind === 'default') {
        request.read.resolve({ done: true, value: undefined });
      } else {
        const desc = request.desc;
        desc.resolve({
          done: true,
          value: new desc.viewCtor(desc.buffer, desc.byteOffset, 0),
        });
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
    this.#byobRequestCache = null;

    // DEFENSIVE, currently unreachable: under the min-read contract every
    // respond commits (satisfied → done: false; under-delivered → fused
    // EOF), so a partially-filled descriptor cannot persist to close().
    // Retained because the contract describes close() fusing with a
    // partial fill, should a future revision reintroduce partial
    // responds.
    const head = this.#requests[0];
    if (
      head !== undefined &&
      head.kind === 'byob' &&
      head.desc.bytesFilled > 0
    ) {
      const desc = head.desc;
      if (desc.bytesFilled % desc.elementSize !== 0) {
        // Closed mid-element: unrecoverable (no queue to carry the
        // remainder). Error everything instead.
        const error = new TypeError(
          'native source closed mid-element on a BYOB read'
        );
        this.#status = 'errored';
        this.#storedError = error;
        this.errorAllReads(error);
        this.#hooks.errorStream(error);
        return;
      }
      ArrayPrototypeSplice(this.#requests, 0, 1);
      desc.resolve({
        done: true,
        value: new desc.viewCtor(
          desc.buffer,
          desc.byteOffset,
          desc.bytesFilled / desc.elementSize
        ),
      });
    }

    this.#settleRemainingAsEof();
    this.#hooks.closeStream();
  }

  errorFromSource(reason: unknown): void {
    if (this.#status !== 'active') return; // late settle tolerated
    this.#status = 'errored';
    this.#storedError = reason;
    this.errorAllReads(reason);
    this.#hooks.errorStream(reason);
  }

  // Spec PromiseCall over the extracted cancel method; resolved if the
  // source declared none. Invoked through the controller dispatch chain
  // (the stream layer's cancel policy callback).
  cancelSource(reason: unknown): Promise<void> {
    if (this.#status === 'active') this.#status = 'closed';
    this.#byobRequestCache = null;
    const cancelAlgorithm = this.#cancelAlgorithm;
    if (cancelAlgorithm === undefined) {
      return PromiseResolve() as Promise<void>;
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
}

// ---------------------------------------------------------------------------
// Construction glue

// Extraction follows the queued controllers' conventions: properties are
// read ONCE, alphabetically, validated, and invoked thereafter only via
// captured uncurryThis wrappers with the source as `this` (spec
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
  const {
    autoAllocateChunkSize,
    cancel: cancelFn,
    expectedLength: rawExpectedLength,
    pull: pullFn,
    tee: teeFn,
    type,
  } = source as {
    autoAllocateChunkSize?: unknown;
    cancel?: unknown;
    expectedLength?: unknown;
    pull?: unknown;
    tee?: unknown;
    type?: unknown;
  };

  // Native sources must not look like queued sources: the reader layer's
  // autoAllocate synthesis is queued-only and relies on this prohibition,
  // and a declared type would suggest the object was built for the queued
  // byte path.
  if (autoAllocateChunkSize !== undefined) {
    throw new TypeError(
      'Native stream sources must not declare autoAllocateChunkSize'
    );
  }
  if (type !== undefined) {
    throw new TypeError('Native stream sources must not declare type');
  }
  if (cancelFn !== undefined && typeof cancelFn !== 'function') {
    throw new TypeError('underlyingSource.cancel must be a function');
  }
  if (pullFn !== undefined && typeof pullFn !== 'function') {
    throw new TypeError('underlyingSource.pull must be a function');
  }
  if (teeFn !== undefined && typeof teeFn !== 'function') {
    throw new TypeError('underlyingSource.tee must be a function');
  }
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
        return PromiseResolve(
          callPull(source, controller, signal)
        ) as Promise<void>;
      } catch (e) {
        return PromiseReject(e) as Promise<void>;
      }
    };
  }
  let cancelAlgorithm: CancelAlgorithm | undefined;
  if (cancelFn !== undefined) {
    const callCancel = uncurryThis(cancelFn);
    cancelAlgorithm = (reason: unknown) => {
      try {
        return PromiseResolve(callCancel(source, reason)) as Promise<void>;
      } catch (e) {
        return PromiseReject(e) as Promise<void>;
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
  const controller = new NativeReadableStreamController(conduit);
  conduit.attachController(controller);
  return { controller, conduit };
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

// Internal namespace, never re-exported to users (kNativeSource and
// kExtractNativeSource excepted, TEMPORARILY, via streams.ts — see the
// module header).
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
  nativeControllerMaybeCloseStream,
  nativeControllerOnReaderRelease,
  nativeControllerTeeSource,
  nativeControllerExtractSource,
  nativeControllerExpectedLength,
};

// Type-only exports (fully erased at runtime — the loader sees only the
// module.exports assignment below, matching the queue.ts pattern).
// NativeStreamInternals lets the require() site in readable.ts cast the
// untyped loader result back to the real shape, preserving the brand
// predicates' type-guard narrowing.
export type { NativeReadableStreamController, NativePullConduit };
export type NativeStreamInternals = typeof nativeStreamInternals;

module.exports = { nativeStreamInternals };
