'use strict';

// WritableStream, WritableStreamDefaultWriter, WritableStreamDefaultController
// (WHATWG Streams §5). Unlike the readable side, the writable side has a
// single consumer (the sink), so there is no shared-queue/cursor machinery —
// the controller keeps a simple spec-shaped FIFO of {chunk, size} plus a
// running total for backpressure.
//
// The state machine follows the spec closely: states are 'writable' →
// ('erroring' →) 'errored', or 'writable' → 'closed'; write/close requests
// are promise pairs; at most one sink operation (write or close) is in
// flight at a time.

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  QueuingStrategy,
  UnderlyingSink,
  WritableStreamDefaultController as WritableStreamDefaultControllerType,
  WritableStreamDefaultWriter as WritableStreamDefaultWriterType,
} from './types';

const {
  AbortController,
  AbortControllerAbort,
  AbortControllerSignalGet,
  ArrayBufferPrototypeByteLengthGet,
  ArrayPrototypePush,
  ArrayPrototypeShift,
  DataViewPrototypeGetByteLength,
  NumberIsNaN,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectFreeze,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  PromisePrototypeThen,
  RangeError,
  Symbol,
  SymbolToStringTag,
  TypeError,
  TypedArrayPrototypeGetByteLength,
  uncurryThis,
} = primordials;

const {
  isArrayBuffer,
  isArrayBufferView,
  isDataView,
  isPromise,
  markPromiseHandled,
} = utils;

// The native backend (leaf module — see the fence conventions in
// native.ts). The cast restores the real shape.
import type { NativeStreamInternals } from './native';
const { nativeStreamInternals } = require('webstreams/native') as {
  nativeStreamInternals: NativeStreamInternals;
};
const { kExtractNativeSink, isNativeUnderlyingSink } = nativeStreamInternals;

const kPrivateSymbol: symbol = Symbol('private');
// Marker for a queued close request in the controller's FIFO.
const kCloseMarker: symbol = Symbol('close');
// Marker for a queued flush request in the controller's FIFO. flush() is a
// workerd extension consumed only by the C++ bridge (cppExports below); it
// resolves once every write queued ahead of it has completed, mirroring the
// legacy WritableStreamInternalController's Flush write-event. Never
// user-visible.
const kFlushMarker: symbol = Symbol('flush');

function isActualObject(value: unknown): value is object {
  return value != null && typeof value === 'object';
}

function assertPrivateSymbol(symbol: symbol): void {
  if (symbol !== kPrivateSymbol) {
    throw new TypeError('Illegal constructor');
  }
}

type WritableState = 'writable' | 'erroring' | 'errored' | 'closed';

interface PendingAbortRequest {
  promise: Promise<void>;
  resolve: () => void;
  reject: (reason: unknown) => void;
  reason: unknown;
  wasAlreadyErroring: boolean;
}

// ---------------------------------------------------------------------------
// Cross-class accessors (assigned in static blocks).

let getWritableStreamState: <W>(stream: WritableStream<W>) => WritableState;
let getWritableStreamStoredError: <W>(stream: WritableStream<W>) => unknown;
let isWritableStreamLocked: <W>(stream: WritableStream<W>) => boolean;
let setWritableStreamWriter: <W>(
  stream: WritableStream<W>,
  writer: WritableStreamDefaultWriter<W> | undefined
) => void;
let writableStreamAbort: <W>(
  stream: WritableStream<W>,
  reason?: unknown
) => Promise<void>;
let writableStreamCloseInternal: <W>(
  stream: WritableStream<W>
) => Promise<void>;
let writableStreamAddWriteRequest: <W>(
  stream: WritableStream<W>
) => Promise<void>;
let writableStreamDealWithRejection: <W>(
  stream: WritableStream<W>,
  error: unknown
) => void;
let writableStreamStartErroring: <W>(
  stream: WritableStream<W>,
  reason: unknown
) => void;
let writableStreamFinishErroringIfNeeded: <W>(
  stream: WritableStream<W>
) => void;
let writableStreamMarkFirstWriteRequestInFlight: <W>(
  stream: WritableStream<W>
) => void;
let writableStreamFinishInFlightWrite: <W>(stream: WritableStream<W>) => void;
let writableStreamFinishInFlightWriteWithError: <W>(
  stream: WritableStream<W>,
  error: unknown
) => void;
let writableStreamMarkCloseRequestInFlight: <W>(
  stream: WritableStream<W>
) => void;
let writableStreamFinishInFlightClose: <W>(stream: WritableStream<W>) => void;
let writableStreamFinishInFlightCloseWithError: <W>(
  stream: WritableStream<W>,
  error: unknown
) => void;
let writableStreamCloseQueuedOrInFlight: <W>(
  stream: WritableStream<W>
) => boolean;
let writableStreamUpdateBackpressure: <W>(
  stream: WritableStream<W>,
  backpressure: boolean
) => void;
let writableStreamHasOperationMarkedInFlight: <W>(
  stream: WritableStream<W>
) => boolean;
let getWritableStreamController: <W>(
  stream: WritableStream<W>
) => WritableStreamDefaultController<W> | undefined;
// Boolean brand check for the C++ bridge (jsgTryUnwrap). The assertion form
// (assertIsWritableStream) throws; this one answers.
let isWritableStream: (value: unknown) => boolean;
let setWritableStreamPendingClosure: <W>(stream: WritableStream<W>) => void;
// Permanently neutralizes a stream on behalf of the C++ bridge (e.g. a
// Socket whose connection is being taken over). See the assignment in the
// static block for the exact precondition/error contract.
let detachWritableStream: <W>(stream: WritableStream<W>) => void;

let controllerGetChunkSize: <W>(
  controller: WritableStreamDefaultController<W>,
  chunk: W
) => number;
let controllerWrite: <W>(
  controller: WritableStreamDefaultController<W>,
  chunk: W,
  chunkSize: number
) => void;
let controllerClose: <W>(
  controller: WritableStreamDefaultController<W>
) => void;
let controllerGetDesiredSize: <W>(
  controller: WritableStreamDefaultController<W>
) => number;
let controllerErrorSteps: <W>(
  controller: WritableStreamDefaultController<W>
) => void;
let controllerAbortSteps: <W>(
  controller: WritableStreamDefaultController<W>,
  reason: unknown
) => Promise<void>;
let controllerSignalAbort: <W>(
  controller: WritableStreamDefaultController<W>,
  reason: unknown
) => void;
// Enqueues a flush marker and returns a promise that resolves once every
// write queued ahead of it has completed (rejects if the stream errors
// first). Consumed only via writableStreamFlush (the C++ bridge).
let controllerFlush: <W>(
  controller: WritableStreamDefaultController<W>
) => Promise<void>;

let writerEnsureReadyPromiseRejected: <W>(
  writer: WritableStreamDefaultWriter<W>,
  error: unknown
) => void;
let writerEnsureClosedPromiseRejected: <W>(
  writer: WritableStreamDefaultWriter<W>,
  error: unknown
) => void;
let writerResolveReadyPromise: <W>(
  writer: WritableStreamDefaultWriter<W>
) => void;
let writerResetReadyPromise: <W>(
  writer: WritableStreamDefaultWriter<W>
) => void;
let writerResolveClosedPromise: <W>(
  writer: WritableStreamDefaultWriter<W>
) => void;
let getWriterStream: <W>(
  writer: WritableStreamDefaultWriter<W>
) => WritableStream<W> | undefined;
let writableStreamBackpressureOf: <W>(stream: WritableStream<W>) => boolean;
// Internal writer operations: the pipe implementation must never dispatch
// through the public prototype methods (user-interceptable once the classes
// are installed on the global).
let writerWriteInternal: <W>(
  writer: WritableStreamDefaultWriter<W>,
  chunk: W
) => Promise<void>;
let writerCloseInternal: <W>(
  writer: WritableStreamDefaultWriter<W>
) => Promise<void>;
let writerReleaseInternal: <W>(writer: WritableStreamDefaultWriter<W>) => void;
let getWriterReadyPromiseInternal: <W>(
  writer: WritableStreamDefaultWriter<W>
) => Promise<void>;
let getWriterClosedPromiseInternal: <W>(
  writer: WritableStreamDefaultWriter<W>
) => Promise<void>;

// ---------------------------------------------------------------------------
// WritableStream

// The shared extractor function installed as kExtractNativeSink on
// native-backed WritableStream instances. Assigned in the static block.
// THIS-BOUND (called as stream[kExtractNativeSink]()), mirroring the
// readable side's kExtractNativeSource calling convention so the C++
// TypeWrapper uses one uniform protocol for both directions.
let extractNativeSink: <W>(this: WritableStream<W>) => object;
let assertIsWritableStream: <W>(self: WritableStream<W>) => void;

class WritableStream<W = unknown> {
  #controller?: WritableStreamDefaultController<W> | undefined;
  #writer?: WritableStreamDefaultWriter<W> | undefined;
  #state: WritableState = 'writable';
  #storedError: unknown = undefined;
  #writeRequests: PromiseWithResolversType<void>[] = [];
  #inFlightWriteRequest: PromiseWithResolversType<void> | undefined;
  #closeRequest: PromiseWithResolversType<void> | undefined;
  #inFlightCloseRequest: PromiseWithResolversType<void> | undefined;
  #pendingAbortRequest: PendingAbortRequest | undefined;
  #backpressure: boolean = false;
  // Back-reference to the native underlying sink (undefined for
  // JS-backed streams). Kept for extraction — C++ unwraps the backing
  // class from the returned object.
  #nativeSink?: object | undefined;
  // TODO(streams-ts): The sockets API needs to be able to tell its Writable
  // that it is pending closure (the Socket is shutting down) before the
  // closure has actually completed. We don't yet fully implement this but we
  // provide for the signal. Mirrors ReadableStream's #pendingClosure.
  // @ts-expect-error
  #pendingClosure: boolean = false;

  static {
    assertIsWritableStream = function <W>(self: WritableStream<W>): void {
      if (!isActualObject(self) || !(#state in self))
        throw new TypeError('Illegal invocation');
    };

    isWritableStream = (value: unknown) => {
      return isActualObject(value) && #state in value;
    };

    setWritableStreamPendingClosure = <W>(stream: WritableStream<W>) => {
      stream.#pendingClosure = true;
    };
    getWritableStreamState = (stream) => stream.#state;
    getWritableStreamStoredError = (stream) => stream.#storedError;
    isWritableStreamLocked = (stream) => stream.#writer !== undefined;
    writableStreamBackpressureOf = (stream) => stream.#backpressure;
    setWritableStreamWriter = (stream, writer) => {
      stream.#writer = writer;
    };
    getWritableStreamController = (stream) => stream.#controller;

    // JS-to-C++ extraction for native-backed writable streams.
    // Mirrors extractNativeSource on the readable side: atomic
    // validate → extract → lock. No "disturbed" concept on writable.
    extractNativeSink = function <W>(this: WritableStream<W>): object {
      assertIsWritableStream(this);
      if (isWritableStreamLocked(this)) {
        throw new TypeError(
          'Cannot extract a native sink from a locked stream'
        );
      }
      const sink = this.#nativeSink;
      if (sink === undefined) {
        throw new TypeError('This stream is not backed by a native sink');
      }
      // Permanently lock via an internal writer (never exposed, never
      // released) — same pattern as readable-side extraction.
      new WritableStreamDefaultWriter<W>(this);
      return sink;
    };

    // C++ bridge detach (JsWritableStream::detach's TS arm): permanently
    // neutralize the stream because its underlying connection is being
    // taken over (e.g. Socket startTls / takeConnectionStream). The
    // precondition errors match the legacy C++ arm EXACTLY
    // (WritableStreamInternalController::detach, internal.c++) — they are
    // user-visible through the sockets API:
    //   locked            -> TypeError, text below (with trailing period)
    //   closed or closing -> TypeError "This WritableStream is closed."
    //   errored (and its 'erroring' precursor, which the legacy
    //   implementation does not distinguish) -> throw the stored error
    detachWritableStream = <W>(stream: WritableStream<W>): void => {
      assertIsWritableStream(stream);
      if (isWritableStreamLocked(stream)) {
        throw new TypeError(
          'This WritableStream is currently locked to a writer.'
        );
      }
      if (isWritableStreamClosedOrClosing(stream)) {
        throw new TypeError('This WritableStream is closed.');
      }
      const state = stream.#state;
      if (state === 'errored' || state === 'erroring') {
        throw stream.#storedError;
      }
      // Release a native sink's C++ state FIRST: the controller's stored
      // hook algorithms close over the sink object, which would otherwise
      // keep the taken-over connection's C++ sink alive until the stream is
      // GC'd (the legacy controller's detach drops its sink immediately);
      // and extraction must never hand out a sink whose connection has been
      // taken over. The hook drops the owned sink without ending or
      // aborting it -- the takeover owns the connection now.
      const nativeSink = stream.#nativeSink;
      if (nativeSink !== undefined) {
        const detachFn = (nativeSink as { detach: (this: object) => void })
          .detach;
        uncurryThis(detachFn)(nativeSink);
        stream.#nativeSink = undefined;
      }
      // Permanently lock via an internal writer (never exposed, never
      // released), leaving the stream unusable for further writes.
      new WritableStreamDefaultWriter<W>(stream);
    };

    writableStreamCloseQueuedOrInFlight = (stream) => {
      return (
        stream.#closeRequest !== undefined ||
        stream.#inFlightCloseRequest !== undefined
      );
    };

    writableStreamHasOperationMarkedInFlight = (stream) => {
      return (
        stream.#inFlightWriteRequest !== undefined ||
        stream.#inFlightCloseRequest !== undefined
      );
    };

    writableStreamAddWriteRequest = (stream) => {
      // Caller guarantees: locked and state 'writable'.
      const request = PromiseWithResolvers() as PromiseWithResolversType<void>;
      ArrayPrototypePush(stream.#writeRequests, request);
      return request.promise;
    };

    writableStreamAbort = <W>(stream: WritableStream<W>, reason?: unknown) => {
      const state = stream.#state;
      if (state === 'closed' || state === 'errored') {
        return PromiseResolve(undefined) as Promise<void>;
      }
      const controller = stream.#controller;
      if (controller !== undefined) {
        // The controller's AbortSignal fires as soon as an abort is
        // requested, letting in-flight sink writes cancel their work.
        controllerSignalAbort(controller, reason);
      }
      // signalAbort dispatches 'abort' events SYNCHRONOUSLY — the sink may
      // have registered listeners on controller.signal, and that user code
      // can re-enter (write/error/close). Re-read the state, per spec.
      const stateNow = stream.#state;
      if (stateNow === 'closed' || stateNow === 'errored') {
        return PromiseResolve(undefined) as Promise<void>;
      }
      if (stream.#pendingAbortRequest !== undefined) {
        return stream.#pendingAbortRequest.promise;
      }
      const wasAlreadyErroring = stateNow === 'erroring';
      const abortReason = wasAlreadyErroring ? undefined : reason;
      const { promise, resolve, reject } =
        PromiseWithResolvers() as PromiseWithResolversType<void>;
      stream.#pendingAbortRequest = {
        promise,
        resolve,
        reject,
        reason: abortReason,
        wasAlreadyErroring,
      };
      if (!wasAlreadyErroring) {
        writableStreamStartErroring(stream, abortReason);
      }
      return promise;
    };

    writableStreamCloseInternal = <W>(stream: WritableStream<W>) => {
      const state = stream.#state;
      if (state === 'closed' || state === 'errored') {
        return PromiseReject(
          new TypeError('Cannot close a stream that is closed or errored')
        ) as Promise<void>;
      }
      // Caller (writer.close / stream.close) has rejected the
      // close-queued-or-in-flight case already; assert-not anyway.
      if (writableStreamCloseQueuedOrInFlight(stream)) {
        return PromiseReject(
          new TypeError('Stream is already closing')
        ) as Promise<void>;
      }
      const request = PromiseWithResolvers() as PromiseWithResolversType<void>;
      stream.#closeRequest = request;
      const writer = stream.#writer;
      if (
        writer !== undefined &&
        stream.#backpressure &&
        state === 'writable'
      ) {
        // Close relieves backpressure waits.
        writerResolveReadyPromise(writer);
      }
      const controller = stream.#controller;
      if (controller !== undefined) {
        controllerClose(controller);
      }
      return request.promise;
    };

    writableStreamDealWithRejection = <W>(
      stream: WritableStream<W>,
      error: unknown
    ) => {
      const state = stream.#state;
      if (state === 'writable') {
        writableStreamStartErroring(stream, error);
        return;
      }
      // state is 'erroring'
      writableStreamFinishErroring(stream);
    };

    writableStreamStartErroring = <W>(
      stream: WritableStream<W>,
      reason: unknown
    ) => {
      // assert: storedError undefined, state 'writable'
      stream.#state = 'erroring';
      stream.#storedError = reason;
      const writer = stream.#writer;
      if (writer !== undefined) {
        writerEnsureReadyPromiseRejected(writer, reason);
      }
      const controller = stream.#controller;
      if (
        !writableStreamHasOperationMarkedInFlight(stream) &&
        controller !== undefined &&
        controllerStartedOf(controller)
      ) {
        writableStreamFinishErroring(stream);
      }
    };

    writableStreamFinishErroringIfNeeded = <W>(stream: WritableStream<W>) => {
      if (
        stream.#state === 'erroring' &&
        !writableStreamHasOperationMarkedInFlight(stream)
      ) {
        writableStreamFinishErroring(stream);
      }
    };

    const writableStreamFinishErroring = <W>(stream: WritableStream<W>) => {
      // assert: state 'erroring', no operations in flight
      stream.#state = 'errored';
      const controller = stream.#controller;
      if (controller !== undefined) {
        controllerErrorSteps(controller); // reset the controller queue
      }
      const storedError = stream.#storedError;
      const writeRequests = stream.#writeRequests;
      stream.#writeRequests = [];
      for (let i = 0; i < writeRequests.length; i++) {
        const request = writeRequests[i] as PromiseWithResolversType<void>;
        request.reject(storedError);
      }
      const abortRequest = stream.#pendingAbortRequest;
      if (abortRequest === undefined) {
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        return;
      }
      stream.#pendingAbortRequest = undefined;
      if (abortRequest.wasAlreadyErroring) {
        abortRequest.reject(storedError);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        return;
      }
      const promise =
        controller !== undefined
          ? controllerAbortSteps(controller, abortRequest.reason)
          : (PromiseResolve() as Promise<void>);
      PromisePrototypeThen(
        promise,
        () => {
          abortRequest.resolve();
          writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        },
        (e: unknown) => {
          abortRequest.reject(e);
          writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        }
      );
    };

    const writableStreamRejectCloseAndClosedPromiseIfNeeded = <W>(
      stream: WritableStream<W>
    ) => {
      // assert: state 'errored'
      const storedError = stream.#storedError;
      const closeRequest = stream.#closeRequest;
      if (closeRequest !== undefined) {
        // assert: no in-flight close
        closeRequest.reject(storedError);
        stream.#closeRequest = undefined;
      }
      const writer = stream.#writer;
      if (writer !== undefined) {
        writerEnsureClosedPromiseRejected(writer, storedError);
      }
    };

    writableStreamMarkFirstWriteRequestInFlight = (stream) => {
      // assert: no in-flight write; writeRequests non-empty
      stream.#inFlightWriteRequest = ArrayPrototypeShift(
        stream.#writeRequests
      ) as PromiseWithResolversType<void>;
    };

    writableStreamFinishInFlightWrite = (stream) => {
      // assert: in-flight write request is set (caller guarantee)
      const request = stream
        .#inFlightWriteRequest as PromiseWithResolversType<void>;
      stream.#inFlightWriteRequest = undefined;
      request.resolve();
    };

    writableStreamFinishInFlightWriteWithError = (stream, error) => {
      // assert: in-flight write request is set (caller guarantee)
      const request = stream
        .#inFlightWriteRequest as PromiseWithResolversType<void>;
      stream.#inFlightWriteRequest = undefined;
      request.reject(error);
      writableStreamDealWithRejection(stream, error);
    };

    writableStreamMarkCloseRequestInFlight = (stream) => {
      // assert: no in-flight close; closeRequest set
      stream.#inFlightCloseRequest = stream.#closeRequest;
      stream.#closeRequest = undefined;
    };

    writableStreamFinishInFlightClose = (stream) => {
      // assert: in-flight close request is set (caller guarantee)
      const request = stream
        .#inFlightCloseRequest as PromiseWithResolversType<void>;
      stream.#inFlightCloseRequest = undefined;
      request.resolve();
      // assert: state 'writable' or 'erroring'
      if (stream.#state === 'erroring') {
        // The close succeeded — the abort (if any) is moot.
        stream.#storedError = undefined;
        const abortRequest = stream.#pendingAbortRequest;
        if (abortRequest !== undefined) {
          abortRequest.resolve();
          stream.#pendingAbortRequest = undefined;
        }
      }
      stream.#state = 'closed';
      const writer = stream.#writer;
      if (writer !== undefined) {
        writerResolveClosedPromise(writer);
      }
      // assert: pendingAbortRequest undefined, storedError undefined
    };

    writableStreamFinishInFlightCloseWithError = (stream, error) => {
      // assert: in-flight close request is set (caller guarantee)
      const request = stream
        .#inFlightCloseRequest as PromiseWithResolversType<void>;
      stream.#inFlightCloseRequest = undefined;
      request.reject(error);
      const abortRequest = stream.#pendingAbortRequest;
      if (abortRequest !== undefined) {
        abortRequest.reject(error);
        stream.#pendingAbortRequest = undefined;
      }
      writableStreamDealWithRejection(stream, error);
    };

    writableStreamUpdateBackpressure = (stream, backpressure) => {
      // assert: state 'writable', close not queued or in flight
      const writer = stream.#writer;
      if (writer !== undefined && backpressure !== stream.#backpressure) {
        if (backpressure) {
          writerResetReadyPromise(writer);
        } else {
          writerResolveReadyPromise(writer);
        }
      }
      stream.#backpressure = backpressure;
    };
  }

  constructor(
    underlyingSink: UnderlyingSink<W> = {},
    strategy: QueuingStrategy<W> = {}
  ) {
    // --- WebIDL strategy dictionary conversion (BEFORE sink reads) ---
    // Per WebIDL, dictionary-typed arguments are converted at the IDL
    // layer before the constructor body runs. strategy is QueuingStrategy
    // (a dictionary); underlyingSink is plain object.
    const sizeFn = strategy.size;
    let sizeAlgorithm: (chunk: W) => number;
    if (sizeFn === undefined) {
      sizeAlgorithm = () => 1;
    } else if (typeof sizeFn !== 'function') {
      throw new TypeError('strategy.size must be a function');
    } else {
      const callSize = uncurryThis(sizeFn);
      sizeAlgorithm = (chunk: W) => callSize(undefined, chunk);
    }
    let highWaterMark = 1;
    if (strategy.highWaterMark !== undefined) {
      highWaterMark = +strategy.highWaterMark;
      if (NumberIsNaN(highWaterMark) || highWaterMark < 0) {
        throw new RangeError('Invalid highWaterMark');
      }
    }

    // --- Now read underlyingSink properties ---
    if (!isActualObject(underlyingSink)) {
      throw new TypeError('underlyingSink must be an object');
    }
    if (underlyingSink.type !== undefined) {
      throw new RangeError('Invalid underlying sink type');
    }

    // Native sink detection: the standard WritableStream machinery
    // drives the sink via start/write/close/abort as-is. The marker
    // exists for pipe dispatch and extraction only.
    if (isNativeUnderlyingSink(underlyingSink)) {
      this.#nativeSink = underlyingSink;
      // Install the extraction marker (same descriptor shape as the
      // readable side: own, non-enumerable, non-writable,
      // non-configurable).
      ObjectDefineProperty(this, kExtractNativeSink, {
        __proto__: null,
        value: extractNativeSink,
      } as PropertyDescriptor);
    }

    this.#controller = new WritableStreamDefaultController<W>(
      kPrivateSymbol,
      this,
      underlyingSink,
      sizeAlgorithm,
      highWaterMark
    );
  }

  get locked(): boolean {
    assertIsWritableStream(this);
    return isWritableStreamLocked(this);
  }

  abort(reason: unknown = undefined): Promise<void> {
    try {
      assertIsWritableStream(this);
      if (isWritableStreamLocked(this)) {
        throw new TypeError('Cannot abort a stream that is locked');
      }
      return writableStreamAbort(this, reason);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  close(): Promise<void> {
    try {
      assertIsWritableStream(this);
      if (isWritableStreamLocked(this)) {
        throw new TypeError('Cannot close a stream that is locked');
      }
      if (writableStreamCloseQueuedOrInFlight(this)) {
        throw new TypeError('Stream is already closing');
      }
      return writableStreamCloseInternal(this);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  getWriter(): WritableStreamDefaultWriterType<W> {
    assertIsWritableStream(this);
    return new WritableStreamDefaultWriter<W>(this);
  }
}

// Started-flag peek used by the stream's erroring machinery; assigned in
// the controller's static block.
let controllerStartedOf: <W>(
  controller: WritableStreamDefaultController<W>
) => boolean;

// ---------------------------------------------------------------------------
// WritableStreamDefaultController

interface QueuedWrite<W> {
  value: W | typeof kCloseMarker | typeof kFlushMarker;
  size: number;
  // Present only on kFlushMarker entries: settled when the marker reaches
  // the queue head (resolve) or the stream errors (reject).
  flushRequest?: PromiseWithResolversType<void>;
}

let assertIsWritableStreamDefaultController: <W>(
  self: WritableStreamDefaultController<W>
) => void;

class WritableStreamDefaultController<
  W = unknown,
> implements WritableStreamDefaultControllerType {
  #stream: WritableStream<W>;
  #queue: QueuedWrite<W>[] = [];
  #queueTotalSize: number = 0;
  #started: boolean = false;
  #strategyHWM: number;
  #sizeAlgorithm: ((chunk: W) => number) | undefined;
  #writeAlgorithm: ((chunk: W) => Promise<void>) | undefined;
  #closeAlgorithm: (() => Promise<void>) | undefined;
  #abortAlgorithm: ((reason: unknown) => Promise<void>) | undefined;
  #abortController = new AbortController();

  static {
    assertIsWritableStreamDefaultController = function <W>(
      self: WritableStreamDefaultController<W>
    ): void {
      if (!isActualObject(self) || !(#stream in self))
        throw new TypeError('Illegal invocation');
    };
    controllerStartedOf = (controller) => controller.#started;

    controllerGetDesiredSize = (controller) => {
      return controller.#strategyHWM - controller.#queueTotalSize;
    };

    controllerSignalAbort = (controller, reason) => {
      AbortControllerAbort(controller.#abortController, reason);
    };

    controllerErrorSteps = <W>(
      controller: WritableStreamDefaultController<W>
    ) => {
      const queue = controller.#queue;
      controller.#queue = [];
      controller.#queueTotalSize = 0;
      // Reject any queued flush requests: the writes ahead of them can no
      // longer complete. The stored error is already set — the erroring
      // machinery assigns it before invoking the error steps.
      const error = getWritableStreamStoredError(controller.#stream);
      for (let i = 0; i < queue.length; i++) {
        const entry = queue[i] as QueuedWrite<W>;
        if (entry.value === kFlushMarker) {
          (entry.flushRequest as PromiseWithResolversType<void>).reject(error);
        }
      }
    };

    controllerAbortSteps = (controller, reason) => {
      const abortAlgorithm = controller.#abortAlgorithm;
      controller.#clearAlgorithms();
      return abortAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : abortAlgorithm(reason);
    };

    controllerClose = (controller) => {
      // Enqueue the close marker (size 0) and advance.
      ArrayPrototypePush(controller.#queue, {
        value: kCloseMarker,
        size: 0,
      });
      controller.#advanceQueueIfNeeded();
    };

    // The flush extension (C++ bridge only; see kFlushMarker). Positional
    // semantics, mirroring the legacy controller's Flush write-event: the
    // returned promise settles once every write queued AHEAD of the marker
    // has completed. Writes enqueued after the flush do not delay it.
    controllerFlush = <W>(
      controller: WritableStreamDefaultController<W>
    ): Promise<void> => {
      // An empty queue means nothing is in flight either: a write stays at
      // the queue head until it settles, and the caller
      // (writableStreamFlush) has already rejected the queued/in-flight
      // close cases.
      if (controller.#queue.length === 0) {
        return PromiseResolve() as Promise<void>;
      }
      const request = PromiseWithResolvers() as PromiseWithResolversType<void>;
      ArrayPrototypePush(controller.#queue, {
        value: kFlushMarker,
        size: 0,
        flushRequest: request,
      });
      controller.#advanceQueueIfNeeded();
      return request.promise;
    };

    // WritableStreamDefaultControllerGetChunkSize (spec §5.5.4)
    // Runs the strategy size algorithm; on failure, errors the stream and
    // returns 1 (spec step 3).  Called BEFORE state checks / write-request
    // enqueue per WritableStreamDefaultWriterWrite step 4.
    controllerGetChunkSize = <W>(
      controller: WritableStreamDefaultController<W>,
      chunk: W
    ): number => {
      const sizeAlgorithm = controller.#sizeAlgorithm;
      if (sizeAlgorithm === undefined) return 1;
      try {
        const size = +sizeAlgorithm(chunk);
        if (NumberIsNaN(size) || size < 0 || size === Infinity) {
          throw new RangeError('Invalid chunk size');
        }
        return size;
      } catch (e) {
        controller.#errorIfNeeded(e);
        return 1;
      }
    };

    // WritableStreamDefaultControllerWrite (spec §5.5.4)
    // Enqueues chunk with pre-computed chunkSize (from controllerGetChunkSize),
    // updates backpressure, and advances the queue.
    controllerWrite = <W>(
      controller: WritableStreamDefaultController<W>,
      chunk: W,
      chunkSize: number
    ) => {
      ArrayPrototypePush(controller.#queue, { value: chunk, size: chunkSize });
      controller.#queueTotalSize += chunkSize;
      const stream = controller.#stream;
      if (
        !writableStreamCloseQueuedOrInFlight(stream) &&
        getWritableStreamState(stream) === 'writable'
      ) {
        writableStreamUpdateBackpressure(
          stream,
          controllerGetDesiredSize(controller) <= 0
        );
      }
      controller.#advanceQueueIfNeeded();
    };
  }

  constructor(
    privateSymbol: symbol,
    stream: WritableStream<W>,
    underlyingSink: UnderlyingSink<W>,
    sizeAlgorithm: (chunk: W) => number,
    highWaterMark: number
  ) {
    assertPrivateSymbol(privateSymbol);
    this.#stream = stream;

    // --- Sink method extraction (alphabetical property reads) ---
    const abortFn = underlyingSink.abort;
    if (abortFn !== undefined && typeof abortFn !== 'function') {
      throw new TypeError('underlyingSink.abort must be a function');
    }
    const closeFn = underlyingSink.close;
    if (closeFn !== undefined && typeof closeFn !== 'function') {
      throw new TypeError('underlyingSink.close must be a function');
    }
    const startFn = underlyingSink.start;
    if (startFn !== undefined && typeof startFn !== 'function') {
      throw new TypeError('underlyingSink.start must be a function');
    }
    const writeFn = underlyingSink.write;
    if (writeFn !== undefined && typeof writeFn !== 'function') {
      throw new TypeError('underlyingSink.write must be a function');
    }

    if (abortFn !== undefined) {
      const callAbort = uncurryThis(abortFn);
      this.#abortAlgorithm = (reason: unknown) => {
        try {
          return PromiseResolve(
            callAbort(underlyingSink, reason)
          ) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }
    if (closeFn !== undefined) {
      const callClose = uncurryThis(closeFn);
      this.#closeAlgorithm = () => {
        try {
          return PromiseResolve(callClose(underlyingSink)) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }
    if (writeFn !== undefined) {
      const callWrite = uncurryThis(writeFn);
      this.#writeAlgorithm = (chunk: W) => {
        try {
          return PromiseResolve(
            callWrite(underlyingSink, chunk, this)
          ) as Promise<void>;
        } catch (e) {
          return PromiseReject(e) as Promise<void>;
        }
      };
    }

    // Strategy already extracted by caller (WebIDL conversion order).
    this.#sizeAlgorithm = sizeAlgorithm;
    this.#strategyHWM = highWaterMark;

    // Initial backpressure signal.
    writableStreamUpdateBackpressure(
      stream,
      controllerGetDesiredSize(this) <= 0
    );

    // --- Start (sync throw propagates out of the WritableStream ctor) ---
    const startResult: unknown =
      startFn === undefined
        ? undefined
        : uncurryThis(startFn)(underlyingSink, this);
    PromisePrototypeThen(
      PromiseResolve(startResult),
      () => {
        this.#started = true;
        this.#advanceQueueIfNeeded();
      },
      (e: unknown) => {
        this.#started = true;
        writableStreamDealWithRejection(this.#stream, e);
      }
    );
  }

  get signal(): AbortSignal {
    assertIsWritableStreamDefaultController(this);
    return AbortControllerSignalGet(this.#abortController);
  }

  error(reason: unknown = undefined): void {
    assertIsWritableStreamDefaultController(this);
    if (getWritableStreamState(this.#stream) !== 'writable') return;
    this.#errorStream(reason);
  }

  #errorIfNeeded(error: unknown): void {
    if (getWritableStreamState(this.#stream) === 'writable') {
      this.#errorStream(error);
    }
  }

  #errorStream(error: unknown): void {
    // WritableStreamDefaultControllerError: clear algorithms, start erroring.
    // Note: the queue is NOT cleared here — it is cleared later during
    // WritableStreamFinishErroring (via controllerErrorSteps).
    this.#clearAlgorithms();
    writableStreamStartErroring(this.#stream, error);
  }

  #clearAlgorithms(): void {
    this.#writeAlgorithm = undefined;
    this.#closeAlgorithm = undefined;
    this.#abortAlgorithm = undefined;
    this.#sizeAlgorithm = undefined;
  }

  #advanceQueueIfNeeded(): void {
    if (!this.#started) return;
    const stream = this.#stream;
    if (writableStreamHasOperationMarkedInFlight(stream)) return;
    const state = getWritableStreamState(stream);
    if (state === 'closed' || state === 'errored') return;
    if (state === 'erroring') {
      writableStreamFinishErroringIfNeeded(stream);
      return;
    }
    const head = this.#queue[0];
    if (head === undefined) return;
    if (head.value === kCloseMarker) {
      this.#processClose();
    } else if (head.value === kFlushMarker) {
      this.#processFlush();
    } else {
      this.#processWrite(head.value as W);
    }
  }

  #processFlush(): void {
    // Every write ahead of the marker has completed (queue processing is
    // strictly serial); the marker itself involves no sink operation.
    const entry = ArrayPrototypeShift(this.#queue) as QueuedWrite<W>;
    (entry.flushRequest as PromiseWithResolversType<void>).resolve();
    // Consecutive markers (or a close queued behind the flush) continue
    // processing in the same turn.
    this.#advanceQueueIfNeeded();
  }

  #processClose(): void {
    const stream = this.#stream;
    writableStreamMarkCloseRequestInFlight(stream);
    // Dequeue the close marker; the queue must then be empty.
    ArrayPrototypeShift(this.#queue);
    this.#queueTotalSize = 0;
    const closeAlgorithm = this.#closeAlgorithm;
    this.#clearAlgorithms();
    const promise =
      closeAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : closeAlgorithm();
    PromisePrototypeThen(
      promise,
      () => {
        writableStreamFinishInFlightClose(stream);
      },
      (e: unknown) => {
        writableStreamFinishInFlightCloseWithError(stream, e);
      }
    );
  }

  #processWrite(chunk: W): void {
    const stream = this.#stream;
    writableStreamMarkFirstWriteRequestInFlight(stream);
    const writeAlgorithm = this.#writeAlgorithm;
    const promise =
      writeAlgorithm === undefined
        ? (PromiseResolve() as Promise<void>)
        : writeAlgorithm(chunk);
    PromisePrototypeThen(
      promise,
      () => {
        writableStreamFinishInFlightWrite(stream);
        const state = getWritableStreamState(stream);
        // Dequeue AFTER the write completes (spec ordering).
        const entry = ArrayPrototypeShift(this.#queue) as QueuedWrite<W>;
        this.#queueTotalSize -= entry.size;
        if (this.#queueTotalSize < 0) this.#queueTotalSize = 0;
        if (
          !writableStreamCloseQueuedOrInFlight(stream) &&
          state === 'writable'
        ) {
          writableStreamUpdateBackpressure(
            stream,
            controllerGetDesiredSize(this) <= 0
          );
        }
        this.#advanceQueueIfNeeded();
      },
      (e: unknown) => {
        if (getWritableStreamState(stream) === 'writable') {
          this.#clearAlgorithms();
        }
        writableStreamFinishInFlightWriteWithError(stream, e);
      }
    );
  }
}

// ---------------------------------------------------------------------------
// WritableStreamDefaultWriter
let assertIsWritableStreamDefaultWriter: <W>(
  self: WritableStreamDefaultWriter<W>
) => void;

class WritableStreamDefaultWriter<
  W = unknown,
> implements WritableStreamDefaultWriterType<W> {
  #stream: WritableStream<W> | undefined;
  #readyPromise!: Promise<void> | PromiseWithResolversType<void>;
  #closedPromise!: Promise<void> | PromiseWithResolversType<void>;

  static {
    getWriterStream = (writer) => writer.#stream;

    assertIsWritableStreamDefaultWriter = function <W>(
      self: WritableStreamDefaultWriter<W>
    ): void {
      if (!isActualObject(self) || !(#stream in self))
        throw new TypeError('Illegal invocation');
    };

    getWriterReadyPromiseInternal = (writer) => {
      const promise = writer.#readyPromise;
      if (isPromise(promise)) return promise as Promise<void>;
      return (promise as PromiseWithResolversType<void>).promise;
    };

    getWriterClosedPromiseInternal = (writer) => {
      const promise = writer.#closedPromise;
      if (isPromise(promise)) return promise as Promise<void>;
      return (promise as PromiseWithResolversType<void>).promise;
    };

    // WritableStreamDefaultWriterWrite (spec §5.3.4)
    // Step order is critical: size algorithm runs BEFORE state re-checks,
    // because size() can re-entrantly call close()/releaseLock()/error().
    writerWriteInternal = <W>(
      writer: WritableStreamDefaultWriter<W>,
      chunk: W
    ) => {
      const stream = writer.#stream;
      if (stream === undefined) {
        return PromiseReject(
          new TypeError('This writer has been released')
        ) as Promise<void>;
      }
      const controller = getWritableStreamController(stream);

      // Step 4: GetChunkSize — runs size() which may re-entrantly mutate
      // stream state (e.g. writer.close() called from size()).
      const chunkSize =
        controller !== undefined
          ? controllerGetChunkSize(controller, chunk)
          : 1;

      // Step 5: Re-check writer.[[stream]] — size() may have called
      // releaseLock().
      if (writer.#stream !== stream) {
        return PromiseReject(
          new TypeError('This writer has been released')
        ) as Promise<void>;
      }

      // Steps 6–9: State checks (AFTER size ran).
      const state = getWritableStreamState(stream);
      if (state === 'errored') {
        return PromiseReject(
          getWritableStreamStoredError(stream)
        ) as Promise<void>;
      }
      if (writableStreamCloseQueuedOrInFlight(stream) || state === 'closed') {
        return PromiseReject(
          new TypeError('Cannot write to a stream that is closing or closed')
        ) as Promise<void>;
      }
      if (state === 'erroring') {
        return PromiseReject(
          getWritableStreamStoredError(stream)
        ) as Promise<void>;
      }

      // Step 11: Add write request.
      const promise = writableStreamAddWriteRequest(stream);

      // Step 12: ControllerWrite (with pre-computed chunkSize, no size()
      // call — that already happened in step 4).
      if (controller !== undefined) {
        controllerWrite(controller, chunk, chunkSize);
      }
      return promise;
    };

    writerCloseInternal = <W>(writer: WritableStreamDefaultWriter<W>) => {
      const stream = writer.#stream;
      if (stream === undefined) {
        return PromiseReject(
          new TypeError('This writer has been released')
        ) as Promise<void>;
      }
      if (writableStreamCloseQueuedOrInFlight(stream)) {
        return PromiseReject(
          new TypeError('Stream is already closing')
        ) as Promise<void>;
      }
      return writableStreamCloseInternal(stream);
    };

    writerReleaseInternal = <W>(writer: WritableStreamDefaultWriter<W>) => {
      const stream = writer.#stream;
      if (stream === undefined) return;
      const releaseError = new TypeError('This writer has been released');
      writerEnsureReadyPromiseRejected(writer, releaseError);
      writerEnsureClosedPromiseRejected(writer, releaseError);
      setWritableStreamWriter(stream, undefined);
      writer.#stream = undefined;
    };

    writerResolveReadyPromise = (writer) => {
      const ready = writer.#readyPromise as PromiseWithResolversType<void>;
      if (typeof ready.resolve === 'function') {
        ready.resolve();
        writer.#readyPromise = ready.promise;
      }
    };

    writerResetReadyPromise = (writer) => {
      const replacement =
        PromiseWithResolvers() as PromiseWithResolversType<void>;
      markPromiseHandled(replacement.promise);
      writer.#readyPromise = replacement;
    };

    writerEnsureReadyPromiseRejected = (writer, error) => {
      const ready = writer.#readyPromise as PromiseWithResolversType<void>;
      if (typeof ready.reject === 'function') {
        ready.reject(error);
        writer.#readyPromise = ready.promise;
      } else {
        const replacement =
          PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(replacement.promise);
        replacement.reject(error);
        writer.#readyPromise = replacement.promise;
      }
    };

    writerResolveClosedPromise = (writer) => {
      const closed = writer.#closedPromise as PromiseWithResolversType<void>;
      if (typeof closed.resolve === 'function') {
        closed.resolve();
        writer.#closedPromise = closed.promise;
      }
    };

    writerEnsureClosedPromiseRejected = (writer, error) => {
      const closed = writer.#closedPromise as PromiseWithResolversType<void>;
      if (typeof closed.reject === 'function') {
        closed.reject(error);
        writer.#closedPromise = closed.promise;
      } else {
        const replacement =
          PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(replacement.promise);
        replacement.reject(error);
        writer.#closedPromise = replacement.promise;
      }
    };
  }

  constructor(stream: WritableStream<W>) {
    if (!isActualObject(stream)) {
      throw new TypeError('stream must be a WritableStream');
    }
    // The locked check doubles as the brand check: the private-field access
    // inside isWritableStreamLocked throws TypeError for non-WritableStream
    // objects.
    if (isWritableStreamLocked(stream)) {
      throw new TypeError('Cannot get a writer for a stream that is locked');
    }
    this.#stream = stream;
    setWritableStreamWriter(stream, this);

    const state = getWritableStreamState(stream);
    switch (state) {
      case 'writable': {
        const closed = PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(closed.promise);
        this.#closedPromise = closed;
        if (
          !writableStreamCloseQueuedOrInFlight(stream) &&
          writableStreamBackpressureOf(stream)
        ) {
          const ready =
            PromiseWithResolvers() as PromiseWithResolversType<void>;
          markPromiseHandled(ready.promise);
          this.#readyPromise = ready;
        } else {
          this.#readyPromise = PromiseResolve() as Promise<void>;
        }
        break;
      }
      case 'erroring': {
        const storedError = getWritableStreamStoredError(stream);
        const ready = PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(ready.promise);
        ready.reject(storedError);
        this.#readyPromise = ready.promise;
        const closed = PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(closed.promise);
        this.#closedPromise = closed;
        break;
      }
      case 'closed': {
        this.#readyPromise = PromiseResolve() as Promise<void>;
        this.#closedPromise = PromiseResolve() as Promise<void>;
        break;
      }
      default: {
        // 'errored'
        const storedError = getWritableStreamStoredError(stream);
        const ready = PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(ready.promise);
        ready.reject(storedError);
        this.#readyPromise = ready.promise;
        const closed = PromiseWithResolvers() as PromiseWithResolversType<void>;
        markPromiseHandled(closed.promise);
        closed.reject(storedError);
        this.#closedPromise = closed.promise;
        break;
      }
    }
  }

  get closed(): Promise<void> {
    try {
      assertIsWritableStreamDefaultWriter(this);
      const promise = this.#closedPromise;
      if (isPromise(promise)) return promise as Promise<void>;
      return (promise as PromiseWithResolversType<void>).promise;
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  get ready(): Promise<void> {
    try {
      assertIsWritableStreamDefaultWriter(this);
      const promise = this.#readyPromise;
      if (isPromise(promise)) return promise as Promise<void>;
      return (promise as PromiseWithResolversType<void>).promise;
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  get desiredSize(): number | null {
    assertIsWritableStreamDefaultWriter(this);
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('This writer has been released');
    }
    const state = getWritableStreamState(stream);
    if (state === 'errored' || state === 'erroring') return null;
    if (state === 'closed') return 0;
    const controller = getWritableStreamController(stream);
    return controller === undefined ? 0 : controllerGetDesiredSize(controller);
  }

  abort(reason: unknown = undefined): Promise<void> {
    try {
      assertIsWritableStreamDefaultWriter(this);
      const stream = this.#stream;
      if (stream === undefined) {
        throw new TypeError('This writer has been released');
      }
      return writableStreamAbort(stream, reason);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  close(): Promise<void> {
    try {
      assertIsWritableStreamDefaultWriter(this);
      if (this.#stream === undefined) {
        throw new TypeError('This writer has been released');
      }
      return writerCloseInternal(this);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  write(chunk: W = undefined as W): Promise<void> {
    try {
      assertIsWritableStreamDefaultWriter(this);
      return writerWriteInternal(this, chunk);
    } catch (e) {
      return PromiseReject(e) as Promise<void>;
    }
  }

  releaseLock(): void {
    assertIsWritableStreamDefaultWriter(this);
    writerReleaseInternal(this);
  }
}

// Spec WritableStreamDefaultWriterCloseWithErrorPropagation — the pipe's
// close path: an already-closing/closed destination is success, an errored
// one propagates its stored error.
function writerCloseWithErrorPropagation<W>(
  writer: WritableStreamDefaultWriter<W>
): Promise<void> {
  const stream = getWriterStream(writer);
  if (stream === undefined) {
    return PromiseReject(
      new TypeError('This writer has been released')
    ) as Promise<void>;
  }
  const state = getWritableStreamState(stream);
  if (writableStreamCloseQueuedOrInFlight(stream) || state === 'closed') {
    return PromiseResolve() as Promise<void>;
  }
  if (state === 'errored') {
    return PromiseReject(getWritableStreamStoredError(stream)) as Promise<void>;
  }
  return writerCloseInternal(writer);
}

const kEnumerable = { __proto__: null, enumerable: true };

ObjectDefineProperties(WritableStream, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(WritableStreamDefaultController, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(WritableStream.prototype, {
  __proto__: null,
  locked: kEnumerable,
  abort: kEnumerable,
  close: kEnumerable,
  getWriter: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'WritableStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(WritableStreamDefaultWriter.prototype, {
  __proto__: null,
  closed: kEnumerable,
  ready: kEnumerable,
  desiredSize: kEnumerable,
  abort: kEnumerable,
  close: kEnumerable,
  write: kEnumerable,
  releaseLock: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'WritableStreamDefaultWriter',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(WritableStreamDefaultController.prototype, {
  __proto__: null,
  signal: kEnumerable,
  error: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'WritableStreamDefaultController',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

// ---------------------------------------------------------------------------
// C++ bridge composites (backing the cppExports below). These compose the
// internal operations; they are never reachable from user code.

// The C++ JsWritableStream::isClosedOrClosing query. Parity target is
// WritableStreamInternalController::isClosedOrClosing(): closed, or a close
// is queued / in flight. Deliberate deltas from the legacy heuristic, both
// confined to the flag-guarded TS path:
//   - errored / erroring answer false (both C++ controllers agree);
//   - a PENDING FLUSH does not count (the legacy internal controller's
//     queue.back().isCloseOrFlush() quirk treats it as "closing");
//   - #pendingClosure does not enter the query (matches C++, where the
//     pending-closure flag is separate state).
function isWritableStreamClosedOrClosing<W>(
  stream: WritableStream<W>
): boolean {
  return (
    getWritableStreamState(stream) === 'closed' ||
    writableStreamCloseQueuedOrInFlight(stream)
  );
}

// The forcible close (JsWritableStream::forceClose's TS arm): the internal
// close algorithm, deliberately blind to the writer lock. Terminal-state
// behavior mirrors WritableStreamInternalController::closeImpl exactly:
// already closed-or-closing resolves (idempotent -- NOT the public close()'s
// "Stream is already closing" rejection), errored rejects with the stored
// error.
function writableStreamClose<W>(stream: WritableStream<W>): Promise<void> {
  if (isWritableStreamClosedOrClosing(stream)) {
    return PromiseResolve() as Promise<void>;
  }
  const state = getWritableStreamState(stream);
  if (state === 'errored' || state === 'erroring') {
    return PromiseReject(getWritableStreamStoredError(stream)) as Promise<void>;
  }
  return writableStreamCloseInternal(stream);
}

// The forcible flush (JsWritableStream::forceFlush's TS arm; the
// lock-checked flavor composes C++-side from the isLocked query + this
// hook). flush() is a workerd extension -- not in the WHATWG spec, not
// JS-exposed. Contract mirrors WritableStreamInternalController::flush:
//   closed or closing -> reject TypeError "This WritableStream has been
//     closed." (exact text);
//   errored / erroring -> reject with the stored error;
//   otherwise resolve once every write queued at call time has completed,
//   rejecting if the stream errors first.
// Delta from legacy (flag-guarded): no "currently being piped to" rejection
// -- the TS pipe holds an ordinary writer lock rather than a queue-level
// pipe event, so a mid-pipe forceFlush simply parks behind the pipe's
// queued writes.
function writableStreamFlush<W>(stream: WritableStream<W>): Promise<void> {
  if (isWritableStreamClosedOrClosing(stream)) {
    return PromiseReject(
      new TypeError('This WritableStream has been closed.')
    ) as Promise<void>;
  }
  const state = getWritableStreamState(stream);
  if (state === 'errored' || state === 'erroring') {
    return PromiseReject(getWritableStreamStoredError(stream)) as Promise<void>;
  }
  const controller = getWritableStreamController(stream);
  if (controller === undefined) {
    // Defensive: the controller is assigned in the constructor and never
    // cleared.
    return PromiseResolve() as Promise<void>;
  }
  return controllerFlush(controller);
}

// Strategy size algorithm for native-backed streams: byte-based
// backpressure, for parity with the legacy WritableStreamInternalController
// (whose highWaterMark is measured in bytes and observable through
// writer.desiredSize / writer.ready on e.g. socket writables). Sizing runs
// BEFORE the sink's own chunk validation, so unknown chunk types count as 1
// here and produce the byte-types error at the sink instead of a strategy
// RangeError. Strings count their UTF-16 length -- an approximation of the
// UTF-8 byte count the sink will actually write (exact within 3x; computing
// the true UTF-8 length per chunk is not worth the cost for a backpressure
// heuristic).
function byteSizeOf(chunk: unknown): number {
  if (isArrayBufferView(chunk)) {
    return isDataView(chunk)
      ? DataViewPrototypeGetByteLength(chunk)
      : TypedArrayPrototypeGetByteLength(chunk);
  }
  if (isArrayBuffer(chunk)) {
    return ArrayBufferPrototypeByteLengthGet(chunk);
  }
  if (typeof chunk === 'string') {
    // .length on a string primitive is an own property read (String exotic
    // object); it never consults the patchable prototype chain.
    return chunk.length;
  }
  // Unknown chunk type (including SharedArrayBuffer, whose byteLength getter
  // is not captured): count 1 and let the sink's validation produce the
  // proper error.
  return 1;
}

// Backing for JsWritableStream::create()'s TypeScript arm: constructs a
// WritableStream over a C++ WritableStreamNativeSink (an object carrying the
// kNativeSink marker; the constructor detects it and installs the extraction
// marker). The strategy policy lives here rather than in C++: with a
// highWaterMark the stream gets byte-based sizing (legacy parity, see
// byteSizeOf); without one the constructor's defaults apply (highWaterMark 1,
// chunk counting). That default is a deliberate, flag-guarded delta from the
// legacy controller, which does no backpressure accounting at all when no
// highWaterMark is configured (desiredSize pinned at 1, ready never pending);
// the spec-default strategy instead signals backpressure (desiredSize 0,
// ready pending) whenever a write is queued or in flight.
function createNativeWritableStream(
  sink: object,
  highWaterMark?: number
): WritableStream<unknown> {
  if (highWaterMark === undefined) {
    return new WritableStream(sink as UnderlyingSink<unknown>);
  }
  return new WritableStream(sink as UnderlyingSink<unknown>, {
    highWaterMark,
    size: byteSizeOf,
  });
}

// The cppExports are not part of the public API. They are exported to the
// C++ side of the implementation to allow for certain internal operations
// to be performed on WritableStream instances.
const cppExports = ObjectFreeze({
  WritableStream,
  createNativeWritableStream,
  detachWritableStream,
  isWritableStream,
  isWritableStreamClosedOrClosing,
  isWritableStreamLocked,
  setWritableStreamPendingClosure,
  writableStreamAbort,
  writableStreamClose,
  writableStreamFlush,
});

module.exports = {
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
  // Internal operations consumed by the pipe implementation in readable.ts.
  // Unreachable from user code: the bootstrap require() is not exposed, and
  // streams.ts deliberately does not re-export this.
  // Sink-side symbols for the pipe dispatch in readable.ts.
  kExtractNativeSink,
  internalsForPipe: {
    isWritableStream,
    isWritableStreamLocked,
    acquireWriter<W>(
      stream: WritableStream<W>
    ): WritableStreamDefaultWriter<W> {
      return new WritableStreamDefaultWriter<W>(stream);
    },
    writerWrite: <W>(writer: WritableStreamDefaultWriter<W>, chunk: W) =>
      writerWriteInternal(writer, chunk),
    writerCloseWithErrorPropagation,
    writerRelease: <W>(writer: WritableStreamDefaultWriter<W>) =>
      writerReleaseInternal(writer),
    writableStreamAbort: <W>(stream: WritableStream<W>, reason: unknown) =>
      writableStreamAbort(stream, reason),
    getState: <W>(stream: WritableStream<W>) => getWritableStreamState(stream),
    getStoredError: <W>(stream: WritableStream<W>) =>
      getWritableStreamStoredError(stream),
    closeQueuedOrInFlight: <W>(stream: WritableStream<W>) =>
      writableStreamCloseQueuedOrInFlight(stream),
    getWriterReadyPromise: <W>(writer: WritableStreamDefaultWriter<W>) =>
      getWriterReadyPromiseInternal(writer),
    getWriterClosedPromise: <W>(writer: WritableStreamDefaultWriter<W>) =>
      getWriterClosedPromiseInternal(writer),
  },

  // Part of the internal implementation. Do not re-export to user code
  cppExports,
};
