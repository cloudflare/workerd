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
} from 'pull-timing';

export {
  byobRequestOnDefaultRead,
  enqueueDiscardsByobRequest,
  closeWithPartiallyFilledView,
  readAfterCloseReturnsEmptyView,
  readDetachesCallerBuffer,
  closeWithPendingUnfilledByobRead,
  controllerType,
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
  byobPartialRespondMisalignsFillOffset,
  readableStreamBytesMismatchedSizes,
  readableStreamBytesMismatchedViewTypes,
  readableStreamBytesEnqueueSubarray,
  readableStreamMultiplePendingReads,
  byobreaderRegression,
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
  readableStreamByteRespondWithNewView,
  readableStreamByteRespondWithNewViewUsesNewElementSize,
  readableStreamAutoAllocateChunkSize,
} from 'respond';

export {
  relockRespondRoutesToSecondReader,
  relockUint16RespondAcrossResponds,
  relockRespondWithNewView,
  relockAutoAllocateRespond,
  relockAutoAllocateEnqueue,
  relockRespondOverflowSecondView,
} from 'release-relock';

export {
  byobMin,
  readMinStagedFulfillment,
  readMinValidation,
  closeBelowMin,
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
} from 'tee';

export {
  enqueueDetachedBuffer,
  readDetachedView,
  respondAfterViewDetached,
  respondWithNewViewForeignBuffer,
  enqueueResizableBuffer,
  readResizableView,
  nonDetachableBuffersRejected,
} from 'buffer-lifecycle';

export { pendingByobReadSurvivesGc, byobRequestSurvivesGc } from 'gc';

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
