// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the node:http server suite. The default export routes
// every incoming Request (a test's env.SERVICE.fetch) to the test's server;
// the named re-exports are the tests. Explicit named re-exports only.

import { route } from 'harness';

export default {
  fetch(request, env, ctx) {
    return route(request, env, ctx);
  },
};

export {
  getRequestEndsImmediately,
  postBodyArrivesAsBuffers,
  lateDataListenerReceivesBody,
  largeBodyArrivesInChunks,
  streamingBodyArrivesIncrementally,
  fixedLengthBodyCarriesContentLength,
  pausedBodyResumesWithoutLoss,
  pausedLargeBodyResumes,
  echoThroughPipe,
  bodyPipedToSeveralDestinations,
  bodyThroughWebTransformPipeline,
} from 'request-body';

export {
  destroyWithErrorEmitsError,
  destroyWithoutErrorClosesQuietly,
  destroyWithErrorAndNoListenerIsSwallowed,
  destroyMidBodyCancelsBodyStream,
  destroyWithoutReasonCancelsBodyStream,
  destroyAfterCompleteLeavesStreamAlone,
} from 'request-destroy';

export {
  implicitHeadersAndChunkTypes,
  bodyStreamsBeforeEnd,
  largeAndManyWrites,
  contentLengthCapsBody,
  noBodyStatuses,
  headResponseHasNoBody,
  rejectNonStandardBodyWritesThrows,
  corkAndUncork,
  backpressureSignaling,
  writesAlwaysAcceptedAfterHeaders,
  webSourcePipelinedIntoResponse,
  finishThenClose,
  finishedOnResponseWaitsForClose,
  writeAfterEndFails,
  contentLengthLies,
} from 'response-body';

export {
  destroyWithErrorBeforeHeadersRejectsFetch,
  destroyBeforeHeadersRejectsFetch,
  destroyWithErrorAfterHeadersErrorsBody,
  destroyAfterHeadersEndsBodyPrematurely,
  clientCancelDestroysResponse,
  handlerThrowBeforeHeadersRejectsFetch,
  handlerThrowAfterPartialBodyRejectsFetch,
  asyncHandlerRejectionDestroysResponse,
} from 'response-lifecycle';

export {
  writtenBufferIsReusableAfterCallback,
  chunkGivenToEndStaysUsable,
  mutationAfterCallbackIsNotSent,
  sharedAndWasmMemoryViewsAreWritten,
  trimmedWriteLeavesBufferIntact,
  emptyAndDetachedViewsContributeNothing,
} from 'buffer-lifecycle';

export {
  pipeHonorsDestinationBackpressure,
  unpipeStopsDelivery,
  erroringDestinationIsUnpiped,
  sourceErrorIsNotForwarded,
} from 'piping';

export {
  bodyStreamErrorAbortsMessage,
  bodyStreamErrorWhilePausedAbortsMessage,
  detachedBodyChunkAbortsMessage,
} from 'request-body-failures';

export { destroyInsideFinish, pauseResumeInsideData } from 'reentrancy';

export { patchedThenPassthroughKeepsData } from 'then-pollution';

export {
  largeRequestBodyThroughBinding,
  manyTinyResponseWrites,
  alternatingWriteShapes,
  splitUtf8ReassembledBySetEncoding,
} from 'data-volumes';
