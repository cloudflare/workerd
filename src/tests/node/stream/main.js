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
  toWebNodeDestroyBecomesAbortError,
  toWebNodeDestroyWithErrorErrorsStream,
  toWebAbortDestroysNodeWritable,
  toWebAbortWithoutReasonDestroysWithAbortError,
  toWebRejectsNonWritable,
  toWebDuckTypedInputYieldsClosedStream,
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

export { duplexFromWebStreamHalves } from 'duplex-from';
