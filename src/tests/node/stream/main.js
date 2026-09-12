// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the node:stream web-interop suite. Explicit named
// re-exports only: a name collision between two modules must be a load-time
// SyntaxError, never a silently dropped test.

export { streamWebReexportsGlobals, adapterEntryPoints } from 'api-surface';

export {
  toWebDeliversPushedChunk,
  toWebCancelDestroysSource,
  toWebCancelWithoutReasonDestroysWithAbortError,
  toWebPipeToFailureDestroysSource,
  toWebRejectsNonReadable,
  toWebCopiesByteChunks,
  toWebObjectModePassesChunksByIdentity,
  toWebObjectModeBackpressureCountsChunks,
  toWebByteModeBackpressureCountsBytes,
  toWebExplicitStrategyOverridesDerived,
  toWebEndClosesStream,
  toWebSourceErrorRejectsRead,
  toWebSourceDestroyBecomesAbortError,
  toWebUnreadableSourceYieldsCancelledStream,
  toWebLyingStrategyDestroysSource,
  toWebCancelFromDataListenerIsQuiet,
  toWebInvalidHighWaterMarkLeavesSourceUntouched,
  toWebLateErrorAfterEndIsSwallowed,
  toWebDestroyInsidePullBecomesAbortError,
} from 'readable-to-web';

export {
  fromWebDeliversDataEvents,
  fromWebErroredAtStartRejectsAsyncIteration,
  fromWebPullErrorRejectsAsyncIteration,
  fromWebRejectsNonReadableStream,
  fromWebValidatesOptionsBeforeLocking,
  fromWebLocksTheStream,
  fromWebLockedInputThrows,
  fromWebPullsOnlyOnDemand,
  fromWebCloseEmitsEndThenClose,
  fromWebErrorWithoutPendingReadDestroys,
  fromWebErrorWithPendingReadDestroys,
  fromWebDetachedChunkDestroysWithTypeError,
  fromWebDestroyCancelsWebStream,
  fromWebDestroyAfterCloseSkipsCancel,
  fromWebEncodingOption,
  fromWebObjectModeOption,
  fromWebHighWaterMarkOption,
  fromWebSignalOption,
} from 'readable-from-web';

export {
  toWebWritesReachNodeSink,
  toWebCloseEndsNodeWritable,
  toWebPipeToCompletes,
  toWebSyncNodeErrorRejectsPendingWrite,
  toWebAsyncNodeErrorErrorsStream,
  toWebFinalErrorRejectsClose,
  toWebNodeEndWithoutCloseAbortsStream,
  toWebCloseAfterNodeEndWaitsForFinish,
  toWebCloseAfterNodeEndRejectsWithFinalError,
  toWebNodeDestroyBecomesAbortError,
  toWebNodeDestroyWithErrorErrorsStream,
  toWebAbortDestroysNodeWritable,
  toWebAbortWithoutReasonDestroysWithAbortError,
  toWebRejectsNonWritable,
  toWebDuckTypedInputYieldsClosedStream,
  toWebLiveDuckIsTakenAtItsWord,
  toWebInvalidHighWaterMarkLeavesWritableUntouched,
  toWebDestroyInsideWriteErrorsOnce,
  toWebAbortInsideWriteFinishesTheWrite,
  toWebInvalidWebChunkErrorsStreamOnly,
  toWebUnwritableSourceYieldsClosedStream,
  toWebStrategyFollowsWritable,
  toWebBackpressureFollowsDrain,
  toWebChunksReachSinkAsNodeWrites,
} from 'writable-to-web';

export {
  fromWebWritesReachWebSink,
  fromWebWebErrorDestroysNodeWritable,
  fromWebSinkRejectionErrorsNodeWritableOnce,
  fromWebSinkCloseRejectionErrorsNodeWritable,
  fromWebBackToBackWritesDeliverChunks,
  fromWebCorkedWritesDeliverChunks,
  fromWebBatchedWriteRejectionFailsCallbacks,
  fromWebRejectsNonWritableStream,
  writableFromWebValidatesOptionsBeforeLocking,
  writableFromWebLocksTheStream,
  fromWebChunksReachSinkAsNodeChunks,
  fromWebDecodeStringsAndObjectMode,
  fromWebEndClosesWebStream,
  fromWebDestroyAbortsOrClosesWebStream,
  fromWebWritesCompleteWhenSinkAccepts,
} from 'writable-from-web';

export {
  toWebPairRoundTrip,
  toWebRejectsNonDuplex,
  toWebDestroyedDuplexYieldsClosedPair,
  toWebHalfDuplexes,
  toWebReadableIsNotByteStream,
  toWebDestroyWithErrorErrorsBothHalves,
  toWebClosingWritableWaitsForReadableEnd,
  toWebReadableEofWaitsForWritableFinish,
} from 'duplex-to-web';

export {
  fromWebPairDetachedChunkDestroysDuplex,
  fromWebPairRoundTrip,
  fromWebObjectModeStrings,
  fromWebPairCorkedWritesDeliverChunks,
  fromWebPairBatchedWriteRejectionFailsCallbacks,
  fromWebPairErroredReadableDestroysDuplex,
  fromWebPairLaterReadableErrorDestroysDuplex,
  fromWebPairErroredWritableDestroysDuplex,
  fromWebPairIterationToCompletionIsClean,
} from 'duplex-from-web';

export {
  toWebAsResponseBody,
  toWebAsRequestBody,
  toWebLargeResponseBody,
  fromWebResponseBody,
  fromWebTextDecoderStreamBody,
  fromWebIdentityTransformWritable,
  fromWebFixedLengthExact,
  fromWebFixedLengthOverwrite,
  fromWebFixedLengthUnderwrite,
  adaptersInPipeThroughChains,
} from 'bodies';

export {
  consumersDrainWebStream,
  textDecodesAcrossChunksAndStrings,
  consumersReleaseLock,
  consumersPropagateStreamError,
  consumersAcceptNodeAndAsyncIterables,
} from 'consumers';

export {
  fromWebStreamChunkTypes,
  fromWebStreamDestroyCancelsSource,
  fromWebStreamErrorPropagates,
} from 'readable-from';

export {
  pipelineWebReadableToNodeWritable,
  pipelineNodeReadableToWebWritable,
  pipelineThroughWebTransform,
  pipelineWebTransformAsSource,
  pipelineGeneratorBetweenWebStreams,
  pipelineWebSinkErrorFailsPipeline,
  pipelineWebSourceErrorFailsPipeline,
  pipelineLockedWebDestinationFails,
  pipelineWebSourceUnconvertibleChunkFails,
  pipelineWebSourcePreservesPromiseChunks,
  pipelineWebSinkFailureWithIdleSource,
  promisesPipelineSignalAbortsIdleWebPipeline,
  pipelineNodeSinkAsyncErrorInterruptsIdleWebSource,
  pipelineNodeSinkErrorCancelsWebSource,
  promisesPipelineTrailingWebWritable,
  promisesPipelineEndFalseLeavesWebWritableOpen,
  promisesPipelineSignalAbortsWebWritable,
  promisesPipelineSignalAbortsPendingWebRead,
  pipelineWebSourceErrorUnderNodeBackpressure,
} from 'pipeline-web';

export {
  interopHooksPresence,
  finishedObservesReadableClose,
  finishedObservesReadableError,
  finishedObservesWritable,
  finishedOnSettledStream,
  finishedWithSignal,
  promisesFinishedWebStreams,
  addAbortSignalErrorsReadable,
  addAbortSignalErrorsWritable,
  addAbortSignalAlreadyAborted,
  addAbortSignalOnTeeBranchSparesSibling,
  addAbortSignalOnTeeBranchThenSiblingCancel,
  addAbortSignalOnTeedAwayBranchIsInert,
  addAbortSignalOnTeedAwayByteBranchIsInert,
  addAbortSignalOnTeeBranchSettlesWithSourceCleanup,
  addAbortSignalOnByteTeeBranchSparesSibling,
  addAbortSignalOnResponseBody,
} from 'finished-and-abort';

export {
  composeValidatesWebStreamPositions,
  composeSingleWebStream,
  composeWebHeadNodeTail,
  composeWebReadableIntoNodeWritable,
  composeEndCompletesBeforeReading,
  composeNodeHeadWebTail,
  composeNodeHeadWebWritableTail,
  composeWebReadableIntoWebWritable,
  composeWebTailDestroyBeforeWrite,
  composeWebTailDestroyUnderBackpressure,
  composeWebTailClosedReadableDestroy,
  composeWebTailUnconvertibleChunkFails,
  composeWebTailUnconvertibleChunkFailsReadableHead,
  composeWebTailDeferredCloseCompletesCleanly,
  composeWebTailBareDestroyIsAbortError,
  readableComposeWithWebTransform,
} from 'compose-web';

export { duplexFromWebStreamHalves } from 'duplex-from';

export {
  patchedThenPassthroughKeepsData,
  hostileThenDuringFromWebLeavesStreamUnlocked,
  hostileThenAfterRegisteringLeavesNothingBehind,
  objectPrototypeThenGetterIsConsultedNotObeyed,
} from 'then-pollution';

export {
  toWebPendingReadSurvivesGc,
  toWebWriterClosedSurvivesGc,
  duplexToWebPendingOperationsSurviveGc,
} from 'gc';
