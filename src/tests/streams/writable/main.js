// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the writable streams suite. Re-exports are explicit
// (not `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  writableGlobalsExist,
  controllerNotConstructable,
  newWritableStream,
} from 'api-surface';

export {
  highWaterMarkValidated,
  sinkTypeValidation,
  argumentConversionOrder,
  readyFulfillTiming,
  nonCallableSizeThrows,
  globalScopePipe,
} from 'construction';

export {
  newWritableStreamWithSink,
  newWritableStreamWithSinkAsync,
  newWritableStreamStartError,
  newWritableStreamWriteError,
  newWritableStreamAbortError,
  newWritableStreamCloseError,
  writableStreamSizeAlgorithm,
  sinkHooksCapturedAtConstruction,
  secondWriteRejectionErrorsStream,
  sinkHooksNotCalledAfterStartThrow,
} from 'sink-algorithms';

export {
  writableStreamMultiplePendingWrites,
  writableStreamWriteSubarray,
  writableStreamWriteAny,
  writableStreamPromisesResolvedInOrder,
  cancelWriteOnReleaseLock,
} from 'write-semantics';

export {
  chunkMutationVisibility,
  detachedBufferChunkPassesThrough,
  detachAfterWriteTiming,
  resizableGrowAfterWrite,
  resizableShrinkOutOfBounds,
  sizeRecordedBeforeDetach,
} from 'buffer-lifecycle';

export {
  writableStreamCloseThrowRejectsPromises,
  writerDoubleClose,
} from 'close-semantics';

export {
  writableStreamWriteAbort,
  writableStreamAbortReadyRejected,
  writableStreamAbortOptional,
  writableStreamAbortWhileStarting,
  writableStreamAbortWhileWriting,
  writableStreamReleaseLockWhileAborting,
  writableStreamAbortTiming,
  writableStreamAbortWriteClosePending,
  writableStreamErrorDuringInFlightWrite,
  writableStreamStartErrorAfterAbort,
  writableStreamRejectedWriteNoPreventAbort,
  writableStreamAbortedTwice,
  writableStreamAbortOnErroredResolves,
  writableStreamSinkAlgNoCallErrorBeforeAbort,
  writableStreamWriterWithPendingAbort,
  errorRaceWithCloseWritable,
} from 'abort-semantics';

export {
  abortBeforeStartReasonIdentity,
  erroredStateReasonIdentity,
  sinkAbortSkippedAfterBadStrategyError,
  inFlightWriteRejectionDuringAbort,
  abortThenControllerErrorInFlight,
  controllerErrorThenAbortInFlight,
  abortSignalReason,
  concurrentAbortPromiseIdentity,
} from 'abort-matrix';

export {
  writableStreamDesiredSize,
  backpressureWritableDesiredSize,
  backpressureWritableSlowSink,
  floatingPointQueueTotals,
  fractionalSizeTruncation,
  invalidSizeReturnRejects,
  desiredSizeWhileErroring,
} from 'backpressure';

export {
  reentrantWriteFromSize,
  releaseLockInsideSize,
  controllerErrorInsideWriteHook,
  sizeNotCalledForDoomedWrite,
  sizeReceiverAndArity,
} from 'reentrancy';

export { thenGetterDoesNotFireOnWriterPromises } from 'then-interceptors';

export { writableStreamGc, writableStreamGcTraceFinishes } from 'gc';

export {
  manySmallWrites,
  largeSingleWrite,
  largeChunkedWrites,
  veryLargeChunkedWrites,
} from 'data-volumes';

export { structuredCloneWritable } from 'transfer';
