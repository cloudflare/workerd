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

const {
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  PromiseResolve,
  PromiseReject,
  PromiseWithResolvers,
  PromisePrototypeThen,
  RangeError,
  Symbol,
  SymbolToStringTag,
  TypeError,
  uncurryThis,
} = primordials;

const { markPromiseHandled } = utils;

const kPrivateSymbol: symbol = Symbol('private');

// What an omitted dictionary argument stands for. Null-prototype: WebIDL
// reads nothing for an omitted dictionary, so neither may we.
const kEmptyDictionary: object = ObjectFreeze({ __proto__: null });

function isActualObject(value: unknown): boolean {
  return value != null && typeof value === 'object';
}

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
const readableControllerDesiredSizeGet = (() => {
  const desc = ObjectGetOwnPropertyDescriptor(
    ReadableStreamDefaultController.prototype,
    'desiredSize'
  );
  if (desc === undefined || desc.get === undefined) {
    throw new TypeError(
      "Expected accessor property 'desiredSize' on prototype"
    );
  }
  return uncurryThis(desc.get);
})() as (controller: object) => number | null;
const writableControllerError = uncurryThis(
  WritableStreamDefaultController.prototype.error
) as (controller: object, reason: unknown) => void;

// WebIDL "a promise resolved with" a callback's result: a new promise, so
// a returned promise settles it two microtasks later than PromiseResolve,
// which adopts it. The transformer's cancel and flush results are settled
// this way; that timing decides whether a same-turn error has reached the
// other side when they settle (WPT transform-streams/cancel.any.js).
function promiseResolvedWith(value: unknown): Promise<void> {
  const { promise, resolve } =
    PromiseWithResolvers() as PromiseWithResolversType<void>;
  resolve(value as void);
  return promise;
}

// ---------------------------------------------------------------------------

let transformStreamDefaultControllerInit: <I, O>(
  controller: TransformStreamDefaultController<I, O>,
  stream: TransformStream<I, O>
) => void;

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
  #stream: TransformStream<I, O> | undefined;

  static {
    assertIsTransformStreamDefaultController = function <I, O>(
      self: TransformStreamDefaultController<I, O>
    ): void {
      if (!isActualObject(self) || !(#stream in self))
        throw new TypeError('Illegal invocation');
    };

    transformStreamDefaultControllerInit = (controller, stream) => {
      controller.#stream = stream;
    };
  }

  constructor(privateSymbol: symbol) {
    assertPrivateSymbol(privateSymbol);
  }

  get desiredSize(): number | null {
    assertIsTransformStreamDefaultController(this);
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    return transformStreamDesiredSize(stream);
  }

  enqueue(chunk: O = undefined as O): void {
    assertIsTransformStreamDefaultController(this);
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    transformStreamEnqueue(stream, chunk);
  }

  error(reason: unknown = undefined): void {
    assertIsTransformStreamDefaultController(this);
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    transformStreamError(stream, reason);
  }

  terminate(): void {
    assertIsTransformStreamDefaultController(this);
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    transformStreamTerminate(stream);
  }
}

class TransformStream<I = unknown, O = unknown> {
  #readable: ReadableStreamType<O>;
  #writable: WritableStreamType<I>;
  // Undefined for ELIDED (zero-algorithm) transforms — no controller is
  // allocated when there are no transformer callbacks to receive it.
  // @ts-expect-error -- retained for debugging/future use
  #controller?: TransformStreamDefaultController<I, O> | undefined;
  #readableController: object | undefined;
  #writableController: object | undefined;
  // Backpressure starts ON: the readable side must pull once before the
  // sink transforms the first chunk.
  #backpressure: boolean = true;
  #backpressureChange: PromiseWithResolversType<void>;
  // The transformer's cancel and flush algorithms (undefined when the
  // transformer has none, or once cleared), and the spec's
  // [[finishPromise]]: whichever of close, abort and cancel runs first
  // settles it, and the others return it.
  #cancelAlgorithm: ((reason: unknown) => Promise<void>) | undefined;
  #flushAlgorithm: (() => Promise<void>) | undefined;
  #finishPromise: Promise<void> | undefined;

  static {
    transformStreamDesiredSize = (stream) => {
      const readableController = stream.#readableController;
      return readableController === undefined
        ? null
        : readableControllerDesiredSizeGet(readableController);
    };

    transformStreamEnqueue = <I, O>(
      stream: TransformStream<I, O>,
      chunk: O
    ) => {
      const readableController = stream.#readableController;
      if (readableController === undefined) {
        throw new TypeError('TransformStream is not fully initialized');
      }
      // Spec TransformStreamDefaultControllerEnqueue step 4:
      // if ReadableStreamDefaultControllerCanCloseOrEnqueue is false,
      // throw a TypeError.  This pre-check must happen BEFORE we attempt
      // the enqueue, so that enqueue-after-error throws TypeError (not
      // the storedError).  The readable controller's own enqueue() does
      // the same check internally; the pre-check here ensures the
      // try/catch below only catches size()-originated errors.
      if (readableInternals.getState(stream.#readable) !== 'readable') {
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
        const storedError = readableInternals.getStoredError(stream.#readable);
        throw storedError !== undefined ? storedError : e;
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
      const readableController = stream.#readableController;
      if (readableController !== undefined) {
        readableControllerError(readableController, reason);
      }
      stream.#errorWritableAndUnblockWrite(reason);
    };

    transformStreamTerminate = <I, O>(stream: TransformStream<I, O>) => {
      const readableController = stream.#readableController;
      if (readableController !== undefined) {
        try {
          readableControllerClose(readableController);
        } catch {
          // Already closed or errored — nothing to do.
        }
      }
      stream.#errorWritableAndUnblockWrite(
        new TypeError('The transform stream has been terminated')
      );
    };
  }

  #setBackpressure(backpressure: boolean): void {
    // Resolve the previous change promise and mint a fresh one — anyone
    // awaiting the old promise (the sink's write algorithm) proceeds.
    this.#backpressureChange.resolve();
    const replacement =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    markPromiseHandled(replacement.promise);
    this.#backpressureChange = replacement;
    this.#backpressure = backpressure;
  }

  // Spec: TransformStreamDefaultControllerClearAlgorithms.
  #clearAlgorithms(): void {
    this.#cancelAlgorithm = undefined;
    this.#flushAlgorithm = undefined;
  }

  // Spec: TransformStreamDefaultSinkCloseAlgorithm.
  #sinkClose(): Promise<void> {
    if (this.#finishPromise !== undefined) return this.#finishPromise;
    const { promise, resolve, reject } =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    this.#finishPromise = promise;
    const flushAlgorithm = this.#flushAlgorithm;
    const flushResult =
      flushAlgorithm !== undefined
        ? flushAlgorithm()
        : (PromiseResolve() as Promise<void>);
    this.#clearAlgorithms();
    markPromiseHandled(
      PromisePrototypeThen(
        flushResult,
        () => {
          if (readableInternals.getState(this.#readable) === 'errored') {
            reject(readableInternals.getStoredError(this.#readable));
            return;
          }
          const rc = this.#readableController;
          if (rc !== undefined) {
            try {
              readableControllerClose(rc);
            } catch {
              // Already closed — acceptable per spec.
            }
          }
          resolve();
        },
        (r: unknown) => {
          const rc = this.#readableController;
          if (rc !== undefined) readableControllerError(rc, r);
          reject(r);
        }
      )
    );
    return promise;
  }

  // Spec: TransformStreamDefaultSinkAbortAlgorithm.
  #sinkAbort(reason: unknown): Promise<void> {
    if (this.#finishPromise !== undefined) return this.#finishPromise;
    const { promise, resolve, reject } =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    this.#finishPromise = promise;
    const cancelAlgorithm = this.#cancelAlgorithm;
    const cancelResult =
      cancelAlgorithm !== undefined
        ? cancelAlgorithm(reason)
        : (PromiseResolve() as Promise<void>);
    this.#clearAlgorithms();
    markPromiseHandled(
      PromisePrototypeThen(
        cancelResult,
        () => {
          if (readableInternals.getState(this.#readable) === 'errored') {
            reject(readableInternals.getStoredError(this.#readable));
            return;
          }
          const rc = this.#readableController;
          if (rc !== undefined) readableControllerError(rc, reason);
          resolve();
        },
        (r: unknown) => {
          const rc = this.#readableController;
          if (rc !== undefined) readableControllerError(rc, r);
          reject(r);
        }
      )
    );
    return promise;
  }

  // Spec: TransformStreamDefaultSourceCancelAlgorithm. The writable's state
  // is read when the cancel algorithm settles (step 7.1.1): an abort, a
  // terminate() or an error that reached it by then rejects the cancel.
  #sourceCancel(reason: unknown): Promise<void> {
    if (this.#finishPromise !== undefined) return this.#finishPromise;
    const { promise, resolve, reject } =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    this.#finishPromise = promise;
    const cancelAlgorithm = this.#cancelAlgorithm;
    const cancelResult =
      cancelAlgorithm !== undefined
        ? cancelAlgorithm(reason)
        : (PromiseResolve() as Promise<void>);
    this.#clearAlgorithms();
    markPromiseHandled(
      PromisePrototypeThen(
        cancelResult,
        () => {
          if (writableInternals.getState(this.#writable) === 'errored') {
            reject(writableInternals.getStoredError(this.#writable));
            return;
          }
          this.#errorWritableAndUnblockWrite(reason);
          resolve();
        },
        (r: unknown) => {
          this.#errorWritableAndUnblockWrite(r);
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
    const writableController = this.#writableController;
    if (writableController !== undefined) {
      writableControllerError(writableController, reason);
    }
    if (this.#backpressure) {
      this.#setBackpressure(false);
    }
  }

  constructor(
    transformer?: Transformer<I, O>,
    writableStrategy?: QueuingStrategy<I>,
    readableStrategy?: QueuingStrategy<O>
  ) {
    transformer ??= kEmptyDictionary as Transformer<I, O>;
    writableStrategy ??= kEmptyDictionary as QueuingStrategy<I>;
    readableStrategy ??= kEmptyDictionary as QueuingStrategy<O>;

    if (!isActualObject(transformer)) {
      throw new TypeError('transformer must be an object');
    }

    // --- Transformer method extraction (alphabetical property reads) ---
    const cancelFn = transformer.cancel;
    if (cancelFn !== undefined && typeof cancelFn !== 'function') {
      throw new TypeError('transformer.cancel must be a function');
    }
    // Non-standard workerd extension: the TOTAL bytes the readable side
    // will produce (undefined = unknown). Advertised through the
    // readable's controller so the C++ bridge derives a Content-Length
    // for bodies built from this transform; not enforced here.
    const expectedLength = readableInternals.normalizeExpectedLength(
      (transformer as { expectedLength?: unknown }).expectedLength
    );
    const flushFn = transformer.flush;
    if (flushFn !== undefined && typeof flushFn !== 'function') {
      throw new TypeError('transformer.flush must be a function');
    }
    if (transformer.readableType !== undefined) {
      throw new RangeError('transformer.readableType must be undefined');
    }
    const startFn = transformer.start;
    if (startFn !== undefined && typeof startFn !== 'function') {
      throw new TypeError('transformer.start must be a function');
    }
    const transformFn = transformer.transform;
    if (transformFn !== undefined && typeof transformFn !== 'function') {
      throw new TypeError('transformer.transform must be a function');
    }
    if (transformer.writableType !== undefined) {
      throw new RangeError('transformer.writableType must be undefined');
    }

    const initialBackpressureChange =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    markPromiseHandled(initialBackpressureChange.promise);
    this.#backpressureChange = initialBackpressureChange;

    // Both inner streams' start algorithms return THIS promise, so
    // neither side processes anything until transformer.start()
    // settles. The controllers adopt it as is (PromiseResolve), where the
    // spec wraps it in a new promise resolved with it, which settles two
    // microtasks later; the two pass-through reactions restore that
    // timing, which decides whether a same-turn terminate() or abort()
    // has errored the writable when a cancel settles.
    const startHolder =
      PromiseWithResolvers() as PromiseWithResolversType<void>;
    const startPromise = PromisePrototypeThen(
      PromisePrototypeThen(startHolder.promise, undefined),
      undefined
    ) as Promise<void>;

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

    if (isElided) {
      // ---- ELIDED PATH ----
      // No controller and no algorithm wrappers.

      const sinkWrite = async (chunk: I): Promise<void> => {
        if (this.#backpressure) {
          await this.#backpressureChange.promise;
          const state = writableInternals.getState(this.#writable);
          if (state === 'erroring' || state === 'errored') {
            throw writableInternals.getStoredError(this.#writable);
          }
        }
        const rc = this.#readableController as object;
        readableControllerEnqueue(rc, chunk);
        const desiredSize = readableControllerDesiredSizeGet(rc);
        const backpressure = desiredSize !== null && desiredSize <= 0;
        if (backpressure !== this.#backpressure) {
          this.#setBackpressure(backpressure);
        }
      };
      this.#writable = new WritableStream(
        {
          __proto__: null,
          start: (c: object) => {
            this.#writableController = c;
            return startPromise;
          },
          write: sinkWrite,
          close: () => this.#sinkClose(),
          abort: (reason: unknown) => this.#sinkAbort(reason),
        },
        writableStrategy
      );

      const sourcePull = (): Promise<void> => {
        this.#setBackpressure(false);
        return this.#backpressureChange.promise;
      };

      const readableHWM =
        readableStrategy.highWaterMark === undefined
          ? 0
          : readableStrategy.highWaterMark;
      this.#readable = new ReadableStream(
        {
          __proto__: null,
          start: (c: object) => {
            this.#readableController = c;
            return startPromise;
          },
          pull: sourcePull,
          cancel: (reason: unknown) => this.#sourceCancel(reason),
        },
        {
          __proto__: null,
          highWaterMark: readableHWM,
          size: readableStrategy.size,
        }
      );
      if (expectedLength !== undefined) {
        readableInternals.setControllerExpectedLength(
          this.#readableController as object,
          expectedLength
        );
      }
      startHolder.resolve();
    } else {
      // ---- STANDARD PATH (transformer has algorithms) ----

      const controller = new TransformStreamDefaultController<I, O>(
        kPrivateSymbol
      );
      transformStreamDefaultControllerInit(controller, this);
      this.#controller = controller;

      let transformAlgorithm: (chunk: I) => Promise<void>;
      if (transformFn === undefined) {
        transformAlgorithm = (chunk: I) => {
          try {
            transformStreamEnqueue(this, chunk as unknown as O);
            return PromiseResolve() as Promise<void>;
          } catch (e) {
            return PromiseReject(e) as Promise<void>;
          }
        };
      } else {
        const callTransform = uncurryThis(transformFn);
        transformAlgorithm = (chunk: I) => {
          try {
            return PromiseResolve(
              callTransform(transformer, chunk, controller)
            ) as Promise<void>;
          } catch (e) {
            return PromiseReject(e) as Promise<void>;
          }
        };
      }
      if (cancelFn !== undefined) {
        const callCancel = uncurryThis(cancelFn);
        this.#cancelAlgorithm = (reason: unknown) => {
          try {
            return promiseResolvedWith(callCancel(transformer, reason));
          } catch (e) {
            return PromiseReject(e) as Promise<void>;
          }
        };
      }
      if (flushFn !== undefined) {
        const callFlush = uncurryThis(flushFn);
        this.#flushAlgorithm = () => {
          try {
            return promiseResolvedWith(callFlush(transformer, controller));
          } catch (e) {
            return PromiseReject(e) as Promise<void>;
          }
        };
      }

      const sinkWrite = async (chunk: I): Promise<void> => {
        if (this.#backpressure) {
          await this.#backpressureChange.promise;
          const state = writableInternals.getState(this.#writable);
          if (state === 'erroring' || state === 'errored') {
            throw writableInternals.getStoredError(this.#writable);
          }
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
      this.#writable = new WritableStream(
        {
          __proto__: null,
          start: (c: object) => {
            this.#writableController = c;
            return startPromise;
          },
          write: sinkWrite,
          close: () => this.#sinkClose(),
          abort: (reason: unknown) => this.#sinkAbort(reason),
        },
        writableStrategy
      );

      const sourcePull = (): Promise<void> => {
        this.#setBackpressure(false);
        return this.#backpressureChange.promise;
      };
      const readableHWM =
        readableStrategy.highWaterMark === undefined
          ? 0
          : readableStrategy.highWaterMark;
      this.#readable = new ReadableStream(
        {
          __proto__: null,
          start: (c: object) => {
            this.#readableController = c;
            return startPromise;
          },
          pull: sourcePull,
          cancel: (reason: unknown) => this.#sourceCancel(reason),
        },
        {
          __proto__: null,
          highWaterMark: readableHWM,
          size: readableStrategy.size,
        }
      );
      if (expectedLength !== undefined) {
        readableInternals.setControllerExpectedLength(
          this.#readableController as object,
          expectedLength
        );
      }

      // --- Start the transformer ---
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

const kEnumerable = { __proto__: null, enumerable: true };

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
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'TransformStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});
ObjectDefineProperties(TransformStreamDefaultController.prototype, {
  __proto__: null,
  desiredSize: kEnumerable,
  enqueue: kEnumerable,
  error: kEnumerable,
  terminate: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'TransformStreamDefaultController',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

module.exports = {
  TransformStream,
  TransformStreamDefaultController,
};
