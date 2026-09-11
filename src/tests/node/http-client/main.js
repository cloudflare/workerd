// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the node:http client suite. Explicit named re-exports
// only.

export {
  bodyArrivesAsBuffers,
  setEncodingYieldsStrings,
  chunkedBodyArrivesIncrementally,
  largeBodyArrivesIntact,
  pauseAndResumeWithoutLoss,
  bodilessResponsesEnd,
  headResponseHasNoBody,
  compressedBodyPassesThrough,
  bodyWaitsForAConsumer,
} from 'response-body';

export {
  stringBodyEchoed,
  endWithBufferSentOnce,
  chunkTypesAndEncodings,
  requestIsSentAtEnd,
  lengthAndTypeReachTheServer,
  emptyPostSendsNoBody,
  getAndHeadIgnoreWrites,
} from 'request-body';

export {
  responseDestroyCloses,
  consumedResponseEndsThenCloses,
  responseDestroyMidBodyReachesServer,
  serverDroppingConnectionAbortsResponse,
  completedResponseIsFinal,
} from 'lifecycle';

export {
  pipeIntoWebSink,
  toWebAsResponseBody,
  pipelineThroughWebTransform,
  consumersAndAsyncIteration,
} from 'interop';
