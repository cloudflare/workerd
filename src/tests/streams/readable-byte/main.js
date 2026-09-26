// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the readable-byte suite. Explicit named re-exports
// only. The default export serves the SELF binding used by the
// integration tests (echo).

export default {
  async fetch(request) {
    const url = new URL(request.url);
    // Chunked byte-source endpoint for the readAtLeast tests.
    if (url.pathname === '/chunked') {
      const enc = new TextEncoder();
      const rs = new ReadableStream({
        type: 'bytes',
        async pull(controller) {
          for (const chunk of ['foo', 'bar', 'b', 'a', 'z']) {
            controller.enqueue(enc.encode(chunk));
            await scheduler.wait(1);
          }
          controller.close();
        },
      });
      return new Response(rs);
    }
    // Chunks foo, bar, baz, each after a delay, so a read of the body is
    // still in flight well after it starts.
    if (url.pathname === '/delayed') {
      const enc = new TextEncoder();
      const chunks = ['foo', 'bar', 'baz'];
      const rs = new ReadableStream({
        async pull(controller) {
          await scheduler.wait(20);
          if (chunks.length > 0) {
            controller.enqueue(enc.encode(chunks.shift()));
          } else {
            controller.close();
          }
        },
      });
      return new Response(rs);
    }
    return new Response(request.body, {
      headers: { 'content-type': 'application/octet-stream' },
    });
  },
};

export {
  sizeStrategyForBytes,
  autoAllocateChunkSizeValidated,
  byteHwmDefaultIsZero,
  syncStartThrow,
} from 'construction';

export {
  pullCountShape,
  pullThrowErrorsStream,
  pullThrowIgnoredIfErrored,
  backpressureByteStreamHwm,
  startPromiseSettledInNewPromise,
} from 'pull-timing';

export {
  byobRequestOnDefaultRead,
  enqueueDiscardsByobRequest,
  closeWithPartiallyFilledView,
  closeWithPartiallyFilledViewDetached,
  readAfterCloseReturnsEmptyView,
  readAfterCancelReturnsEmptyView,
  readDetachesCallerBuffer,
  closeWithPendingUnfilledByobRead,
  controllerType,
  errorAfterCloseWithQueuedBytes,
  cancelWithPartiallyFilledPull,
  readViewThenCancelOrdering,
} from 'controller';

export {
  byobUint16Array,
  byobUint32Array,
  byobFloat32Array,
  byobFloat64Array,
  byobDataView,
  byobMixedViewTypes,
  byobViewOffset,
  byobAutoAllocateSizes,
  autoAllocateDefaultReadTakesQueuedChunk,
  byobPartialRespondMisalignsFillOffset,
  readableStreamBytesMismatchedSizes,
  readableStreamBytesMismatchedViewTypes,
  readableStreamBytesEnqueueSubarray,
  readableStreamMultiplePendingReads,
  byobreaderRegression,
  partialViewThenDefaultRead,
  nativeByobMultiByteViews,
} from 'byob-reader';

export {
  responseBodyMethodsJsByob,
  requestBodyMethodsJsByob,
  jsSource,
  jsSourceAsyncPull,
  jsByteSource,
  jsByteSourceMultipleChunks,
  jsTeeSource,
  jsTeeClose,
  bigEnqueue,
  bigEnqueueBytes,
  bigEnqueueViaIdentityTransform,
  bigEnqueueViaIdentityTransformAsync,
  bigEnqueueViaJsTransform,
  bigEnqueueViaJsTransformAsync,
  bigEnqueueViaJsTransformSplit,
  enqueueChunkMultipleTimes,
  enqueueChunkMultipleTimesBytes,
  bigEnqueueOddSize,
  multistepTransform,
  multistepTransformPreventClose,
  jsSourceError,
  jsSourceErrorAsync,
  jsErroredSourceAsync,
  jsErroredSourceAsyncDelayed,
  jsNotBytesInPull,
  jsNotBytesInStart,
  jsTeeError,
  jsTeeErrorByob,
  jsSourceTeed,
  jsByteSourceLargeData,
  jsByteSourceLargeDataEnqueue,
  bodyPumpByobRequestPresence,
  readableStreamByteRespond,
  respondAfterCloseFromLaterMicrotask,
  respondAfterCloseAndReleaseFromLaterMicrotask,
  readableStreamByteRespondWithNewView,
  readableStreamByteRespondWithNewViewUsesNewElementSize,
  respondRemainderSettlesHeadFirst,
  readableStreamAutoAllocateChunkSize,
} from 'respond';

export {
  relockRespondRoutesToSecondReader,
  relockUint16RespondAcrossResponds,
  relockRespondWithNewView,
  relockAutoAllocateRespond,
  relockAutoAllocateEnqueue,
  relockTwoPendingRespond,
  relockAutoAllocateTwoPendingRespond,
  relockPartialHeadThenEnqueue,
  relockPartialHeadThenEnqueueShapes,
  relockRespondOverflowSecondView,
} from 'release-relock';

export {
  byobMin,
  readMinStagedFulfillment,
  readMinValidation,
  closeBelowMin,
  closedOrderAtEndOfData,
  minMetThenClose,
  readAtLeastDefaultReaderThrows,
  byobReaderConstraints,
  readAtLeastByobReader,
} from 'read-min';

export {
  teeClonesChunksPerBranch,
  teeByteStreamDefaultReaders,
  teeByteStreamMixedReaders,
  teeCancelComposite,
  teeErrorPropagatesToBothBranches,
  teeReleasedPendingRead,
  teePipeAbortReleasesPendingRead,
  teeReleasedPartialReadByob,
  teeReleasedPartialReadDefault,
  teeReleasedPartialReadBuffered,
  teeReleasedPartialReadPiped,
  teeAfterReleasedPartialRead,
  teeOfBranchWithReleasedPartialRead,
  teeKeepsHeldByobRequest,
  teeSoleBranchUsesHeldByobRequest,
  teeHeldByobRequestWithReleasedBytes,
  teeHeldByobRequestAfterCloseOrError,
  teeHeldByobRequestAcrossNestedTee,
  teeHeldByobRequestEnqueueFillsByobRead,
  teeHeldByobRequestNewViewAndAutoAllocate,
  teeNativeBodyAfterReleaseMidRead,
  teeClosedNativeBodyLocksOriginal,
  teeBranchFractionalCloseErrorsBranch,
  teeSoleBranchFractionalCloseSkipsSourceCancel,
} from 'tee';

export {
  enqueueDetachedBuffer,
  readDetachedView,
  respondAfterViewDetached,
  respondWithNewViewForeignBuffer,
  enqueueResizableBuffer,
  readResizableView,
  resizableByobRequestCannotShrink,
  resizableBuffersDeliveredFixedLength,
  sharedBuffersRejected,
  nonDetachableBuffersRejected,
} from 'buffer-lifecycle';

export {
  teeBranchesCollected,
  teeSurvivorBranchCollected,
  teeBranchesCollectedPullStops,
  pendingByobReadSurvivesGc,
  byobRequestSurvivesGc,
} from 'gc';

export {
  byobRoundtrip,
  readAtLeastOnEchoedBody,
  bytesMethodOnByteSource,
} from 'integration';

export {
  defaultStreamNoByobReader,
  closedPromiseByteReaders,
  cancelPendingReadsByteReaders,
  lockedByteStreamOpsThrow,
  byteDesiredSizeAccounting,
  byteGlobalsExist,
} from 'js-compat';

export {
  drainingReaderSweepsByteBacklog,
  drainingReaderDrivesBytePull,
  drainingReaderByteErrorPropagation,
  drainingReaderByteCancelReachesSource,
} from 'draining-reader';

export {
  smallByteTransfer,
  mediumByteTransfer,
  largeByteTransferDefaultReader,
  largeByteTransferByobReader,
  veryLargeByteTransfer,
  veryLargeByteTransferMismatchedViews,
} from 'data-volumes';

export {
  patchedByteControllerErrorStillErrors,
  readAtLeastIgnoresPatchedRead,
  speciesNotConsultedByInternalCopies,
  nativeSourceIgnoresPollutedMembers,
  thenGetterFiresOncePerByteRead,
} from 'pollution';
