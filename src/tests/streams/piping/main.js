// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the piping suite. Explicit named re-exports only.

export {
  pipeThroughJsToInternal,
  pipeThroughJsToInternalErroredSource,
  pipeToJsToInternalErroredSource,
  pipeThroughJsToInternalErroredSourcePreventAbort,
  pipeToJsToInternalErroredSourcePreventAbort,
  pipeThroughJsToInternalErroredDest,
  pipeToJsToInternalErroredDest,
  pipeThroughJsToInternalCloses,
  pipeThroughJsToInternalPreventClose,
  pipeThroughJsByobToInternal,
  pipeToJsByobToInternal,
  pipeToInternalToJsSimple,
  pipeToInternalToJsError,
  pipeToInternalToJsErrorPrevent,
  pipeToInternalToJsClose,
  pipeToInternalToJsClosePrevent,
  pipeToJsToJsSimple,
  pipeToJsToJsErrorReadable,
  pipeToJsToJsErrorReadablePrevent,
  pipeToJsToJsErrorWritable,
  pipeToJsToJsErrorWritablePrevent,
  pipeToJsToJsCloseReadable,
  pipeToJsToJsCloseReadablePrevent,
  pipeToJsToJsTee,
  pipeToJsToJsCancelAlready,
  pipeToJsToNativeCancelAlready,
  pipeToJsToJsCancel,
  pipeToJsToNativeCancel,
  pipeToJsToInternalAbortMidRead,
  pipeToJsToInternalAbortPreventCancel,
  pipeToJsToInternalAbortPreventAbort,
  pipeToJsToJsAbortMidRead,
  pipeToJsToJsAbortPreventCancel,
  pipeToJsToJsCloseQueuedDestination,
  pipeToJsToJsCloseQueuedDestinationPreventCancel,
} from 'pipe-matrix';

export {
  optionGetterReadOrder,
  throwingOptionGetter,
  invalidSignalRejected,
  brandChecks,
  pipeThroughLockedEndpoints,
} from 'api-surface';

export {
  sourceStartsErrored,
  sourceStartsErroredPreventAbort,
  sourceErroredAfterChunkHwmZero,
  sourceErroredAfterChunkHwmZeroPreventAbort,
  destStartsErrored,
  destStartsErroredPreventCancel,
  errorTypePreservationPipeTo,
  errorTypePreservationPipeThrough,
  destAbortPromiseStates,
  preventAbortAndCancelCombo,
  shutdownWaitsForInFlightWrite,
} from 'error-propagation';

export {
  externalCloseOnPipedDestRejects,
  externalAbortOnPipedDestRejects,
  destWriteThrowsMidPipe,
  destWriteThrowsMidPipePreventCancel,
  destControllerErrorsMidPipe,
} from 'close-propagation';

export {
  backpressurePipeChain,
  pipeStopsPullingWhenDestStalls,
} from 'flow-control';

export {
  cancelPropagationThroughIdentity,
  cancelPropagationThroughJsTransform,
  fixedLengthStreamPipeExact,
  fixedLengthStreamPipeOverflow,
  fixedLengthStreamPipeUnderflow,
  closedSourceToClosedDest,
  closedSourceToLiveDest,
} from 'interop';

export {
  largePipeJsToJs,
  veryLargePipeChain,
  largePipeJsToIdentity,
  largePipeIdentityToJs,
} from 'data-volumes';

export {
  sabViewThroughCompressionRoundTrip,
  sabViewThroughIdentityTransform,
  sabViewThroughJsPipeChain,
  resizableViewThroughIdentityTransform,
  resizableViewThroughJsPipeChain,
} from 'special-buffers';
