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
  WritableStreamDefaultWriter as WritableStreamDefaultWriterType,
} from './types';
import type {
  ByteQueueEntry,
  ByteStreamConsumer as ByteStreamConsumerType,
  ByteStreamCursor as ByteStreamCursorType,
  ErrorStreamCallback,
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
  ArrayBufferPrototypeTransferToFixedLength,
  ArrayPrototypePush,
  AsyncIteratorPrototype,
  BigInt,
  DataView,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  JSONParse,
  MathMax,
  MathMin,
  Number,
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
  RangeError,
  SafeArrayIterator,
  SafeWeakMap,
  Symbol,
  SymbolAsyncIterator,
  SymbolFor,
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

const {
  isArrayBuffer,
  isArrayBufferView,
  isPromise,
  isSharedArrayBuffer,
  markPromiseHandled,
} = utils;

const {
  StreamQueue,
  QueueCursor,
  ByteStreamCursor,
  CLOSE_SENTINEL,
  cloneArrayBuffer,
  createReadResult,
} = require('webstreams/queue');

// Internal writable operations for the pipe (never re-exported to users).
const {
  kExtractNativeSink,
  internalsForPipe: writableInternals,
} = require('webstreams/writable');

import type { ViewExtentHelpers } from './view-extent';
const { viewByteExtent } =
  require('webstreams/view-extent') as ViewExtentHelpers;

// The native backend (see the fence conventions in native.ts and
// queue.ts). The cast restores the real shape the untyped loader erases,
// so the brand predicates keep their type-guard narrowing.
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
  nativeControllerError,
  nativeControllerMaybeCloseStream,
  nativeControllerOnReaderRelease,
  nativeControllerTeeSource,
  nativeControllerExtractSource,
  nativeControllerExpectedLength,
  nativeControllerPeekSource,
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
// table): once the last consumer of a teed queue has left, the source's
// cancel algorithm receives a single AggregateError carrying the reason of
// every consumer that left (in the order they left), rather than the spec's
// two-element array. Tees of tee branches add consumers to the same queue,
// so the list is flat by construction.
function makeCompositeCancelReason(reasons: unknown[]): AggregateError {
  // SafeArrayIterator: AggregateError's iterable conversion must not run
  // through the patchable %ArrayIteratorPrototype%.
  return new AggregateError(
    new SafeArrayIterator(reasons),
    'All readable stream tee branches were canceled'
  ) as AggregateError;
}

const kPrivateSymbol = Symbol('private');

// What an omitted (or null) dictionary argument stands for. Null-prototype:
// WebIDL reads nothing for an omitted dictionary, so neither may we.
const kEmptyDictionary: object = ObjectFreeze({ __proto__: null });

function isActualObject(value: unknown) {
  return value != null && typeof value === 'object';
}

// The spec's "Type(x) is Object", which INCLUDES callables -- used for the iterator
// protocol's object checks in from() (GetIterator/IteratorNext accept function-valued
// iterators and results). Distinct from isActualObject, which deliberately excludes
// functions for option-bag validation.
function isObjectLike(value: unknown) {
  return (
    value !== null && (typeof value === 'object' || typeof value === 'function')
  );
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
  options: ConvertedPipeOptions
) => Promise<void>;
let readableStreamPipeTo: <R>(
  source: ReadableStream<R>,
  destination: WritableStreamType<R>,
  options?: StreamPipeOptions
) => Promise<void>;
let readableStreamTee: <R>(
  stream: ReadableStream<R>
) => [ReadableStream<R>, ReadableStream<R>];
// The C++ bridge arm of JsReadableStream::detach(): takes over the stream's
// internal state into a fresh stream, leaving the original a permanently
// locked, disturbed husk. Assigned in ReadableStream's static block.
let detachReadableStream: <R>(
  stream: ReadableStream<R>,
  ignoreDisturbed: boolean
) => ReadableStream<R>;
let readableStreamReaderGenericCancel: (
  reader: object,
  reason?: unknown
) => Promise<void>;
let readableStreamReaderGenericRelease: (reader: object) => void;
let readableStreamDefaultReaderRead: <R>(
  reader: ReadableStreamDefaultReaderType<R>,
  readRequest: ReadableStreamAsyncIteratorReadRequest<R>
) => void;

// Closes a stream whose data has moved to another stream (extraction,
// native detach, tee of a native stream or of a branch) and drops its
// controller, so it neither reaches nor retains the moved source. Legacy
// C++ leaves the same streams locked, disturbed and closed.
let closeReadableStreamHusk: <R>(stream: ReadableStream<R>) => void;

// Errors a queued tee branch alone (see [kControllerErrorFunction]).
let readableStreamErrorBranch: <R>(
  stream: ReadableStream<R>,
  reason: unknown
) => void;

// A tee branch's byte cursor errors its branch alone.
const errorTeeBranchFromCursor: ErrorStreamCallback = (e, owner) => {
  if (owner !== undefined) {
    readableStreamErrorBranch(owner as ReadableStream<unknown>, e);
  }
};

let isReadableStream: (value: unknown) => boolean;
let isByteStreamController: (value: unknown) => boolean;

// C++-recognition brand (JsReadableStream::tryUnwrapTs): an own,
// non-enumerable marker stamped on every instance by the constructor, so
// the C++ bridge can recognize TypeScript streams via an own-property
// probe, without executing JavaScript. That constraint is load-bearing:
// unwrap runs during RPC deserialization, inside V8's no-JS-execution
// scope. Proxies deliberately do not convey it (the C++ check rejects
// proxies up front), matching the #-brand's no-tunneling behavior.
const kReadableStreamBrand: symbol = utils.getApiSymbol('kReadableStreamBrand');

// The Node.js stream-interop hooks, keyed by the well-known symbols Node's
// own web streams carry. node:stream's finished()/eos() observes a web
// stream's completion through `stream[kIsClosedPromise].promise` (which
// settles with the stream: fulfilled on close, rejected with the stored
// error), and addAbortSignal() errors a stream from outside through
// `stream[kControllerErrorFunction](reason)`. Both are prototype members
// (a getter and a method) so instances pay nothing until asked. The
// symbols are deliberately the Symbol.for() ones rather than API-registry
// symbols: userland ports of Node's streams look for exactly these.
const kIsClosedPromise: symbol = SymbolFor('nodejs.webstream.isClosedPromise');
const kControllerErrorFunction: symbol = SymbolFor(
  'nodejs.webstream.controllerErrorFunction'
);

// Settles the stream's closed-promise hook (if one was ever requested) to
// match a state transition; assigned in ReadableStream's static block.
let settleReadableStreamClosedPromise: <R>(stream: ReadableStream<R>) => void;
let setReadableStreamInteropErrorHook: <R>(
  stream: ReadableStream<R>,
  hook: ((reason: unknown) => void) | undefined
) => void;

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
// A reader released its lock. The native controller drops its cached
// byobRequest; the queued byte controller keeps its own (spec).
let controllerOnReaderRelease: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => void;
// A consumer of the controller's queue leaves it — cancelled, or errored
// alone before close is requested (controllerConsumerErrored). Only the last
// one to leave cancels the underlying source, with the reasons of every
// consumer that left (see makeCompositeCancelReason); the others receive a
// promise that settles with that cancel, or with undefined once the source
// has closed or errored on its own (spec ReadableStreamTee's shared cancel
// promise). A native controller has one consumer, so the leaving is its
// cancel.
let controllerConsumerLeaving: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType,
  reason: unknown,
  isLastConsumer: boolean
) => Promise<void>;
// A consumer of a QUEUED controller's queue errors alone (see
// readableStreamErrorBranch). Before close is requested it leaves as a
// cancelled one does (controllerConsumerLeaving). Once close has been
// requested the source has nothing more to produce, and an errored branch is
// no cancel (the spec errors that branch's controller and never forwards it
// to the source): it leaves without a reason, and if it was the last
// consumer the source ends as when every consumer has drained.
let controllerConsumerErrored: (
  controller:
    ReadableStreamDefaultControllerType | ReadableByteStreamControllerType,
  reason: unknown,
  isLastConsumer: boolean
) => Promise<void>;
// The stream a QUEUED controller was created for — the source's own stream,
// as opposed to the tee branches sharing the controller. undefined for a
// native controller.
let controllerStream: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => object | undefined;
// The controller's error() for internal callers, which must not dispatch
// through the user-patchable prototype method.
let controllerError: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType,
  reason: unknown
) => void;
let getReaderStream: <R>(reader: object) => ReadableStream<R> | undefined;

// BACKEND-DISPATCH point #4: the shared extractor function installed
// on native-backed streams as kExtractNativeSource. Assigned in
// ReadableStream's static block (needs private-field access).
let extractNativeSource: <R>(this: ReadableStream<R>) => object;

// The non-standard expectedLength pass-through for the DrainingReader
// (and the C++ bridge). Chained like the other controller helpers:
// default → the value installed by the TransformStream expectedLength
// extension (undefined otherwise); queued byte → cached construction
// value; native → cached construction value (joined in ReadableStream's
// static block).
let getControllerExpectedLength: (
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
) => bigint | undefined;
let byteControllerEnqueueBatch: (
  controller: ReadableByteStreamController,
  chunks: ArrayBufferView[]
) => void;
let byteControllerSetConsumptionHook: (
  controller: ReadableByteStreamController,
  hook: (() => void) | undefined
) => void;
let setDefaultControllerExpectedLength: <R>(
  controller: ReadableStreamDefaultController<R>,
  length: bigint | undefined
) => void;

let setReadableStreamPendingClosure: <R>(stream: ReadableStream<R>) => void;
let isReadableStreamPendingClosure: <R>(stream: ReadableStream<R>) => boolean;
let getReadableStreamOnEof: <R>(stream: ReadableStream<R>) => Promise<void>;
let getReadableStreamExpectedLength: <R>(
  stream: ReadableStream<R>
) => bigint | undefined;
// Non-detaching access to a native-backed stream's underlying source object
// for the C++ bridge's encoding-aware tryGetLength arm; undefined for
// queued-backed streams. Assigned in ReadableStream's static block.
let getReadableStreamNativeSource: <R>(
  stream: ReadableStream<R>
) => object | undefined;
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

    // #closedPromise holds either a settled Promise or a still-pending
    // resolvers record, told apart with the native isPromise: a duck-typed
    // `typeof x.resolve` would read Object.prototype on the Promise.
    resolveGenericReaderPromise = (reader: object) => {
      const base = getReaderBase(reader);
      const promise = base.#closedPromise;
      if (!isPromise(promise)) {
        promise.resolve();
        base.#closedPromise = promise.promise;
      }
    };

    rejectGenericReaderPromise = (reader: object, reason?: unknown) => {
      const base = getReaderBase(reader);
      const promise = base.#closedPromise;
      if (!isPromise(promise)) {
        promise.reject(reason);
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
      // controllers, the head pull-into descriptor STAYS (readerType →
      // 'none'); the byobRequest is NOT invalidated per spec.
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

// The user-visible error for reads/pipes/tees attempted after the stream's
// owning object initiated closure (see #pendingClosure). The text matches
// the legacy internal controller's exactly.
function pendingClosureError(): TypeError {
  return new TypeError(
    'This ReadableStream belongs to an object that is closing.'
  );
}

// Spec PullSteps for a chunk (or the end of the stream) already at the
// consumer: dequeue, pull trigger, then drain-then-close, all synchronous.
// Undefined when the read would have to wait.
function readBufferedSync<R>(
  reader: object,
  stream: ReadableStream<R>,
  consumer: StreamConsumerType<R>,
  controller:
    | ReadableStreamDefaultControllerType
    | ReadableByteStreamControllerType
    | NativeReadableStreamControllerType
    | undefined
): ReadableStreamReadResult<R> | undefined {
  const result = consumer.tryReadSync(reader) as
    ReadableStreamReadResult<R> | undefined;
  if (result === undefined) return undefined;
  if (controller !== undefined) {
    controllerPullIfNeeded(controller);
    controllerMaybeCloseStream(controller);
  }
  if (result.done) {
    readableStreamClose(stream);
  }
  return result;
}

// The pipe's synchronous read: the next buffered chunk, taken whole as a
// default read takes it. Only for a readable
// stream; the pump's shutdown checks establish that. Undefined when the
// read would wait or fail; defaultReaderReadInternal handles those.
function pipeReadBuffered<R>(
  reader: object,
  stream: ReadableStream<R>
): ReadableStreamReadResult<R> | undefined {
  if (isReadableStreamPendingClosure(stream)) return undefined;
  const consumer = getReadableStreamConsumer(stream);
  if (consumer === undefined) return undefined;
  return readBufferedSync<R>(
    reader,
    stream,
    consumer,
    getReadableStreamController(stream)
  );
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
  if (isReadableStreamPendingClosure(stream)) {
    return PromiseReject(pendingClosureError()) as Promise<
      ReadableStreamReadResult<R>
    >;
  }
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

  // --- Synchronous fast path (spec PullSteps step 3) ---
  // When data is immediately available the spec dequeues, performs the
  // drain-then-close check, and fulfills the read request all in one
  // synchronous call. An async/await on an already-resolved promise
  // would insert a microtask gap between the dequeue and the close,
  // causing reader.closed to resolve one tick too late. tryReadSync
  // returns the result directly (no promise wrapping) so the close
  // check runs in the same synchronous call. This includes byte streams
  // with autoAllocateChunkSize: queued bytes are handed over as the head
  // chunk, uncopied; only an empty queue allocates.
  const syncResult = readBufferedSync<R>(reader, stream, consumer, controller);
  if (syncResult !== undefined) {
    return PromiseResolve(syncResult);
  }
  let useAsyncPath = false;
  if (controller !== undefined && isByteStreamController(controller)) {
    const autoAllocateChunkSize = getByteControllerAutoAllocateChunkSize(
      controller as ReadableByteStreamController
    );
    if (autoAllocateChunkSize !== undefined) {
      useAsyncPath = true;
    }
  }

  // --- Async fallback ---
  // Data is not immediately available (pending reads queued, or native
  // source); with autoAllocateChunkSize the read waits on an allocated
  // pull-into descriptor (spec PullSteps step 4). Handle completion
  // asynchronously.
  return defaultReaderReadInternalAsync<R>(
    reader,
    stream,
    consumer,
    controller,
    useAsyncPath
  );
}

// Submit a default-style read through the BYOB machinery via a synthetic
// auto-allocate pull-into descriptor (spec ReadableByteStreamController
// PullSteps step 4, [[autoAllocateChunkSize]] present): the source's pull
// then observes a byobRequest over the auto-allocated buffer. Shared by
// the default reader's read path and the draining reader's
// empty-fallback wait-read (the body/pipe pump).
function readViaAutoAllocateDescriptor(
  consumer: ByteStreamConsumerType,
  autoAllocateChunkSize: number,
  reader: object
): Promise<ReadableStreamReadResult<ArrayBufferView>> {
  const withResolvers = PromiseWithResolvers() as PromiseWithResolversType<
    ReadableStreamReadResult<ArrayBufferView>
  >;
  const descriptor: PullIntoDescriptor = {
    buffer: new ArrayBuffer(autoAllocateChunkSize),
    bufferByteLength: autoAllocateChunkSize,
    byteOffset: 0,
    byteLength: autoAllocateChunkSize,
    bytesFilled: 0,
    minimumFill: 1,
    elementSize: 1,
    viewCtor: Uint8Array,
    readerType: 'default',
    settledAtEndOfData: false,
    promise: withResolvers.promise,
    resolve: withResolvers.resolve,
    reject: withResolvers.reject,
    reader,
  };
  return consumer.readBYOB(descriptor);
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
    promise = readViaAutoAllocateDescriptor(
      consumer as unknown as ByteStreamConsumerType,
      autoAllocateChunkSize as number,
      reader
    );
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

  read<T extends ArrayBufferView>(
    view: T,
    options: ReadableStreamBYOBReaderReadOptions = kEmptyDictionary as ReadableStreamBYOBReaderReadOptions
  ): Promise<ReadableStreamReadResult<T>> {
    try {
      return this.#read(view, options);
    } catch (e) {
      // A foreign `this`: the brand check throws, and promise-returning
      // operations reject (WebIDL).
      return PromiseReject(e) as Promise<ReadableStreamReadResult<T>>;
    }
  }

  // The read body; readAtLeast() must not dispatch through the
  // user-patchable prototype's read(). Not returned from an async read():
  // that would add two microtasks to every result.
  async #read<T extends ArrayBufferView>(
    view: T,
    options: ReadableStreamBYOBReaderReadOptions
  ): Promise<ReadableStreamReadResult<T>> {
    // --- View validation (spec read(view, options) steps 1-3) ---
    if (!isArrayBufferView(view)) {
      throw new TypeError('view must be an ArrayBufferView');
    }
    const info = getUnsharedViewInfo(view, 'view');
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
    if (isReadableStreamPendingClosure(stream)) {
      throw pendingClosureError();
    }
    setReadableStreamDisturbed(stream);
    if (getReadableStreamGetState(stream) === 'errored') {
      throw getReadableStreamStoredError(stream);
    }
    // Transfer the buffer regardless of state (spec step).
    const transferred = ArrayBufferPrototypeTransferToFixedLength(info.buffer);
    if (getReadableStreamGetState(stream) === 'closed') {
      // Closed or cancelled alike: a zero-length view over the transferred
      // buffer (spec ReadableByteStreamControllerPullInto "closed" branch).
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
      settledAtEndOfData: false,
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

  readAtLeast<T extends ArrayBufferView>(
    minElements: number,
    view: T
  ): Promise<ReadableStreamReadResult<T>> {
    try {
      return this.#read(view, { min: minElements });
    } catch (e) {
      return PromiseReject(e) as Promise<ReadableStreamReadResult<T>>;
    }
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
//
// Both settle the Node.js interop closed-promise and drop the interop error
// hook: the hook fires only from 'readable', so once the state is terminal
// it can never run, and keeping it would retain the transform pair for as
// long as this half lives (the ClearAlgorithms discipline of the
// controllers, applied to the stream's own slot).
function readableStreamClose<R>(stream: ReadableStream<R>): void {
  if (getReadableStreamGetState(stream) !== 'readable') return;
  setReadableStreamState(stream, 'closed');
  const reader = getReadableStreamReader(stream);
  if (reader !== undefined) {
    resolveGenericReaderPromise(reader);
  }
  settleReadableStreamClosedPromise(stream);
  setReadableStreamInteropErrorHook(stream, undefined);
}

function readableStreamError<R>(stream: ReadableStream<R>, e: unknown): void {
  if (getReadableStreamGetState(stream) !== 'readable') return;
  setReadableStreamState(stream, 'errored');
  setReadableStreamStoredError(stream, e);
  const reader = getReadableStreamReader(stream);
  if (reader !== undefined) {
    rejectGenericReaderPromise(reader, e);
  }
  settleReadableStreamClosedPromise(stream);
  setReadableStreamInteropErrorHook(stream, undefined);
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

// getViewInfo() for the byte-stream trust boundaries, which reject
// SharedArrayBuffer-backed views (WebIDL ArrayBufferView without
// [AllowShared]).
function getUnsharedViewInfo(view: ArrayBufferView, what: string): ViewInfo {
  const info = getViewInfo(view);
  if (isSharedArrayBuffer(info.buffer)) {
    throw new TypeError(`${what} must not be backed by a SharedArrayBuffer`);
  }
  return info;
}

// Validate and normalize a user-provided chunk for a byte stream at the
// enqueue()/respondWithNewView() trust boundary: snapshot metadata, reject
// zero-length views and zero-length (or detached — detached buffers report
// byteLength 0) buffers, and transfer the backing buffer. The returned
// triple references the TRANSFERRED buffer; the caller's view is detached.
// All byte-stream transfers are to fixed length (spec
// TransferArrayBuffer), so a source cannot shrink a buffer we hold.
function validateAndTransferView(view: ArrayBufferView): ByteQueueEntry {
  if (!isArrayBufferView(view)) {
    throw new TypeError('chunk must be an ArrayBufferView');
  }
  const info = getUnsharedViewInfo(view, 'chunk');
  if (info.byteLength === 0) {
    throw new TypeError('chunk must have a non-zero byteLength');
  }
  if (ArrayBufferPrototypeByteLengthGet(info.buffer) === 0) {
    throw new TypeError(
      "chunk's backing buffer must have a non-zero byteLength"
    );
  }
  return {
    buffer: ArrayBufferPrototypeTransferToFixedLength(info.buffer),
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
  // The non-standard expectedLength pass-through (undefined = unknown).
  // Default controllers never read it from an underlying source — it is
  // set ONLY through the internal setter, by the TransformStream
  // constructor's workerd `expectedLength` extension, so the C++ bridge
  // can derive Content-Length for bodies built from such transforms.
  #expectedLength: bigint | undefined = undefined;
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
  // The source has reached its end: errored, cancelled, or closed with
  // every consumer drained (the spec's "stream is no longer readable",
  // which gates error()). Tracked here rather than read off #stream: once
  // that stream is teed away its state follows the source's own events
  // (see #maybeCloseStream), while what the branches have yet to drain
  // after a close is this controller's business.
  #done: boolean = false;
  // Consumers that left the queue (see controllerConsumerLeaving) while
  // others remained: their reasons, for the composite the last one to leave
  // hands the source, and the promise their cancel() returned, settled with
  // the source's cancel or with undefined once the source has closed or
  // errored on its own.
  #departedReasons: unknown[] = [];
  #pendingCancel: PromiseWithResolversType<void> | undefined;

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

    // Default controllers never read expectedLength from an underlying
    // source (silently ignored if declared); it is populated only via the
    // internal setter (the TransformStream `expectedLength` extension).
    // The byte and native arms of this chain report their own cached
    // values.
    getControllerExpectedLength = (controller) =>
      (controller as ReadableStreamDefaultController).#expectedLength;

    setDefaultControllerExpectedLength = (controller, length) => {
      controller.#expectedLength = length;
    };

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

    controllerConsumerLeaving = (controller, reason, isLastConsumer) => {
      if (#queue in controller) {
        return (controller as ReadableStreamDefaultController).#consumerLeaving(
          reason,
          isLastConsumer
        );
      }
      return PromiseResolve() as Promise<void>;
    };

    controllerConsumerErrored = (controller, reason, isLastConsumer) => {
      if (#queue in controller) {
        return (controller as ReadableStreamDefaultController).#consumerErrored(
          reason,
          isLastConsumer
        );
      }
      return PromiseResolve() as Promise<void>;
    };

    controllerStream = (controller) => {
      if (#queue in controller) {
        return (controller as ReadableStreamDefaultController).#stream;
      }
      return undefined;
    };

    controllerError = (controller, reason) => {
      if (#queue in controller) {
        (controller as ReadableStreamDefaultController).#error(reason);
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
      // Every consumer has been collected (see StreamQueue#noConsumers).
      // Release the source rather than cancel it: GC timing must not run
      // the user's cancel callback. The size algorithm stays, so enqueue()
      // sizes chunks as before; the queue drops them.
      this.#pullAlgorithm = undefined;
      this.#cancelAlgorithm = undefined;
    }) as StreamQueueType<R, R>;
    const cursor = new QueueCursor(this.#queue, stream) as QueueCursorType<
      R,
      R
    >;
    this.#queue.anchorCursor(cursor);
    setReadableStreamConsumer(stream, cursor);

    // --- Start ---
    // Per spec, start is invoked synchronously and a sync throw propagates
    // out of the ReadableStream constructor.
    const startResult: unknown =
      startFn === undefined
        ? undefined
        : uncurryThis(startFn)(underlyingSource, this);
    PromisePrototypeThen(
      writableInternals.promiseResolvedWith(startResult),
      () => {
        this.#started = true;
        this.#callPullIfNeeded();
      },
      (e: unknown) => {
        this.#error(e);
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
      QueueCursorType<R, R> | undefined;
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
      this.#error(e);
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
    this.#error(reason);
  }

  // Internal callers error through here: the prototype's error() is
  // user-patchable.
  #error(reason: unknown): void {
    if (this.#done) return;
    this.#done = true;
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
    // The source settled on its own: consumers that had left are owed
    // undefined (spec ReadableStreamTee step 14.c.ii).
    this.#pendingCancel?.resolve();
  }

  #canCloseOrEnqueue(): boolean {
    // #cancelPromise marks a cancelled source: CancelSteps cleared its
    // algorithms, whatever state its stream reports.
    return (
      !this.#closeRequested &&
      this.#cancelPromise === undefined &&
      getReadableStreamGetState(this.#stream) === 'readable'
    );
  }

  #maybeCloseStream(): void {
    if (!this.#closeRequested) return;
    // A stream whose cursor has reached the close sentinel transitions to
    // 'closed'; one with buffered data before the sentinel stays 'readable'
    // until drained (drain-then-close on subsequent reads).
    //
    // The source's own stream. With a cursor it is the queue's only
    // consumer (tee and detach take its cursor away) and drains to the
    // sentinel like any; without one it consumes nothing, so there is
    // nothing to drain and it closes now. The source's own events close it
    // — close requested here, cancelled in #cancelSteps, errored in
    // error() — never the branches' progress (the queued tee model,
    // AGENTS.md). Its closing does not end the source: that is #done,
    // below, once every consumer has drained.
    const parentCursor = getReadableStreamConsumer(this.#stream) as
      QueueCursorType<R, R> | undefined;
    if (parentCursor !== undefined) {
      if (this.#queue.getEntry(parentCursor.position) !== CLOSE_SENTINEL) {
        return;
      }
      readableStreamClose(this.#stream);
    } else {
      // Check every live consumer stream (the tee branches).
      let anyOpen = false;
      const owners = this.#queue.getLiveOwners();
      for (let i = 0; i < owners.length; i++) {
        const owner = owners[i] as ReadableStream<R>;
        const cursor = getReadableStreamConsumer(owner) as
          QueueCursorType<R, R> | undefined;
        if (cursor === undefined) continue;
        if (this.#queue.getEntry(cursor.position) === CLOSE_SENTINEL) {
          readableStreamClose(owner);
        } else {
          anyOpen = true;
        }
      }
      readableStreamClose(this.#stream);
      if (anyOpen) return;
    }
    this.#done = true;
    this.#clearAlgorithms();
    // Every remaining consumer has closed: the source will never be
    // cancelled, and consumers that had left are owed undefined (spec
    // ReadableStreamTee step 14.b.v).
    this.#pendingCancel?.resolve();
  }

  // A consumer leaves the queue; see controllerConsumerLeaving. Decided
  // BEFORE the cursor's removal (QueueCursor.cancelStream): removing it
  // first would make the leaving consumer look like it was not the last
  // (parking the cancel in #pendingCancel with nobody left to settle it)
  // and fire the all-cursors-gone hook, which clears the cancel algorithm
  // before #cancelSteps could run it.
  #consumerLeaving(reason: unknown, isLastConsumer: boolean): Promise<void> {
    const reasons = this.#departedReasons;
    ArrayPrototypePush(reasons, reason);
    if (isLastConsumer) {
      return this.#cancelSteps(
        reasons.length > 1 ? makeCompositeCancelReason(reasons) : reason
      );
    }
    this.#pendingCancel ??=
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    return this.#pendingCancel.promise;
  }

  // A consumer errors alone; see controllerConsumerErrored. Decided before
  // the cursor's removal, as #consumerLeaving is.
  #consumerErrored(reason: unknown, isLastConsumer: boolean): Promise<void> {
    if (!this.#closeRequested) {
      return this.#consumerLeaving(reason, isLastConsumer);
    }
    if (isLastConsumer) {
      // As in #maybeCloseStream once every consumer has drained.
      this.#done = true;
      this.#clearAlgorithms();
      this.#pendingCancel?.resolve();
    }
    return PromiseResolve() as Promise<void>;
  }

  #shouldCallPull(): boolean {
    if (!this.#started) return false;
    if (!this.#canCloseOrEnqueue()) return false;
    // The pending-read clause is what keeps a fast consumer from starving
    // when the queue is at the high water mark: a consumer that reads
    // faster than the HWM drains must still trigger pulls. With every
    // consumer collected there is nobody to pull for.
    return this.#queue.wantsPull();
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
        this.#error(e);
      }
    );
  }

  // Cancel the underlying source (spec CancelSteps). Idempotent and cached.
  // Reached through #consumerLeaving once the last consumer has left the
  // queue — an explicit stream/branch cancel, or a branch errored through
  // the Node.js interop hook. The all-cursors-gone GC hook never comes
  // here: it only releases the source (see the constructor).
  #cancelSteps(reason: unknown): Promise<void> {
    if (this.#cancelPromise !== undefined) return this.#cancelPromise;
    this.#done = true;
    // The source's own stream, if teed away, closes with this cancel, as
    // readableStreamCancel closes an un-teed stream before its cancel
    // steps; being locked, nothing else can reach it.
    if (getReadableStreamConsumer(this.#stream) === undefined) {
      readableStreamClose(this.#stream);
    }
    const cancelAlgorithm = this.#cancelAlgorithm;
    this.#clearAlgorithms();
    this.#cancelPromise =
      cancelAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : cancelAlgorithm(reason);
    // Consumers that left earlier were promised the source's cancel.
    this.#pendingCancel?.resolve(this.#cancelPromise);
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
// tee() and detach are about to replace the cursor `from`; `wasSole` when
// it was the queue's only cursor.
let byteControllerOnFork: (
  controller: ReadableByteStreamController,
  from: ByteStreamCursorType,
  wasSole: boolean
) => void;

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
    const info = getUnsharedViewInfo(view, 'view');
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
  // As the default controller's: see there.
  #done: boolean = false;
  #departedReasons: unknown[] = [];
  #pendingCancel: PromiseWithResolversType<void> | undefined;
  #byobRequest: ReadableStreamBYOBRequest | null = null;
  // A released reader's head pull-into, taken over from the sole cursor at
  // tee()/detach so that a byobRequest held across the fork keeps working
  // (spec: the controller's head, untouched by the fork). The forked
  // cursors hold copies of its filled bytes (adoptReleasedBytes). While it
  // is set, byobRequest is over it, and respond() enqueues its bytes, old
  // and new, for every cursor, dropping their copies. enqueue(), error(),
  // cancel and a closed-state respond(0) discard it.
  #releasedHead: PullIntoDescriptor | undefined;

  static {
    isByteStreamController = (value: unknown) => {
      return isActualObject(value) && #queue in value;
    };

    // Enqueues several chunks, in order, notifying the consumers once after
    // the last, so a pending BYOB read on any cursor fills across all of
    // them before it is answered (a per-chunk notify would answer it with
    // the first). Only for internal sources (the identity streams); each
    // chunk is validated and accounted as by enqueue().
    byteControllerEnqueueBatch = (controller, chunks) => {
      const last = chunks.length - 1;
      for (let i = 0; i <= last; i++) {
        controller.#enqueueChunk(chunks[i] as ArrayBufferView, i === last);
      }
    };

    byteControllerSetConsumptionHook = (controller, hook) => {
      controller.#queue.setConsumptionHook(hook);
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

    const prevConsumerLeaving = controllerConsumerLeaving;
    controllerConsumerLeaving = (controller, reason, isLastConsumer) => {
      if (#queue in controller) {
        return controller.#consumerLeaving(reason, isLastConsumer);
      }
      return prevConsumerLeaving(controller, reason, isLastConsumer);
    };

    const prevConsumerErrored = controllerConsumerErrored;
    controllerConsumerErrored = (controller, reason, isLastConsumer) => {
      if (#queue in controller) {
        return controller.#consumerErrored(reason, isLastConsumer);
      }
      return prevConsumerErrored(controller, reason, isLastConsumer);
    };

    const prevControllerStream = controllerStream;
    controllerStream = (controller) => {
      if (#queue in controller) {
        return controller.#stream;
      }
      return prevControllerStream(controller);
    };

    const prevControllerError = controllerError;
    controllerError = (controller, reason) => {
      if (#queue in controller) {
        controller.#error(reason);
      } else {
        prevControllerError(controller, reason);
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

    byteControllerOnFork = (controller, from, wasSole) => {
      // Fork needs an unlocked stream, so a head here is a released one.
      const head = from.headPullInto;
      if (
        wasSole &&
        head !== undefined &&
        controller.#releasedHead === undefined
      ) {
        controller.#releasedHead = head;
        return;
      }
      // A request over `from`'s head would outlive it.
      if (controller.#releasedHead === undefined) {
        controller.#invalidateByobRequest();
      }
    };

    const prevOnReaderRelease = controllerOnReaderRelease;
    controllerOnReaderRelease = (controller) => {
      if (#queue in controller) {
        // Spec: releaseLock does NOT invalidate the byobRequest. The head
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
      // Every consumer has been collected: release the source, as the
      // default controller's hook does.
      this.#clearAlgorithms();
      this.#invalidateByobRequest();
      this.#releasedHead = undefined;
    }) as StreamQueueType<ByteQueueEntry, Uint8Array>;
    const cursor = new ByteStreamCursor(this.#queue, stream);
    this.#queue.anchorCursor(cursor);
    // Wire up the fractional-element-at-close error callback so the
    // cursor can error the stream when a BYOB read lands at the close
    // sentinel with a non-element-aligned partial fill.
    cursor.errorStreamCallback = (e: unknown) => {
      this.#error(e);
    };
    setReadableStreamConsumer(stream, cursor);

    // --- Start (sync throw propagates out of the ReadableStream ctor) ---
    const startResult: unknown =
      startFn === undefined
        ? undefined
        : uncurryThis(startFn)(underlyingSource, this);
    PromisePrototypeThen(
      writableInternals.promiseResolvedWith(startResult),
      () => {
        this.#started = true;
        this.#callPullIfNeeded();
      },
      (e: unknown) => {
        this.#error(e);
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
      const released = this.#releasedHead;
      if (released !== undefined) {
        const request = new ReadableStreamBYOBRequest(kPrivateSymbol);
        initializeByobRequest(
          request,
          this,
          new Uint8Array(
            released.buffer,
            released.byteOffset + released.bytesFilled,
            released.byteLength - released.bytesFilled
          ),
          released.minimumFill - released.bytesFilled
        );
        this.#byobRequest = request;
        return request;
      }
      // Zero-copy is only unambiguous with exactly one consumer, and only
      // when it has a head pull-into descriptor (from a BYOB read, or
      // auto-allocated for a default read when autoAllocateChunkSize is
      // set). Otherwise the source must fall back to enqueue(). The
      // request object is cached for identity until invalidated.
      const cursor = this.#queue.singleCursor as
        ByteStreamCursorType | undefined;
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
    this.#enqueueChunk(chunk, true);
  }

  // enqueue()'s steps. With `notify` false the chunk is queued without
  // notifying the consumers (byteControllerEnqueueBatch notifies with its
  // last chunk).
  #enqueueChunk(chunk: ArrayBufferView, notify: boolean): void {
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
    const released = this.#releasedHead;
    if (released !== undefined) {
      // Spec steps 8.4-8.5 for the controller's released head: its buffer
      // is transferred, and the cursors' copies of its bytes go ahead of
      // the chunk below.
      this.#releasedHead = undefined;
      if (!released.settledAtEndOfData) {
        released.buffer = ArrayBufferPrototypeTransferToFixedLength(
          released.buffer
        );
      }
    }
    const drainCursor = this.#queue.singleCursor as
      ByteStreamCursorType | undefined;
    if (drainCursor !== undefined) {
      const head = drainCursor.headPullInto;
      if (head !== undefined) {
        // Spec step 8.4: transfer the head descriptor's buffer so that
        // old captured views are detached.
        head.buffer = ArrayBufferPrototypeTransferToFixedLength(head.buffer);
      }
      // Spec step 8.5: a released head's bytes go ahead of the chunk.
      drainCursor.flushReleasedHead();
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
    } else {
      // Step 8.5 on a shared queue: each cursor keeps its own released bytes.
      this.#queue.forEachLiveCursor((cursor) => {
        (cursor as unknown as ByteStreamCursorType).flushReleasedHead();
      });
    }
    this.#queue.enqueue({ value: entry, size: entry.byteLength }, notify);
    // The cursors' notify() (run by queue.enqueue) services pending
    // pull-intos and default reads alike.
    if (notify) this.#callPullIfNeeded();
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
    // complete the element can never arrive. Only a cursor whose fractional
    // fill errors the whole stream counts: the source's own, or its
    // successor after a detach, which inherits its callback. A tee branch's
    // errors that branch alone (the spec closes each branch through its own
    // controller), which its cursor does itself when the close reaches it,
    // so this close() succeeds and the sibling keeps every byte.
    const hasFractionalFill = this.#queue.someLiveCursor((cursor) => {
      const byteCursor = cursor as unknown as ByteStreamCursorType;
      if (byteCursor.errorStreamCallback === errorTeeBranchFromCursor) {
        return false;
      }
      const head: PullIntoDescriptor | undefined = byteCursor.headPullInto;
      return head !== undefined && head.bytesFilled % head.elementSize !== 0;
    });
    if (hasFractionalFill) {
      const e = new TypeError(
        'Insufficient bytes to fill elements in the given view'
      );
      this.#error(e);
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
      this.#error(e);
      throw e;
    }
    this.#closeRequested = true;
    this.#queue.close();
    this.#maybeCloseStream();
  }

  error(reason: unknown = undefined): void {
    assertIsReadableByteStreamController(this);
    this.#error(reason);
  }

  // Internal callers error through here: the prototype's error() is
  // user-patchable.
  #error(reason: unknown): void {
    if (this.#done) return;
    this.#done = true;
    this.#invalidateByobRequest();
    this.#releasedHead = undefined;
    // Branch propagation — see the default controller's error() for why.
    const owners = this.#queue.getLiveOwners();
    this.#queue.error(reason);
    this.#clearAlgorithms();
    for (let i = 0; i < owners.length; i++) {
      readableStreamError(owners[i] as ReadableStream<Uint8Array>, reason);
    }
    readableStreamError(this.#stream, reason);
    this.#pendingCancel?.resolve();
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
    const released = this.#releasedHead;
    const cursor =
      released === undefined
        ? (this.#queue.singleCursor as ByteStreamCursorType | undefined)
        : undefined;
    const head = released ?? cursor?.headPullInto;
    if (head === undefined) {
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
    // the result view and any remainder view target the NEW buffer. A
    // descriptor already settled at end-of-data has handed that buffer to
    // the reader's result (see PullIntoDescriptor.settledAtEndOfData), so
    // it is left alone: the commit below has nothing left to resolve.
    if (!head.settledAtEndOfData) {
      head.buffer = ArrayBufferPrototypeTransferToFixedLength(head.buffer);
    }
    this.#invalidateByobRequest();
    if (cursor === undefined) {
      this.#respondToReleasedHead(bytesWritten, state);
    } else if (state === 'closed') {
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
  // writable side; without erroring here the readable would hang forever.
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
      this.#error(e);
      throw e;
    }
    this.#bytesDelivered = delivered;
  }

  // byobRequest.respondWithNewView(view).
  #respondWithNewView(view: ArrayBufferView): void {
    const released = this.#releasedHead;
    const cursor =
      released === undefined
        ? (this.#queue.singleCursor as ByteStreamCursorType | undefined)
        : undefined;
    const head = released ?? cursor?.headPullInto;
    if (head === undefined) {
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
    // Same settled-descriptor exemption as respond(): the reader already
    // owns the delivered buffer, so the replacement view's buffer is not
    // adopted in its place.
    if (!head.settledAtEndOfData) {
      head.buffer = ArrayBufferPrototypeTransferToFixedLength(info.buffer);
    }
    this.#invalidateByobRequest();
    if (cursor === undefined) {
      this.#respondToReleasedHead(info.byteLength, state);
    } else if (state === 'closed') {
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

  // respond()/respondWithNewView() on #releasedHead, validated, its buffer
  // re-transferred and the request invalidated (spec RespondInReadableState
  // step 3, EnqueueDetachedPullIntoToQueue; closed: RespondInClosedState,
  // which drops it).
  #respondToReleasedHead(
    bytesWritten: number,
    state: 'readable' | 'closed' | 'errored'
  ): void {
    const head = this.#releasedHead as PullIntoDescriptor;
    this.#releasedHead = undefined;
    if (state === 'closed') return;
    this.#accountDelivery(bytesWritten);
    const filled = head.bytesFilled + bytesWritten;
    this.#queue.forEachLiveCursor((cursor) => {
      (cursor as unknown as ByteStreamCursorType).dropReleasedHead();
    });
    this.#queue.enqueue({
      value: {
        buffer: cloneArrayBuffer(head.buffer, head.byteOffset, filled),
        byteOffset: 0,
        byteLength: filled,
      },
      size: filled,
    });
    this.#maybeCloseStream();
    this.#callPullIfNeeded();
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
    // Mirror of the default controller's logic — see that for comments.
    const parentCursor = getReadableStreamConsumer(this.#stream) as
      QueueCursorType<ByteQueueEntry, Uint8Array> | undefined;
    if (parentCursor !== undefined) {
      if (this.#queue.getEntry(parentCursor.position) !== CLOSE_SENTINEL) {
        return;
      }
      readableStreamClose(this.#stream);
    } else {
      let anyOpen = false;
      const owners = this.#queue.getLiveOwners();
      for (let i = 0; i < owners.length; i++) {
        const owner = owners[i] as ReadableStream<unknown>;
        const cursor = getReadableStreamConsumer(owner) as
          QueueCursorType<ByteQueueEntry, Uint8Array> | undefined;
        if (cursor === undefined) continue;
        if (this.#queue.getEntry(cursor.position) === CLOSE_SENTINEL) {
          readableStreamClose(owner);
        } else {
          anyOpen = true;
        }
      }
      readableStreamClose(this.#stream);
      if (anyOpen) return;
    }
    this.#done = true;
    this.#clearAlgorithms();
    this.#pendingCancel?.resolve();
  }

  // As the default controller's: see there.
  #consumerLeaving(reason: unknown, isLastConsumer: boolean): Promise<void> {
    const reasons = this.#departedReasons;
    ArrayPrototypePush(reasons, reason);
    if (isLastConsumer) {
      return this.#cancelSteps(
        reasons.length > 1 ? makeCompositeCancelReason(reasons) : reason
      );
    }
    this.#pendingCancel ??=
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    return this.#pendingCancel.promise;
  }

  // As the default controller's: see there.
  #consumerErrored(reason: unknown, isLastConsumer: boolean): Promise<void> {
    if (!this.#closeRequested) {
      return this.#consumerLeaving(reason, isLastConsumer);
    }
    if (isLastConsumer) {
      this.#done = true;
      this.#invalidateByobRequest();
      this.#clearAlgorithms();
      this.#pendingCancel?.resolve();
    }
    return PromiseResolve() as Promise<void>;
  }

  #shouldCallPull(): boolean {
    if (!this.#started) return false;
    if (!this.#canCloseOrEnqueue()) return false;
    return this.#queue.wantsPull();
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
        this.#error(e);
      }
    );
  }

  #cancelSteps(reason: unknown): Promise<void> {
    if (this.#cancelPromise !== undefined) return this.#cancelPromise;
    this.#done = true;
    if (getReadableStreamConsumer(this.#stream) === undefined) {
      readableStreamClose(this.#stream);
    }
    this.#invalidateByobRequest();
    this.#releasedHead = undefined;
    const cancelAlgorithm = this.#cancelAlgorithm;
    this.#clearAlgorithms();
    this.#cancelPromise =
      cancelAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : cancelAlgorithm(reason);
    this.#pendingCancel?.resolve(this.#cancelPromise);
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
  if (isReadableStreamPendingClosure(stream)) {
    throw pendingClosureError();
  }
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
    //
    // QUEUED-BYTE-SPECIFIC (sanctioned, mirrors defaultReaderReadInternal):
    // with autoAllocateChunkSize set, the wait-read goes through the BYOB
    // machinery so the source's pull observes a byobRequest over the
    // auto-allocated buffer — the body and pipe pumps drive
    // respond()-oriented sources exactly like C++'s BYOB pump.
    let promise: Promise<ReadableStreamReadResult<unknown>> | undefined;
    if (controller !== undefined && isByteStreamController(controller)) {
      const autoAllocateChunkSize = getByteControllerAutoAllocateChunkSize(
        controller as ReadableByteStreamController
      );
      if (autoAllocateChunkSize !== undefined) {
        promise = readViaAutoAllocateDescriptor(
          consumer as unknown as ByteStreamConsumerType,
          autoAllocateChunkSize,
          reader
        );
      }
    }
    if (promise === undefined) {
      promise = consumer.read(reader);
    }
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
  // (byte/native report their cached value; a default controller the
  // value the TransformStream expectedLength extension installed, if
  // any). Returns undefined after release.
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
    options: { maxSize?: number } = kEmptyDictionary
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

// StreamPipeOptions after WebIDL conversion: plain data, no getters.
interface ConvertedPipeOptions {
  readonly preventAbort: boolean;
  readonly preventCancel: boolean;
  readonly preventClose: boolean;
  readonly signal: AbortSignal | undefined;
}

// WebIDL conversion of StreamPipeOptions. The pipe methods run it before
// their locked checks, so a getter cannot change a lock after it is checked.
// Members are read once each in spec order (WPT
// piping/throwing-options.any.js).
function convertPipeOptions(options: unknown): ConvertedPipeOptions {
  // WebIDL: null and undefined become {}.
  if (options == null) options = kEmptyDictionary;
  if (!isActualObject(options)) {
    throw new TypeError('Pipe options must be an object');
  }
  const dict = options as StreamPipeOptions;
  const preventAbort = !!dict.preventAbort;
  const preventCancel = !!dict.preventCancel;
  const preventClose = !!dict.preventClose;
  const signal = dict.signal;
  if (signal !== undefined) {
    // Brand check. Under the modern JSG layout the captured `aborted` getter
    // throws for non-AbortSignal receivers; under the instance-property
    // layout (old compat dates) the capture is a plain read that cannot
    // brand-check, so additionally require the boolean a genuine signal's
    // own data property carries. (A forged {aborted: boolean} slips through
    // under old dates only; the native fast path's C++ unwrap rejects it.)
    let aborted: unknown;
    try {
      aborted = AbortSignalAbortedGet(signal);
    } catch {
      throw new TypeError('options.signal must be an AbortSignal');
    }
    if (typeof aborted !== 'boolean') {
      throw new TypeError('options.signal must be an AbortSignal');
    }
  }
  return {
    __proto__: null,
    preventAbort,
    preventCancel,
    preventClose,
    signal,
  } as ConvertedPipeOptions;
}

// The pipe (spec ReadableStreamPipeTo). Internal operations only on both
// ends — locks are held for the duration. Chunks are read only while the
// destination desires them: the pump moves buffered chunks until
// desiredSize runs out, and reads the next chunk when none is buffered. It
// resumes from the destination's ready hook when a write completes, and
// when a read delivers, so a buffered backlog moves without a promise per
// wake-up. Writes are not awaited individually (the close path queues
// behind them, and write failures surface through the destination's closed
// promise). A shutdown waits for the writes made before running its
// action, including the write of a chunk that a pending read delivers
// during the wait.
function pipeToInternal<R>(
  source: ReadableStream<R>,
  destination: WritableStreamType<R>,
  options: ConvertedPipeOptions
): Promise<void> {
  const { preventAbort, preventCancel, preventClose, signal } = options;

  // Lock both ends. The callers have checked both locks; release the reader
  // if the writer still cannot be acquired.
  const reader = new ReadableStreamDefaultReader<R>(source);
  let writer: WritableStreamDefaultWriterType<R>;
  try {
    writer = writableInternals.acquireWriter(destination);
  } catch (e) {
    // The catch here is purely defensive. The acquireWriter
    // should not actually throw.
    readableStreamReaderGenericRelease(reader);
    throw e;
  }
  setReadableStreamDisturbed(source);

  const { promise, resolve, reject } =
    PromiseWithResolvers() as PromiseWithResolversType<void>;

  let shuttingDown = false;
  let abortRegistration: AbortAlgorithmHandle | undefined;

  const finalize = (error?: { reason: unknown }): void => {
    writableInternals.setReadyHook(destination, undefined);
    writableInternals.writerRelease(writer);
    readableStreamReaderGenericRelease(reader);
    abortRegistration?.remove();
    if (error !== undefined) {
      reject(error.reason);
    } else {
      resolve();
    }
  };

  // Settles with the pipe's latest write, so that shutdownWithAction can
  // wait for every write to be acknowledged.
  let lastWriteSettled: Promise<void> = PromiseResolve(
    undefined
  ) as Promise<void>;

  // Whether a shutdown is waiting for the pipe's writes before its action.
  let waitingForWrites = false;

  // Calls `then` once the latest write has settled, waiting again if a
  // write was made meanwhile.
  const waitForWrites = (then: () => void): void => {
    const settled = lastWriteSettled;
    const check = (): void => {
      if (settled === lastWriteSettled) {
        then();
      } else {
        waitForWrites(then);
      }
    };
    PromisePrototypeThen(settled, check, check);
  };

  // Set when a write rejects NON-FATALLY (dest still writable — the
  // internal transforms' invalid-chunk contract) while a clean shutdown
  // is already waiting for write acknowledgment; runAction upgrades the
  // clean close into a pipe failure with this reason. See
  // onWriteRejectedNonFatally.
  let pendingNonFatalWriteFailure: { reason: unknown } | undefined;

  const shutdownWithAction = (
    action: (() => Promise<unknown>) | undefined,
    error?: { reason: unknown }
  ): void => {
    if (shuttingDown) return;
    shuttingDown = true;

    const runAction = (): void => {
      waitingForWrites = false;
      // A non-fatal tail-write rejection recorded during the
      // acknowledgment wait upgrades a CLEAN shutdown into a failure
      // (the C++ pipe, which awaits each write, can never reach its
      // close step past a failed write). Shutdowns that already carry
      // an error keep it — first cause wins.
      if (error === undefined && pendingNonFatalWriteFailure !== undefined) {
        const reason = pendingNonFatalWriteFailure.reason;
        const failureActions = nonFatalWriteFailureActions(reason);
        if (failureActions.length === 0) {
          finalize({ reason });
          return;
        }
        PromisePrototypeThen(
          combineShutdownActions(failureActions),
          () => finalize({ reason }),
          (actionError: unknown) => finalize({ reason: actionError })
        );
        return;
      }
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

    // Spec: if dest is writable with no close queued or in flight, write
    // the chunks that have been read and wait until every chunk that has
    // been read has been written. A read pending now can still deliver a
    // chunk; readNextChunk writes it while the wait lasts.
    const destState = writableInternals.getState(destination);
    if (
      destState === 'writable' &&
      !writableInternals.closeQueuedOrInFlight(destination)
    ) {
      waitingForWrites = true;
      waitForWrites(runAction);
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

  // Runs `actions` in parallel and settles when all have settled,
  // rejecting with the first failure (spec: shutdown actions run in
  // parallel). Shared by the abort-signal algorithm and the non-fatal
  // write-rejection path.
  const combineShutdownActions = (
    actions: (() => Promise<unknown>)[]
  ): Promise<void> => {
    let remaining = actions.length;
    let failed: { reason: unknown } | undefined;
    const all = PromiseWithResolvers() as PromiseWithResolversType<void>;
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
  };

  // The failure actions for a non-fatal write rejection: abort the
  // destination (unless preventAbort) and cancel the source (unless
  // preventCancel), both with the write's reason — the C++ pipe outcome
  // for a rejected write.
  const nonFatalWriteFailureActions = (
    e: unknown
  ): (() => Promise<unknown>)[] => {
    const actions: (() => Promise<unknown>)[] = [];
    if (!preventAbort) {
      ArrayPrototypePush(actions, () =>
        writableInternals.getState(destination) === 'writable'
          ? writableInternals.writableStreamAbort(destination, e)
          : (PromiseResolve(undefined) as Promise<void>)
      );
    }
    if (!preventCancel) {
      ArrayPrototypePush(actions, () =>
        getReadableStreamGetState(source) === 'readable'
          ? readableStreamCancel(source, e)
          : (PromiseResolve(undefined) as Promise<void>)
      );
    }
    return actions;
  };

  // Workerd extension: the internal transforms (identity, compression)
  // reject an invalid chunk's write NON-FATALLY, leaving the destination
  // writable — a state unreachable under pure WHATWG semantics, where any
  // sink rejection errors the destination (and the closed-promise
  // observer handles it). Without this, the failed chunk would be
  // silently dropped and the pipe would complete. Match the C++ pipe
  // outcome instead: treat the rejection as a pipe failure.
  const onWriteRejectedNonFatally = (e: unknown): void => {
    const actions = nonFatalWriteFailureActions(e);
    shutdownWithAction(
      actions.length === 0 ? undefined : () => combineShutdownActions(actions),
      { reason: e }
    );
  };

  // Spec: an abort algorithm, not an 'abort' listener, so a synthetic event
  // cannot abort the pipe and a user listener cannot prevent it.
  if (signal !== undefined) {
    const abortAlgorithm = (): void => {
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
          : () => combineShutdownActions(actions),
        { reason: abortReason }
      );
    };
    if (AbortSignalAbortedGet(signal)) {
      abortAlgorithm();
    } else {
      abortRegistration = utils.addAbortAlgorithm(signal, abortAlgorithm);
    }
  }

  // The spec's shutdown conditions, "applied in order":
  //   1. Source errored  →  abort dest
  //   2. Dest errored   →  cancel source
  //   3. Source closed   →  close dest
  //   4. Dest close-queued/closed  →  TypeError + cancel source
  // Checked at pipe start (terminal states shut down synchronously in this
  // priority) and before the pump reads. Returns whether the pipe is
  // shutting down.
  const checkConditions = (): boolean => {
    if (shuttingDown) return true;
    const srcState = getReadableStreamGetState(source);
    if (srcState === 'errored') {
      onSourceErrored(getReadableStreamStoredError(source));
      return true;
    }
    const destState = writableInternals.getState(destination);
    if (destState === 'errored') {
      onDestErrored(writableInternals.getStoredError(destination));
      return true;
    }
    if (srcState === 'closed') {
      onSourceDone();
      return true;
    }
    if (
      writableInternals.closeQueuedOrInFlight(destination) ||
      destState === 'closed'
    ) {
      onDestClosedEarly();
      return true;
    }
    return false;
  };
  checkConditions();

  const writeChunk = (chunk: R): void => {
    const writePromise = writableInternals.writerWrite(writer, chunk);
    // One reaction per write. A rejection that leaves the destination
    // WRITABLE is the internal transforms' non-fatal invalid-chunk
    // rejection — fail the pipe with it (see onWriteRejectedNonFatally).
    // Fatal rejections error the destination and are handled by the
    // closed-promise observer instead. When a clean shutdown is already
    // waiting for acknowledgment, record the failure for runAction's
    // upgrade path. The reaction's promise settles once the write has,
    // which is what shutdownWithAction waits for; the writable serializes
    // writes, so every earlier write has settled by then too.
    lastWriteSettled = PromisePrototypeThen(
      writePromise,
      undefined,
      (e: unknown) => {
        if (writableInternals.getState(destination) !== 'writable') return;
        if (shuttingDown) {
          pendingNonFatalWriteFailure ??= { reason: e };
          return;
        }
        onWriteRejectedNonFatally(e);
      }
    ) as Promise<void>;
  };

  // Spec: no reads while the writer's desiredSize is <= 0 or null.
  const destinationDesiresChunks = (): boolean =>
    writableInternals.getState(destination) === 'writable' &&
    !writableInternals.closeQueuedOrInFlight(destination) &&
    !writableInternals.hasBackpressure(destination);

  let pumping = false;
  let readPending = false;

  // Moves buffered chunks while the destination desires them, then reads
  // the next chunk if none is buffered. A write ends the batch
  // synchronously when a size() or sink callback errors the destination or
  // aborts the pipe; an asynchronous write rejection shuts the pipe down
  // when it arrives. Write completions reach the pump through the ready
  // hook, never while it runs.
  const pump = (): void => {
    if (pumping || readPending) return;
    pumping = true;
    try {
      while (!checkConditions() && destinationDesiresChunks()) {
        const result = pipeReadBuffered<R>(reader, source);
        if (result === undefined) {
          readNextChunk();
          return;
        }
        if (result.done) {
          onSourceDone();
          return;
        }
        writeChunk(result.value as R);
      }
    } finally {
      pumping = false;
    }
  };

  const readNextChunk = (): void => {
    readPending = true;
    PromisePrototypeThen(
      defaultReaderReadInternal<R>(reader, source),
      (result: ReadableStreamReadResult<R>) => {
        readPending = false;
        if (shuttingDown) {
          // A chunk read while the shutdown waits for writes is written,
          // and the wait extends to its write. Once the wait is over the
          // chunk is dropped. The result arrives asynchronously, so when
          // the shutdown has no write to wait for, even a chunk enqueued
          // in the shutdown's turn arrives too late.
          if (
            waitingForWrites &&
            !result.done &&
            writableInternals.willAcceptWrite(destination)
          ) {
            writeChunk(result.value as R);
          }
          return;
        }
        if (result.done) {
          onSourceDone();
          return;
        }
        writeChunk(result.value as R);
        pump();
      },
      (e: unknown) => {
        readPending = false;
        if (!shuttingDown) onSourceErrored(e);
      }
    );
  };

  // ---- Reactive shutdown triggers (spec: "in parallel") ----
  // Forward close/error propagation from the source. A close reaches the
  // pump, which propagates it once the chunk of a pending read is written.
  PromisePrototypeThen(
    getGenericReaderClosedPromise(reader),
    pump,
    (e: unknown) => {
      if (!shuttingDown) onSourceErrored(e);
    }
  );

  // Backward error propagation: destination errors cancel the source.
  PromisePrototypeThen(
    writableInternals.getWriterClosedPromise(writer),
    undefined,
    (e: unknown) => {
      if (!shuttingDown) onDestErrored(e);
    }
  );

  // The first pump runs once the writer is ready; later ones from the
  // ready hook and from reads.
  if (!shuttingDown) {
    writableInternals.setReadyHook(destination, pump);
    PromisePrototypeThen(
      writableInternals.getWriterReadyPromise(writer),
      pump,
      () => {}
    );
  }

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
  // during controller setup, removed on cancel/tee. A queued stream that
  // has been teed (the source's own stream, or a branch teed again) keeps
  // none: its former consumer's place in the queue went to the two
  // branches, and it is left permanently locked. The source's own stream
  // still closes and errors with the source; a branch teed again is closed
  // at the tee (see readableStreamTee).
  #consumer?: StreamConsumerType<R> | undefined;
  #disturbed: boolean = false;
  #state: 'readable' | 'closed' | 'errored' = 'readable';
  #storedError?: unknown;
  // The Node.js interop closed-promise (see kIsClosedPromise), created on
  // first request and settled by readableStreamClose/readableStreamError.
  // Terminal-state shells (tee and detach copies) never transition, so a
  // request against one is settled immediately from its state.
  #closedPromise?: PromiseWithResolversType<void> | undefined;
  // A transform pair's notification when the Node.js interop hook errors
  // this half (internalsForTransform.setInteropErrorHook). Dropped when the
  // stream leaves 'readable', so a settled half does not retain the pair.
  #interopErrorHook?: ((reason: unknown) => void) | undefined;
  // The pending-closure gate (JsReadableStream::setPendingClosure): set by
  // the stream's owning object (a Socket) the moment its closure begins, so
  // that new reads, pipes, and tees fail fast with a descriptive error
  // instead of racing the teardown. Mirrors the legacy internal controller's
  // isPendingClosure checks; cancel and the teardown's own operations are
  // deliberately not gated. Carried by detach() to the detached stream.
  #pendingClosure: boolean = false;
  // The C++ bridge's EOF-signal resolver (JsReadableStream::onEof), armed by
  // getReadableStreamOnEof and fired by the native backend's source-driven
  // close hook (see the closeStream wiring in the constructor's native
  // branch). Only native-backed streams ever fire it: the legacy JS
  // controller never signals EOF, and the queued backend matches that.
  // Not carried by detach(): the husk's subscription stays dormant, like the
  // legacy eofResolverPair, which remains with the husk after a detach.
  #onEofResolver?: (() => void) | undefined;

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

    setReadableStreamInteropErrorHook = (stream, hook) => {
      stream.#interopErrorHook = hook;
    };

    closeReadableStreamHusk = (stream) => {
      stream.#controller = undefined;
      stream.#consumer = undefined;
      readableStreamClose(stream);
    };

    isReadableStreamPendingClosure = <R>(stream: ReadableStream<R>) => {
      return stream.#pendingClosure;
    };

    getReadableStreamOnEof = <R>(stream: ReadableStream<R>) => {
      // The EOF signal for the sockets API (allowHalfOpen: false teardown):
      // resolves when a native source's EOF is observed through the conduit
      // (see the closeStream hook wiring in the constructor's native
      // branch). At most one subscription per stream (the C++ caller's
      // precondition); arming after the stream already closed never
      // resolves, matching the legacy signalEof/eofResolverPair behavior.
      // Queued streams never fire it (the legacy JS controller never
      // signals EOF).
      const { promise, resolve } =
        PromiseWithResolvers() as PromiseWithResolversType<void>;
      stream.#onEofResolver = resolve;
      return promise;
    };

    getReadableStreamExpectedLength = <R>(stream: ReadableStream<R>) => {
      const controller = stream.#controller;
      if (controller === undefined) return undefined;
      return getControllerExpectedLength(controller);
    };

    getReadableStreamNativeSource = <R>(stream: ReadableStream<R>) => {
      const controller = stream.#controller;
      if (controller !== undefined && isNativeController(controller)) {
        return nativeControllerPeekSource(controller);
      }
      return undefined;
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
      const controller = stream.#controller;
      let cancelPromise: Promise<void> = PromiseResolve() as Promise<void>;
      // BACKEND-BLIND: the STREAM layer owns the source-cancel POLICY (the
      // controller's consumer-leaving rule: only the last consumer of a
      // teed queue cancels the source, with every departed consumer's
      // reason; the others wait for it) and hands it to the consumer, which
      // owns the teardown MECHANICS (resolve reads as done, ordering vs
      // removal, last-consumer determination). See
      // StreamConsumer.cancelStream in queue.ts.
      const decideSourceCancel = (isLastConsumer: boolean): Promise<void> => {
        if (controller === undefined) {
          return PromiseResolve() as Promise<void>;
        }
        return controllerConsumerLeaving(controller, reason, isLastConsumer);
      };
      if (consumer !== undefined) {
        cancelPromise = consumer.cancelStream(reason, decideSourceCancel);
        stream.#consumer = undefined;
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
      options: ConvertedPipeOptions
    ) => {
      // The pending-closure gate (see #pendingClosure). The prototype
      // pipeThrough reaches pipeToInternal through here WITHOUT passing
      // readableStreamPipeTo's precondition block, so the gate applies
      // again at this junction. Like the legacy controller's gate (which
      // rejects before any locking), the rejection happens before
      // pipeToInternal locks the endpoints or touches the transform; the
      // prototype method marks the pipe promise handled, so this surfaces
      // exactly like legacy -- endpoints untouched, hidden rejection. It
      // is deliberately a rejection rather than a throw for the same
      // reason. (The pipeTo route re-checks harmlessly; its own gate also
      // guards the native+native fast path, which returns before reaching
      // here.)
      if (source.#pendingClosure) {
        return PromiseReject(pendingClosureError()) as Promise<void>;
      }
      return pipeToInternal(source, destination, options);
    };

    // The shared pipeTo implementation, used by both the prototype method
    // and the C++ bridge entry point (JsReadableStream::pipeTo dispatches
    // here via cppExports rather than through the user-patchable pipeTo
    // property -- the same captured-call discipline as cancel/tee). The
    // brand assert lives in the prototype method; the C++ arm passes
    // genuine handles by construction.
    readableStreamPipeTo = <R>(
      source: ReadableStream<R>,
      destination: WritableStreamType<R>,
      options: unknown = kEmptyDictionary
    ): Promise<void> => {
      try {
        // WebIDL argument conversion first: the destination brand check,
        // then the options, all before the locked checks and before the
        // fast path below permanently consumes both endpoints. Both paths
        // receive the converted values and never re-read the user's object.
        if (!writableInternals.isWritableStream(destination)) {
          throw new TypeError(
            "Failed to execute 'pipeTo': destination is not a WritableStream"
          );
        }
        const converted = convertPipeOptions(options);
        const { preventAbort, preventCancel, preventClose } = converted;
        if (isReadableStreamLocked(source)) {
          throw new TypeError('Cannot pipe a stream that is locked');
        }
        if (writableInternals.isWritableStreamLocked(destination)) {
          throw new TypeError('Cannot pipe to a locked writable stream');
        }
        if (source.#pendingClosure) {
          throw pendingClosureError();
        }

        // PIPE DISPATCH: if both source and dest are native-backed, take
        // the fast path -- extract both and let the sink's pipeFrom hook
        // arrange the pipe entirely at the C++ layer. Both markers are
        // own-property reads (non-destructive); extraction happens only
        // after both are confirmed. If both are native, pipeFrom MUST
        // exist (invariant -- its absence is a contract violation, not a
        // fallback trigger).
        //
        // Because extraction permanently consumes both endpoints, the fast
        // path is additionally gated on (a) no prevent* option -- the
        // legacy internal pipe leaves the un-prevented endpoint unlocked
        // and usable after the pipe settles (e.g. the socket-concatenation
        // pattern: body1.pipeTo(sock.writable, {preventClose: true})
        // followed by body2.pipeTo(sock.writable)), which extraction
        // cannot honor -- (b) both endpoints in their normal flowing
        // states, so pipes involving closed/errored endpoints reject with
        // the spec-mandated stored errors, and (c) no write queued or in
        // flight on the destination, since extraction would move the
        // native sink out from under it (e.g. a header written without
        // awaiting it, then the writer released). The JS pump handles all
        // of those cases (its writes queue behind the destination's
        // pending ones, and it releases both locks in its finalize).
        // TODO(streams-ts): revisit extending the fast path to the
        // prevent* options (e.g. reversible extraction) so those pipes can
        // also run entirely at the C++ layer.
        // The destination is brand-checked above, so a Proxy's
        // getOwnPropertyDescriptor trap cannot observe the symbol.
        const sourceExtractor = ObjectGetOwnPropertyDescriptor(
          source,
          kExtractNativeSource
        )?.value as ((this: ReadableStream<R>) => object) | undefined;
        const sinkExtractor = ObjectGetOwnPropertyDescriptor(
          destination,
          kExtractNativeSink
        )?.value as ((this: object) => object) | undefined;
        if (
          sourceExtractor !== undefined &&
          sinkExtractor !== undefined &&
          !preventAbort &&
          !preventCancel &&
          !preventClose &&
          !source.#disturbed &&
          source.#state === 'readable' &&
          writableInternals.getState(destination) === 'writable' &&
          // Closing must be propagated backward (and extraction would move
          // the native sink out from under its in-flight end()): a
          // close-queued destination takes the JS pump, which rejects it.
          !writableInternals.closeQueuedOrInFlight(destination) &&
          // A write queued before the controller started would reach the
          // extracted sink and be dropped, and pipeFrom() refuses a sink
          // with a write in flight.
          !writableInternals.writeQueuedOrInFlight(destination)
        ) {
          // Captured-call discipline: Function.prototype.call is patchable,
          // so re-bind both extractors and the hook through uncurryThis
          // (captured Reflect machinery) instead of .call().
          const nativeSource = uncurryThis(sourceExtractor)(source);
          const nativeSink = uncurryThis(sinkExtractor)(destination) as Record<
            string,
            unknown
          >;
          const pipeFrom = nativeSink.pipeFrom as
            | ((source: object, opts: ConvertedPipeOptions) => Promise<void>)
            | undefined;
          if (pipeFrom === undefined) {
            throw new TypeError(
              'Native sink is missing the required pipeFrom hook'
            );
          }
          return uncurryThis(pipeFrom)(nativeSink, nativeSource, converted);
        }
        return readableStreamPipeThroughTo(source, destination, converted);
      } catch (e) {
        return PromiseReject(e) as Promise<void>;
      }
    };

    readableStreamErrorBranch = <R>(
      stream: ReadableStream<R>,
      reason: unknown
    ) => {
      if (stream.#state !== 'readable') return;
      const controller = stream.#controller;
      if (controller === undefined) return;
      // QUEUED INVARIANT: a tee branch of a queued stream — its consumer is
      // necessarily a QueueCursor (tee precedent); sanctioned cast.
      const cursor = stream.#consumer as QueueCursorType<R, R> | undefined;
      if (cursor === undefined) return;
      stream.#consumer = undefined;
      cursor.errorAllReads(reason);
      readableStreamError(stream, reason);
      // Decided BEFORE the cursor's removal, for the reasons given at
      // #consumerLeaving (as in QueueCursor.cancelStream). The controller is
      // queued: its branch's consumer is a QueueCursor (above).
      markPromiseHandled(
        controllerConsumerErrored(
          controller as
            | ReadableStreamDefaultControllerType
            | ReadableByteStreamControllerType,
          reason,
          cursor.queue.cursorCount === 1
        )
      );
      cursor.queue.removeCursor(cursor);
    };

    // BACKEND-DISPATCH: tee is one of the five sanctioned dispatch points
    // (native-stream-integration.md §10). The native branch runs first:
    // the source's tee hook produces a PAIR of new native source objects
    // (leaving the original source closed), each wrapped in a fresh
    // ReadableStream via ordinary construction. Branches are fully
    // independent — no shared consumer and no composite-cancel wiring
    // (deliberate divergence from the queued model below: branch cancels
    // go to each branch's OWN source). The parent is left locked and
    // closed (closeReadableStreamHusk).
    readableStreamTee = <R>(stream: ReadableStream<R>) => {
      // The locked precondition lives HERE (not in the prototype method) so that every
      // entry point shares it -- the method after its brand assert, and the C++
      // JsReadableStream::tee arm via cppExports, which calls this directly.
      if (isReadableStreamLocked(stream)) {
        throw new TypeError('Cannot tee a stream that is locked');
      }
      if (stream.#pendingClosure) {
        throw pendingClosureError();
      }
      const controller = stream.#controller;
      if (isNativeController(controller)) {
        if (stream.#state !== 'readable') {
          // Closed/errored native parents produce two branches in the
          // same state and are locked, mirroring the queued behavior
          // below — without touching the (closed) source.
          const b1 = new ReadableStream<R>(kPrivateSymbol as never);
          const b2 = new ReadableStream<R>(kPrivateSymbol as never);
          b1.#state = stream.#state;
          b2.#state = stream.#state;
          b1.#storedError = stream.#storedError;
          b2.#storedError = stream.#storedError;
          acquireReadableStreamDefaultReader(stream);
          return [b1, b2] as [ReadableStream<R>, ReadableStream<R>];
        }
        // Sources from the tee hook are full native sources; ordinary
        // construction validates and wires each branch.
        const sources = nativeControllerTeeSource(controller);
        const branch1 = new ReadableStream<R>(
          sources[0] as UnderlyingSource<R>
        );
        const branch2 = new ReadableStream<R>(
          sources[1] as UnderlyingSource<R>
        );
        if (!isReadableStreamLocked(stream)) {
          acquireReadableStreamDefaultReader(stream);
        }
        closeReadableStreamHusk(stream);
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
        // all-cursors-gone hook mid-tee, dropping the buffered entries and
        // releasing the source.
        const totalSize = cursor.remainingSize;
        const wasSole = queue.singleCursor === cursor;
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
        if (isBytes) {
          const from = cursor as unknown as ByteStreamCursorType;
          const to1 = branch1.#consumer as unknown as ByteStreamCursorType;
          const to2 = branch2.#consumer as unknown as ByteStreamCursorType;
          to1.adoptReleasedBytes(from);
          to2.adoptReleasedBytes(from);
          // A branch shares the controller with its siblings, so a
          // fractional fill at close errors it alone.
          to1.errorStreamCallback = errorTeeBranchFromCursor;
          to2.errorStreamCallback = errorTeeBranchFromCursor;
          byteControllerOnFork(
            controller as ReadableByteStreamController,
            from,
            wasSole
          );
        }
        queue.removeCursor(cursor);
        stream.#consumer = undefined;
        // With close already requested, the source's own stream — now
        // consuming nothing — closes here rather than when a branch next
        // reads (see #maybeCloseStream).
        if (controller !== undefined) controllerMaybeCloseStream(controller);
      }

      // Cancellation needs no wiring of its own: the branches are now two
      // consumers of the shared queue like any others, and the controller's
      // consumer-leaving rule (controllerConsumerLeaving) cancels the source
      // once the last of them has left — tees of tee branches simply add
      // consumers to the same queue.

      // Per spec, tee() locks the original permanently. Acquire a real
      // reader (never exposed, so it can never be released).
      if (!isReadableStreamLocked(stream)) {
        acquireReadableStreamDefaultReader(stream);
      }
      // A teed branch (not the source's own stream) consumes nothing now,
      // and no source event would ever reach it: close it.
      if (controller !== undefined && controllerStream(controller) !== stream) {
        closeReadableStreamHusk(stream);
      }

      return [branch1, branch2] as [ReadableStream<R>, ReadableStream<R>];
    };

    // The C++ bridge arm of JsReadableStream::detach(): take over the
    // stream's internal state into a fresh stream, leaving the original a
    // permanently locked, disturbed husk (the "create a proxy" step of the
    // fetch spec, implemented as a state transfer instead of an identity
    // pipe). The transfer mechanics follow tee's: state-copy shells for
    // non-readable streams, cursor transfer on the shared queue for the
    // queued backend, and the extraction machinery for the native backend
    // (a fresh conduit is REQUIRED there -- the old conduit's stream hooks
    // close over the original stream, so moving it would route source
    // close/error to the husk).
    detachReadableStream = <R>(
      stream: ReadableStream<R>,
      ignoreDisturbed: boolean
    ): ReadableStream<R> => {
      assertIsReadableStream(stream);
      // Precondition order and error texts match the legacy
      // ReadableStream::detach().
      if (stream.#disturbed && !ignoreDisturbed) {
        throw new TypeError('The ReadableStream has already been read.');
      }
      if (isReadableStreamLocked(stream)) {
        throw new TypeError('The ReadableStream has been locked to a reader.');
      }

      // Neutralize the original: no consumer, disturbed, permanently locked
      // via an internal reader (never exposed, never released) -- the same
      // pattern extraction and tee use. A native husk is disturbed, locked
      // and closed instead (closeReadableStreamHusk drops its consumer); a
      // queued one stays the controller's stream, closing and erroring with
      // the source.
      const neutralize = (): void => {
        stream.#consumer = undefined;
        stream.#disturbed = true;
        if (!isReadableStreamLocked(stream)) {
          acquireReadableStreamDefaultReader(stream);
        }
      };

      // Closed/errored: the detached stream is a state-copy shell (tee's
      // non-readable precedent); the underlying source is not touched.
      if (stream.#state !== 'readable') {
        const shell = new ReadableStream<R>(kPrivateSymbol as never);
        shell.#state = stream.#state;
        shell.#storedError = stream.#storedError;
        shell.#pendingClosure = stream.#pendingClosure;
        neutralize();
        return shell;
      }

      const controller = stream.#controller;
      if (controller !== undefined && isNativeController(controller)) {
        // Native-backed: extract the source and construct a fresh stream
        // over it through ordinary native construction (fresh conduit,
        // fresh stream hooks capturing the NEW stream; matches the legacy
        // internal controller's detach, which builds a fresh controller
        // over the removed source). expectedLength is re-read live from
        // the source, so residual accounting -- including any stashed
        // bytes -- stays exact.
        const source = nativeControllerExtractSource(controller);
        stream.#disturbed = true;
        if (!isReadableStreamLocked(stream)) {
          acquireReadableStreamDefaultReader(stream);
        }
        closeReadableStreamHusk(stream);
        const detached = new ReadableStream<R>(source as UnderlyingSource<R>);
        detached.#pendingClosure = stream.#pendingClosure;
        return detached;
      }

      // Queued (JS-backed): tee's transfer recipe, moving rather than
      // forking -- a fresh shell adopts the SHARED controller and a cursor
      // at the original cursor's exact position (partial entry consumption
      // survives the move).
      const shell = new ReadableStream<R>(kPrivateSymbol as never);
      shell.#controller = controller;
      shell.#pendingClosure = stream.#pendingClosure;

      // QUEUED INVARIANT: this branch is queued-backend territory -- the
      // consumer is necessarily a QueueCursor (position/byteOffset/queue
      // are cursor-only concepts); sanctioned cast (tee precedent).
      const cursor = stream.#consumer as QueueCursorType<R, R> | undefined;
      if (cursor !== undefined) {
        const queue = cursor.queue;
        const isBytes =
          controller !== undefined && isByteStreamController(controller);
        const totalSize = cursor.remainingSize;
        const wasSole = queue.singleCursor === cursor;
        // ORDER MATTERS: attach the shell's cursor BEFORE removing the
        // original -- removing the sole cursor first would fire the
        // all-cursors-gone hook mid-detach, dropping the buffered entries
        // and releasing the source (tee precedent).
        shell.#consumer = isBytes
          ? new ByteStreamCursor(
              queue,
              shell,
              cursor.position,
              cursor.byteOffset,
              totalSize
            )
          : new QueueCursor(
              queue,
              shell,
              cursor.position,
              cursor.byteOffset,
              totalSize
            );
        if (isBytes) {
          const from = cursor as unknown as ByteStreamCursorType;
          const to = shell.#consumer as unknown as ByteStreamCursorType;
          to.adoptReleasedBytes(from);
          to.errorStreamCallback = from.errorStreamCallback;
          byteControllerOnFork(
            controller as ReadableByteStreamController,
            from,
            wasSole
          );
        }
        queue.removeCursor(cursor);
      }
      neutralize();
      // As in tee: with close already requested, the husk closes now if it
      // is the source's own stream (see #maybeCloseStream).
      if (controller !== undefined) controllerMaybeCloseStream(controller);
      return shell;
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

    settleReadableStreamClosedPromise = <R>(stream: ReadableStream<R>) => {
      const closed = stream.#closedPromise;
      if (closed === undefined) return;
      if (stream.#state === 'closed') {
        closed.resolve();
      } else if (stream.#state === 'errored') {
        closed.reject(stream.#storedError);
      }
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

    // A native conduit has a single consumer: its leaving is its cancel.
    const prevConsumerLeaving = controllerConsumerLeaving;
    controllerConsumerLeaving = (controller, reason, isLastConsumer) => {
      if (isNativeController(controller)) {
        return nativeControllerCancelSteps(controller, reason);
      }
      return prevConsumerLeaving(controller, reason, isLastConsumer);
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

    const prevControllerError = controllerError;
    controllerError = (controller, reason) => {
      if (isNativeController(controller)) {
        nativeControllerError(controller, reason);
      } else {
        prevControllerError(controller, reason);
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
      // Atomic: validate -> extract -> lock+disturb+close. No TOCTOU gap.
      const source = nativeControllerExtractSource(controller);
      this.#disturbed = true;
      // Permanent lock via an internal reader (never exposed, never
      // released) — same pattern as tee's parent locking.
      acquireReadableStreamDefaultReader(this);
      closeReadableStreamHusk(this);
      return source;
    };
  }

  constructor(
    underlyingSource: UnderlyingSource<R> = kEmptyDictionary as UnderlyingSource<R>,
    strategy: QueuingStrategy<R> = kEmptyDictionary as QueuingStrategy<R>
  ) {
    // The C++-recognition brand (see kReadableStreamBrand). Stamped before
    // the early returns below so every instance carries it: internal
    // shells and native-backed streams included.
    ObjectDefineProperty(this, kReadableStreamBrand, {
      __proto__: null,
      value: true,
    } as PropertyDescriptor);

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
          closeStream: () => {
            readableStreamClose(this);
            // The C++ bridge's EOF signal (JsReadableStream::onEof): the
            // conduit calls this hook only for SOURCE-driven closes, i.e.
            // whenever the native source's EOF is observed through the
            // conduit -- reader reads, async iteration, and DrainingReader
            // consumption alike. Cancel and error take other paths, and
            // extraction-based pumps detach the source before its EOF could
            // be observed here, so none of those fire the signal.
            const resolveEof = this.#onEofResolver;
            if (resolveEof !== undefined) {
              this.#onEofResolver = undefined;
              resolveEof();
            }
          },
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
    options: { mode?: 'byob' } | null = kEmptyDictionary
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
    // The default keeps Function.length at 1 (IDL harness check).
    options: StreamPipeOptions = kEmptyDictionary as StreamPipeOptions
  ): ReadableStreamType<T> {
    assertIsReadableStream(this);
    // WebIDL argument conversion precedes the locked checks, so no user
    // code runs between those checks and pipeToInternal taking the locks.
    // Dictionary members are read in alphabetical order: "readable" is read
    // and brand-checked before "writable" (WPT pipe-through.any.js).
    const readable = transform.readable;
    if (!isReadableStream(readable)) {
      throw new TypeError(
        "Failed to execute 'pipeThrough': readable is not a ReadableStream"
      );
    }
    const writable = transform.writable;
    if (!writableInternals.isWritableStream(writable)) {
      throw new TypeError(
        "Failed to execute 'pipeThrough': writable is not a WritableStream"
      );
    }
    const converted = convertPipeOptions(options);
    if (isReadableStreamLocked(this)) {
      throw new TypeError('Cannot pipe a stream that is locked');
    }
    if (writableInternals.isWritableStreamLocked(writable)) {
      throw new TypeError('Cannot pipe to a locked writable stream');
    }
    const promise = readableStreamPipeThroughTo(this, writable, converted);
    markPromiseHandled(promise);
    return readable;
  }

  pipeTo(
    destination: WritableStream<R>,
    // WebIDL: optional dictionary — null/undefined both become {}.
    // The default keeps Function.length at 1 (IDL harness check).
    options: StreamPipeOptions = kEmptyDictionary as StreamPipeOptions
  ): Promise<void> {
    try {
      assertIsReadableStream(this);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
    // The shared implementation (also the C++ bridge entry point) carries
    // the locked/options preconditions and the pipe dispatch.
    return readableStreamPipeTo(this, destination, options);
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
          defaultControllerEnqueue(controller, chunk);
          defaultControllerClose(controller);
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
        if (!isObjectLike(asyncIterator)) {
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
              if (!isObjectLike(next)) {
                throw new TypeError('The result of next() must be an object');
              }
              if (next.done) {
                return defaultControllerClose(controller);
              }
              defaultControllerEnqueue(controller, next.value);
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
              if (!isObjectLike(ret)) {
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
        if (!isObjectLike(syncIterator)) {
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
              if (!isObjectLike(next)) {
                throw new TypeError('The result of next() must be an object');
              }
              // Await the value: the async-from-sync iterator wrapper
              // resolves each value through PromiseResolve, which
              // awaits thenables (including Promises).
              const value = await next.value;
              if (next.done) {
                defaultControllerClose(controller);
                return;
              }
              defaultControllerEnqueue(controller, value as R);
            },
            async cancel(reason?: unknown) {
              const returnMethod = syncIterator.return;
              if (returnMethod === undefined) return;
              if (typeof returnMethod !== 'function') {
                throw new TypeError('Iterator return() is not a function');
              }
              const ret = uncurryThis(returnMethod)(syncIterator, reason);
              if (!isObjectLike(ret)) {
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

  values(
    options: { preventCancel?: boolean } = kEmptyDictionary
  ): AsyncIterableIterator<R> {
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

  // Node.js interop (see kIsClosedPromise): an object whose promise settles
  // with the stream — fulfilled on close, rejected with the stored error.
  // Nothing may ever look at the rejection, so it is marked handled.
  get [kIsClosedPromise](): { promise: Promise<void> } {
    assertIsReadableStream(this);
    let closed = this.#closedPromise;
    if (closed === undefined) {
      closed = PromiseWithResolvers() as PromiseWithResolversType<void>;
      markPromiseHandled(closed.promise);
      this.#closedPromise = closed;
      settleReadableStreamClosedPromise(this);
    }
    return { promise: closed.promise };
  }

  // Node.js interop (see kControllerErrorFunction): errors a readable stream
  // from outside, as its controller's error() does — pending reads reject
  // and the state becomes errored.
  //
  // The source's own stream errors through its controller, which errors
  // every consumer of the queue (the tee branches, if any); a native-backed
  // stream also cancels its C++ source, which has lost its consumer. A
  // queued source's cancel steps do not run, so a transform pair learns of
  // it through its interop error hook instead, once this half has errored:
  // the entry half always goes first. A queued tee branch shares that
  // controller, so it errors alone: its pending reads reject and it leaves
  // the queue as a cancelled branch would — the source is cancelled once
  // no consumer remains, with the reason of every consumer that left
  // (controllerConsumerLeaving) — unless the source has already requested
  // close, which no cancel follows (controllerConsumerErrored). A branch
  // that has itself been teed was closed by tee() (closeReadableStreamHusk),
  // so the hook does nothing to it.
  [kControllerErrorFunction](reason: unknown): void {
    assertIsReadableStream(this);
    if (this.#state !== 'readable') return;
    const controller = this.#controller;
    if (controller === undefined) {
      readableStreamError(this, reason);
      return;
    }
    if (isNativeController(controller)) {
      controllerError(controller, reason);
      markPromiseHandled(controllerCancelSteps(controller, reason));
      return;
    }
    if (controllerStream(controller) === this) {
      // Taken before the error, which drops the slot (readableStreamError);
      // like the writable's abort hook, it fires once.
      const hook = this.#interopErrorHook;
      this.#interopErrorHook = undefined;
      controllerError(controller, reason);
      if (hook !== undefined) hook(reason);
      return;
    }
    readableStreamErrorBranch(this, reason);
  }
}

// Body consumption for the C++ bridge (arrayBuffer/bytes/text/json). Two
// byte bounds apply: the caller's memory limit, capped at 128 MB, and the
// stream's declared expectedLength. A breach names its cause and cancels
// the stream with it, as the C++ AllReader does; a declaration the limit
// cannot hold is refused before a byte is read.
const kMaximumAllowedLimit = 128n * 1024n * 1024n;

function acquireReadableStreamDrainingReader<R>(
  stream: ReadableStream<R>
): ReadableStreamDrainingReader<R> {
  return new ReadableStreamDrainingReader<R>(stream);
}

// The collected bytes, copied out of the drained chunks as they arrive so
// nothing is retained per chunk. Blocks grow geometrically from the first
// chunk's size to kCollectBlockSize; a declared length that fits one block
// is allocated whole up front, so a body that meets it fills exactly one
// block and is handed over without a further copy.
const kCollectBlockSize = 1024 * 1024;
const kStreamingDecode = ObjectFreeze({ __proto__: null, stream: true });

class CollectedBytes {
  #blocks: Uint8Array[] = []; // full
  #block: Uint8Array | undefined = undefined; // being filled
  #blockSize = 0; // of the most recent block
  #filled = 0; // of #block
  #length = 0;
  readonly #firstBlockSize: number; // 0: size to the first chunk

  constructor(firstBlockSize: number) {
    this.#firstBlockSize = firstBlockSize;
  }

  get length(): number {
    return this.#length;
  }

  append(
    buffer: ArrayBufferLike,
    byteOffset: number,
    byteLength: number
  ): void {
    while (byteLength > 0) {
      const block = this.#block ?? this.#addBlock(byteLength);
      const take = MathMin(byteLength, this.#blockSize - this.#filled);
      TypedArrayPrototypeSet(
        block,
        new Uint8Array(buffer, byteOffset, take),
        this.#filled
      );
      this.#filled += take;
      this.#length += take;
      byteOffset += take;
      byteLength -= take;
      if (this.#filled === this.#blockSize) {
        ArrayPrototypePush(this.#blocks, block);
        this.#block = undefined;
      }
    }
  }

  #addBlock(needed: number): Uint8Array {
    let size: number;
    if (this.#length === 0 && this.#firstBlockSize !== 0) {
      size = this.#firstBlockSize;
    } else {
      size = MathMin(kCollectBlockSize, MathMax(this.#blockSize * 2, needed));
    }
    const block = new Uint8Array(size);
    this.#block = block;
    this.#blockSize = size;
    this.#filled = 0;
    return block;
  }

  // Every block as a view of its bytes, the partial one trimmed.
  #views(): Uint8Array[] {
    const blocks = this.#blocks;
    const views: Uint8Array[] = [];
    for (let i = 0; i < blocks.length; i++) {
      ArrayPrototypePush(views, blocks[i] as Uint8Array);
    }
    const block = this.#block;
    if (block !== undefined && this.#filled > 0) {
      ArrayPrototypePush(
        views,
        new Uint8Array(TypedArrayPrototypeGetBuffer(block), 0, this.#filled)
      );
    }
    return views;
  }

  toArrayBuffer(): ArrayBuffer {
    const blocks = this.#blocks;
    if (this.#block === undefined && blocks.length === 1) {
      return TypedArrayPrototypeGetBuffer(blocks[0] as Uint8Array);
    }
    const result = new ArrayBuffer(this.#length);
    const out = new Uint8Array(result);
    const views = this.#views();
    let offset = 0;
    for (let i = 0; i < views.length; i++) {
      const view = views[i] as Uint8Array;
      TypedArrayPrototypeSet(out, view, offset);
      offset += TypedArrayPrototypeGetByteLength(view);
    }
    return result;
  }

  toText(): string {
    if (this.#length === 0) return '';
    const decoder = new TextDecoder();
    const views = this.#views();
    if (views.length === 1) return TextDecoderDecode(decoder, views[0]);
    let result = '';
    for (let i = 0; i < views.length; i++) {
      result += TextDecoderDecode(decoder, views[i], kStreamingDecode);
    }
    return result + TextDecoderDecode(decoder);
  }
}

// Cancels the stream with a consumption failure and rethrows it. A cancel
// that rejects replaces it, as in the C++ AllReader.
async function failCollect(reader: object, error: Error): Promise<never> {
  await cancelReadableStreamGenericReader(reader, error);
  throw error;
}

async function collectChunks<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<CollectedBytes> {
  if (isReadableStreamUnusable(stream)) {
    throw new TypeError('Cannot consume a stream that is locked or disturbed');
  }
  const reader = acquireReadableStreamDrainingReader(stream);
  if (limit > kMaximumAllowedLimit) limit = kMaximumAllowedLimit;
  // The declaration is the exact total the stream will deliver, so one
  // beyond the limit settles the outcome before a byte is read.
  const declared = getReadableStreamExpectedLength(stream);
  if (declared !== undefined && declared > limit) {
    return failCollect(
      reader,
      new TypeError('Memory limit would be exceeded before EOF.')
    );
  }
  const declaredBinds = declared !== undefined;
  const bound = Number(declaredBinds ? declared : limit);
  const collected = new CollectedBytes(
    declaredBinds && bound <= kCollectBlockSize ? bound : 0
  );
  while (true) {
    const result = await drainingReaderReadInternal<R>(reader, stream);
    const drained = result.chunks as unknown[];
    for (let i = 0; i < drained.length; i++) {
      const chunk = drained[i];
      // Drained chunks are untrusted values: any BufferSource contributes
      // its bytes, with the extent pinned at drain time; anything else
      // fails with the same TypeError the C++ bridge pump uses. Detached
      // or out-of-bounds inputs are skipped with the other empties (see
      // view-extent.ts).
      let buffer: ArrayBufferLike;
      let byteOffset: number;
      let byteLength: number;
      if (isArrayBufferView(chunk)) {
        const extent = viewByteExtent(chunk);
        buffer = extent.buffer;
        byteOffset = extent.byteOffset;
        byteLength = extent.byteLength;
      } else if (isArrayBuffer(chunk)) {
        buffer = chunk;
        byteOffset = 0;
        byteLength = ArrayBufferPrototypeByteLengthGet(chunk);
      } else {
        return failCollect(
          reader,
          new TypeError('This ReadableStream did not return bytes.')
        );
      }
      if (byteLength === 0) continue;
      if (collected.length + byteLength > bound) {
        return failCollect(
          reader,
          declaredBinds
            ? new RangeError(
                'stream delivered more bytes than its declared expectedLength'
              )
            : new TypeError('Memory limit exceeded before EOF.')
        );
      }
      collected.append(buffer, byteOffset, byteLength);
    }
    if (result.done) return collected;
  }
}

async function consumeReadableStreamAsArrayBuffer<R>(
  stream: ReadableStream<R>,
  limit: bigint
): Promise<ArrayBuffer> {
  return (await collectChunks(stream, limit)).toArrayBuffer();
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
  return (await collectChunks(stream, limit)).toText();
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
  // WebIDL: the same function object as values(), not enumerable.
  [SymbolAsyncIterator]: {
    __proto__: null,
    value: ReadableStream.prototype.values,
    writable: true,
    enumerable: false,
    configurable: true,
  },
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

// Captured controller operations for INTERNAL stream production (from() and
// the C++ iterable-body arm via the cppExports below). The prototype is
// user-reachable once the class is installed as a global, so internal
// production must not dispatch through it -- per WHATWG, from() uses
// internal controller operations, unaffected by prototype patching.
const defaultControllerEnqueue = uncurryThis(
  ReadableStreamDefaultController.prototype.enqueue
) as (controller: object, chunk: unknown) => void;
const defaultControllerClose = uncurryThis(
  ReadableStreamDefaultController.prototype.close
) as (controller: object) => void;
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
  // Internal controller operations for the C++ iterable-body arm
  // (JsReadableStream::from): production must not dispatch through the
  // user-patchable controller prototype. See defaultControllerEnqueue.
  readableControllerEnqueue: (controller: object, chunk: unknown): void =>
    defaultControllerEnqueue(controller, chunk),
  readableControllerClose: (controller: object): void =>
    defaultControllerClose(controller),
  consumeReadableStreamAsArrayBuffer,
  consumeReadableStreamAsJSON,
  consumeReadableStreamAsText,
  consumeReadableStreamAsUint8Array,
  detachReadableStream,
  getReadableStreamExpectedLength,
  getReadableStreamNativeSource,
  getReadableStreamIsDisturbed,
  getReadableStreamOnEof,
  isReadableStream,
  isReadableStreamLocked,
  readableStreamCancel,
  readableStreamPipeTo,
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
  // Internal operations consumed by the transform pairs (transform.ts,
  // identity.ts, compression.ts): the TransformStream cancel/flush
  // coordination (finishPromise guard), the workerd expectedLength
  // extension, and the interop error hook. Unreachable from user code.
  internalsForTransform: ObjectFreeze({
    getState: <R>(stream: ReadableStream<R>) =>
      getReadableStreamGetState(stream),
    getStoredError: <R>(stream: ReadableStream<R>) =>
      getReadableStreamStoredError(stream),
    normalizeExpectedLength,
    // The identity streams' delivery (see identity.ts): a batched enqueue
    // with one notification, and the queue's consumption notification.
    enqueueBytesBatch: (controller: object, chunks: ArrayBufferView[]) =>
      byteControllerEnqueueBatch(
        controller as ReadableByteStreamController,
        chunks
      ),
    setConsumptionHook: (controller: object, hook: (() => void) | undefined) =>
      byteControllerSetConsumptionHook(
        controller as ReadableByteStreamController,
        hook
      ),
    setControllerExpectedLength: <R>(
      controller: object,
      length: bigint | undefined
    ) =>
      setDefaultControllerExpectedLength(
        controller as ReadableStreamDefaultController<R>,
        length
      ),
    // A transform pair's notification, called synchronously with the
    // reason after the Node.js interop hook has errored this stream (the
    // only external error path that bypasses the source's cancel steps).
    // Fires at most once; the stream drops it on leaving 'readable'.
    // undefined clears it.
    setInteropErrorHook: <R>(
      stream: ReadableStream<R>,
      hook: ((reason: unknown) => void) | undefined
    ): void => setReadableStreamInteropErrorHook(stream, hook),
  }),

  // Part of the internal implementation. Do not re-export to user code
  cppExports,
};
