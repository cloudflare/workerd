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

class TransformStreamDefaultController<
  I = unknown,
  O = unknown,
> implements TransformStreamDefaultControllerType<O> {
  #stream: TransformStream<I, O> | undefined;

  static {
    transformStreamDefaultControllerInit = (controller, stream) => {
      controller.#stream = stream;
    };
  }

  constructor(privateSymbol: symbol) {
    assertPrivateSymbol(privateSymbol);
  }

  get desiredSize(): number | null {
    if (!(#stream in this)) throw new TypeError('Illegal invocation');
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    return transformStreamDesiredSize(stream);
  }

  enqueue(chunk: O = undefined as O): void {
    if (!(#stream in this)) throw new TypeError('Illegal invocation');
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    transformStreamEnqueue(stream, chunk);
  }

  error(reason: unknown = undefined): void {
    if (!(#stream in this)) throw new TypeError('Illegal invocation');
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    transformStreamError(stream, reason);
  }

  terminate(): void {
    if (!(#stream in this)) throw new TypeError('Illegal invocation');
    const stream = this.#stream;
    if (stream === undefined) {
      throw new TypeError('Controller is not attached to a stream');
    }
    transformStreamTerminate(stream);
  }

  [SymbolToStringTag] = 'TransformStreamDefaultController';
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
  // Set by the constructor to a closure that clears the transformer
  // algorithm references.  Called from #errorWritableAndUnblockWrite
  // (which lives outside the constructor scope and cannot access the
  // algorithm locals directly).  Undefined for ELIDED transforms.
  #clearAlgorithms: (() => void) | undefined;

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

  #errorWritableAndUnblockWrite(reason: unknown): void {
    // Spec: TransformStreamErrorWritableAndUnblockWrite step 1 —
    // TransformStreamDefaultControllerClearAlgorithms.  Prevents a
    // later readable.cancel() from invoking the transformer's cancel
    // callback after the stream has already errored/terminated.
    if (this.#clearAlgorithms !== undefined) {
      this.#clearAlgorithms();
      this.#clearAlgorithms = undefined;
    }
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
    transformer ??= {} as Transformer<I, O>;
    writableStrategy ??= {} as QueuingStrategy<I>;
    readableStrategy ??= {} as QueuingStrategy<O>;

    if (!isActualObject(transformer)) {
      throw new TypeError('transformer must be an object');
    }

    // --- Transformer method extraction (alphabetical property reads) ---
    const cancelFn = transformer.cancel;
    if (cancelFn !== undefined && typeof cancelFn !== 'function') {
      throw new TypeError('transformer.cancel must be a function');
    }
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

    // --- ELISION CHECK ---
    // A transformer with ZERO algorithms (no transform/flush/start/cancel)
    // is semantically equivalent to no transformer: every write enqueues
    // the chunk unchanged into the readable queue. The elided path skips
    // controller allocation, the start-promise gate, and per-write
    // algorithm wrappers while preserving the spec-observable backpressure
    // handshake (writer.desiredSize, writer.ready, write-settlement
    // timing). An empty transformer {} is equivalent to undefined.
    const isElided =
      cancelFn === undefined &&
      flushFn === undefined &&
      startFn === undefined &&
      transformFn === undefined;

    if (isElided) {
      // ---- ELIDED PATH ----
      // No controller, no start gating, no algorithm wrappers.

      const sinkWrite = async (chunk: I): Promise<void> => {
        while (this.#backpressure) {
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
      const sinkClose = (): void => {
        readableControllerClose(this.#readableController as object);
      };
      const sinkAbort = (reason: unknown): void => {
        readableControllerError(this.#readableController as object, reason);
      };

      this.#writable = new WritableStream(
        {
          start: (c: object) => {
            this.#writableController = c;
          },
          write: sinkWrite,
          close: sinkClose,
          abort: sinkAbort,
        },
        writableStrategy
      );

      const sourcePull = (): Promise<void> => {
        this.#setBackpressure(false);
        return this.#backpressureChange.promise;
      };
      const sourceCancel = (reason: unknown): void => {
        this.#errorWritableAndUnblockWrite(reason);
      };

      const readableHWM =
        readableStrategy.highWaterMark === undefined
          ? 0
          : readableStrategy.highWaterMark;
      this.#readable = new ReadableStream(
        {
          start: (c: object) => {
            this.#readableController = c;
          },
          pull: sourcePull,
          cancel: sourceCancel,
        },
        { highWaterMark: readableHWM, size: readableStrategy.size }
      );
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
      let cancelAlgorithm: ((reason: unknown) => Promise<void>) | undefined;
      if (cancelFn !== undefined) {
        const callCancel = uncurryThis(cancelFn);
        cancelAlgorithm = (reason: unknown) => {
          try {
            return PromiseResolve(
              callCancel(transformer, reason)
            ) as Promise<void>;
          } catch (e) {
            return PromiseReject(e) as Promise<void>;
          }
        };
      }
      let flushAlgorithm: (() => Promise<void>) | undefined;
      if (flushFn !== undefined) {
        const callFlush = uncurryThis(flushFn);
        flushAlgorithm = () => {
          try {
            return PromiseResolve(
              callFlush(transformer, controller)
            ) as Promise<void>;
          } catch (e) {
            return PromiseReject(e) as Promise<void>;
          }
        };
      }

      // Spec: TransformStreamDefaultControllerClearAlgorithms — releases
      // the transformer closures so they cannot be invoked after
      // close/abort/cancel/error/terminate.  Stored on the instance so
      // #errorWritableAndUnblockWrite (outside this closure scope) can
      // reach them.
      const clearAlgorithms = (): void => {
        cancelAlgorithm = undefined;
        flushAlgorithm = undefined;
      };
      this.#clearAlgorithms = clearAlgorithms;

      // Spec §6.4.2: [[finishPromise]] — coordinates cancel, abort, and
      // close so that whichever runs first wins; parallel operations
      // return the SAME promise.
      let finishPromise: Promise<void> | undefined;

      // Both inner streams' start algorithms return THIS promise, so
      // neither side processes anything until transformer.start()
      // settles.
      const startHolder =
        PromiseWithResolvers() as PromiseWithResolversType<void>;

      const sinkWrite = async (chunk: I): Promise<void> => {
        while (this.#backpressure) {
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
      // Spec: TransformStreamDefaultSinkCloseAlgorithm
      const sinkClose = (): Promise<void> => {
        if (finishPromise !== undefined) return finishPromise;
        const {
          promise,
          resolve: finishResolve,
          reject: finishReject,
        } = PromiseWithResolvers() as PromiseWithResolversType<void>;
        finishPromise = promise;
        const flushResult =
          flushAlgorithm !== undefined
            ? flushAlgorithm()
            : (PromiseResolve() as Promise<void>);
        // Spec: TransformStreamDefaultControllerClearAlgorithms
        clearAlgorithms();
        markPromiseHandled(
          PromisePrototypeThen(
            flushResult,
            () => {
              if (readableInternals.getState(this.#readable) === 'errored') {
                finishReject(readableInternals.getStoredError(this.#readable));
              } else {
                const rc = this.#readableController;
                if (rc !== undefined) {
                  try {
                    readableControllerClose(rc);
                  } catch {
                    // Already closed — acceptable per spec.
                  }
                }
                finishResolve();
              }
            },
            (r: unknown) => {
              const rc = this.#readableController;
              if (rc !== undefined) {
                readableControllerError(rc, r);
              }
              finishReject(r);
            }
          )
        );
        return finishPromise;
      };
      // Spec: TransformStreamDefaultSinkAbortAlgorithm
      const sinkAbort = (reason: unknown): Promise<void> => {
        if (finishPromise !== undefined) return finishPromise;
        const {
          promise,
          resolve: finishResolve,
          reject: finishReject,
        } = PromiseWithResolvers() as PromiseWithResolversType<void>;
        finishPromise = promise;
        const cancelResult =
          cancelAlgorithm !== undefined
            ? cancelAlgorithm(reason)
            : (PromiseResolve() as Promise<void>);
        // Spec: TransformStreamDefaultControllerClearAlgorithms
        clearAlgorithms();
        markPromiseHandled(
          PromisePrototypeThen(
            cancelResult,
            () => {
              if (readableInternals.getState(this.#readable) === 'errored') {
                finishReject(readableInternals.getStoredError(this.#readable));
              } else {
                const rc = this.#readableController;
                if (rc !== undefined) {
                  readableControllerError(rc, reason);
                }
                finishResolve();
              }
            },
            (r: unknown) => {
              const rc = this.#readableController;
              if (rc !== undefined) {
                readableControllerError(rc, r);
              }
              finishReject(r);
            }
          )
        );
        return finishPromise;
      };

      this.#writable = new WritableStream(
        {
          start: (c: object) => {
            this.#writableController = c;
            return startHolder.promise;
          },
          write: sinkWrite,
          close: sinkClose,
          abort: sinkAbort,
        },
        writableStrategy
      );

      const sourcePull = (): Promise<void> => {
        this.#setBackpressure(false);
        return this.#backpressureChange.promise;
      };
      // Spec: TransformStreamDefaultSourceCancelAlgorithm
      const sourceCancel = (reason: unknown): Promise<void> => {
        if (finishPromise !== undefined) return finishPromise;
        const {
          promise,
          resolve: finishResolve,
          reject: finishReject,
        } = PromiseWithResolvers() as PromiseWithResolversType<void>;
        finishPromise = promise;
        // Snapshot writable state before calling the cancel algorithm so we
        // can detect if the callback itself errored the writable (via
        // controller.error()).  The spec checks writable.[[state]] at
        // microtask time, but that also catches unrelated user code
        // (e.g. controller.enqueue() on the now-closed readable) that
        // runs between cancel() and the microtask.  Comparing before/after
        // detects only errors caused by the cancel algorithm.
        const writableCleanBeforeCancel =
          writableInternals.getState(this.#writable) === 'writable';
        const cancelResult =
          cancelAlgorithm !== undefined
            ? cancelAlgorithm(reason)
            : (PromiseResolve() as Promise<void>);
        // Spec: TransformStreamDefaultControllerClearAlgorithms
        clearAlgorithms();
        const writableErroredByCancel =
          writableCleanBeforeCancel &&
          writableInternals.getState(this.#writable) !== 'writable';
        markPromiseHandled(
          PromisePrototypeThen(
            cancelResult,
            () => {
              if (writableErroredByCancel) {
                finishReject(writableInternals.getStoredError(this.#writable));
              } else {
                this.#errorWritableAndUnblockWrite(reason);
                finishResolve();
              }
            },
            (r: unknown) => {
              this.#errorWritableAndUnblockWrite(r);
              finishReject(r);
            }
          )
        );
        return finishPromise;
      };

      const readableHWM =
        readableStrategy.highWaterMark === undefined
          ? 0
          : readableStrategy.highWaterMark;
      this.#readable = new ReadableStream(
        {
          start: (c: object) => {
            this.#readableController = c;
            return startHolder.promise;
          },
          pull: sourcePull,
          cancel: sourceCancel,
        },
        { highWaterMark: readableHWM, size: readableStrategy.size }
      );

      // --- Start the transformer ---
      const startResult: unknown =
        startFn === undefined
          ? undefined
          : uncurryThis(startFn)(transformer, controller);
      startHolder.resolve(startResult as void | PromiseLike<void>);
    }
  }

  get readable(): ReadableStreamType<O> {
    if (!(#readable in this)) throw new TypeError('Illegal invocation');
    return this.#readable;
  }

  get writable(): WritableStreamType<I> {
    if (!(#writable in this)) throw new TypeError('Illegal invocation');
    return this.#writable;
  }

  [SymbolToStringTag] = 'TransformStream';
}

ObjectDefineProperties(TransformStream, {
  length: { value: 0 },
});
ObjectDefineProperties(TransformStreamDefaultController, {
  length: { value: 0 },
});
ObjectDefineProperties(TransformStream.prototype, {
  readable: { enumerable: true },
  writable: { enumerable: true },
});
ObjectDefineProperties(TransformStreamDefaultController.prototype, {
  desiredSize: { enumerable: true },
  enqueue: { enumerable: true },
  error: { enumerable: true },
  terminate: { enumerable: true },
});

module.exports = {
  TransformStream,
  TransformStreamDefaultController,
};
