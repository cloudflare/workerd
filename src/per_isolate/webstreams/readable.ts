'use strict';

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  QueuingStrategy,
  ReadableByteStreamController as ReadableByteStreamControllerType,
  ReadableStream as ReadableStreamType,
  ReadableStreamBYOBReaderReadOptions,
  ReadableStreamBYOBReader as ReadableStreamBYOBReaderType,
  ReadableStreamBYOBRequest as ReadableStreamBYOBRequestType,
  ReadableStreamDefaultController as ReadableStreamDefaultControllerType,
  ReadableStreamDefaultReader as ReadableStreamDefaultReaderType,
  ReadableStreamReader as ReadableStreamReaderType,
  ReadableStreamReadResult,
  StreamPipeOptions,
  TransformStream as TransformStreamType,
  UnderlyingByteSource,
  UnderlyingDefaultSource,
  UnderlyingSource,
  WritableStream as WritableStreamType,
} from './types';
import type {
  ByteQueueEntry,
  ByteStreamConsumer as ByteStreamConsumerType,
  ByteStreamCursor as ByteStreamCursorType,
  PullIntoDescriptor,
  QueueCursor as QueueCursorType,
  StreamConsumer as StreamConsumerType,
  StreamQueue as StreamQueueType,
} from './queue';
import type {
  NativeReadableStreamController as NativeReadableStreamControllerType,
  NativeStreamInternals,
} from './native';

const {
  AbortSignalAbortedGet,
  AbortSignalReasonGet,
  AggregateError,
  ArrayBuffer,
  ArrayBufferPrototypeByteLengthGet,
  ArrayBufferPrototypeDetachedGet,
  ArrayBufferPrototypeTransfer,
  ArrayPrototypePush,
  AsyncIteratorPrototype,
  BigInt,
  DataView,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  EventTargetAddEventListener,
  EventTargetRemoveEventListener,
  JSONParse,
  NumberIsNaN,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  ObjectSetPrototypeOf,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  PromisePrototypeThen,
  SafePromise,
  RangeError,
  SafeArrayIterator,
  SafeWeakMap,
  Symbol,
  SymbolAsyncIterator,
  SymbolIterator,
  SymbolToStringTag,
  TextDecoder,
  TextDecoderDecode,
  TypeError,
  TypedArrayCtorByName,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetLength,
  TypedArrayPrototypeGetSymbolToStringTag,
  TypedArrayPrototypeSet,
  Uint8Array,
  uncurryThis,
} = primordials;

// SafePromise.race is used by the pipe algorithm where species protection
// is needed for the race's internal .then() calls on its arguments.
const SafePromiseRace = SafePromise.race;

const { isArrayBufferView, isPromise, markPromiseHandled } = utils;

const {
  StreamQueue,
  QueueCursor,
  ByteStreamCursor,
  CLOSE_SENTINEL,
  createReadResult,
} = require('webstreams/queue');

// Internal writable operations for the pipe (never re-exported to users).
const {
  kExtractNativeSink,
  internalsForPipe: writableInternals,
} = require('webstreams/writable');

// The native backend (leaf module — see the fence conventions in native.ts
// and queue.ts). The cast restores the real shape the untyped loader
// erases, so the brand predicates keep their type-guard narrowing.
const { nativeStreamInternals } = require('webstreams/native') as {
  nativeStreamInternals: NativeStreamInternals;
};
const {
  kExtractNativeSource,
  isNativeUnderlyingSource,
  isNativeController,
  createNativeReadableStreamParts,
  nativeControllerPullIfNeeded,
  nativeControllerCancelSteps,
  nativeControllerMaybeCloseStream,
  nativeControllerOnReaderRelease,
  nativeControllerTeeSource,
  nativeControllerExtractSource,
  nativeControllerExpectedLength,
} = nativeStreamInternals;

// Normalizes the non-standard `expectedLength` extension property on
// byte-stream underlying sources: the TOTAL bytes the source promises to
// produce (undefined = unknown; the C++ side then uses chunked
// encoding). Accepts a non-negative bigint or non-negative integer
// number (normalized to bigint — totals can exceed MAX_SAFE_INTEGER).
// Only meaningful for type:'bytes' sources; the default controller never
// reads it. Duplicated for the native backend in native.ts.
function normalizeExpectedLength(value: unknown): bigint | undefined {
  if (value === undefined) return undefined;
  if (typeof value === 'bigint') {
    if (value < 0n) {
      throw new RangeError('expectedLength must be non-negative');
    }
    return value;
  }
  if (typeof value === 'number') {
    if (NumberIsNaN(value) || value % 1 !== 0) {
      throw new TypeError('expectedLength must be an integer');
    }
    if (value < 0) {
      throw new RangeError('expectedLength must be non-negative');
    }
    return BigInt(value);
  }
  throw new TypeError(
    'expectedLength must be a non-negative bigint or integer'
  );
}

// --- Composite tee-cancel reasons ------------------------------------------
// DELIBERATE SPEC DIVERGENCE (recorded in the design doc's divergence
// table): when all tee branches have cancelled, the source's cancel
// algorithm receives a single flattened AggregateError carrying every
// branch's reason, rather than the spec's two-element array. Re-tee
// composites are flattened into one level: the WeakMap below brands OUR
// composite errors (unforgeably) and carries their flat reason list — a
// user-supplied AggregateError used as a cancel reason is never unpacked.
const teeCompositeReasons = new SafeWeakMap();

function appendCancelReason(flat: unknown[], reason: unknown): void {
  // WeakMap.get on a non-object key returns undefined (no throw).
  const nested = teeCompositeReasons.get(reason) as unknown[] | undefined;
  if (nested !== undefined) {
    for (let i = 0; i < nested.length; i++) {
      ArrayPrototypePush(flat, nested[i]);
    }
  } else {
    ArrayPrototypePush(flat, reason);
  }
}

function makeCompositeCancelReason(
  reason1: unknown,
  reason2: unknown
): AggregateError {
  const flat: unknown[] = [];
  appendCancelReason(flat, reason1);
  appendCancelReason(flat, reason2);
  // SafeArrayIterator: AggregateError's iterable conversion must not run
  // through the patchable %ArrayIteratorPrototype%.
  const error = new AggregateError(
    new SafeArrayIterator(flat),
    'All readable stream tee branches were canceled'
  ) as AggregateError;
  teeCompositeReasons.set(error, flat);
  return error;
}

const kPrivateSymbol = Symbol('private');

function isActualObject(value: unknown) {
  return value != null && typeof value === 'object';
}

function assertPrivateSymbol(symbol: symbol) {
  if (symbol !== kPrivateSymbol) {
    throw new TypeError('Illegal constructor');
  }
}

let isReadableStreamLocked: <R>(stream: ReadableStream<R>) => boolean;
let isReadableStreamUnusable: <R>(stream: ReadableStream<R>) => boolean;
let getReaderBase: <R>(reader: object) => ReadableStreamReaderBase<R>;
let isReaderBoundToStream: (reader: object) => boolean;
let acquireReadableStreamDefaultReader: <R>(
  stream: ReadableStream<R>
) => ReadableStreamDefaultReader<R>;
let initializeReadableStreamGenericReader: <R>(
  stream: ReadableStream<R>,
  reader: ReadableStreamReaderBase<R>
) => void;
let cancelReadableStreamGenericReader: (
  reader: object,
  reason?: unknown
) => Promise<void>;
let acquireReadableStreamBYOBReader: <R>(
  stream: ReadableStream<R>
) => ReadableStreamBYOBReaderType;
let readableStreamCancel: <R>(
  stream: ReadableStream<R>,
  reason?: unknown
) => Promise<void>;
let readableStreamPipeThroughTo: <R>(
  source: ReadableStream<R>,
  destination: WritableStreamType<R>,
  options?: StreamPipeOptions
) => Promise<void>;
let readableStreamTee: <R>(
  stream: ReadableStream<R>
) => [ReadableStream<R>, ReadableStream<R>];
let readableStreamReaderGenericCancel: (
  reader: object,
  reason?: unknown
) => Promise<void>;
let readableStreamReaderGenericRelease: (reader: object) => void;
let readableStreamDefaultReaderRead: <R>(
  reader: ReadableStreamDefaultReaderType<R>,
  readRequest: ReadableStreamAsyncIteratorReadRequest<R>
) => void;
let isReadableStream: (value: unknown) => boolean;
let isByteStreamController: (value: unknown) => boolean;

// BACKEND-DISPATCH: the byte-CAPABLE gate (one of the five sanctioned
// dispatch points). True for any controller whose backend can satisfy
// BYOB reads: the queued byte controller, or ANY native controller —
// native sources are byte-capable by definition (the marker is
// sufficient). Distinct from isByteStreamController, which remains the
// QUEUED-byte brand check used by the queued-only paths (autoAllocate
// synthesis, tee's cursor fork).
function isByteCapableController(value: unknown): boolean {
  return isByteStreamController(value) || isNativeController(value);
}

let getReadableStreamController: <R>(
  stream: ReadableStream<R>
) =>
  | ReadableStreamDefaultControllerType
  | ReadableByteStreamControllerType
  | NativeReadableStreamControllerType
  | undefined;
let getReadableStreamReader: <R>(
  stream: ReadableStream<R>
) => ReadableStreamReaderType<R> | undefined;
// The stream OWNS its consumer (readers only borrow it while locked; it
// persists across reader attach/detach). Created during controller setup.
// The consumer is the FENCE between backends: a QueueCursor/
// ByteStreamCursor (queued) today, a NativePullConduit (native) later.
// The reader layer must stay backend-blind — it programs against
// StreamConsumer only; backend-specific access (cursor position/queue for
// tee, the controllers' close checks) happens behind sanctioned casts at
// the enumerated dispatch points.
let getReadableStreamConsumer: <R>(
  stream: ReadableStream<R>
) => StreamConsumerType<R> | undefined;
let setReadableStreamConsumer: <R>(
  stream: ReadableStream<R>,
  consumer: StreamConsumerType<R> | undefined
) => void;
// Controller internals exposed for the reader/stream layers. Each takes the
// full controller union; the implementations are CHAINED (see the
// BACKEND-DISPATCH note at the default controller's static block): queued
// default assigns first, queued byte wraps it, and the native backend's
// wrap is joined at the bottom of this module (its brand lives across the
// module fence in native.ts).
let controllerPullIfNeeded: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => void;
let controllerCancelSteps: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType,
  reason: unknown
) => Promise<void>;
let controllerMaybeCloseStream: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => void;
// Invalidate any cached byobRequest when a reader releases its lock (the
// release rejects the pending pull-intos, so a cached request would point
// at a dead descriptor). No-op for default controllers.
let controllerOnReaderRelease: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => void;
let getReaderStream: <R>(reader: object) => ReadableStream<R> | undefined;

// BACKEND-DISPATCH point #4: the shared extractor function installed
// on native-backed streams as kExtractNativeSource. Assigned in
// ReadableStream's static block (needs private-field access).
let extractNativeSource: <R>(this: ReadableStream<R>) => object;

// The non-standard expectedLength pass-through for the DrainingReader
// (and the C++ bridge). Chained like the other controller helpers:
// default → undefined; queued byte → cached construction value; native →
// cached construction value (joined in ReadableStream's static block).
let getControllerExpectedLength: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => bigint | undefined;

let setReadableStreamPendingClosure: <R>(stream: ReadableStream<R>) => void;
let getReadableStreamOnEof: <R>(stream: ReadableStream<R>) => Promise<void>;
let getReadableStreamExpectedLength: <R>(
  stream: ReadableStream<R>
) => bigint | undefined;
let getReadableStreamGetState: <R>(
  stream: ReadableStream<R>
) => 'readable' | 'closed' | 'errored';
let getReadableStreamIsDisturbed: <R>(stream: ReadableStream<R>) => boolean;
let getReadableStreamStoredError: <R>(stream: ReadableStream<R>) => unknown;
let setReadableStreamState: <R>(
  stream: ReadableStream<R>,
  state: 'readable' | 'closed' | 'errored'
) => void;
// Marks the stream disturbed (one-way — there is no un-disturb).
let setReadableStreamDisturbed: <R>(stream: ReadableStream<R>) => void;
let setReadableStreamStoredError: <R>(
  stream: ReadableStream<R>,
  error: unknown
) => void;
let setReadableStreamReader: <R>(
  stream: ReadableStream<R>,
  reader: ReadableStreamReaderType<R> | undefined
) => void;
let getGenericReaderClosedPromise: (reader: object) => Promise<void>;
let resolveGenericReaderPromise: (reader: object) => void;
let rejectGenericReaderPromise: (reader: object, reason?: unknown) => void;
// Tee-branch lifecycle: fire #onBranchSettled if set. Called from
// readableStreamClose/readableStreamError so the shared cancel-promise
// settles when the source closes/errors and at least one sibling was
// already cancelled. Assigned in ReadableStream's static block.
let notifyBranchSettled: <R>(stream: ReadableStream<R>) => void;
// Set the #onBranchSettled callback for tee branches. Assigned in
// ReadableStream's static block.
let setOnBranchSettled: <R>(
  stream: ReadableStream<R>,
  cb: (() => void) | undefined
) => void;

interface ReadableStreamIteratorState<R> {
  done: boolean;
  current?: Promise<IteratorResult<R>> | undefined;
}

class ReadableStreamAsyncIteratorReadRequest<R> {
  #reader: ReadableStreamDefaultReaderType;
  #state: ReadableStreamIteratorState<R>;
  #promise: PromiseWithResolversType<IteratorResult<R>>;
  constructor(
    reader: ReadableStreamDefaultReaderType,
    state: ReadableStreamIteratorState<R>,
    promise: PromiseWithResolversType<IteratorResult<R>>
  ) {
    this.#reader = reader;
    this.#state = state;
    this.#promise = promise;
  }

  chunk(chunk: R) {
    this.#state.current = undefined;
    this.#promise.resolve(createReadResult(chunk, false));
  }

  close() {
    this.#state.current = undefined;
    this.#state.done = true;
    readableStreamReaderGenericRelease(this.#reader);
    this.#promise.resolve(createReadResult(undefined, true));
  }

  error(error: unknown) {
    this.#state.current = undefined;
    this.#state.done = true;
    readableStreamReaderGenericRelease(this.#reader);
    this.#promise.reject(error);
  }
}

// --- ReadableStream async iterator prototype (spec §4.2.4) -----------------
// Shared prototype with next/return methods; per-instance state stored in a
// WeakMap. This matches the spec's ReadableStreamAsyncIteratorPrototype which
// sits between each iterator instance and %AsyncIteratorPrototype%.
interface IteratorInternalState {
  reader: ReadableStreamDefaultReaderType;
  preventCancel: boolean;
  state: ReadableStreamIteratorState<unknown>;
  started: boolean;
}

const iteratorStateMap = new SafeWeakMap() as WeakMap<
  object,
  IteratorInternalState
>;

function getIteratorState(iter: object): IteratorInternalState {
  const s = iteratorStateMap.get(iter);
  if (s === undefined) {
    throw new TypeError('Illegal invocation');
  }
  return s;
}

function iteratorNextSteps(s: IteratorInternalState) {
  if (s.state.done) {
    return PromiseResolve(createReadResult(undefined, true));
  }
  if (!isReaderBoundToStream(s.reader)) {
    throw new TypeError('The reader is not bound to a ReadableStream');
  }
  const promise = PromiseWithResolvers();
  readableStreamDefaultReaderRead(
    s.reader,
    new ReadableStreamAsyncIteratorReadRequest(s.reader, s.state, promise)
  );
  return promise.promise;
}

async function iteratorReturnSteps(s: IteratorInternalState, value: unknown) {
  if (s.state.done) {
    return createReadResult(value, true);
  }
  s.state.done = true;

  if (!isReaderBoundToStream(s.reader)) {
    throw new TypeError('The reader is not bound to a ReadableStream');
  }

  if (!s.preventCancel) {
    const result = readableStreamReaderGenericCancel(s.reader, value);
    readableStreamReaderGenericRelease(s.reader);
    await result;
    return createReadResult(value, true);
  }

  readableStreamReaderGenericRelease(s.reader);
  return createReadResult(value, true);
}

const ReadableStreamAsyncIteratorPrototype = ObjectSetPrototypeOf(
  {
    next(this: object) {
      const s = getIteratorState(this);
      if (!s.started) {
        s.state.current = PromiseResolve();
        s.started = true;
      }
      s.state.current =
        s.state.current !== undefined
          ? PromisePrototypeThen(
              s.state.current,
              () => iteratorNextSteps(s),
              () => iteratorNextSteps(s)
            )
          : iteratorNextSteps(s);
      return s.state.current;
    },

    return(this: object, error: unknown) {
      const s = getIteratorState(this);
      s.started = true;
      s.state.current =
        s.state.current !== undefined
          ? PromisePrototypeThen(
              s.state.current,
              () => iteratorReturnSteps(s, error),
              () => iteratorReturnSteps(s, error)
            )
          : iteratorReturnSteps(s, error);
      return s.state.current;
    },

    [SymbolAsyncIterator](this: object) {
      return this;
    },
  },
  AsyncIteratorPrototype
);
// ---- end async iterator prototype ------------------------------------------

class ReadableStreamReaderBase<R> {
  #stream?: ReadableStream<R> | undefined;
  // @ts-expect-error
  #closedPromise: Promise<void> | PromiseWithResolversType<void>;
  // NOTE: pending reads live on the stream's cursor (PendingRead entries
  // keyed by reader identity), not on the reader — the cursor outlives
  // reader attach/detach cycles.

  static {
    initializeReadableStreamGenericReader = <R>(
      stream: ReadableStream<R>,
      base: ReadableStreamReaderBase<R>
    ) => {
      base.#stream = stream;

      switch (getReadableStreamGetState(stream)) {
        case 'readable': {
          base.#closedPromise = PromiseWithResolvers();
          markPromiseHandled(
            (base.#closedPromise as PromiseWithResolversType<void>).promise
          );
          break;
        }
        case 'closed': {
          base.#closedPromise = PromiseResolve();
          break;
        }
        case 'errored': {
          const stored = getReadableStreamStoredError(stream);
          // Because we need to mark the promise as handled, and because
          // Promise.reject does not give us the ability to do so before
          // the promise is reported as rejected, we need to create the
          // promise with PromiseWithResolvers and reject it manually.
          const promise: PromiseWithResolversType<void> =
            PromiseWithResolvers();
          markPromiseHandled(promise.promise);
          promise.reject(stored);
          base.#closedPromise = promise.promise;
          break;
        }
      }
    };

    getGenericReaderClosedPromise = (reader: object) => {
      const base = getReaderBase(reader);
      const promise = base.#closedPromise;
      if (isPromise(promise)) {
        return promise as Promise<void>;
      }
      if (!isPromise((promise as PromiseWithResolversType<void>).promise)) {
        throw new TypeError('invalid reader state');
      }
      return (promise as PromiseWithResolversType<void>).promise;
    };

    resolveGenericReaderPromise = (reader: object) => {
      const base = getReaderBase(reader);
      const promise = base.#closedPromise as PromiseWithResolversType<void>;
      const maybeResolve = promise.resolve;
      if (typeof maybeResolve === 'function') {
        maybeResolve();
        base.#closedPromise = promise.promise;
      }
    };

    rejectGenericReaderPromise = (reader: object, reason?: unknown) => {
      const base = getReaderBase(reader);
      const promise = base.#closedPromise as PromiseWithResolversType<void>;
      const maybeReject = promise.reject;
      if (typeof maybeReject === 'function') {
        maybeReject(reason);
        base.#closedPromise = promise.promise;
      }
    };

    isReaderBoundToStream = (reader: object) => {
      const base = getReaderBase(reader);
      return base.#stream !== undefined;
    };

    cancelReadableStreamGenericReader = (reader: object, reason?: unknown) => {
      const base = getReaderBase(reader);
      const stream = base.#stream;
      if (stream === undefined) {
        return PromiseReject(
          new TypeError('This reader has been released')
        ) as Promise<void>;
      }
      return readableStreamCancel(stream, reason);
    };

    readableStreamReaderGenericRelease = (reader: object) => {
      const base = getReaderBase(reader);
      const stream = base.#stream;
      if (stream === undefined) return;
      const releaseError = new TypeError('This reader has been released');
      // Per spec: a still-pending closedPromise is rejected; a settled one
      // is replaced with a fresh rejected promise. Both are marked handled.
      if (getReadableStreamGetState(stream) === 'readable') {
        rejectGenericReaderPromise(reader, releaseError);
      } else {
        const replacement: PromiseWithResolversType<void> =
          PromiseWithResolvers();
        markPromiseHandled(replacement.promise);
        replacement.reject(releaseError);
        base.#closedPromise = replacement.promise;
      }
      // Reject THIS reader's pending reads. The consumer itself persists —
      // it is stream-owned, and a future reader resumes where it left off.
      const consumer = getReadableStreamConsumer(stream);
      if (consumer !== undefined) {
        consumer.cancelReadsForReader(reader, releaseError);
      }
      // Notify the controller that the reader was released. For byte
      // controllers, the pull-into descriptors STAY (readerType → 'none');
      // the byobRequest is NOT invalidated per spec.
      const controller = getReadableStreamController(stream);
      if (controller !== undefined) {
        controllerOnReaderRelease(controller);
      }
      setReadableStreamReader(stream, undefined);
      base.#stream = undefined;
    };

    readableStreamReaderGenericCancel = (reader: object, reason?: unknown) => {
      return cancelReadableStreamGenericReader(reader, reason);
    };

    getReaderStream = <R>(reader: object) => {
      const base = getReaderBase<R>(reader);
      return base.#stream;
    };
  }
}

// The default-read core, shared by ReadableStreamDefaultReader.read() and
// the async-iterator read path. Internal callers MUST use this rather than
// the public read() — reader prototypes end up user-reachable, so internal
// dispatch through them would be interceptable.
//
// BACKEND-BLIND: this function programs against StreamConsumer and must
// never branch on the backend. (The autoAllocate check below is the ONE
// sanctioned queued-byte-specific check — see the marker.)
function defaultReaderReadInternal<R>(
  reader: object,
  stream: ReadableStream<R>
): Promise<ReadableStreamReadResult<R>> {
  setReadableStreamDisturbed(stream);
  const state = getReadableStreamGetState(stream);
  if (state === 'closed')
    return PromiseResolve(createReadResult(undefined, true)) as Promise<
      ReadableStreamReadResult<R>
    >;
  if (state === 'errored')
    return PromiseReject(getReadableStreamStoredError(stream)) as Promise<
      ReadableStreamReadResult<R>
    >;
  const consumer = getReadableStreamConsumer(stream);
  if (consumer === undefined) {
    // The consumer was detached (cancelled/tee'd-away) — nothing to read.
    return PromiseResolve(createReadResult(undefined, true)) as Promise<
      ReadableStreamReadResult<R>
    >;
  }
  const controller = getReadableStreamController(stream);

  // --- Synchronous fast path (spec PullSteps) ---
  // When data is immediately available the spec dequeues, performs the
  // drain-then-close check, and fulfills the read request all in one
  // synchronous call. An async/await on an already-resolved promise
  // would insert a microtask gap between the dequeue and the close,
  // causing reader.closed to resolve one tick too late. tryReadSync
  // returns the result directly (no promise wrapping) so the close
  // check runs in the same synchronous call.
  //
  // The autoAllocateChunkSize path is always async (BYOB machinery),
  // so it skips this fast path.
  let useAsyncPath = false;
  if (controller !== undefined && isByteStreamController(controller)) {
    const autoAllocateChunkSize = getByteControllerAutoAllocateChunkSize(
      controller as ReadableByteStreamController
    );
    if (autoAllocateChunkSize !== undefined) {
      useAsyncPath = true;
    }
  }
  if (!useAsyncPath) {
    const syncResult = consumer.tryReadSync(reader) as
      | ReadableStreamReadResult<R>
      | undefined;
    if (syncResult !== undefined) {
      // Spec PullSteps ordering: pull trigger, then drain-then-close,
      // then fulfill the read request — all synchronous.
      if (controller !== undefined) controllerPullIfNeeded(controller);
      if (controller !== undefined) controllerMaybeCloseStream(controller);
      if (syncResult.done) {
        readableStreamClose(stream);
      }
      return PromiseResolve(syncResult);
    }
  }

  // --- Async fallback ---
  // Data is not immediately available (pending reads queued, or native
  // source, or autoAllocateChunkSize BYOB path). Fall through to the
  // promise-based read and handle completion asynchronously.
  return defaultReaderReadInternalAsync<R>(
    reader,
    stream,
    consumer,
    controller,
    useAsyncPath
  );
}

// Async continuation of defaultReaderReadInternal for cases where
// the data is not synchronously available.
async function defaultReaderReadInternalAsync<R>(
  reader: object,
  stream: ReadableStream<R>,
  consumer: StreamConsumerType<R>,
  controller: ReadableStreamDefaultControllerType | undefined,
  isByteAutoAllocate: boolean
): Promise<ReadableStreamReadResult<R>> {
  let promise: Promise<ReadableStreamReadResult<unknown>>;
  if (isByteAutoAllocate) {
    // QUEUED-BYTE-SPECIFIC (sanctioned exception to backend-blindness):
    // autoAllocateChunkSize exists only on the queued byte controller —
    // native sources are forbidden from declaring it (they allocate their
    // own buffers for default reads), so a native stream correctly falls
    // through to the plain consumer.read() below.
    const autoAllocateChunkSize = getByteControllerAutoAllocateChunkSize(
      controller as ReadableByteStreamController
    );
    const withResolvers = PromiseWithResolvers() as PromiseWithResolversType<
      ReadableStreamReadResult<ArrayBufferView>
    >;
    const descriptor: PullIntoDescriptor = {
      buffer: new ArrayBuffer(autoAllocateChunkSize as number),
      bufferByteLength: autoAllocateChunkSize as number,
      byteOffset: 0,
      byteLength: autoAllocateChunkSize as number,
      bytesFilled: 0,
      minimumFill: 1,
      elementSize: 1,
      viewCtor: Uint8Array,
      readerType: 'default',
      promise: withResolvers.promise,
      resolve: withResolvers.resolve,
      reject: withResolvers.reject,
      reader,
    };
    const byteConsumer = consumer as unknown as ByteStreamConsumerType;
    promise = byteConsumer.readBYOB(descriptor);
  } else {
    promise = consumer.read(reader);
  }

  // A read request is a pull trigger (spec PullSteps ordering: the pull's
  // synchronous side-effects run before the read result is delivered).
  if (controller !== undefined) controllerPullIfNeeded(controller);
  const result = (await promise) as ReadableStreamReadResult<R>;
  // Drain-then-close (spec HandleQueueDrain): if this read consumed the
  // last data with close requested, the controller's primary stream
  // transitions now — checked on every read completion, not just done
  // results, because the read that drains the final chunk resolves with
  // done: false.
  if (controller !== undefined) controllerMaybeCloseStream(controller);
  if (result.done) {
    // This stream's cursor reached the sentinel. Close THIS stream (tee
    // branches close independently of the controller's primary stream).
    readableStreamClose(stream);
  }
  return result;
}

class ReadableStreamDefaultReader<
  R,
> implements ReadableStreamDefaultReaderType<R> {
  #base: ReadableStreamReaderBase<R>;

  static {
    // Wire getReaderBase for the first reader type. The BYOB and draining
    // readers chain onto this in their own static blocks.
    const defaultGetBase = <R>(reader: object) => {
      return (reader as ReadableStreamDefaultReader<R>).#base;
    };
    getReaderBase = defaultGetBase;

    readableStreamDefaultReaderRead = <R>(
      reader: ReadableStreamDefaultReaderType<R>,
      readRequest: ReadableStreamAsyncIteratorReadRequest<R>
    ) => {
      const stream = getReaderStream<R>(reader);
      if (stream === undefined) {
        readRequest.error(new TypeError('This reader has been released'));
        return;
      }
      PromisePrototypeThen(
        defaultReaderReadInternal<R>(reader, stream),
        (result: ReadableStreamReadResult<R>) => {
          if (result.done) {
            readRequest.close();
          } else {
            readRequest.chunk(result.value as R);
          }
        },
        (e: unknown) => {
          readRequest.error(e);
        }
      );
    };
  }

  constructor(stream: ReadableStream<R>) {
    this.#base = new ReadableStreamReaderBase();
    if (isReadableStreamLocked(stream)) {
      throw new TypeError('Cannot get a reader for a stream that is locked');
    }
    setReadableStreamReader(
      stream,
      this as unknown as ReadableStreamReaderType<R>
    );
    initializeReadableStreamGenericReader(stream, this.#base);
  }

  get closed(): Promise<void> {
    try {
      return getGenericReaderClosedPromise(this);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  cancel(reason: unknown = undefined): Promise<void> {
    try {
      return cancelReadableStreamGenericReader(this, reason);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  read(): Promise<ReadableStreamReadResult<R>> {
    try {
      const stream = getReaderStream<R>(this);
      if (stream === undefined) {
        return PromiseReject(
          new TypeError('This reader has been released')
        ) as Promise<ReadableStreamReadResult<R>>;
      }
      return defaultReaderReadInternal<R>(this, stream);
    } catch (e) {
      return PromiseReject(e) as Promise<ReadableStreamReadResult<R>>;
    }
  }

  releaseLock(): void {
    if (!isReaderBoundToStream(this)) return;
    readableStreamReaderGenericRelease(this);
  }
}

class ReadableStreamBYOBReader implements ReadableStreamBYOBReaderType {
  #base: ReadableStreamReaderBase<ArrayBufferView>;

  static {
    const prev = getReaderBase;
    getReaderBase = <R>(reader: object) => {
      if (#base in reader) {
        return reader.#base as unknown as ReadableStreamReaderBase<R>;
      }
      return prev<R>(reader);
    };
  }

  constructor(stream: ReadableStream<ArrayBufferView>) {
    this.#base = new ReadableStreamReaderBase();
    if (isReadableStreamLocked(stream)) {
      throw new TypeError('Cannot get a reader for a stream that is locked');
    }
    const controller = getReadableStreamController(stream);
    // The byte-CAPABLE gate (see isByteCapableController): queued byte
    // controllers and ALL native controllers pass (native sources are
    // byte-capable by definition). Using the queued-only brand here would
    // wrongly reject getReader({mode:'byob'}) on a native stream.
    if (!isByteCapableController(controller)) {
      throw new TypeError(
        'BYOB reader can only be used on a stream with a byte source'
      );
    }
    setReadableStreamReader(
      stream,
      this as unknown as ReadableStreamReaderType<ArrayBufferView>
    );
    initializeReadableStreamGenericReader(stream, this.#base);
  }

  get closed(): Promise<void> {
    try {
      return getGenericReaderClosedPromise(this);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  cancel(reason: unknown = undefined): Promise<void> {
    try {
      return cancelReadableStreamGenericReader(this, reason);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  async read<T extends ArrayBufferView>(
    view: T,
    options: ReadableStreamBYOBReaderReadOptions = {}
  ): Promise<ReadableStreamReadResult<T>> {
    // --- View validation (spec read(view, options) steps 1-3) ---
    if (!isArrayBufferView(view)) {
      throw new TypeError('view must be an ArrayBufferView');
    }
    const info = getViewInfo(view);
    if (info.byteLength === 0) {
      throw new TypeError('view must have a non-zero byteLength');
    }
    if (ArrayBufferPrototypeByteLengthGet(info.buffer) === 0) {
      throw new TypeError(
        "view's backing buffer must have a non-zero byteLength"
      );
    }
    if (!isActualObject(options)) {
      throw new TypeError('options must be an object');
    }
    // --- min validation (steps 4-6). min is in ELEMENTS for typed arrays
    // (bytes for DataView, where info.length === byteLength). NO clamping:
    // out-of-range is an error, not a request to do less.
    let min = 1;
    if (options.min !== undefined) {
      min = +options.min;
      if (NumberIsNaN(min) || min % 1 !== 0 || min < 0) {
        throw new TypeError('options.min must be a non-negative integer');
      }
      if (min === 0) {
        throw new TypeError('options.min must be greater than 0');
      }
      if (min > info.length) {
        throw new RangeError(
          'options.min must not exceed the length of the view'
        );
      }
    }
    const stream = getReaderStream(this);
    if (stream === undefined) {
      throw new TypeError('This reader has been released');
    }
    setReadableStreamDisturbed(stream);
    if (getReadableStreamGetState(stream) === 'errored') {
      throw getReadableStreamStoredError(stream);
    }
    // Transfer the buffer regardless of state (spec step).
    const transferred = ArrayBufferPrototypeTransfer(info.buffer);
    if (getReadableStreamGetState(stream) === 'closed') {
      // The stream may be closed with no consumer remaining (e.g. after
      // cancel clears it). Without a consumer, no pull-into can be
      // registered. Return { value: undefined, done: true } — matching
      // browser behavior and the C++ implementation.
      const consumer = getReadableStreamConsumer(stream);
      if (consumer === undefined) {
        return createReadResult(undefined as unknown as T, true);
      }
      // Normal close with a live consumer: return a zero-length view over
      // the transferred buffer (spec ReadableByteStreamControllerPullInto
      // step 2 "closed" branch).
      const emptyView = new info.viewCtor(transferred, info.byteOffset, 0);
      return createReadResult(emptyView as T, true);
    }
    // BACKEND-BLIND: the byte-consumer interface covers both backends; the
    // cast is justified by the byte-capable reader gate in the constructor.
    const consumer = getReadableStreamConsumer(
      stream
    ) as unknown as ByteStreamConsumerType;
    const withResolvers = PromiseWithResolvers() as PromiseWithResolversType<
      ReadableStreamReadResult<ArrayBufferView>
    >;
    const descriptor: PullIntoDescriptor = {
      buffer: transferred,
      bufferByteLength: ArrayBufferPrototypeByteLengthGet(transferred),
      byteOffset: info.byteOffset,
      byteLength: info.byteLength,
      bytesFilled: 0,
      minimumFill: min * info.elementSize,
      elementSize: info.elementSize,
      viewCtor: info.viewCtor,
      readerType: 'byob',
      promise: withResolvers.promise,
      resolve: withResolvers.resolve,
      reject: withResolvers.reject,
      reader: this,
    };
    const promise = consumer.readBYOB(descriptor);
    const controller = getReadableStreamController(stream);
    if (controller !== undefined) controllerPullIfNeeded(controller);
    const result = await promise;
    // Drain-then-close (spec HandleQueueDrain): a BYOB fill that consumed
    // the last queued bytes with close requested must transition the
    // stream — without this, the NEXT read(view) would pend forever (BYOB
    // descriptors are not auto-committed at the sentinel).
    if (controller !== undefined) controllerMaybeCloseStream(controller);
    return result as unknown as ReadableStreamReadResult<T>;
  }

  async readAtLeast<T extends ArrayBufferView>(
    minElements: number,
    view: T
  ): Promise<ReadableStreamReadResult<T>> {
    return this.read(view, { min: minElements });
  }

  releaseLock(): void {
    if (!isReaderBoundToStream(this)) return;
    readableStreamReaderGenericRelease(this);
  }
}

// Stream-level state transitions shared by the controllers and (in later
// phases) the read paths. Defined as plain functions — they only use the
// static-block-exported accessors, which are assigned at class-definition
// time, strictly before any of this can run.
function readableStreamClose<R>(stream: ReadableStream<R>): void {
  if (getReadableStreamGetState(stream) !== 'readable') return;
  setReadableStreamState(stream, 'closed');
  const reader = getReadableStreamReader(stream);
  if (reader !== undefined) {
    resolveGenericReaderPromise(reader);
  }
  notifyBranchSettled(stream);
}

function readableStreamError<R>(stream: ReadableStream<R>, e: unknown): void {
  if (getReadableStreamGetState(stream) !== 'readable') return;
  setReadableStreamState(stream, 'errored');
  setReadableStreamStoredError(stream, e);
  const reader = getReadableStreamReader(stream);
  if (reader !== undefined) {
    rejectGenericReaderPromise(reader, e);
  }
  notifyBranchSettled(stream);
}

// Metadata snapshot of an ArrayBufferView, captured at a trust boundary.
// All reads go through captured prototype getters — view.buffer et al. are
// patchable accessors, and view.constructor is user-controllable.
interface ViewInfo {
  buffer: ArrayBuffer;
  byteOffset: number;
  byteLength: number;
  elementSize: number; // BYTES_PER_ELEMENT; 1 for DataView
  length: number; // element count; byteLength for DataView
  // The view's REAL constructor, resolved from the internal type slot via
  // the primordials name→ctor map (DataView capture for DataViews).
  viewCtor: new (
    buffer: ArrayBuffer,
    byteOffset: number,
    length: number
  ) => ArrayBufferView;
}

// PRECONDITION: caller has verified isArrayBufferView(view). Every
// ArrayBufferView is either a typed array (identified via the internal
// [[TypedArrayName]] slot) or a DataView; the DataView getters brand-check
// the receiver, so nothing dishonest survives this function.
function getViewInfo(view: ArrayBufferView): ViewInfo {
  const name = TypedArrayPrototypeGetSymbolToStringTag(view);
  if (name !== undefined) {
    const viewCtor = TypedArrayCtorByName[name];
    return {
      buffer: TypedArrayPrototypeGetBuffer(view),
      byteOffset: TypedArrayPrototypeGetByteOffset(view),
      byteLength: TypedArrayPrototypeGetByteLength(view),
      // BYTES_PER_ELEMENT is a non-writable, non-configurable own property
      // of the captured constructor — safe to read.
      elementSize: viewCtor.BYTES_PER_ELEMENT,
      length: TypedArrayPrototypeGetLength(view),
      viewCtor,
    };
  }
  const dataView = view as DataView;
  const byteLength = DataViewPrototypeGetByteLength(dataView);
  return {
    buffer: DataViewPrototypeGetBuffer(dataView),
    byteOffset: DataViewPrototypeGetByteOffset(dataView),
    byteLength,
    elementSize: 1,
    length: byteLength,
    viewCtor: DataView,
  };
}

// Validate and normalize a user-provided chunk for a byte stream at the
// enqueue()/respondWithNewView() trust boundary: snapshot metadata, reject
// zero-length views and zero-length (or detached — detached buffers report
// byteLength 0) buffers, and transfer the backing buffer. The returned
// triple references the TRANSFERRED buffer; the caller's view is detached.
function validateAndTransferView(view: ArrayBufferView): ByteQueueEntry {
  if (!isArrayBufferView(view)) {
    throw new TypeError('chunk must be an ArrayBufferView');
  }
  const info = getViewInfo(view);
  if (info.byteLength === 0) {
    throw new TypeError('chunk must have a non-zero byteLength');
  }
  if (ArrayBufferPrototypeByteLengthGet(info.buffer) === 0) {
    throw new TypeError(
      "chunk's backing buffer must have a non-zero byteLength"
    );
  }
  return {
    buffer: ArrayBufferPrototypeTransfer(info.buffer),
    byteOffset: info.byteOffset,
    byteLength: info.byteLength,
  };
}

let assertIsReadableStreamDefaultController: <R>(
  self: ReadableStreamDefaultController<R>
) => void;

class ReadableStreamDefaultController<
  R = unknown,
> implements ReadableStreamDefaultControllerType<R> {
  #stream: ReadableStream<R>;
  #queue: StreamQueueType<R, R>;
  // Algorithms are cleared (closures dropped) on close-complete, error, and
  // cancel, per spec ClearAlgorithms.
  #sizeAlgorithm: ((chunk: R) => number) | undefined;
  #pullAlgorithm: (() => Promise<void>) | undefined;
  #cancelAlgorithm: ((reason: unknown) => Promise<void>) | undefined;
  #started: boolean = false;
  #pulling: boolean = false;
  #pullAgain: boolean = false;
  #closeRequested: boolean = false;
  #cancelPromise: Promise<void> | undefined;

  static {
    assertIsReadableStreamDefaultController = function <R>(
      self: ReadableStreamDefaultController<R>
    ): void {
      if (!isActualObject(self) || !(#queue in self))
        throw new TypeError('Illegal invocation');
    };

    // BACKEND-DISPATCH: the chained controller helpers (one of the five
    // sanctioned dispatch points). Each backend wraps the previous
    // implementation behind its own brand check — private-brand `in` for
    // the queued controllers, NEVER instanceof: these classes end up on
    // the global, so Symbol.hasInstance on them is user-reachable. The
    // byte controller chains in its static block below; the native
    // controller's wrap is joined at the bottom of this module (its brand
    // predicate crosses the module fence from native.ts).
    controllerPullIfNeeded = (controller) => {
      if (#queue in controller) {
        (controller as ReadableStreamDefaultController).#callPullIfNeeded();
      }
      // The byte controller wires its own branch in the byte pass.
    };

    // expectedLength is byte-stream-only: the default controller never
    // reads it from the source (silently ignored if declared) and always
    // reports undefined.
    getControllerExpectedLength = () => undefined;

    controllerCancelSteps = (controller, reason) => {
      if (#queue in controller) {
        return (controller as ReadableStreamDefaultController).#cancelSteps(
          reason
        );
      }
      return PromiseResolve();
    };

    controllerMaybeCloseStream = (controller) => {
      if (#queue in controller) {
        (controller as ReadableStreamDefaultController).#maybeCloseStream();
      }
    };

    // Default controllers have no byobRequest to invalidate; the byte
    // controller's static block wraps this with the real implementation.
    controllerOnReaderRelease = (_controller) => {};
  }

  constructor(
    privateSymbol: symbol,
    stream: ReadableStream<R>,
    underlyingSource: UnderlyingDefaultSource<R>,
    sizeAlgorithm: (chunk: R) => number,
    highWaterMark: number
  ) {
    assertPrivateSymbol(privateSymbol);
    this.#stream = stream;

    // --- Underlying source method extraction ---
    // Methods are read ONCE here (spec: dictionary conversion /
    // CreateAlgorithmFromUnderlyingMethod) and invoked via captured
    // uncurryThis wrappers with the source object as `this`. Property reads
    // are in alphabetical order to match WebIDL dictionary conversion.
    const cancelFn = underlyingSource.cancel;
    if (cancelFn !== undefined && typeof cancelFn !== 'function') {
      throw new TypeError('underlyingSource.cancel must be a function');
    }
    const pullFn = underlyingSource.pull;
    if (pullFn !== undefined && typeof pullFn !== 'function') {
      throw new TypeError('underlyingSource.pull must be a function');
    }
    const startFn = underlyingSource.start;
    if (startFn !== undefined && typeof startFn !== 'function') {
      throw new TypeError('underlyingSource.start must be a function');
    }

    if (cancelFn !== undefined) {
      const callCancel = uncurryThis(cancelFn);
      // Spec PromiseCall: sync throws become rejections; result is always
      // a promise.
      this.#cancelAlgorithm = (reason: unknown) => {
        try {
          return PromiseResolve(
            callCancel(underlyingSource, reason)
          ) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }
    if (pullFn !== undefined) {
      const callPull = uncurryThis(pullFn);
      this.#pullAlgorithm = () => {
        try {
          return PromiseResolve(
            callPull(underlyingSource, this)
          ) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }

    // Strategy already extracted by caller (WebIDL conversion order).
    this.#sizeAlgorithm = sizeAlgorithm;

    // --- Queue + the stream's own cursor ---
    this.#queue = new StreamQueue(highWaterMark, () => {
      // Last consumer went away (GC-driven cursor cleanup after all
      // branch streams became unreachable). Silently stop the source
      // by clearing algorithms — do NOT invoke the user's cancel
      // callback, because GC timing is nondeterministic and must not
      // produce user-observable side effects (the cancel callback
      // could push to event arrays, resolve promises, etc.).
      this.#clearAlgorithms();
    }) as StreamQueueType<R, R>;
    setReadableStreamConsumer(
      stream,
      new QueueCursor(this.#queue, stream) as QueueCursorType<R, R>
    );

    // --- Start ---
    // Per spec, start is invoked synchronously and a sync throw propagates
    // out of the ReadableStream constructor.
    const startResult: unknown =
      startFn === undefined
        ? undefined
        : uncurryThis(startFn)(underlyingSource, this);
    PromisePrototypeThen(
      PromiseResolve(startResult),
      () => {
        this.#started = true;
        this.#callPullIfNeeded();
      },
      (e: unknown) => {
        this.error(e);
      }
    );
  }

  get desiredSize(): number | null {
    assertIsReadableStreamDefaultController(this);
    // null when ERRORED, 0 when CLOSED, computed while readable — including
    // while close-requested-but-still-draining (spec GetDesiredSize).
    switch (getReadableStreamGetState(this.#stream)) {
      case 'errored':
        return null;
      case 'closed':
        return 0;
      default:
        return this.#queue.desiredSize;
    }
  }

  enqueue(chunk: R = undefined as R): void {
    assertIsReadableStreamDefaultController(this);
    if (!this.#canCloseOrEnqueue()) {
      throw new TypeError(
        'Cannot enqueue a chunk into a stream that is closed or closing'
      );
    }
    // Spec step 3: if the stream has a default reader with pending read
    // requests, fulfill the first one directly — no size() call, no queue.
    // This must be checked BEFORE size() to avoid reentrant reads from
    // inside size() being fulfilled eagerly by the queue notification.
    const cursor = getReadableStreamConsumer(this.#stream) as
      | QueueCursorType<R, R>
      | undefined;
    if (
      cursor !== undefined &&
      cursor.hasPendingRead &&
      isReadableStreamLocked(this.#stream)
    ) {
      cursor.fulfillFirstPendingRead(chunk);
      this.#callPullIfNeeded();
      return;
    }
    // Snapshot pending-read state before size(). Reads added reentrantly
    // from inside size() must NOT be auto-filled by this enqueue's queue
    // notification — the spec's EnqueueValueWithSize just stores the
    // chunk; the reentrant reads wait for the next enqueue to fulfill
    // them via the direct path (step 3 above).
    const hadPendingRead = cursor !== undefined && cursor.hasPendingRead;
    let size: number;
    try {
      // assert: sizeAlgorithm is set (canCloseOrEnqueue guard above
      // rejects after clearAlgorithms)
      size = +(this.#sizeAlgorithm as (chunk: R) => number)(chunk as R);
      // Spec EnqueueValueWithSize: NaN, negative, and +Infinity sizes are
      // RangeErrors.
      if (NumberIsNaN(size) || size < 0 || size === Infinity) {
        throw new RangeError('Invalid chunk size');
      }
    } catch (e) {
      // A throwing size() (or invalid size) errors the stream AND
      // propagates to the caller (spec enqueue error steps).
      this.error(e);
      throw e;
    }
    // Suppress cursor notification when reads were added reentrantly
    // during size(). The chunk goes into the queue (updating the size
    // accounting) but pending reads are left unfilled until the next
    // enqueue triggers the direct-fulfillment path (step 3).
    const notify = hadPendingRead || !cursor?.hasPendingRead;
    this.#queue.enqueue({ value: chunk as R, size }, notify);
    this.#callPullIfNeeded();
  }

  close(): void {
    assertIsReadableStreamDefaultController(this);
    if (!this.#canCloseOrEnqueue()) {
      throw new TypeError(
        'Cannot close a stream that is already closed or closing'
      );
    }
    this.#closeRequested = true;
    this.#queue.close(); // pushes the sentinel; notifies all cursors
    // If the stream's own cursor has already drained to the sentinel, the
    // stream closes immediately; otherwise the read paths complete the
    // transition when the cursor reaches the sentinel (drain-then-close).
    this.#maybeCloseStream();
  }

  error(reason: unknown = undefined): void {
    assertIsReadableStreamDefaultController(this);
    if (getReadableStreamGetState(this.#stream) !== 'readable') return;
    // Propagate to every live consumer stream (tee branches) via the
    // cursors' weak owner refs — no strong retention of branches. The
    // primary stream is handled explicitly: a tee'd-away parent has no
    // cursor. readableStreamError is state-guarded, so overlap is fine.
    const owners = this.#queue.getLiveOwners();
    this.#queue.error(reason); // rejects pending reads, drops buffered data
    this.#clearAlgorithms();
    for (let i = 0; i < owners.length; i++) {
      readableStreamError(owners[i] as ReadableStream<R>, reason);
    }
    readableStreamError(this.#stream, reason);
  }

  #canCloseOrEnqueue(): boolean {
    // #cancelPromise doubles as the "source cancelled" flag: after
    // CancelSteps (explicit cancel or the all-cursors-gone hook) the
    // algorithms are cleared and enqueue/close must be rejected even thoughs
    // the original stream object may still report state 'readable' (the
    // GC-driven path has no stream left to transition).
    return (
      !this.#closeRequested &&
      this.#cancelPromise === undefined &&
      getReadableStreamGetState(this.#stream) === 'readable'
    );
  }

  #maybeCloseStream(): void {
    if (!this.#closeRequested) return;
    // Check ALL live consumer streams (tee branches + the parent).
    // A stream whose cursor has reached the close sentinel transitions
    // to 'closed'; streams with buffered data before the sentinel stay
    // 'readable' until drained (drain-then-close on subsequent reads).
    let anyOpen = false;
    const owners = this.#queue.getLiveOwners();
    for (let i = 0; i < owners.length; i++) {
      const owner = owners[i] as ReadableStream<R>;
      const cursor = getReadableStreamConsumer(owner) as
        | QueueCursorType<R, R>
        | undefined;
      if (cursor === undefined) continue;
      if (this.#queue.getEntry(cursor.position) === CLOSE_SENTINEL) {
        readableStreamClose(owner);
      } else {
        anyOpen = true;
      }
    }
    // Also check the parent stream itself (pre-tee path: only one consumer).
    const parentCursor = getReadableStreamConsumer(this.#stream) as
      | QueueCursorType<R, R>
      | undefined;
    if (parentCursor !== undefined) {
      if (this.#queue.getEntry(parentCursor.position) === CLOSE_SENTINEL) {
        readableStreamClose(this.#stream);
      } else {
        anyOpen = true;
      }
    }
    if (!anyOpen) {
      this.#clearAlgorithms();
    }
  }

  #shouldCallPull(): boolean {
    if (!this.#started) return false;
    if (!this.#canCloseOrEnqueue()) return false;
    // The pending-read clause is what keeps a fast consumer from starving
    // when the queue is at the high water mark: a consumer that reads
    // faster than the HWM drains must still trigger pulls.
    return this.#queue.desiredSize > 0 || this.#queue.anyCursorHasPendingRead();
  }

  #callPullIfNeeded(): void {
    if (!this.#shouldCallPull()) return;
    if (this.#pulling) {
      this.#pullAgain = true;
      return;
    }
    this.#pulling = true;
    const pullAlgorithm = this.#pullAlgorithm;
    const result =
      pullAlgorithm === undefined ? PromiseResolve() : pullAlgorithm();
    PromisePrototypeThen(
      result,
      () => {
        this.#pulling = false;
        if (this.#pullAgain) {
          this.#pullAgain = false;
          this.#callPullIfNeeded();
        }
      },
      (e: unknown) => {
        this.error(e);
      }
    );
  }

  // Cancel the underlying source (spec CancelSteps). Idempotent and cached:
  // this can be reached from both an explicit stream/branch cancel and the
  // all-cursors-gone hook.
  #cancelSteps(reason: unknown): Promise<void> {
    if (this.#cancelPromise !== undefined) return this.#cancelPromise;
    const cancelAlgorithm = this.#cancelAlgorithm;
    this.#clearAlgorithms();
    this.#cancelPromise =
      cancelAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : cancelAlgorithm(reason);
    return this.#cancelPromise;
  }

  #clearAlgorithms(): void {
    this.#pullAlgorithm = undefined;
    this.#cancelAlgorithm = undefined;
    this.#sizeAlgorithm = undefined;
  }
}

// Cross-class accessors for the BYOB request/controller pairing, assigned
// in the respective static blocks. The request methods delegate to the byte
// controller (assigned later in module evaluation — only ever called at
// runtime, after all classes are defined).
let initializeByobRequest: (
  request: ReadableStreamBYOBRequest,
  controller: ReadableByteStreamController,
  view: Uint8Array,
  atLeast: number
) => void;
let invalidateByobRequestObject: (request: ReadableStreamBYOBRequest) => void;
let byteControllerRespond: (
  controller: ReadableByteStreamController,
  bytesWritten: number
) => void;
let byteControllerRespondWithNewView: (
  controller: ReadableByteStreamController,
  view: ArrayBufferView
) => void;
let getByteControllerAutoAllocateChunkSize: (
  controller: ReadableByteStreamController
) => number | undefined;

let assertIsReadableStreamBYOBRequest: (
  self: ReadableStreamBYOBRequest
) => void;

class ReadableStreamBYOBRequest implements ReadableStreamBYOBRequestType {
  // All null once invalidated. Per spec, EVERY respond()/
  // respondWithNewView()/enqueue() invalidates the outstanding request;
  // the next byobRequest access mints a fresh one over the remainder.
  #controller: ReadableByteStreamController | null = null;
  #view: Uint8Array | null = null;
  #atLeast: number | null = null;

  static {
    assertIsReadableStreamBYOBRequest = function (
      self: ReadableStreamBYOBRequest
    ): void {
      if (!isActualObject(self) || !(#view in self))
        throw new TypeError('Illegal invocation');
    };

    initializeByobRequest = (request, controller, view, atLeast) => {
      request.#controller = controller;
      request.#view = view;
      request.#atLeast = atLeast;
    };

    invalidateByobRequestObject = (request) => {
      request.#controller = null;
      request.#view = null;
      request.#atLeast = null;
    };
  }

  constructor(privateSymbol: symbol) {
    assertPrivateSymbol(privateSymbol);
  }

  get view(): Uint8Array | null {
    assertIsReadableStreamBYOBRequest(this);
    return this.#view;
  }

  // Non-standard workerd extension (compat parity with the existing
  // implementation's getAtLeast): the minimum number of BYTES (never
  // elements) the source must still deliver before the outstanding read
  // is satisfied — i.e., the head descriptor's remaining minimum
  // (minimumFill − bytesFilled), captured at mint. The capture stays
  // fresh because every fill path (respond/respondWithNewView/enqueue)
  // invalidates this request and the next access mints a new one. For a
  // min-less read(view) this equals the view's element size, matching the
  // old implementation's max(elementSize, atLeast) floor. null once
  // invalidated, mirroring the old kj::Maybe behavior.
  get atLeast(): number | null {
    assertIsReadableStreamBYOBRequest(this);
    return this.#atLeast;
  }

  respond(bytesWritten: number): void {
    assertIsReadableStreamBYOBRequest(this);
    if (this.#controller === null) {
      throw new TypeError('This BYOB request has been invalidated');
    }
    byteControllerRespond(this.#controller, bytesWritten);
  }

  respondWithNewView(view: ArrayBufferView): void {
    assertIsReadableStreamBYOBRequest(this);
    if (this.#controller === null) {
      throw new TypeError('This BYOB request has been invalidated');
    }
    if (!isArrayBufferView(view)) {
      throw new TypeError('view must be an ArrayBufferView');
    }
    // Spec step 2: detached buffers are TypeError, not RangeError.
    const info = getViewInfo(view);
    if (ArrayBufferPrototypeDetachedGet(info.buffer)) {
      throw new TypeError("The view's buffer has been detached");
    }
    byteControllerRespondWithNewView(this.#controller, view);
  }
}

let assertIsReadableByteStreamController: (
  self: ReadableByteStreamController
) => void;

class ReadableByteStreamController implements ReadableByteStreamControllerType {
  #stream: ReadableStream<Uint8Array>;
  #queue: StreamQueueType<ByteQueueEntry, Uint8Array>;
  #pullAlgorithm: (() => Promise<void>) | undefined;
  #cancelAlgorithm: ((reason: unknown) => Promise<void>) | undefined;
  #autoAllocateChunkSize: number | undefined;
  // The non-standard exact-total byte contract (undefined = unknown).
  // The source must deliver exactly this many bytes over its lifetime
  // (via enqueue AND byobRequest responds combined): overflow at
  // delivery and underflow at close() are RangeError violations.
  // Consumer-initiated cancel is exempt.
  #expectedLength: bigint | undefined;
  #bytesDelivered: bigint = 0n;
  #started: boolean = false;
  #pulling: boolean = false;
  #pullAgain: boolean = false;
  #closeRequested: boolean = false;
  #cancelPromise: Promise<void> | undefined;
  #byobRequest: ReadableStreamBYOBRequest | null = null;

  static {
    isByteStreamController = (value: unknown) => {
      return isActualObject(value) && #queue in value;
    };

    assertIsReadableByteStreamController = function (
      self: ReadableByteStreamController
    ): void {
      if (!isByteStreamController(self))
        throw new TypeError('Illegal invocation');
    };

    // Chain the controller dispatch helpers. The default controller's
    // static block (which runs earlier in module evaluation) assigned the
    // initial implementations; we wrap them. Note the two classes' #queue
    // private names are distinct brands, so the `in` checks discriminate
    // correctly.
    const prevPullIfNeeded = controllerPullIfNeeded;
    controllerPullIfNeeded = (controller) => {
      if (#queue in controller) {
        controller.#callPullIfNeeded();
      } else {
        prevPullIfNeeded(controller);
      }
    };

    const prevCancelSteps = controllerCancelSteps;
    controllerCancelSteps = (controller, reason) => {
      if (#queue in controller) {
        return controller.#cancelSteps(reason);
      }
      return prevCancelSteps(controller, reason);
    };

    const prevMaybeCloseStream = controllerMaybeCloseStream;
    controllerMaybeCloseStream = (controller) => {
      if (#queue in controller) {
        controller.#maybeCloseStream();
      } else {
        prevMaybeCloseStream(controller);
      }
    };

    byteControllerRespond = (controller, bytesWritten) => {
      controller.#respond(bytesWritten);
    };

    byteControllerRespondWithNewView = (controller, view) => {
      controller.#respondWithNewView(view);
    };

    getByteControllerAutoAllocateChunkSize = (controller) => {
      return controller.#autoAllocateChunkSize;
    };

    const prevOnReaderRelease = controllerOnReaderRelease;
    controllerOnReaderRelease = (controller) => {
      if (#queue in controller) {
        // Spec: releaseLock does NOT invalidate the byobRequest. The
        // pull-into descriptor stays in pendingPullIntos with readerType
        // set to 'none'; a future respond() will enqueue the data into the
        // queue for the next reader instead of resolving a read promise.
      } else {
        prevOnReaderRelease(controller);
      }
    };

    const prevExpectedLength = getControllerExpectedLength;
    getControllerExpectedLength = (controller) => {
      if (#queue in controller) {
        return (controller as ReadableByteStreamController).#expectedLength;
      }
      return prevExpectedLength(controller);
    };
  }

  constructor(
    privateSymbol: symbol,
    stream: ReadableStream<Uint8Array>,
    underlyingSource: UnderlyingByteSource,
    highWaterMark: number
  ) {
    assertPrivateSymbol(privateSymbol);
    this.#stream = stream;

    // --- Underlying source extraction (alphabetical property reads) ---
    const autoAllocateChunkSize = underlyingSource.autoAllocateChunkSize;
    if (autoAllocateChunkSize !== undefined) {
      const size = +autoAllocateChunkSize;
      // Approximates WebIDL [EnforceRange] unsigned long long plus the
      // spec's explicit zero check.
      if (NumberIsNaN(size) || size <= 0 || size % 1 !== 0) {
        throw new TypeError('autoAllocateChunkSize must be a positive integer');
      }
      this.#autoAllocateChunkSize = size;
    }
    const cancelFn = underlyingSource.cancel;
    if (cancelFn !== undefined && typeof cancelFn !== 'function') {
      throw new TypeError('underlyingSource.cancel must be a function');
    }
    // Non-standard extension (byte streams only): the TOTAL bytes this
    // source promises to produce. Read once and cached; this controller
    // enforces the contract at enqueue/respond (overflow) and close()
    // (underflow). Exposed to the C++ bridge via the DrainingReader.
    this.#expectedLength = normalizeExpectedLength(
      underlyingSource.expectedLength
    );
    const pullFn = underlyingSource.pull;
    if (pullFn !== undefined && typeof pullFn !== 'function') {
      throw new TypeError('underlyingSource.pull must be a function');
    }
    const startFn = underlyingSource.start;
    if (startFn !== undefined && typeof startFn !== 'function') {
      throw new TypeError('underlyingSource.start must be a function');
    }

    if (cancelFn !== undefined) {
      const callCancel = uncurryThis(cancelFn);
      this.#cancelAlgorithm = (reason: unknown) => {
        try {
          return PromiseResolve(
            callCancel(underlyingSource, reason)
          ) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }
    if (pullFn !== undefined) {
      const callPull = uncurryThis(pullFn);
      this.#pullAlgorithm = () => {
        try {
          return PromiseResolve(
            callPull(underlyingSource, this)
          ) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }

    // Strategy already extracted by caller (WebIDL conversion order).

    // --- Queue + the stream's own (byte) cursor ---
    this.#queue = new StreamQueue(highWaterMark, () => {
      // Last consumer went away (GC-driven cursor cleanup). Silently
      // stop the source — see the default controller's hook for the
      // rationale (GC must not produce user-observable side effects).
      this.#clearAlgorithms();
    }) as StreamQueueType<ByteQueueEntry, Uint8Array>;
    const cursor = new ByteStreamCursor(this.#queue, stream);
    // Wire up the fractional-element-at-close error callback so the
    // cursor can error the stream when a BYOB read lands at the close
    // sentinel with a non-element-aligned partial fill.
    cursor.errorStreamCallback = (e: unknown) => {
      this.error(e);
    };
    setReadableStreamConsumer(stream, cursor);

    // --- Start (sync throw propagates out of the ReadableStream ctor) ---
    const startResult: unknown =
      startFn === undefined
        ? undefined
        : uncurryThis(startFn)(underlyingSource, this);
    PromisePrototypeThen(
      PromiseResolve(startResult),
      () => {
        this.#started = true;
        this.#callPullIfNeeded();
      },
      (e: unknown) => {
        this.error(e);
      }
    );
  }

  get desiredSize(): number | null {
    assertIsReadableByteStreamController(this);
    switch (getReadableStreamGetState(this.#stream)) {
      case 'errored':
        return null;
      case 'closed':
        return 0;
      default:
        return this.#queue.desiredSize;
    }
  }

  get byobRequest(): ReadableStreamBYOBRequestType | null {
    assertIsReadableByteStreamController(this);
    if (this.#byobRequest === null) {
      // Zero-copy is only unambiguous with exactly one consumer, and only
      // when it has a head pull-into descriptor (from a BYOB read, or
      // auto-allocated for a default read when autoAllocateChunkSize is
      // set). Otherwise the source must fall back to enqueue(). The
      // request object is cached for identity until invalidated.
      const cursor = this.#queue.singleCursor as
        | ByteStreamCursorType
        | undefined;
      if (cursor === undefined) return null;
      const view = cursor.pendingPullIntoView;
      const head = cursor.headPullInto;
      if (view === undefined || head === undefined) return null;
      const request = new ReadableStreamBYOBRequest(kPrivateSymbol);
      // atLeast = the head descriptor's remaining minimum, in bytes. The
      // head is unfulfilled by construction (a fulfilled descriptor is
      // shifted before any request could be minted), so this is >= 1.
      initializeByobRequest(
        request,
        this,
        view,
        head.minimumFill - head.bytesFilled
      );
      this.#byobRequest = request;
    }
    return this.#byobRequest;
  }

  enqueue(chunk: ArrayBufferView): void {
    assertIsReadableByteStreamController(this);
    if (!this.#canCloseOrEnqueue()) {
      throw new TypeError(
        'Cannot enqueue a chunk into a stream that is closed or closing'
      );
    }
    // Trust boundary: snapshot metadata via captured getters, validate,
    // transfer the backing buffer, normalize to a {buffer, byteOffset,
    // byteLength} triple. Queue internals never touch the user's view.
    const entry = validateAndTransferView(chunk);
    // EXPECTED-LENGTH CONTRACT: overflow check before the entry reaches
    // the queue. (The buffer was already transferred above — a refused
    // overflow chunk's buffer is detached. Contract violators lose the
    // buffer; the stream errors via the pull-rejection path anyway.)
    this.#accountDelivery(entry.byteLength);
    // Per spec, enqueue invalidates the outstanding byobRequest (a fresh
    // one over the updated remainder is minted on next access).
    this.#invalidateByobRequest();
    const drainCursor = this.#queue.singleCursor as
      | ByteStreamCursorType
      | undefined;
    if (drainCursor !== undefined) {
      const head = drainCursor.headPullInto;
      if (head !== undefined) {
        // Spec step 8.4: transfer the head descriptor's buffer so that
        // old captured views are detached.
        head.buffer = ArrayBufferPrototypeTransfer(head.buffer);
      }
      // Spec step 8.5: if the head pending pull-into has readerType 'none'
      // (leftover from releaseLock), drain it before adding the new chunk.
      drainCursor.drainNoneDescriptors();
      // Spec step 9.3: if the head descriptor is an auto-allocate
      // (readerType 'default'), discard it and fulfill the pending default
      // read directly from the enqueued chunk. The auto-allocate buffer is
      // abandoned — the result uses the chunk's (smaller) buffer.
      const autoDesc = drainCursor.shiftAutoAllocateDescriptor();
      if (autoDesc !== undefined) {
        const view = new Uint8Array(
          entry.buffer,
          entry.byteOffset,
          entry.byteLength
        );
        autoDesc.resolve(createReadResult(view, false));
        this.#callPullIfNeeded();
        return;
      }
    }
    this.#queue.enqueue({ value: entry, size: entry.byteLength });
    // The cursors' notify() (run by queue.enqueue) services pending
    // pull-intos and default reads alike.
    this.#callPullIfNeeded();
  }

  close(): void {
    assertIsReadableByteStreamController(this);
    if (!this.#canCloseOrEnqueue()) {
      throw new TypeError(
        'Cannot close a stream that is already closed or closing'
      );
    }
    // Spec: closing with a fractional-element partial fill in a head
    // descriptor is a TypeError that also errors the stream — the bytes to
    // complete the element can never arrive. Checked across ALL live
    // cursors: tee branches each track their own partial fills through the
    // enqueue-path min-read machinery.
    const hasFractionalFill = this.#queue.someLiveCursor((cursor) => {
      const head: PullIntoDescriptor | undefined = (
        cursor as unknown as ByteStreamCursorType
      ).headPullInto;
      return head !== undefined && head.bytesFilled % head.elementSize !== 0;
    });
    if (hasFractionalFill) {
      const e = new TypeError(
        'Insufficient bytes to fill elements in the given view'
      );
      this.error(e);
      throw e;
    }
    // EXPECTED-LENGTH CONTRACT: closing before delivering the declared
    // total is underflow — error the stream and throw (mirrors the
    // fractional-fill violation above). Data still buffered in the queue
    // counts as delivered: the source produced it.
    if (
      this.#expectedLength !== undefined &&
      this.#bytesDelivered < this.#expectedLength
    ) {
      const e = new RangeError(
        'byte source closed before producing its declared expectedLength'
      );
      this.error(e);
      throw e;
    }
    this.#closeRequested = true;
    this.#queue.close();
    this.#maybeCloseStream();
  }

  error(reason: unknown = undefined): void {
    assertIsReadableByteStreamController(this);
    if (getReadableStreamGetState(this.#stream) !== 'readable') return;
    this.#invalidateByobRequest();
    // Branch propagation — see the default controller's error() for why.
    const owners = this.#queue.getLiveOwners();
    this.#queue.error(reason);
    this.#clearAlgorithms();
    for (let i = 0; i < owners.length; i++) {
      readableStreamError(owners[i] as ReadableStream<Uint8Array>, reason);
    }
    readableStreamError(this.#stream, reason);
  }

  // byobRequest.respond(bytesWritten) — the zero-copy path.
  #respond(bytesWritten: number): void {
    bytesWritten = +bytesWritten;
    if (
      NumberIsNaN(bytesWritten) ||
      bytesWritten < 0 ||
      bytesWritten % 1 !== 0
    ) {
      throw new TypeError('bytesWritten must be a non-negative integer');
    }
    const cursor = this.#queue.singleCursor as ByteStreamCursorType | undefined;
    const head = cursor === undefined ? undefined : cursor.headPullInto;
    if (cursor === undefined || head === undefined) {
      throw new TypeError('No pending BYOB request');
    }
    const state = getReadableStreamGetState(this.#stream);
    if (state === 'closed') {
      if (bytesWritten !== 0) {
        throw new TypeError(
          'bytesWritten must be zero after the stream is closed'
        );
      }
    } else {
      if (bytesWritten === 0) {
        throw new TypeError(
          'bytesWritten must be non-zero while the stream is readable'
        );
      }
      if (head.bytesFilled + bytesWritten > head.byteLength) {
        throw new RangeError(
          'bytesWritten exceeds the remaining space in the view'
        );
      }
    }
    // Spec: the descriptor's buffer is re-transferred on every respond;
    // the result view and any remainder view target the NEW buffer.
    head.buffer = ArrayBufferPrototypeTransfer(head.buffer);
    this.#invalidateByobRequest();
    if (state === 'closed') {
      // respond(0)-while-closed: commit all pending descriptors with
      // { done: true, value: filled-so-far view }.
      cursor.commitPullIntosOnClose();
    } else {
      // EXPECTED-LENGTH CONTRACT: respond() bytes count toward the total
      // (the second ingress path alongside enqueue).
      this.#accountDelivery(bytesWritten);
      cursor.respondBYOB(bytesWritten);
      // The respond may have drained the queue with close requested.
      this.#maybeCloseStream();
      this.#callPullIfNeeded();
    }
  }

  // Exact-total accounting (see #expectedLength). On overflow (more bytes
  // than declared), errors the readable side and throws — the throw
  // propagates to the enqueue/respond caller. The readable must be
  // errored explicitly here because when the caller is an external sink
  // (e.g. IdentityTransformStream.sinkWrite), the throw only errors the
  // writable side; without this.error() the readable would hang forever.
  #accountDelivery(byteLength: number): void {
    if (this.#expectedLength === undefined) {
      // When there is no expectedLength, we don't need to perform any accounting.
      return;
    }
    const delivered = this.#bytesDelivered + BigInt(byteLength);
    if (delivered > this.#expectedLength) {
      const e = new RangeError(
        'byte source delivered more bytes than its declared expectedLength'
      );
      this.error(e);
      throw e;
    }
    this.#bytesDelivered = delivered;
  }

  // byobRequest.respondWithNewView(view).
  #respondWithNewView(view: ArrayBufferView): void {
    const cursor = this.#queue.singleCursor as ByteStreamCursorType | undefined;
    const head = cursor === undefined ? undefined : cursor.headPullInto;
    if (cursor === undefined || head === undefined) {
      throw new TypeError('No pending BYOB request');
    }
    const info = getViewInfo(view);
    const state = getReadableStreamGetState(this.#stream);
    if (state === 'closed') {
      if (info.byteLength !== 0) {
        throw new TypeError(
          'The view must be zero-length after the stream is closed'
        );
      }
    } else if (info.byteLength === 0) {
      throw new TypeError(
        'The view must be non-zero-length while the stream is readable'
      );
    }
    if (head.byteOffset + head.bytesFilled !== info.byteOffset) {
      throw new RangeError(
        'The view byteOffset must match the bytes already filled'
      );
    }
    // Spec step: "If firstDescriptor's buffer byte length ≠
    // view.[[ViewedArrayBuffer]].[[ArrayBufferByteLength]], throw a
    // RangeError."  We compare against the stored bufferByteLength (not
    // head.buffer.byteLength, which may be 0 if the buffer was transferred).
    if (
      head.bufferByteLength !== ArrayBufferPrototypeByteLengthGet(info.buffer)
    ) {
      throw new RangeError(
        "The view's buffer must have the same byteLength as the request"
      );
    }
    if (head.bytesFilled + info.byteLength > head.byteLength) {
      throw new RangeError('The view exceeds the remaining space');
    }
    head.buffer = ArrayBufferPrototypeTransfer(info.buffer);
    this.#invalidateByobRequest();
    if (state === 'closed') {
      cursor.commitPullIntosOnClose();
    } else {
      // EXPECTED-LENGTH CONTRACT: counts toward the total like respond().
      this.#accountDelivery(info.byteLength);
      cursor.respondBYOB(info.byteLength);
      // The respond may have drained the queue with close requested.
      this.#maybeCloseStream();
      this.#callPullIfNeeded();
    }
  }

  #invalidateByobRequest(): void {
    if (this.#byobRequest !== null) {
      invalidateByobRequestObject(this.#byobRequest);
      this.#byobRequest = null;
    }
  }

  #canCloseOrEnqueue(): boolean {
    return (
      !this.#closeRequested &&
      this.#cancelPromise === undefined &&
      getReadableStreamGetState(this.#stream) === 'readable'
    );
  }

  #maybeCloseStream(): void {
    if (!this.#closeRequested) return;
    // Check ALL live consumer streams (tee branches + the parent).
    // Mirror of the default controller's logic — see that for comments.
    let anyOpen = false;
    const owners = this.#queue.getLiveOwners();
    for (let i = 0; i < owners.length; i++) {
      const owner = owners[i] as ReadableStream<unknown>;
      const cursor = getReadableStreamConsumer(owner) as
        | QueueCursorType<ByteQueueEntry, Uint8Array>
        | undefined;
      if (cursor === undefined) continue;
      if (this.#queue.getEntry(cursor.position) === CLOSE_SENTINEL) {
        readableStreamClose(owner);
      } else {
        anyOpen = true;
      }
    }
    const parentCursor = getReadableStreamConsumer(this.#stream) as
      | QueueCursorType<ByteQueueEntry, Uint8Array>
      | undefined;
    if (parentCursor !== undefined) {
      if (this.#queue.getEntry(parentCursor.position) === CLOSE_SENTINEL) {
        readableStreamClose(this.#stream);
      } else {
        anyOpen = true;
      }
    }
    if (!anyOpen) {
      this.#clearAlgorithms();
    }
  }

  #shouldCallPull(): boolean {
    if (!this.#started) return false;
    if (!this.#canCloseOrEnqueue()) return false;
    return this.#queue.desiredSize > 0 || this.#queue.anyCursorHasPendingRead();
  }

  #callPullIfNeeded(): void {
    if (!this.#shouldCallPull()) return;
    if (this.#pulling) {
      this.#pullAgain = true;
      return;
    }
    this.#pulling = true;
    const pullAlgorithm = this.#pullAlgorithm;
    const result =
      pullAlgorithm === undefined ? PromiseResolve() : pullAlgorithm();
    PromisePrototypeThen(
      result,
      () => {
        this.#pulling = false;
        if (this.#pullAgain) {
          this.#pullAgain = false;
          this.#callPullIfNeeded();
        }
      },
      (e: unknown) => {
        this.error(e);
      }
    );
  }

  #cancelSteps(reason: unknown): Promise<void> {
    if (this.#cancelPromise !== undefined) return this.#cancelPromise;
    this.#invalidateByobRequest();
    const cancelAlgorithm = this.#cancelAlgorithm;
    this.#clearAlgorithms();
    this.#cancelPromise =
      cancelAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : cancelAlgorithm(reason);
    return this.#cancelPromise;
  }

  #clearAlgorithms(): void {
    this.#pullAlgorithm = undefined;
    this.#cancelAlgorithm = undefined;
  }
}

function setupReadableByteStreamControllerFromUnderlyingSource<R>(
  stream: ReadableStream<R>,
  underlyingSource: UnderlyingSource<R>,
  highWaterMark: number
): ReadableByteStreamControllerType {
  return new ReadableByteStreamController(
    kPrivateSymbol,
    stream as unknown as ReadableStream<Uint8Array>,
    underlyingSource as UnderlyingByteSource,
    highWaterMark
  );
}

function setupReadableStreamDefaultControllerFromUnderlyingSource<R>(
  stream: ReadableStream<R>,
  underlyingSource: UnderlyingSource<R>,
  sizeAlgorithm: (chunk: R) => number,
  highWaterMark: number
): ReadableStreamDefaultControllerType {
  return new ReadableStreamDefaultController<R>(
    kPrivateSymbol,
    stream,
    underlyingSource as UnderlyingDefaultSource<R>,
    sizeAlgorithm,
    highWaterMark
  ) as ReadableStreamDefaultControllerType;
}

export interface DrainingReadResult<R> {
  chunks: R[]; // bulk chunks drained from the queue, in order
  done: boolean; // true if the close sentinel was reached
}

// Null-prototype factory for DrainingReadResult — same rationale as
// createReadResult (see queue.ts): prevents Object.prototype.then
// interception when the result flows through promise resolution.
function createDrainResult<R>(
  chunks: R[],
  done: boolean
): DrainingReadResult<R> {
  const result = ObjectCreate(null) as DrainingReadResult<R>;
  result.chunks = chunks;
  result.done = done;
  return result;
}

// The draining-read core: collect everything buffered at the cursor in one
// shot; when nothing is buffered, fall back to a single pending read and
// then sweep whatever arrived alongside it. After draining, the controller
// is pumped so a synchronous source can immediately refill.
async function drainingReaderReadInternal<R>(
  reader: object,
  stream: ReadableStream<R>,
  maxSize?: number
): Promise<DrainingReadResult<R>> {
  setReadableStreamDisturbed(stream);
  const state = getReadableStreamGetState(stream);
  if (state === 'closed') return createDrainResult<R>([], true);
  if (state === 'errored') throw getReadableStreamStoredError(stream);
  // BACKEND-BLIND: drains whatever the consumer has buffered (a native
  // consumer never has a backlog beyond its overflow slot — the
  // empty-then-wait fallback below covers it naturally).
  const consumer = getReadableStreamConsumer(stream);
  if (consumer === undefined) return createDrainResult<R>([], true);
  const controller = getReadableStreamController(stream);

  let result = consumer.drain(maxSize) as DrainingReadResult<R>;
  if (result.chunks.length === 0 && !result.done) {
    // Nothing buffered — wait for one chunk through the normal pending-read
    // machinery (FIFO with everything else), then sweep the rest.
    const promise = consumer.read(reader);
    if (controller !== undefined) controllerPullIfNeeded(controller);
    const single = await promise;
    if (single.done) {
      result = createDrainResult<R>([], true);
    } else {
      const chunks: R[] = [single.value as R];
      const more = consumer.drain(maxSize) as DrainingReadResult<R>;
      for (let i = 0; i < more.chunks.length; i++) {
        ArrayPrototypePush(chunks, more.chunks[i] as R);
      }
      result = createDrainResult(chunks, more.done);
    }
  }
  // Pump: draining freed queue space, so the source may pull again
  // immediately (synchronous sources refill before we return).
  if (controller !== undefined) controllerPullIfNeeded(controller);
  if (result.done) {
    if (controller !== undefined) controllerMaybeCloseStream(controller);
    readableStreamClose(stream);
  }
  return result;
}

// Bulk reader for pipeTo and the C++ bridge (design doc "Draining Reads").
// Lock-based exclusivity: while held, no default/BYOB reader can interfere
// with the cursor. Internal-only for now (not exposed via getReader();
// see Open Question 3).
class ReadableStreamDrainingReader<R> {
  #base: ReadableStreamReaderBase<R>;

  static {
    const prev = getReaderBase;
    getReaderBase = <R>(reader: object) => {
      if (#base in reader) {
        return reader.#base as unknown as ReadableStreamReaderBase<R>;
      }
      return prev<R>(reader);
    };
  }

  constructor(stream: ReadableStream<R>) {
    this.#base = new ReadableStreamReaderBase();
    if (isReadableStreamLocked(stream)) {
      throw new TypeError('Cannot get a reader for a stream that is locked');
    }
    setReadableStreamReader(
      stream,
      this as unknown as ReadableStreamReaderType<R>
    );
    initializeReadableStreamGenericReader(stream, this.#base);
  }

  get closed(): Promise<void> {
    try {
      return getGenericReaderClosedPromise(this);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  // The non-standard expectedLength pass-through for the C++ bridge: the
  // TOTAL bytes the underlying source declared it will produce
  // (undefined = unknown → chunked encoding). A construction-time value,
  // cached on the controller; backend-blind via the chained helper
  // (byte/native report their cached value; default streams report
  // undefined). Returns undefined after release.
  get expectedLength(): bigint | undefined {
    const stream = getReaderStream<R>(this);
    if (stream === undefined) return undefined;
    const controller = getReadableStreamController(stream);
    if (controller === undefined) return undefined;
    return getControllerExpectedLength(controller);
  }

  cancel(reason?: unknown): Promise<void> {
    try {
      return cancelReadableStreamGenericReader(this, reason);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  // Drains all currently buffered chunks (up to the soft limit maxSize, in
  // strategy size units — bytes for byte streams). Always makes progress:
  // waits for at least one chunk when nothing is buffered.
  async read(
    options: { maxSize?: number } = {}
  ): Promise<DrainingReadResult<R>> {
    const stream = getReaderStream<R>(this);
    if (stream === undefined) {
      throw new TypeError('This reader has been released');
    }
    let maxSize: number | undefined;
    if (options.maxSize !== undefined) {
      maxSize = +options.maxSize;
      if (NumberIsNaN(maxSize) || maxSize <= 0) {
        throw new TypeError('options.maxSize must be a positive number');
      }
    }
    return drainingReaderReadInternal<R>(this, stream, maxSize);
  }

  releaseLock(): void {
    if (!isReaderBoundToStream(this)) return;
    readableStreamReaderGenericRelease(this);
  }
}

// The pipe (spec ReadableStreamPipeTo). Internal operations only on both
// ends — locks are held for the duration. Backpressure is honored by
// awaiting the writer's ready promise before every read; individual writes
// are intentionally not awaited (the close path queues behind them, and
// write failures surface through the destination's closed promise).
//
// The shutdown actions (abort/cancel/close) are started immediately rather
// than after explicitly waiting for in-flight sink operations: the
// writable state machine already serializes abort/close against in-flight
// writes via its pendingAbortRequest / close-marker machinery.
function pipeToInternal<R>(
  source: ReadableStream<R>,
  destination: WritableStreamType<R>,
  options: StreamPipeOptions = {}
): Promise<void> {
  // Spec-mandated read order (§4.9.1): preventAbort, preventCancel,
  // preventClose, signal. WPT piping/throwing-options.any.js verifies
  // that getter side-effects occur in exactly this sequence.
  const preventAbort = !!options.preventAbort;
  const preventCancel = !!options.preventCancel;
  const preventClose = !!options.preventClose;
  const signal = options.signal;
  if (signal !== undefined) {
    // Brand check: the captured getter throws for non-AbortSignal objects.
    try {
      AbortSignalAbortedGet(signal);
    } catch {
      throw new TypeError('options.signal must be an AbortSignal');
    }
  }

  // Lock both ends. The pipe uses the draining reader: every pump
  // iteration moves ALL buffered chunks to the destination in one step
  // (per-chunk promise overhead only when the source is empty).
  const reader = new ReadableStreamDrainingReader<R>(source);
  const writer = writableInternals.acquireWriter(destination);
  setReadableStreamDisturbed(source);

  const { promise, resolve, reject } =
    PromiseWithResolvers() as PromiseWithResolversType<void>;

  let shuttingDown = false;
  let abortAlgorithm: (() => void) | undefined;

  const finalize = (error?: { reason: unknown }): void => {
    writableInternals.writerRelease(writer);
    readableStreamReaderGenericRelease(reader);
    if (signal !== undefined && abortAlgorithm !== undefined) {
      EventTargetRemoveEventListener(signal, 'abort', abortAlgorithm);
    }
    if (error !== undefined) {
      reject(error.reason);
    } else {
      resolve();
    }
  };

  // Shared "last write" promise: the pump updates this before each batch
  // of writes so that shutdownWithAction can wait for them to be acknowledged.
  let lastWriteSettled: Promise<void> = PromiseResolve(
    undefined
  ) as Promise<void>;

  const shutdownWithAction = (
    action: (() => Promise<unknown>) | undefined,
    error?: { reason: unknown }
  ): void => {
    if (shuttingDown) return;
    shuttingDown = true;

    const runAction = (): void => {
      if (action === undefined) {
        finalize(error);
        return;
      }
      PromisePrototypeThen(
        action(),
        () => finalize(error),
        (actionError: unknown) => finalize({ reason: actionError })
      );
    };

    // Spec: "If dest.[[state]] is 'writable' and
    // WritableStreamCloseQueuedOrInFlight(dest) is false, wait until
    // every chunk that has been written has been acknowledged."
    const destState = writableInternals.getState(destination);
    if (
      destState === 'writable' &&
      !writableInternals.closeQueuedOrInFlight(destination)
    ) {
      PromisePrototypeThen(lastWriteSettled, runAction, runAction);
    } else {
      runAction();
    }
  };

  const onSourceErrored = (e: unknown): void => {
    shutdownWithAction(
      preventAbort
        ? undefined
        : () => writableInternals.writableStreamAbort(destination, e),
      { reason: e }
    );
  };

  const onDestErrored = (e: unknown): void => {
    shutdownWithAction(
      preventCancel ? undefined : () => readableStreamCancel(source, e),
      { reason: e }
    );
  };

  const onDestClosedEarly = (): void => {
    const e = new TypeError('Destination closed before the pipe completed');
    shutdownWithAction(
      preventCancel ? undefined : () => readableStreamCancel(source, e),
      { reason: e }
    );
  };

  const onSourceDone = (): void => {
    shutdownWithAction(
      preventClose
        ? undefined
        : () => writableInternals.writerCloseWithErrorPropagation(writer)
    );
  };

  if (signal !== undefined) {
    abortAlgorithm = () => {
      const abortReason = AbortSignalReasonGet(signal);
      const actions: (() => Promise<unknown>)[] = [];
      if (!preventAbort) {
        ArrayPrototypePush(actions, () =>
          // Spec step 14.1.3: only abort if dest is still writable.
          writableInternals.getState(destination) === 'writable'
            ? writableInternals.writableStreamAbort(destination, abortReason)
            : (PromiseResolve(undefined) as Promise<void>)
        );
      }
      if (!preventCancel) {
        ArrayPrototypePush(actions, () =>
          // Spec step 14.1.4: only cancel if source is still readable.
          getReadableStreamGetState(source) === 'readable'
            ? readableStreamCancel(source, abortReason)
            : (PromiseResolve(undefined) as Promise<void>)
        );
      }
      shutdownWithAction(
        actions.length === 0
          ? undefined
          : () => {
              // Settle when all actions settle; reject with the first
              // failure (spec: shutdown actions run in parallel).
              let remaining = actions.length;
              let failed: { reason: unknown } | undefined;
              const all =
                PromiseWithResolvers() as PromiseWithResolversType<void>;
              for (let i = 0; i < actions.length; i++) {
                const action = actions[i] as () => Promise<unknown>;
                PromisePrototypeThen(
                  action(),
                  () => {
                    if (--remaining === 0) {
                      if (failed !== undefined) {
                        all.reject(failed.reason);
                      } else {
                        all.resolve();
                      }
                    }
                  },
                  (e: unknown) => {
                    failed ??= { reason: e };
                    if (--remaining === 0) all.reject(failed.reason);
                  }
                );
              }
              return all.promise;
            },
        { reason: abortReason }
      );
    };
    if (AbortSignalAbortedGet(signal)) {
      abortAlgorithm();
    } else {
      EventTargetAddEventListener(signal, 'abort', abortAlgorithm);
    }
  }

  // ---- Synchronous pre-checks (spec conditions 1-4) ----
  // The spec's "in parallel" conditions must be "applied in order":
  //   1. Source errored  →  abort dest
  //   2. Dest errored   →  cancel source
  //   3. Source closed   →  close dest
  //   4. Dest close-queued/closed  →  TypeError + cancel source
  // When streams are already in terminal states at pipe start, we must
  // honour this priority synchronously — the async pump loop and
  // microtask-scheduled reactive handlers cannot guarantee spec ordering.
  if (!shuttingDown) {
    const srcState = getReadableStreamGetState(source);
    if (srcState === 'errored') {
      // Condition 1: source already errored → abort dest with source error
      onSourceErrored(getReadableStreamStoredError(source));
    } else {
      const destState = writableInternals.getState(destination);
      if (destState === 'errored') {
        // Condition 2: dest already errored → cancel source with dest error
        onDestErrored(writableInternals.getStoredError(destination));
      } else if (srcState === 'closed') {
        // Condition 3: source already closed → close dest
        onSourceDone();
      } else if (
        writableInternals.closeQueuedOrInFlight(destination) ||
        destState === 'closed'
      ) {
        // Condition 4: dest already closing/closed → TypeError
        onDestClosedEarly();
      }
    }
  }

  // ---- Reactive shutdown triggers (spec: "in parallel") ----
  // The pump loop must not block indefinitely on backpressure or reads
  // when the other end closes, errors, or the signal fires. We use a
  // shared "poke" promise that is resolved whenever the pump should
  // re-evaluate its state (source close/error, dest error).
  let pokeResolve: (() => void) | undefined;
  let pokePromise: Promise<unknown> | undefined;

  const resetPoke = (): void => {
    const pwr = PromiseWithResolvers() as PromiseWithResolversType<void>;
    pokePromise = pwr.promise;
    pokeResolve = pwr.resolve;
  };
  resetPoke();

  const poke = (): void => {
    const r = pokeResolve;
    if (r !== undefined) {
      pokeResolve = undefined;
      r();
    }
  };

  // Forward close/error propagation from the source.
  PromisePrototypeThen(
    getGenericReaderClosedPromise(reader),
    () => {
      if (!shuttingDown) poke(); // source closed — wake the pump
    },
    (e: unknown) => {
      if (!shuttingDown) {
        onSourceErrored(e);
        poke();
      }
    }
  );

  // Backward error propagation: destination errors cancel the source.
  PromisePrototypeThen(
    writableInternals.getWriterClosedPromise(writer),
    undefined,
    (e: unknown) => {
      if (!shuttingDown) {
        onDestErrored(e);
        poke();
      }
    }
  );

  const pump = async (): Promise<void> => {
    while (!shuttingDown) {
      // Wait for backpressure to drop, but also react to shutdown triggers.
      // SafePromise.race avoids prototype-then interception.
      try {
        await SafePromiseRace([
          writableInternals.getWriterReadyPromise(writer),
          pokePromise,
        ]);
      } catch (e) {
        if (!shuttingDown) onDestErrored(e);
        return;
      }
      if (shuttingDown) return;
      resetPoke(); // arm the next poke for the next iteration

      // Spec conditions must be checked in priority order (1-4):
      // source error > dest error > source close > dest close-early.
      const srcState = getReadableStreamGetState(source);
      if (srcState === 'errored') {
        // Condition 1: source errored → abort dest
        if (!shuttingDown)
          onSourceErrored(getReadableStreamStoredError(source));
        return;
      }
      const destState = writableInternals.getState(destination);
      if (destState === 'errored') {
        // Condition 2: dest errored → cancel source
        if (!shuttingDown)
          onDestErrored(writableInternals.getStoredError(destination));
        return;
      }
      if (srcState === 'closed') {
        // Condition 3: source closed → close dest
        onSourceDone();
        return;
      }
      if (
        writableInternals.closeQueuedOrInFlight(destination) ||
        destState === 'closed'
      ) {
        // Condition 4: dest closing/closed → TypeError
        onDestClosedEarly();
        return;
      }

      let result: DrainingReadResult<R>;
      try {
        result = await drainingReaderReadInternal<R>(reader, source);
      } catch (e) {
        if (!shuttingDown) onSourceErrored(e);
        return;
      }
      if (shuttingDown) return;
      resetPoke();

      // Move the whole batch; the destination FIFO preserves order and the
      // close marker (if done) queues behind these writes.
      const chunks = result.chunks;
      for (let i = 0; i < chunks.length; i++) {
        const writePromise = writableInternals.writerWrite(
          writer,
          chunks[i] as R
        );
        markPromiseHandled(writePromise);
        // Track the last write so shutdownWithAction can wait for it.
        // The writable serializes writes, so when this settles all
        // preceding writes have already settled.
        lastWriteSettled = PromisePrototypeThen(
          writePromise,
          () => {},
          () => {}
        ) as Promise<void>;
      }
      if (result.done) {
        onSourceDone();
        return;
      }
    }
  };
  markPromiseHandled(pump());

  return promise;
}

let assertIsReadableStream: <W>(self: ReadableStream<W>) => void;

class ReadableStream<R> {
  #controller?:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
    | undefined;
  #reader?:
    | ReadableStreamDefaultReaderType<R>
    | ReadableStreamBYOBReaderType
    | undefined;
  // The stream's own CONSUMER — the backend fence (see queue.ts and
  // native-stream-integration.md §10). A QueueCursor/ByteStreamCursor for
  // queued (JS-backed) streams; a NativePullConduit for native streams.
  // The stream owns it (readers only borrow it while locked); created
  // during controller setup, removed on cancel/tee.
  #consumer?: StreamConsumerType<R> | undefined;
  // Tee branches get a composite-cancel hook (spec ReadableStreamTee): the
  // source is cancelled with [reason1, reason2] only when ALL branches have
  // cancelled, and every branch's cancel() promise settles together at that
  // point. undefined for non-branch streams.
  #onCancel?: ((reason: unknown) => Promise<void>) | undefined;
  // Tee-branch lifecycle: called when this branch's stream closes or errors
  // so the shared cancel-promise can settle if any sibling was already
  // cancelled. Mirrors the spec's "resolve cancelPromise" in the done/error
  // paths of ReadableStreamDefaultTee's pull loop.
  #onBranchSettled?: (() => void) | undefined;
  #disturbed: boolean = false;
  #state: 'readable' | 'closed' | 'errored' = 'readable';
  #storedError?: unknown;
  // TODO(streams-ts): The sockets API needs to be able to tell its Readable
  // not to accept reads because it is pending closure. We don't yet fully
  // implement this but we provide for the signal.
  // @ts-expect-error
  #pendingClosure: boolean = false;

  static {
    isReadableStream = (value: unknown) => {
      return isActualObject(value) && #state in value;
    };

    assertIsReadableStream = function <W>(self: ReadableStream<W>): void {
      if (!isReadableStream(self)) throw new TypeError('Illegal invocation');
    };

    setReadableStreamPendingClosure = <R>(stream: ReadableStream<R>) => {
      stream.#pendingClosure = true;
    };

    getReadableStreamOnEof = <R>(_stream: ReadableStream<R>) => {
      // TODO(streams-ts): implement this for the sockets API. For now, just return a non-resolving promise
      const { promise } =
        PromiseWithResolvers() as PromiseWithResolversType<void>;
      return promise;
    };

    getReadableStreamExpectedLength = <R>(stream: ReadableStream<R>) => {
      const controller = stream.#controller;
      if (controller === undefined) return undefined;
      return getControllerExpectedLength(controller);
    };

    isReadableStreamLocked = <R>(stream: ReadableStream<R>) => {
      // The spec definition (a reader is attached) works unchanged for this
      // implementation: tee() keeps the parent locked permanently via a
      // real, never-exposed internal reader, and the pipe/draining paths
      // hold ordinary reader locks.
      return stream.#reader !== undefined;
    };

    isReadableStreamUnusable = <R>(stream: ReadableStream<R>) => {
      assertIsReadableStream(stream);
      return stream.#disturbed || isReadableStreamLocked(stream);
    };

    readableStreamCancel = <R>(stream: ReadableStream<R>, reason?: unknown) => {
      stream.#disturbed = true;
      const state = stream.#state;
      if (state === 'closed') {
        return PromiseResolve() as Promise<void>;
      }
      if (state === 'errored') {
        return PromiseReject(stream.#storedError) as Promise<void>;
      }
      // Close FIRST (spec ReadableStreamCancel): transitions state and
      // resolves the attached reader's closedPromise.
      readableStreamClose(stream);
      const consumer = stream.#consumer;
      const onCancel = stream.#onCancel;
      const controller = stream.#controller;
      let cancelPromise: Promise<void> = PromiseResolve() as Promise<void>;
      // BACKEND-BLIND: the STREAM layer owns the source-cancel POLICY
      // (tee composite hook vs direct controller cancel) and hands it to
      // the consumer, which owns the teardown MECHANICS (resolve reads as
      // done, ordering vs removal, last-consumer determination). See
      // StreamConsumer.cancelStream in queue.ts.
      const decideSourceCancel = (isLastConsumer: boolean): Promise<void> => {
        if (onCancel !== undefined) {
          // Tee branch: the composite-cancel hook collects this branch's
          // vote+reason; the source is cancelled only once every sibling
          // has cancelled (spec ReadableStreamTee). The returned promise
          // stays PENDING until then.
          return onCancel(reason);
        }
        if (isLastConsumer && controller !== undefined) {
          // Only the LAST consumer's cancel reaches the underlying source.
          return controllerCancelSteps(controller, reason);
        }
        return PromiseResolve() as Promise<void>;
      };
      if (consumer !== undefined) {
        cancelPromise = consumer.cancelStream(reason, decideSourceCancel);
        stream.#consumer = undefined;
      } else if (onCancel !== undefined) {
        cancelPromise = onCancel(reason);
      }
      // Per spec the returned promise fulfills with undefined.
      return PromisePrototypeThen(
        cancelPromise,
        () => undefined
      ) as Promise<void>;
    };

    acquireReadableStreamDefaultReader = <R>(stream: ReadableStream<R>) => {
      return new ReadableStreamDefaultReader<R>(stream);
    };

    acquireReadableStreamBYOBReader = <R>(stream: ReadableStream<R>) => {
      return new ReadableStreamBYOBReader(
        stream as unknown as ReadableStream<ArrayBufferView>
      );
    };

    readableStreamPipeThroughTo = <R>(
      source: ReadableStream<R>,
      destination: WritableStreamType<R>,
      options?: StreamPipeOptions
    ) => {
      return pipeToInternal(source, destination, options);
    };

    // BACKEND-DISPATCH: tee is one of the five sanctioned dispatch points
    // (native-stream-integration.md §10). The native branch runs first:
    // the source's tee hook produces a PAIR of new native source objects
    // (leaving the original source closed), each wrapped in a fresh
    // ReadableStream via ordinary construction. Branches are fully
    // independent — no shared consumer and no composite-cancel wiring
    // (deliberate divergence from the queued model below: branch cancels
    // go to each branch's OWN source). The parent becomes the same inert
    // locked shell as in the queued model.
    readableStreamTee = <R>(stream: ReadableStream<R>) => {
      // The locked precondition lives HERE (not in the prototype method) so that every
      // entry point shares it -- the method after its brand assert, and the C++
      // JsReadableStream::tee arm via cppExports, which calls this directly.
      if (isReadableStreamLocked(stream)) {
        throw new TypeError('Cannot tee a stream that is locked');
      }
      const controller = stream.#controller;
      if (isNativeController(controller)) {
        if (stream.#state !== 'readable') {
          // Closed/errored native parents produce two branches in the
          // same state, mirroring the queued behavior below — without
          // touching the (closed) source.
          const b1 = new ReadableStream<R>(kPrivateSymbol as never);
          const b2 = new ReadableStream<R>(kPrivateSymbol as never);
          b1.#state = stream.#state;
          b2.#state = stream.#state;
          b1.#storedError = stream.#storedError;
          b2.#storedError = stream.#storedError;
          return [b1, b2] as [ReadableStream<R>, ReadableStream<R>];
        }
        // Sources from the tee hook are full native sources; ordinary
        // construction validates and wires each branch.
        const [source1, source2] = nativeControllerTeeSource(controller);
        const branch1 = new ReadableStream<R>(source1 as UnderlyingSource<R>);
        const branch2 = new ReadableStream<R>(source2 as UnderlyingSource<R>);
        stream.#consumer = undefined;
        if (!isReadableStreamLocked(stream)) {
          acquireReadableStreamDefaultReader(stream);
        }
        return [branch1, branch2] as [ReadableStream<R>, ReadableStream<R>];
      }
      const state = stream.#state;

      // Branch streams are shells wired to the SHARED controller — same
      // queue, per-branch cursors.
      const branch1 = new ReadableStream<R>(kPrivateSymbol as never);
      const branch2 = new ReadableStream<R>(kPrivateSymbol as never);
      branch1.#controller = controller;
      branch2.#controller = controller;

      // QUEUED INVARIANT: this branch is queued-backend territory — the
      // consumer is necessarily a QueueCursor (position/byteOffset/queue
      // are cursor-only concepts); sanctioned cast.
      const cursor = stream.#consumer as QueueCursorType<R, R> | undefined;

      if (state === 'errored') {
        branch1.#state = 'errored';
        branch2.#state = 'errored';
        branch1.#storedError = stream.#storedError;
        branch2.#storedError = stream.#storedError;
      } else if (state === 'closed') {
        branch1.#state = 'closed';
        branch2.#state = 'closed';
      } else if (cursor !== undefined) {
        const queue = cursor.queue;
        const isBytes =
          controller !== undefined && isByteStreamController(controller);
        // Each branch forks at the original cursor's position AND
        // byteOffset — partial entry consumption survives the fork. Byte
        // streams need ByteStreamCursor (BYOB reads on branches).
        //
        // ORDER MATTERS: add the branch cursors BEFORE removing the
        // original — removing the sole cursor first would fire the
        // all-cursors-gone hook and cancel the underlying source mid-tee.
        const totalSize = cursor.remainingSize;
        branch1.#consumer = isBytes
          ? new ByteStreamCursor(
              queue,
              branch1,
              cursor.position,
              cursor.byteOffset,
              totalSize
            )
          : new QueueCursor(
              queue,
              branch1,
              cursor.position,
              cursor.byteOffset,
              totalSize
            );
        branch2.#consumer = isBytes
          ? new ByteStreamCursor(
              queue,
              branch2,
              cursor.position,
              cursor.byteOffset,
              totalSize
            )
          : new QueueCursor(
              queue,
              branch2,
              cursor.position,
              cursor.byteOffset,
              totalSize
            );
        queue.removeCursor(cursor);
        stream.#consumer = undefined;
      }

      // Composite cancel (spec ReadableStreamTee): each branch's cancel()
      // records its reason and returns a SHARED promise that settles only
      // once both branches have cancelled — at which point the source is
      // cancelled with [reason1, reason2]. Re-tees relay through the
      // parent branch's own hook so nesting composes.
      const parentOnCancel = stream.#onCancel;
      let canceled1 = false;
      let canceled2 = false;
      let canceling1 = false;
      let canceling2 = false;
      let reason1: unknown;
      let reason2: unknown;
      const cancelHolder =
        PromiseWithResolvers() as PromiseWithResolversType<void>;
      const performSourceCancel = (composite: unknown): Promise<void> => {
        if (parentOnCancel !== undefined) {
          // Re-tee: relay through the parent branch's hook. The composite
          // is branded, so the parent-level composition FLATTENS it — the
          // final AggregateError carries every leaf reason at one level.
          return parentOnCancel(composite);
        }
        if (controller === undefined) {
          return PromiseResolve() as Promise<void>;
        }
        return controllerCancelSteps(controller, composite);
      };
      const makeBranchCancel = (which: 1 | 2) => {
        return (reason: unknown): Promise<void> => {
          if (which === 1) {
            canceling1 = true;
            canceled1 = true;
            reason1 = reason;
          } else {
            canceling2 = true;
            canceled2 = true;
            reason2 = reason;
          }
          if (canceled1 && canceled2) {
            // Resolving with the source-cancel promise adopts it: both
            // branches' pending cancel() promises settle with its outcome.
            cancelHolder.resolve(
              performSourceCancel(makeCompositeCancelReason(reason1, reason2))
            );
          }
          // Clear the canceling guard after the synchronous cancel path
          // completes. The guard prevents onBranchSettled from resolving
          // cancelHolder during the cancel itself (readableStreamCancel
          // calls readableStreamClose before onCancel). After this point,
          // if the sibling branch closes/errors from the source, the
          // cancelHolder should resolve (spec step 14.b.v / 14.c.ii).
          if (which === 1) {
            canceling1 = false;
          } else {
            canceling2 = false;
          }
          return cancelHolder.promise;
        };
      };
      branch1.#onCancel = makeBranchCancel(1);
      branch2.#onCancel = makeBranchCancel(2);

      // Spec ReadableStreamDefaultTee: when the underlying reader signals
      // done (or error), the cancel promise is resolved if either branch
      // was already cancelled (steps 14.b.v / 14.c.ii). In our shared-
      // queue model the equivalent is: a branch stream transitions to
      // closed/errored via readableStreamClose/readableStreamError while
      // the other branch was already cancelled.
      //
      // Guard: only resolve when the SOURCE closes/errors a branch whose
      // sibling was already cancelled. Do NOT resolve when a branch is
      // being cancelled itself — readableStreamCancel calls
      // readableStreamClose before onCancel runs, and onCancel handles
      // cancelHolder via performSourceCancel.
      const onBranchSettled = (): void => {
        if ((canceled1 && !canceling1) || (canceled2 && !canceling2)) {
          cancelHolder.resolve();
        }
      };
      setOnBranchSettled(branch1, onBranchSettled);
      setOnBranchSettled(branch2, onBranchSettled);

      // Per spec, tee() locks the original permanently. Acquire a real
      // reader (never exposed, so it can never be released) — the original
      // becomes an inert locked shell that retains only enough state to
      // report locked === true.
      if (!isReadableStreamLocked(stream)) {
        acquireReadableStreamDefaultReader(stream);
      }

      return [branch1, branch2] as [ReadableStream<R>, ReadableStream<R>];
    };

    getReadableStreamGetState = <R>(stream: ReadableStream<R>) => {
      return stream.#state;
    };

    getReadableStreamIsDisturbed = <R>(stream: ReadableStream<R>) => {
      return stream.#disturbed;
    };

    getReadableStreamStoredError = <R>(stream: ReadableStream<R>) => {
      return stream.#storedError;
    };

    setReadableStreamState = <R>(
      stream: ReadableStream<R>,
      state: 'readable' | 'closed' | 'errored'
    ) => {
      stream.#state = state;
    };

    setReadableStreamDisturbed = <R>(stream: ReadableStream<R>) => {
      stream.#disturbed = true;
    };

    setReadableStreamStoredError = <R>(
      stream: ReadableStream<R>,
      error: unknown
    ) => {
      stream.#storedError = error;
    };

    setReadableStreamReader = <R>(
      stream: ReadableStream<R>,
      reader: ReadableStreamReaderType<R> | undefined
    ) => {
      stream.#reader = reader;
    };

    getReadableStreamController = <R>(stream: ReadableStream<R>) => {
      return stream.#controller;
    };

    getReadableStreamReader = <R>(stream: ReadableStream<R>) => {
      return stream.#reader;
    };

    getReadableStreamConsumer = <R>(stream: ReadableStream<R>) => {
      return stream.#consumer;
    };

    setReadableStreamConsumer = <R>(
      stream: ReadableStream<R>,
      consumer: StreamConsumerType<R> | undefined
    ) => {
      stream.#consumer = consumer;
    };

    notifyBranchSettled = <R>(stream: ReadableStream<R>) => {
      const cb = stream.#onBranchSettled;
      if (cb !== undefined) cb();
    };

    setOnBranchSettled = <R>(
      stream: ReadableStream<R>,
      cb: (() => void) | undefined
    ) => {
      stream.#onBranchSettled = cb;
    };

    // BACKEND-DISPATCH: the native side of the chained-controller-helpers
    // dispatch point (whose canonical marker sits at the default
    // controller's static block). The queued controllers wrap the chain
    // inside their own static blocks because their brands are private
    // names; the native controller's brand lives across the module fence
    // in native.ts, so its predicate and behaviors arrive as exported
    // internals and the wrap happens here instead. This static block is
    // the right host: it runs after both queued controller classes (file
    // order — their static blocks assigned the implementations being
    // wrapped) and before any stream can exist, and a static block (unlike
    // straight-line module code) reads the chain lets without tripping
    // definite-assignment analysis.
    const prevPullIfNeeded = controllerPullIfNeeded;
    controllerPullIfNeeded = (controller) => {
      if (isNativeController(controller)) {
        nativeControllerPullIfNeeded(controller);
      } else {
        prevPullIfNeeded(controller);
      }
    };

    const prevCancelSteps = controllerCancelSteps;
    controllerCancelSteps = (controller, reason) => {
      if (isNativeController(controller)) {
        return nativeControllerCancelSteps(controller, reason);
      }
      return prevCancelSteps(controller, reason);
    };

    const prevMaybeCloseStream = controllerMaybeCloseStream;
    controllerMaybeCloseStream = (controller) => {
      if (isNativeController(controller)) {
        nativeControllerMaybeCloseStream(controller);
      } else {
        prevMaybeCloseStream(controller);
      }
    };

    const prevOnReaderRelease = controllerOnReaderRelease;
    controllerOnReaderRelease = (controller) => {
      if (isNativeController(controller)) {
        nativeControllerOnReaderRelease(controller);
      } else {
        prevOnReaderRelease(controller);
      }
    };

    const prevGetExpectedLength = getControllerExpectedLength;
    getControllerExpectedLength = (controller) => {
      if (isNativeController(controller)) {
        return nativeControllerExpectedLength(controller);
      }
      return prevGetExpectedLength(controller);
    };

    // BACKEND-DISPATCH point #4: JS-to-C++ extraction
    // (prepareReadableStreamForCpp in the design doc). A single shared
    // function installed as the value of kExtractNativeSource on every
    // native-backed stream. The TypeWrapper detects the property's
    // presence (own-property get, no JS code execution) and calls it
    // to extract the native underlying source; absent means "queued,
    // use DrainingReader". One-shot: subsequent calls throw.
    extractNativeSource = function <R>(this: ReadableStream<R>): object {
      assertIsReadableStream(this);
      if (isReadableStreamLocked(this)) {
        throw new TypeError(
          'Cannot extract a native source from a locked stream'
        );
      }
      if (this.#disturbed) {
        throw new TypeError(
          'Cannot extract a native source from a disturbed stream'
        );
      }
      const controller = this.#controller;
      if (!isNativeController(controller)) {
        throw new TypeError('This stream is not backed by a native source');
      }
      // Atomic: validate -> extract -> lock+disturb. No TOCTOU gap.
      const source = nativeControllerExtractSource(controller);
      this.#consumer = undefined;
      this.#disturbed = true;
      // Permanent lock via an internal reader (never exposed, never
      // released) — same pattern as tee's parent locking.
      acquireReadableStreamDefaultReader(this);
      return source;
    };
  }

  constructor(
    underlyingSource: UnderlyingSource<R> = {},
    strategy: QueuingStrategy<R> = {}
  ) {
    // Internal shell creation (tee branches): skip controller setup
    // entirely — the tee wiring attaches the SHARED controller and a
    // forked cursor afterwards. The private symbol is unreachable from
    // user code.
    if ((underlyingSource as unknown) === kPrivateSymbol) {
      return;
    }

    // BACKEND-DISPATCH: stream construction (one of the five sanctioned
    // dispatch points): native vs queued byte vs queued default.
    //
    // The native check runs FIRST: native sources are forbidden from
    // declaring `type` (enforced during native extraction), so a
    // native-marked source must never fall through to the queued
    // branches. The strategy argument is PERMANENTLY ignored on the
    // native branch: native pacing is purely demand-driven (the source
    // contractually ignores desiredSize).
    if (isNativeUnderlyingSource(underlyingSource)) {
      const { controller, conduit } = createNativeReadableStreamParts(
        underlyingSource,
        {
          // Stream-level transitions for the far side of the module
          // fence. Both helpers are state-guarded, so redundant calls
          // (e.g. the reader layer's done-result close racing the
          // conduit's hook) are harmless.
          closeStream: () => readableStreamClose(this),
          errorStream: (reason: unknown) => readableStreamError(this, reason),
        }
      );
      this.#controller = controller;
      // Sanctioned cast: the conduit is byte-oriented (Uint8Array), but
      // the stream's R is caller-chosen; same shape as the queued cursor
      // attachments in the controller constructors.
      this.#consumer = conduit as unknown as StreamConsumerType<R>;
      // BACKEND-DISPATCH point #4: install the JS-to-C++ extraction
      // marker. Own, non-enumerable, non-writable, non-configurable.
      // The value is the shared extractor function (this-bound at call
      // time). Bootstrap phase: a regular symbol; final: private API
      // symbol (invisible to JS entirely).
      ObjectDefineProperty(this, kExtractNativeSource, {
        __proto__: null,
        value: extractNativeSource,
      } as PropertyDescriptor);
      return;
    }

    // --- WebIDL strategy dictionary conversion (BEFORE source reads) ---
    // Per WebIDL, dictionary-typed arguments are converted (property reads
    // happen) at the IDL layer before the constructor body runs. The
    // `strategy` parameter is QueuingStrategy (a dictionary); the
    // `underlyingSource` parameter is plain `object` (no dictionary
    // conversion). We simulate this by reading strategy properties first.
    const sizeFn = strategy.size;
    let sizeAlgorithm: (chunk: R) => number;
    if (sizeFn === undefined) {
      sizeAlgorithm = () => 1;
    } else if (typeof sizeFn !== 'function') {
      throw new TypeError('strategy.size must be a function');
    } else {
      const callSize = uncurryThis(sizeFn);
      sizeAlgorithm = (chunk: R) => callSize(undefined, chunk);
    }
    const rawHWM = strategy.highWaterMark;

    // WebIDL: the 'type' member is a string enum — perform ToString() on
    // non-undefined values so that objects with toString()/valueOf() work
    // (per spec: "Let type be ? Get(underlyingSourceDict, "type").
    //  If type is not undefined, set type to ? ToString(type).")
    const rawType = underlyingSource.type;
    const type = rawType === undefined ? undefined : `${rawType}`;

    if (type === 'bytes') {
      // Byte streams: size() is forbidden, highWaterMark defaults to 0.
      if (sizeFn !== undefined) {
        throw new RangeError(
          'The strategy for a byte stream cannot have a size function'
        );
      }
      let highWaterMark = 0;
      if (rawHWM !== undefined) {
        highWaterMark = +rawHWM;
        if (NumberIsNaN(highWaterMark) || highWaterMark < 0) {
          throw new RangeError('Invalid highWaterMark');
        }
      }
      this.#controller = setupReadableByteStreamControllerFromUnderlyingSource(
        this,
        underlyingSource,
        highWaterMark
      );
    } else {
      // The spec says, "Assert: underlyingSourceDict["type"] does not exist"
      // but we're not going to be that strict about it. We'll assert only
      // that its value is `undefined`.
      if (type !== undefined) {
        throw new TypeError(`Invalid underlying source type: ${type}`);
      }
      // Default streams: highWaterMark defaults to 1.
      let highWaterMark = 1;
      if (rawHWM !== undefined) {
        highWaterMark = +rawHWM;
        if (NumberIsNaN(highWaterMark) || highWaterMark < 0) {
          throw new RangeError('Invalid highWaterMark');
        }
      }
      this.#controller =
        setupReadableStreamDefaultControllerFromUnderlyingSource(
          this,
          underlyingSource,
          sizeAlgorithm,
          highWaterMark
        );
    }
  }

  get locked(): boolean {
    assertIsReadableStream(this);
    return isReadableStreamLocked(this);
  }

  cancel(reason: unknown = undefined): Promise<void> {
    try {
      assertIsReadableStream(this);
      if (isReadableStreamLocked(this)) {
        throw new TypeError('Cannot cancel a stream that is locked');
      }
      return readableStreamCancel(this, reason) as Promise<void>;
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  getReader(
    options: { mode?: 'byob' } | null = {}
  ): ReadableStreamReaderType<R> {
    assertIsReadableStream(this);
    // WebIDL dictionary conversion: null and undefined become {},
    // objects have their properties read, primitives are rejected.
    if (options != null && !isActualObject(options)) {
      throw new TypeError('Reader options must be an object');
    }

    // WebIDL: the 'mode' member is a string enum — perform ToString() on
    // non-undefined values so that objects with toString()/valueOf() work.
    const rawMode = isActualObject(options) ? options.mode : undefined;
    const mode = rawMode === undefined ? undefined : `${rawMode}`;
    if (mode === undefined) {
      return acquireReadableStreamDefaultReader(this);
    }
    if (mode !== 'byob') {
      throw new TypeError(`Invalid reader mode: ${mode}`);
    }
    return acquireReadableStreamBYOBReader(this);
  }

  pipeThrough<T>(
    transform: TransformStreamType<R, T>,
    // WebIDL: optional dictionary — null/undefined both become {}.
    // Default = {} preserves Function.length = 1 (IDL harness check).
    options: StreamPipeOptions = {}
  ): ReadableStreamType<T> {
    assertIsReadableStream(this);
    if (isReadableStreamLocked(this)) {
      throw new TypeError('Cannot pipe a stream that is locked');
    }
    // WebIDL dictionary conversion: alphabetical member order means
    // "readable" is read (and brand-checked) BEFORE "writable".  The
    // WPT pipe-through.any.js tests verify that a bad readable stops
    // the writable getter from ever being called.
    const readable = transform.readable;
    if (!isReadableStream(readable)) {
      throw new TypeError(
        "Failed to execute 'pipeThrough': readable is not a ReadableStream"
      );
    }
    const writable = transform.writable;
    if (writable.locked) {
      throw new TypeError('Cannot pipe to a locked writable stream');
    }
    // WebIDL: null coerces to {} for optional dictionaries.
    if (options === null) options = {} as StreamPipeOptions;
    if (!isActualObject(options)) {
      throw new TypeError('Pipe options must be an object');
    }
    const promise = readableStreamPipeThroughTo(this, writable, options);
    markPromiseHandled(promise);
    return readable;
  }

  pipeTo(
    destination: WritableStream<R>,
    // WebIDL: optional dictionary — null/undefined both become {}.
    // Default = {} preserves Function.length = 1 (IDL harness check).
    options: StreamPipeOptions = {}
  ): Promise<void> {
    try {
      assertIsReadableStream(this);
      if (isReadableStreamLocked(this)) {
        throw new TypeError('Cannot pipe a stream that is locked');
      }
      if (destination.locked) {
        throw new TypeError('Cannot pipe to a locked writable stream');
      }
      // WebIDL: null coerces to {} for optional dictionaries.
      if (options === null) options = {} as StreamPipeOptions;
      if (!isActualObject(options)) {
        throw new TypeError('Pipe options must be an object');
      }
      // PIPE DISPATCH: if both source and dest are native-backed, take
      // the fast path — extract both and let the sink's pipeFrom hook
      // arrange the pipe entirely at the C++ layer. Both markers are
      // own-property reads (non-destructive); extraction happens only
      // after both are confirmed. If both are native, pipeFrom MUST
      // exist (invariant — its absence is a contract violation, not a
      // fallback trigger).
      const sourceExtractor = ObjectGetOwnPropertyDescriptor(
        this,
        kExtractNativeSource
      )?.value as ((this: ReadableStream<R>) => object) | undefined;
      const sinkExtractor = ObjectGetOwnPropertyDescriptor(
        destination,
        kExtractNativeSink
      )?.value as ((this: object) => object) | undefined;
      if (sourceExtractor !== undefined && sinkExtractor !== undefined) {
        // Captured-call discipline: Function.prototype.call is patchable,
        // so re-bind both extractors and the hook through uncurryThis
        // (captured Reflect machinery) instead of .call().
        const nativeSource = uncurryThis(sourceExtractor)(this);
        const nativeSink = uncurryThis(sinkExtractor)(destination) as Record<
          string,
          unknown
        >;
        const pipeFrom = nativeSink.pipeFrom as
          | ((source: object, opts: StreamPipeOptions) => Promise<void>)
          | undefined;
        if (pipeFrom === undefined) {
          throw new TypeError(
            'Native sink is missing the required pipeFrom hook'
          );
        }
        return uncurryThis(pipeFrom)(nativeSink, nativeSource, options);
      }
      return readableStreamPipeThroughTo(this, destination, options);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  tee(): [ReadableStream<R>, ReadableStream<R>] {
    assertIsReadableStream(this);
    // The locked precondition is enforced by readableStreamTee itself (shared with the
    // C++ bridge entry point).
    return readableStreamTee(this);
  }

  static from<R>(
    iterable: Iterable<R> | AsyncIterable<R> | R
  ): ReadableStream<R> {
    // We are intentionally a bit more lax in what we accept here.
    // The spec says AsyncIterable. We allow Iterable as well. If
    // a String or ArrayBufferView is passed, we will treat it as
    // a single chunk, rather than an iterable of chunks.
    if (typeof iterable === 'string' || isArrayBufferView(iterable)) {
      // INTENTIONAL SPEC DIVERGENCE: The spec treats strings as
      // iterables and iterates them code-point-by-code-point. We
      // deliberately treat strings (and ArrayBufferViews) as single
      // chunks instead — iterating a string through a stream one
      // code point at a time is both surprising to users and has
      // terrible performance. This causes the WPT test
      // "ReadableStream.from accepts a string" to fail.
      // The ArrayBufferView case avoids traversing the patchable
      // %ArrayIteratorPrototype%.
      const chunk = iterable as unknown as R;
      return new ReadableStream<R>({
        pull(controller: ReadableStreamDefaultControllerType) {
          controller.enqueue(chunk);
          controller.close();
        },
      });
    }

    // It can't be an iterable if it's not an actual object.
    if (isActualObject(iterable)) {
      // Check @@asyncIterator first, but only if the value is non-null.
      // Per spec, a null/undefined @@asyncIterator is ignored and we
      // fall through to @@iterator (WPT: "from ignores a null @@asyncIterator").
      const asyncMethod =
        SymbolAsyncIterator in iterable
          ? (iterable as AsyncIterable<R>)[primordials.SymbolAsyncIterator]
          : undefined;
      if (asyncMethod != null) {
        const asyncIterator: AsyncIterator<R> =
          uncurryThis(asyncMethod)(iterable);
        if (!isActualObject(asyncIterator)) {
          throw new TypeError('The iterator method must return an object');
        }
        // HWM 0: the iterator's next() must only be called in response
        // to a consumer read(), never eagerly (WPT: "calls next() after
        // first read()").
        return new ReadableStream<R>(
          {
            async pull(controller: ReadableStreamDefaultControllerType) {
              // If the pull method throws, the stream will error.
              const next = await asyncIterator.next();
              if (!isActualObject(next)) {
                throw new TypeError('The result of next() must be an object');
              }
              if (next.done) {
                return controller.close();
              }
              controller.enqueue(next.value);
            },
            async cancel(reason?: unknown) {
              const returnMethod = asyncIterator.return;
              // Per spec, iterators without a return() method cancel
              // silently. But if return exists and is not callable,
              // cancel must reject with TypeError.
              if (returnMethod === undefined) return;
              if (typeof returnMethod !== 'function') {
                throw new TypeError('Iterator return() is not a function');
              }
              const ret = await uncurryThis(returnMethod)(
                asyncIterator,
                reason
              );
              if (!isActualObject(ret)) {
                throw new TypeError('The return method must return an object');
              }
            },
          },
          { highWaterMark: 0 }
        );
      }

      if (SymbolIterator in iterable) {
        const method = (iterable as Iterable<R>)[primordials.SymbolIterator];
        const syncIterator: Iterator<R> = uncurryThis(method)(iterable);
        if (!isActualObject(syncIterator)) {
          throw new TypeError('The iterator method must return an object');
        }
        // HWM 0: same rationale as the async path above — next() must
        // be deferred until a consumer read() arrives.
        return new ReadableStream<R>(
          {
            // The spec uses GetIterator(asyncIterable, async) which
            // wraps the sync iterator in an async-from-sync wrapper.
            // That wrapper awaits each value via PromiseResolve, so
            // e.g. an iterable of promises yields the resolved values,
            // not the Promise objects.
            async pull(controller: ReadableStreamDefaultControllerType) {
              // If the pull method throws, the stream will error.
              const next = syncIterator.next();
              if (!isActualObject(next)) {
                throw new TypeError('The result of next() must be an object');
              }
              // Await the value: the async-from-sync iterator wrapper
              // resolves each value through PromiseResolve, which
              // awaits thenables (including Promises).
              const value = await next.value;
              if (next.done) {
                controller.close();
                return;
              }
              controller.enqueue(value as R);
            },
            async cancel(reason?: unknown) {
              const returnMethod = syncIterator.return;
              if (returnMethod === undefined) return;
              if (typeof returnMethod !== 'function') {
                throw new TypeError('Iterator return() is not a function');
              }
              const ret = uncurryThis(returnMethod)(syncIterator, reason);
              if (!isActualObject(ret)) {
                throw new TypeError('The return method must return an object');
              }
            },
          },
          { highWaterMark: 0 }
        );
      }
    }

    throw new TypeError('The argument must be sync or async iterable');
  }

  values(options: { preventCancel?: boolean } = {}): AsyncIterableIterator<R> {
    assertIsReadableStream(this);
    if (!isActualObject(options)) {
      throw new TypeError('Options must be an object');
    }
    const { preventCancel } = options;

    if (isReadableStreamLocked(this)) {
      throw new TypeError('Cannot get an iterator for a stream that is locked');
    }
    const reader = acquireReadableStreamDefaultReader(this);

    const iter = ObjectCreate(ReadableStreamAsyncIteratorPrototype);
    iteratorStateMap.set(iter, {
      reader,
      preventCancel: !!preventCancel,
      state: { done: false, current: undefined },
      started: false,
    });
    return iter;
  }

  [SymbolAsyncIterator](options?: {
    preventCancel?: boolean;
  }): AsyncIterableIterator<R> {
    return this.values(options);
  }
}

const kMaximumAllowedLimit = 128n * 1024n * 1024n; // 128 MB

function bigIntMin(a: bigint, b: bigint): bigint {
  return a < b ? a : b;
}

function adjustLimit(
  limit: bigint,
  maybeExpectedLength: bigint | undefined
): bigint {
  let result = bigIntMin(kMaximumAllowedLimit, limit);
  if (maybeExpectedLength !== undefined) {
    result = bigIntMin(result, maybeExpectedLength);
  }
  return result;
}

function acquireReadableStreamDrainingReader<R>(
  stream: ReadableStream<R>
): ReadableStreamDrainingReader<R> {
  return new ReadableStreamDrainingReader<R>(stream);
}

type CollectChunksResult = {
  amountRead: bigint;
  chunks: Uint8Array[];
};

async function collectChunks<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<CollectChunksResult> {
  if (isReadableStreamUnusable(stream)) {
    throw new TypeError('Cannot consume a stream that is locked or disturbed');
  }
  const reader = acquireReadableStreamDrainingReader(stream);
  limit = adjustLimit(limit, getReadableStreamExpectedLength(stream));
  let amountRead = 0n;
  const chunks: Uint8Array[] = [];
  while (true) {
    const result = await reader.read();
    for (const chunk of result.chunks as Uint8Array[]) {
      const length = TypedArrayPrototypeGetByteLength(chunk);
      if (length === 0) continue;
      amountRead += BigInt(length);
      if (amountRead > limit) {
        throw new RangeError(
          `Stream exceeded the maximum allowed limit of ${Number(limit)} bytes`
        );
      }
      ArrayPrototypePush(chunks, chunk);
    }
    if (result.done) break;
  }

  return { amountRead, chunks };
}

async function consumeReadableStreamAsArrayBuffer<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<ArrayBuffer> {
  const { amountRead, chunks } = await collectChunks(stream, limit);

  const res = new ArrayBuffer(Number(amountRead));
  const u8 = new Uint8Array(res);
  let offset = 0;
  for (const chunk of chunks) {
    TypedArrayPrototypeSet(u8, chunk, offset);
    offset += TypedArrayPrototypeGetByteLength(chunk);
  }

  return res;
}

async function consumeReadableStreamAsUint8Array<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<Uint8Array> {
  return new Uint8Array(
    await consumeReadableStreamAsArrayBuffer(stream, limit)
  );
}

async function consumeReadableStreamAsText<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<string> {
  const { amountRead, chunks } = await collectChunks(stream, limit);

  let res = '';
  if (amountRead === 0n) return res;

  const decoder = new TextDecoder();
  for (const chunk of chunks) {
    res += TextDecoderDecode(decoder, chunk, { __proto__: null, stream: true });
  }
  res += TextDecoderDecode(decoder); // flush

  return res;
}

async function consumeReadableStreamAsJSON<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<unknown> {
  return JSONParse(await consumeReadableStreamAsText(stream, limit));
}

const kEnumerable = { __proto__: null, enumerable: true };

ObjectDefineProperties(ReadableStreamDefaultReader.prototype, {
  __proto__: null,
  closed: kEnumerable,
  cancel: kEnumerable,
  read: kEnumerable,
  releaseLock: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ReadableStreamDefaultReader',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(ReadableStreamBYOBReader.prototype, {
  __proto__: null,
  closed: kEnumerable,
  cancel: kEnumerable,
  read: kEnumerable,
  releaseLock: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ReadableStreamBYOBReader',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(ReadableStream, {
  __proto__: null,
  from: kEnumerable,
});
ObjectDefineProperties(ReadableStream.prototype, {
  __proto__: null,
  locked: kEnumerable,
  cancel: kEnumerable,
  getReader: kEnumerable,
  pipeThrough: kEnumerable,
  pipeTo: kEnumerable,
  tee: kEnumerable,
  values: kEnumerable,
  [SymbolAsyncIterator]: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ReadableStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(ReadableStreamDefaultController, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(ReadableByteStreamController, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(ReadableStreamBYOBRequest, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(ReadableStreamDefaultController.prototype, {
  __proto__: null,
  close: kEnumerable,
  enqueue: kEnumerable,
  error: kEnumerable,
  desiredSize: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ReadableStreamDefaultController',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(ReadableByteStreamController.prototype, {
  __proto__: null,
  close: kEnumerable,
  enqueue: kEnumerable,
  error: kEnumerable,
  byobRequest: kEnumerable,
  desiredSize: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ReadableByteStreamController',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(ReadableStreamBYOBRequest.prototype, {
  __proto__: null,
  respond: kEnumerable,
  respondWithNewView: kEnumerable,
  view: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ReadableStreamBYOBRequest',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(ReadableStreamDefaultController.prototype.enqueue, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(ReadableByteStreamController.prototype.enqueue, {
  __proto__: null,
  length: { __proto__: null, value: 1 },
});

// The cppExports are not part of the public API. They are exported to the
// C++ side of the implementation to allow for certain internal operations
// to be performed on ReadableStream instances.
const cppExports = ObjectFreeze({
  ReadableStream,
  acquireReadableStreamDrainingReader,
  consumeReadableStreamAsArrayBuffer,
  consumeReadableStreamAsJSON,
  consumeReadableStreamAsText,
  consumeReadableStreamAsUint8Array,
  getReadableStreamExpectedLength,
  getReadableStreamIsDisturbed,
  getReadableStreamOnEof,
  isReadableStream,
  isReadableStreamLocked,
  readableStreamCancel,
  readableStreamTee,
  setReadableStreamPendingClosure,
});

module.exports = {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamDefaultController,
  ReadableByteStreamController,
  ReadableStreamBYOBRequest,
  // Internal-only (not re-exported by streams.ts): the bulk-read path for
  // pipeTo and the C++ bridge. See Open Question 3 in the design doc for
  // possible future public exposure.
  ReadableStreamDrainingReader,
  // Internal operations consumed by the TransformStream cancel/flush
  // coordination (finishPromise guard). Unreachable from user code.
  internalsForTransform: ObjectFreeze({
    getState: <R>(stream: ReadableStream<R>) =>
      getReadableStreamGetState(stream),
    getStoredError: <R>(stream: ReadableStream<R>) =>
      getReadableStreamStoredError(stream),
  }),

  // Part of the internal implementation. Do not re-export to user code
  cppExports,
};
