'use strict';

// TransformStream and TransformStreamDefaultController (WHATWG Streams §6).
//
// A TransformStream is a {readable, writable} pair: the writable side's sink
// runs the transformer over incoming chunks, which enqueue results onto the
// readable side. Backpressure flows from the readable side to the writable
// side through a change-promise that the sink's write algorithm awaits.
//
// The two inner streams are created with module-owned source/sink objects
// (safe to property-read), and all cross-controller operations dispatch
// through prototype methods CAPTURED at bootstrap — never through the
// user-patchable prototypes.

import type {
  PromiseWithResolvers as PromiseWithResolversType,
  QueuingStrategy,
  ReadableStream as ReadableStreamType,
  Transformer,
  TransformStreamDefaultController as TransformStreamDefaultControllerType,
  WritableStream as WritableStreamType,
} from './types';
import type { CommonHelpers } from './common';

const {
  ObjectDefineProperties,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  PromisePrototypeThen,
  RangeError,
  Symbol,
  SymbolToStringTag,
  TypeError,
  getProtoGetter,
  uncurryThis,
} = primordials;

const { markPromiseHandled } = utils;

const {
  isActualObject,
  kNullProtoObject,
  kEnumerable,
  toStringTagProperty,
  promiseResolvedWith,
  newHandledPending,
  requireObject,
  convertQueuingStrategy,
  convertTransformer,
} = require('webstreams/common') as CommonHelpers;

const kPrivateSymbol: symbol = Symbol('private');

function assertPrivateSymbol(symbol: symbol): void {
  if (symbol !== kPrivateSymbol) {
    throw new TypeError('Illegal constructor');
  }
}

// --- Inner stream classes + captured internal dispatch -------------------
// Capturing our own prototype methods at bootstrap time gives safe internal
// dispatch with zero refactoring of the inner classes.

const {
  ReadableStream,
  ReadableStreamDefaultController,
  internalsForTransform: readableInternals,
} = require('webstreams/readable');
const {
  WritableStream,
  WritableStreamDefaultController,
  internalsForPipe: writableInternals,
} = require('webstreams/writable');

const readableControllerEnqueue = uncurryThis(
  ReadableStreamDefaultController.prototype.enqueue
) as (controller: object, chunk: unknown) => void;
const readableControllerClose = uncurryThis(
  ReadableStreamDefaultController.prototype.close
) as (controller: object) => void;
const readableControllerError = uncurryThis(
  ReadableStreamDefaultController.prototype.error
) as (controller: object, reason: unknown) => void;
const readableControllerDesiredSizeGet = getProtoGetter<
  (controller: object) => number | null
>(ReadableStreamDefaultController.prototype, 'desiredSize');
const writableControllerError = uncurryThis(
  WritableStreamDefaultController.prototype.error
) as (controller: object, reason: unknown) => void;

interface TransformerAlgorithms<I> {
  transform: ((chunk: I) => Promise<void>) | undefined;
  flush: (() => Promise<void>) | undefined;
  cancel: ((reason: unknown) => Promise<void>) | undefined;
}

// SetUpTransformStreamDefaultControllerFromTransformer's algorithms. Built
// outside the TransformStream constructor so that `transformer` is captured
// only by these closures, not by the constructor's shared context (which
// every closure the constructor creates keeps alive): once ClearAlgorithms
// drops them, the transformer is collectable. cancel and flush settle the
// transformer's results in a new promise (promiseResolvedWith); that timing
// decides whether a same-turn error has reached the other side when they
// settle.
function makeTransformerAlgorithms<I, O>(
  transformer: Transformer<I, O>,
  controller: TransformStreamDefaultController<I, O>,
  transformFn: Transformer<I, O>['transform'],
  flushFn: Transformer<I, O>['flush'],
  cancelFn: Transformer<I, O>['cancel']
): TransformerAlgorithms<I> {
  let transform: TransformerAlgorithms<I>['transform'];
  if (transformFn !== undefined) {
    const callTransform = uncurryThis(transformFn);
    // Adopts the result (PromiseResolve) rather than settling a new
    // promise with it as cancel and flush do: on the per-chunk path
    // that would cost an allocation and a microtask or two per write.
    // A returned promise therefore settles the write earlier than in
    // the spec.
    transform = (chunk: I) => {
      try {
        return PromiseResolve(callTransform(transformer, chunk, controller));
      } catch (e) {
        return PromiseReject(e);
      }
    };
  }
  let flush: TransformerAlgorithms<I>['flush'];
  if (flushFn !== undefined) {
    const callFlush = uncurryThis(flushFn);
    flush = () => {
      try {
        return promiseResolvedWith(callFlush(transformer, controller));
      } catch (e) {
        return PromiseReject(e);
      }
    };
  }
  let cancel: TransformerAlgorithms<I>['cancel'];
  if (cancelFn !== undefined) {
    const callCancel = uncurryThis(cancelFn);
    cancel = (reason: unknown) => {
      try {
        return promiseResolvedWith(callCancel(transformer, reason));
      } catch (e) {
        return PromiseReject(e);
      }
    };
  }
  return { transform, flush, cancel };
}

// ---------------------------------------------------------------------------

let transformStreamEnqueue: <I, O>(
  stream: TransformStream<I, O>,
  chunk: O
) => void;
let transformStreamError: <I, O>(
  stream: TransformStream<I, O>,
  reason: unknown
) => void;
let transformStreamTerminate: <I, O>(stream: TransformStream<I, O>) => void;
let transformStreamDesiredSize: <I, O>(
  stream: TransformStream<I, O>
) => number | null;

let assertIsTransformStreamDefaultController: <I, O>(
  self: TransformStreamDefaultController<I, O>
) => void;

class TransformStreamDefaultController<
  I = unknown,
  O = unknown,
> implements TransformStreamDefaultControllerType<O> {
  #stream: TransformStream<I, O>;

  static {
    assertIsTransformStreamDefaultController = function <I, O>(
      self: TransformStreamDefaultController<I, O>
    ): void {
      if (!isActualObject(self) || !(#stream in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor(privateSymbol: symbol, stream: TransformStream<I, O>) {
    assertPrivateSymbol(privateSymbol);
    this.#stream = stream;
  }

  get desiredSize(): number | null {
    assertIsTransformStreamDefaultController(this);
    return transformStreamDesiredSize(this.#stream);
  }

  enqueue(chunk: O = undefined as O): void {
    assertIsTransformStreamDefaultController(this);
    transformStreamEnqueue(this.#stream, chunk);
  }

  error(reason: unknown = undefined): void {
    assertIsTransformStreamDefaultController(this);
    transformStreamError(this.#stream, reason);
  }

  terminate(): void {
    assertIsTransformStreamDefaultController(this);
    transformStreamTerminate(this.#stream);
  }
}

class TransformStream<I = unknown, O = unknown> {
  #readable: ReadableStreamType<O>;
  #writable: WritableStreamType<I>;
  // Undefined for ELIDED (zero-algorithm) transforms — no controller is
  // allocated when there are no transformer callbacks to receive it.
  // @ts-expect-error -- retained for debugging/future use
  #controller?: TransformStreamDefaultController<I, O> | undefined;
  // Recorded by the inner streams' start algorithms, which run while the
  // constructor builds the pair, before any user code can reach it.
  #readableController!: object;
  #writableController!: object;
  // Backpressure starts ON: the readable side must pull once before the
  // sink transforms the first chunk.
  #backpressure: boolean = true;
  #backpressureChange: PromiseWithResolversType<void>;
  // The transformer's algorithms (undefined when the transformer has
  // none, or once cleared; see makeTransformerAlgorithms), and the spec's
  // [[finishPromise]]: whichever of close, abort and cancel runs first
  // settles it, and the others return it.
  #transformAlgorithm: ((chunk: I) => Promise<void>) | undefined;
  #cancelAlgorithm: ((reason: unknown) => Promise<void>) | undefined;
  #flushAlgorithm: (() => Promise<void>) | undefined;
  #finishPromise: Promise<void> | undefined;

  static {
    transformStreamDesiredSize = (stream) =>
      readableControllerDesiredSizeGet(stream.#readableController);

    transformStreamEnqueue = <I, O>(
      stream: TransformStream<I, O>,
      chunk: O
    ) => {
      const readableController = stream.#readableController;
      // Spec TransformStreamDefaultControllerEnqueue step 4:
      // if ReadableStreamDefaultControllerCanCloseOrEnqueue is false
      // (close requested, or the readable no longer readable), throw a
      // TypeError.  The readable controller's own enqueue() makes the
      // same check, but its TypeError would land in the catch below,
      // which treats every error as size()'s and errors the writable.
      if (!readableInternals.canCloseOrEnqueue(readableController)) {
        throw new TypeError(
          'Cannot enqueue a chunk into a stream that is closed or has been errored'
        );
      }
      try {
        readableControllerEnqueue(readableController, chunk);
      } catch (e) {
        // size() threw — error the writable side and unblock backpressure.
        stream.#errorWritableAndUnblockWrite(e);
        // Spec step 5.2: throw stream.[[readable]].[[storedError]], not
        // the caught exception.  When size() calls controller.error(e1)
        // then throws e2, the storedError is e1 — the first error wins.
        // When size() closed the readable first (readable.cancel(),
        // terminate()), the readable has no stored error and this throws
        // undefined, as the spec and Node do.
        throw readableInternals.getStoredError(stream.#readable);
      }
      // Mirror the readable side's backpressure state.
      const desiredSize = readableControllerDesiredSizeGet(readableController);
      const backpressure = desiredSize !== null && desiredSize <= 0;
      if (backpressure !== stream.#backpressure) {
        stream.#setBackpressure(backpressure);
      }
    };

    transformStreamError = <I, O>(
      stream: TransformStream<I, O>,
      reason: unknown
    ) => {
      readableControllerError(stream.#readableController, reason);
      stream.#errorWritableAndUnblockWrite(reason);
    };

    transformStreamTerminate = <I, O>(stream: TransformStream<I, O>) => {
      stream.#closeReadable();
      stream.#errorWritableAndUnblockWrite(
        new TypeError('The transform stream has been terminated')
      );
    };
  }

  #setBackpressure(backpressure: boolean): void {
    // Resolve the previous change promise and mint a fresh one — anyone
    // awaiting the old promise (the sink's write algorithm) proceeds.
    this.#backpressureChange.resolve();
    this.#backpressureChange = newHandledPending();
    this.#backpressure = backpressure;
  }

  // Spec: TransformStreamDefaultControllerClearAlgorithms.
  #clearAlgorithms(): void {
    this.#transformAlgorithm = undefined;
    this.#cancelAlgorithm = undefined;
    this.#flushAlgorithm = undefined;
  }

  // Spec: ReadableStreamDefaultControllerClose, which returns at once when
  // the readable can no longer close or enqueue (close requested, closed or
  // errored); the captured close() throws then.
  #closeReadable(): void {
    const readableController = this.#readableController;
    if (readableInternals.canCloseOrEnqueue(readableController)) {
      readableControllerClose(readableController);
    }
  }

  // Spec: TransformStreamDefaultSinkCloseAlgorithm ('close'),
  // TransformStreamDefaultSinkAbortAlgorithm ('abort') and
  // TransformStreamDefaultSourceCancelAlgorithm ('cancel'). They differ in
  // the algorithm they run, flush or cancel, and in the half they end: the
  // sink's close and abort end the readable, the source's cancel the
  // writable. Whichever runs first sets the finish promise before its
  // algorithm runs, so one started from inside flush() or cancel() returns
  // it, as does any started later. The ended half's state is read when the
  // algorithm settles (step 7.1.1): an abort, a terminate() or an error that
  // reached it by then rejects the finish with its stored error.
  #finish(kind: 'close' | 'abort' | 'cancel', reason?: unknown): Promise<void> {
    if (this.#finishPromise !== undefined) return this.#finishPromise;
    const { promise, resolve, reject } =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    this.#finishPromise = promise;
    const algorithm: ((reason: unknown) => Promise<void>) | undefined =
      kind === 'close' ? this.#flushAlgorithm : this.#cancelAlgorithm;
    const result =
      algorithm !== undefined ? algorithm(reason) : PromiseResolve();
    this.#clearAlgorithms();
    const endsWritable = kind === 'cancel';
    const internals = endsWritable ? writableInternals : readableInternals;
    const ended = endsWritable ? this.#writable : this.#readable;
    const errorEnded = endsWritable
      ? (e: unknown): void => {
          this.#errorWritableAndUnblockWrite(e);
        }
      : (e: unknown): void => {
          readableControllerError(this.#readableController, e);
        };
    markPromiseHandled(
      PromisePrototypeThen(
        result,
        () => {
          if (internals.getState(ended) === 'errored') {
            reject(internals.getStoredError(ended));
            return;
          }
          if (kind === 'close') {
            this.#closeReadable();
          } else {
            errorEnded(reason);
          }
          resolve();
        },
        (r: unknown) => {
          errorEnded(r);
          reject(r);
        }
      )
    );
    return promise;
  }

  #errorWritableAndUnblockWrite(reason: unknown): void {
    // Spec: TransformStreamErrorWritableAndUnblockWrite step 1 —
    // TransformStreamDefaultControllerClearAlgorithms.  Prevents a
    // later readable.cancel() from invoking the transformer's cancel
    // callback after the stream has already errored/terminated.
    this.#clearAlgorithms();
    writableControllerError(this.#writableController, reason);
    if (this.#backpressure) {
      this.#setBackpressure(false);
    }
  }

  constructor(
    transformer: Transformer<I, O> = kNullProtoObject as Transformer<I, O>,
    writableStrategy: QueuingStrategy<I> = kNullProtoObject as QueuingStrategy<I>,
    readableStrategy: QueuingStrategy<O> = kNullProtoObject as QueuingStrategy<O>
  ) {
    // WebIDL converts the arguments at the IDL layer, before the constructor
    // body runs: the transformer is typed `object` (null and every other
    // primitive throw, nothing is read), and each strategy is a
    // QueuingStrategy dictionary, the writable's first.
    requireObject(transformer, 'transformer must be an object');
    const writableStrategyDict = convertQueuingStrategy(writableStrategy);
    const readableStrategyDict = convertQueuingStrategy(readableStrategy);

    // The body converts the transformer to a Transformer dictionary before
    // it checks anything, so a bad callback beats readableType and
    // writableType.
    const {
      cancel: cancelFn,
      expectedLength,
      flush: flushFn,
      readableType,
      start: startFn,
      transform: transformFn,
      writableType,
    } = convertTransformer(transformer);
    if (readableType !== undefined) {
      throw new RangeError('transformer.readableType must be undefined');
    }
    if (writableType !== undefined) {
      throw new RangeError('transformer.writableType must be undefined');
    }

    this.#backpressureChange = newHandledPending();

    // Both inner streams' start algorithms return THIS promise, so
    // neither side processes anything until transformer.start()
    // settles.
    const startHolder =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    const startPromise = startHolder.promise;

    // --- ELISION CHECK ---
    // A transformer with ZERO algorithms (no transform/flush/start/cancel)
    // is semantically equivalent to no transformer: every write enqueues
    // the chunk unchanged into the readable queue. The elided path skips
    // controller allocation and per-write algorithm wrappers while
    // preserving the spec-observable backpressure handshake
    // (writer.desiredSize, writer.ready, write-settlement timing) and the
    // close/abort/cancel coordination. An empty transformer {} is
    // equivalent to undefined.
    const isElided =
      cancelFn === undefined &&
      flushFn === undefined &&
      startFn === undefined &&
      transformFn === undefined;

    let controller: TransformStreamDefaultController<I, O> | undefined;
    let sinkWrite: (chunk: I) => Promise<void>;
    if (isElided) {
      // ---- ELIDED PATH ----
      // No controller and no algorithm wrappers.

      sinkWrite = async (chunk: I): Promise<void> => {
        if (this.#backpressure) {
          await this.#backpressureChange.promise;
          const state = writableInternals.getState(this.#writable);
          if (state === 'erroring' || state === 'errored') {
            throw writableInternals.getStoredError(this.#writable);
          }
        }
        // With the finish promise set, a readable.cancel() started before
        // this write reached the sink; close and abort wait for this
        // write. The cancel errors the writable when it settles, so
        // reject as the standard path's cleared-algorithms check does.
        const finishPromise = this.#finishPromise;
        if (finishPromise !== undefined) {
          await PromisePrototypeThen(finishPromise, undefined, () => {});
          throw writableInternals.getStoredError(this.#writable);
        }
        // Enqueue as the default transform algorithm does: a throwing
        // size() then settles the write as on the standard path.
        transformStreamEnqueue(this, chunk as unknown as O);
      };
    } else {
      // ---- STANDARD PATH (transformer has algorithms) ----

      controller = new TransformStreamDefaultController<I, O>(
        kPrivateSymbol,
        this
      );
      this.#controller = controller;

      const algorithms = makeTransformerAlgorithms(
        transformer,
        controller,
        transformFn,
        flushFn,
        cancelFn
      );
      // No transform hook: enqueue the chunk unchanged (spec step 2's
      // default transformAlgorithm).
      this.#transformAlgorithm =
        algorithms.transform ??
        ((chunk: I) => {
          try {
            transformStreamEnqueue(this, chunk as unknown as O);
            return PromiseResolve();
          } catch (e) {
            return PromiseReject(e);
          }
        });
      this.#flushAlgorithm = algorithms.flush;
      this.#cancelAlgorithm = algorithms.cancel;

      sinkWrite = async (chunk: I): Promise<void> => {
        if (this.#backpressure) {
          await this.#backpressureChange.promise;
          const state = writableInternals.getState(this.#writable);
          if (state === 'erroring' || state === 'errored') {
            throw writableInternals.getStoredError(this.#writable);
          }
        }
        // Cleared algorithms: a write that reached the sink after
        // readable.cancel() cleared them, before the cancel settles and
        // errors the writable (or after controller.error(), terminate() or
        // a size() error, which error it at once). The spec leaves this
        // undefined. The chunk is not transformed, and the write rejects
        // with the writable's stored error once the finish promise has
        // settled; every settlement of the cancel's errors the writable
        // first. (Node fulfills the write, dropping the chunk; C++ rejects
        // it with a TypeError.)
        const transformAlgorithm = this.#transformAlgorithm;
        if (transformAlgorithm === undefined) {
          const finishPromise = this.#finishPromise;
          if (finishPromise !== undefined) {
            await PromisePrototypeThen(finishPromise, undefined, () => {});
          }
          throw writableInternals.getStoredError(this.#writable);
        }
        // Spec TransformStreamDefaultControllerPerformTransform: a
        // rejection from the transform algorithm errors BOTH sides
        // (TransformStreamError), then rethrows to reject the write.
        return PromisePrototypeThen(
          transformAlgorithm(chunk),
          undefined,
          (e: unknown) => {
            transformStreamError(this, e);
            throw e;
          }
        ) as Promise<void>;
      };
    }

    // --- The inner streams (both paths) ---
    this.#writable = new WritableStream(
      {
        __proto__: null,
        start: (c: object) => {
          this.#writableController = c;
          return startPromise;
        },
        write: sinkWrite,
        close: () => this.#finish('close'),
        abort: (reason: unknown) => this.#finish('abort', reason),
      },
      // Converted already: the inner stream reads its own data properties,
      // and its default highWaterMark (1) is the TransformStream's.
      writableStrategyDict as QueuingStrategy<I>
    );

    const sourcePull = (): Promise<void> => {
      this.#setBackpressure(false);
      return this.#backpressureChange.promise;
    };
    this.#readable = new ReadableStream(
      {
        __proto__: null,
        start: (c: object) => {
          this.#readableController = c;
          return startPromise;
        },
        pull: sourcePull,
        cancel: (reason: unknown) => this.#finish('cancel', reason),
      },
      {
        __proto__: null,
        // The TransformStream default, where the ReadableStream's is 1.
        highWaterMark: readableStrategyDict.highWaterMark ?? 0,
        size: readableStrategyDict.size,
      }
    );
    // Non-standard workerd extension: the TOTAL bytes the readable side
    // will produce (undefined = unknown). Advertised through the
    // readable's controller so the C++ bridge derives a Content-Length
    // for bodies built from this transform; not enforced here.
    if (expectedLength !== undefined) {
      readableInternals.setControllerExpectedLength(
        this.#readableController,
        expectedLength
      );
    }

    // --- Start the transformer (the elided path has none to start) ---
    if (controller === undefined) {
      startHolder.resolve();
    } else {
      const startResult: unknown =
        startFn === undefined
          ? undefined
          : uncurryThis(startFn)(transformer, controller);
      startHolder.resolve(startResult as void | PromiseLike<void>);
    }

    // The Node.js interop hook errors one half without running the sink's
    // abort or the source's cancel; error the pair as controller.error()
    // would (TransformStreamError).
    const errorPair = (reason: unknown): void => {
      transformStreamError(this, reason);
    };
    writableInternals.setInteropErrorHook(this.#writable, errorPair);
    readableInternals.setInteropErrorHook(this.#readable, errorPair);
  }

  get readable(): ReadableStreamType<O> {
    if (!isActualObject(this) || !(#readable in this))
      throw new TypeError('Illegal invocation');
    return this.#readable;
  }

  get writable(): WritableStreamType<I> {
    if (!isActualObject(this) || !(#writable in this))
      throw new TypeError('Illegal invocation');
    return this.#writable;
  }
}

ObjectDefineProperties(TransformStream, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(TransformStreamDefaultController, {
  __proto__: null,
  length: { __proto__: null, value: 0 },
});
ObjectDefineProperties(TransformStream.prototype, {
  __proto__: null,
  readable: kEnumerable,
  writable: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('TransformStream'),
});
ObjectDefineProperties(TransformStreamDefaultController.prototype, {
  __proto__: null,
  desiredSize: kEnumerable,
  enqueue: kEnumerable,
  error: kEnumerable,
  terminate: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('TransformStreamDefaultController'),
});

module.exports = {
  TransformStream,
  TransformStreamDefaultController,
};
