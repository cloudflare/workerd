// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the readable suite. Explicit named re-exports only
// (a collision must be a load-time error, not a silently dropped test).
//
// The default export serves the SELF binding used by the integration
// tests: it echoes the request body back, letting tests round-trip a
// ReadableStream through the fetch machinery in both directions.

export default {
  async fetch(request) {
    return new Response(request.body, {
      headers: { 'content-type': 'application/octet-stream' },
    });
  },
};

export {
  readableGlobalsExist,
  controllerNotConstructable,
  getReaderBadModeThrows,
  defaultStreamNoByobReader,
  lockedLifecycle,
} from 'api-surface';

export {
  garbageSourceValidation,
  invalidTypeValidation,
  argumentConversionOrder,
  highWaterMarkValidated,
  hwmDefaultIsOne,
} from 'construction';

export {
  pullCountShape,
  pullOnLastChunkRead,
  pullSerialized,
  pullRejectionErrorsStream,
  pullThrowSecondCall,
  syncStartThrow,
  asyncStartRejectionErrorsStream,
  cancelWithPendingPull,
} from 'source-algorithms';

export {
  desiredSizeAccounting,
  enqueueWithPendingReadSkipsQueue,
  desiredSizeTerminalStates,
  controllerErrorRejectsReads,
  errorIdempotence,
  closeTerminality,
  closeDrainsQueue,
} from 'controller';

export {
  readsResolveInOrder,
  releaseLockRejectsPendingReads,
  closedReplacedOnReleaseAfterClose,
  closedReplacedOnReleaseAfterError,
  closedRejectsWithUndefinedError,
  readerCancelResolvesReadsAndClosed,
  queuedChunksSurviveReaderSwap,
} from 'reader';

export {
  cancelReasonIdentity,
  cancelLockedStreamRejects,
  cancelHookRejectionPropagates,
  cancelDiscardsQueue,
} from 'cancel';

export {
  sizeErrorsStreamThenThrows,
  sizeErrorsStreamThenReturnsInfinity,
  sizeMustBeFunction,
  invalidSizeReturnValue,
} from 'bad-strategies';

export {
  queueMathNearMaxSafeInteger,
  queueMathNearZeroClamped,
  queueMathNearZeroEndsZero,
} from 'queue-math';

export {
  teeConsumeOneBranchFully,
  teeDifferentReadRates,
  teeCancelSlowBranch,
  teeLargeChunkCount,
  teeAfterPartialRead,
  teeErrorPropagatesBothBranches,
  teeCancelReasonComposite,
  teeCancelReverseOrder,
  teePullPerRead,
} from 'tee';

export {
  transformTeeReentrancySynchronousCancel,
  transformStreamTeeReentrancy,
  teeWithCancelMidStream,
} from 'tee-reentrancy';

export {
  readableStreamFromAsyncGenerator,
  readableStreamFromSyncGenerator,
  readableStreamFromSyncGenerator2,
  readableStreamFromAsyncCanceled,
  readableStreamFromThrowingAsyncGen,
  readableStreamFromNoopAsyncGen,
  readableStreamFromCancelRejectsWhenReturnRejects,
  readableStreamFromCancelRejectsWhenReturnThrows,
  readableStreamFromCancelRejectsWhenReturnNotMethod,
  readableStreamFromCancelRejectsWhenReturnNonObject,
  readableStreamFromCancelResolvesWhenReturnMissing,
  fromString,
  fromReturnValidationMessages,
} from 'from';

export {
  asyncIteratorBreakCancels,
  asyncIteratorReturnMethod,
  asyncIteratorReturnThenNext,
  asyncIteratorPreventCancel,
  asyncIteratorOnClosedStream,
  asyncIteratorOnErroredStream,
  asyncIteratorLocksStream,
  returnThenNextNoAwait,
  nextThenReturnNoAwait,
  iteratorPrototypeShape,
} from 'async-iteration';

export {
  enqueueInsideSize,
  closeInsideSize,
  cancelInsideSize,
  readInsideSize,
} from 'reentrancy';

export {
  chunkHeldByReference,
  detachWhileQueuedObserved,
} from 'buffer-lifecycle';

export {
  readAllTextRequestSmall,
  readAllTextRequestBig,
  readAllTextResponseSmall,
  readAllTextResponseBig,
  readAllTextFailedPull,
  readAllTextFailedStart,
  readAllTextControllerError,
  readAllTextLargeBody,
  bodyConsumptionNormalizesBufferSourceChunks,
  bodyConsumptionPinsResizableArrayBufferExtent,
  cloneWithStreamBody,
  cancelBodyThenConsume,
  fetchBodyRoundtrip,
  fetchRequestBodyRoundtrip,
  echoedBodyIsConsumable,
} from 'integration-body';

export {
  disturbedStreamIntoResponse,
  lockedStreamIntoResponse,
  bodyIdentityAndLockCoupling,
} from 'integration-locked-disturbed';

export { pendingReadSurvivesGc, asyncIterationSurvivesGc } from 'gc';

export { thenGetterFireCountOnRead } from 'then-interceptors';

export {
  drainingReaderSweepsQueuedBacklog,
  drainingReaderPullDriven,
  drainingReaderErrorPropagation,
  drainingReaderCancelReachesSource,
  drainingReaderLockExclusivity,
} from 'draining-reader';

export {
  mediumChunkCountTransfer,
  largeSingleStringChunk,
  veryLargeAggregateTransfer,
  largeTransferThroughTee,
} from 'data-volumes';
