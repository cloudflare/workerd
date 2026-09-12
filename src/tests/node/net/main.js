// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the node:net suite. Explicit named re-exports only.

export {
  handleLocksBothHalves,
  connectThenReady,
  writeBeforeAndAfterConnect,
  destroyBeforeConnect,
  immediateDestroySkipsConnect,
  writesBeforeConnectAreDeferred,
} from 'connect-lifecycle';

export {
  echoDeliversBuffers,
  echoWithEncoding,
  echoLargeMultiByteString,
  bytesWrittenLarge,
  echoLargeVolume,
  corkedWritesBatch,
  byteAccounting,
} from 'echo-roundtrip';

export {
  defaultDisallowsHalfOpen,
  halfOpenWriteAfterPeerEof,
  halfOpenRequiresExplicitEnd,
  peerEofEndsBothSides,
  writeAfterPeerEofIsEpipe,
  peerEofWithoutConsumerEnds,
} from 'half-close';

export {
  bufferSizeTracksQueue,
  destroyWithoutError,
  writeAfterDestroy,
  writeWithoutHandle,
  writeRejectsInvalidChunk,
  endFlushesQueuedWrites,
  endCallbackForms,
  closedSocketIsInert,
  destroyWithError,
} from 'end-and-destroy';

export {
  pauseStopsDelivery,
  writeBackpressureAndDrain,
  corkCyclesStaySynchronous,
  readRestartsReadLoop,
} from 'backpressure';

export {
  idleSocketTimesOut,
  incomingDataResetsTimeout,
  zeroClearsTimeout,
} from 'timeouts';

export {
  fixedBufferReceivesEveryFill,
  fixedSubarrayKeepsItsRange,
  generatedBuffersReceiveFills,
  callbackFalseStopsReading,
  generatorThrowDestroysSocket,
  generatorGarbageDestroysSocket,
  emptyOrDetachedBufferDestroysSocketWithEnobufs,
  sharedOnreadBufferDestroysSocket,
} from 'onread';

export {
  pipeIntoWritableFromWeb,
  socketAsResponseBody,
  pipelineSocketThroughWebTransform,
  pipelineWebSourceIntoSocket,
  duplexToWebRoundTrip,
  socketHalvesAreLocked,
} from 'interop';
