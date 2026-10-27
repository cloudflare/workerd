// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the readable suite. Explicit named re-exports only
// (a collision must be a load-time error, not a silently dropped test).
//
// The default export serves the SELF binding used by the integration
// tests: it echoes the request body back, letting tests round-trip a
// ReadableStream through the fetch machinery in both directions, and
// reports the Content-Length the request carried. /gzip serves
// expected-length.js's compressed response instead.

import { compressedResponse } from 'expected-length';

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/gzip') {
      return compressedResponse(url);
    }
    return new Response(request.body, {
      headers: {
        'content-type': 'application/octet-stream',
        'observed-content-length': String(
          request.headers.get('content-length')
        ),
      },
    });
  },
};

export {
  readableGlobalsExist,
  controllerNotConstructable,
  getReaderBadModeThrows,
  defaultStreamNoByobReader,
  getReaderOptionsShapes,
  lockedLifecycle,
} from 'api-surface';

export {
  garbageSourceValidation,
  invalidTypeValidation,
  argumentConversionOrder,
  strategyArgumentShapes,
  conversionErrorPrecedence,
  byteMembersOnDefaultSource,
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
  startPromiseSettledInNewPromise,
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
  errorAfterCloseWithQueuedChunk,
  controllerType,
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
  queueMathResidueClampedAtDequeue,
  queueMathPositiveResidueKept,
  queueMathResidueClampedPerTeeBranch,
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
  teeBackpressureFollowsSlowestBranch,
  teeCancelAfterSiblingDrainedToClose,
  teeCancelSettlesWhenErroredBranchLeavesAfterClose,
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
  fromReadsNextOnce,
  fromSyncResultReadsDoneBeforeValue,
  fromIteratorLookupsAreGets,
  fromAcceptsObjectsOnly,
  fromCancelReturnLookup,
  fromArrayBufferViewIsOneChunk,
  fromIteratorProtocolEdges,
  fromSyncIterableSettlesEarly,
} from 'from';

export {
  asyncIteratorBreakCancels,
  asyncIteratorReturnMethod,
  asyncIteratorReturnThenNext,
  asyncIteratorPreventCancel,
  valuesOptionsShapes,
  asyncIteratorOnClosedStream,
  asyncIteratorOnErroredStream,
  asyncIteratorLocksStream,
  returnThenNextNoAwait,
  nextThenReturnNoAwait,
  iteratorPrototypeShape,
  iteratorMethodsRejectForeignThis,
  firstNextPullsSynchronously,
  nextFromPullReadsBehindOuter,
  nextFromEarlierContinuationReadsAhead,
  nextFromEarlierContinuationBeatsReturn,
  nextAfterRejectedNextIsDone,
} from 'async-iteration';

export {
  enqueueInsideSize,
  closeInsideSize,
  closeInsideSizeWithQueuedChunk,
  closeInsideSizeTeeDrainedBranch,
  closeInsideSizeTeeBranchesBehind,
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
  readAllTextKeepsEmbeddedNul,
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
  nonBytesChunkCancelsSource,
  everyConsumerCancelsOnNonBytes,
  nonBytesCancelRejectionReplacesFailure,
  nonBytesChunkInClosingBatch,
  transformExpectedLengthOverflow,
  transformExpectedLengthMet,
  sourceExpectedLengthOverflow,
} from 'integration-body-failures';

export {
  chunksCollectibleDuringConsumption,
  oddChunkSizesAssembleIntact,
  chunkWiderThanABlockAssemblesIntact,
  declaredLengthBodyIsExact,
} from 'integration-body-memory';

export {
  forkBeforeReadKeepsDeclaredLength,
  forkAfterReadSendsChunked,
  compressedResponseDropsDeclaredLength,
} from 'expected-length';

export {
  disturbedStreamIntoResponse,
  lockedStreamIntoResponse,
  bodyIdentityAndLockCoupling,
  teedStreamIntoResponse,
  bodyUsedAfterBodyTee,
} from 'integration-locked-disturbed';

export {
  pendingReadSurvivesGc,
  asyncIterationSurvivesGc,
  readableStreamFromPendingPromiseCollects,
  controllerOnlyHeldStreamLiveness,
  teeBranchesCollected,
  teeBranchesCollectedReleaseBacklog,
  teeSurvivorBranchCollected,
  teeBranchesCollectedPullStops,
  sourceReleasedOnceEnded,
} from 'gc';

export {
  thenGetterFireCountOnRead,
  thenGetterFiresOncePerRead,
  thenGetterTimingForWaitingRead,
  thenGetterNotConsultedByPipeReads,
  thenGetterPerIteratorNext,
  thenGetterPerIteratorReturn,
  thenGetterCancelsBranchDuringEnqueue,
  thenGetterCancelsBranchDuringEnqueueBacklog,
  thenGetterCancelsBranchDuringClose,
  thenGetterCancelsTwoBranchesDuringEnqueue,
  thenGetterReleasesReaderWhileReadsSettle,
  thenPatchOnlyInterceptsConsumptionResult,
} from 'then-interceptors';

export {
  drainingReaderSweepsQueuedBacklog,
  drainingReaderMaxSizeEndsAtClose,
  drainingReaderSizeOverflowEndsAtClose,
  drainingReaderPullDriven,
  drainingReaderErrorPropagation,
  drainingReaderCancelReachesSource,
  drainingReaderLockExclusivity,
  drainingReaderReportsDeclaredLength,
} from 'draining-reader';

export {
  mediumChunkCountTransfer,
  largeSingleStringChunk,
  veryLargeAggregateTransfer,
  largeTransferThroughTee,
} from 'data-volumes';

export {
  backpressureReadableHwmZero,
  backpressureReadableHwmOne,
  backpressureReadableHwmLarge,
} from 'backpressure';

export { structuredCloneReadable } from 'transfer';

export {
  patchedArrayIteratorKeepsBody,
  replacedNumberKeepsBody,
  patchedObjectThenKeepsText,
  patchedControllerErrorStillErrors,
  omittedDictionariesReadNothing,
  fromBuildsNoDictionariesFromObjectPrototype,
  asyncIteratorShape,
} from 'pollution';

export { readerClosedIsMarkedHandled } from 'unhandled-rejection';
