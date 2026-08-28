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
