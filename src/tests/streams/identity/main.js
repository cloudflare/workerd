// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the identity streams suite. Re-exports every test from the
// per-behavior modules so `workerd test` discovers them all. Re-exports are
// explicit (not `export *`) so that a name collision between two modules is
// a load-time SyntaxError instead of a silently dropped test.

export {
  toStringTag,
  fixedLengthIsSubclassOfIdentity,
  sidesAreStreamInstances,
  readableWritableAreStable,
  propertyPlacement,
  prototypeAccessorBrandChecks,
} from 'api-surface';

export {
  identityConstruction,
  identityBrandChecks,
  fixedLengthValidLengths,
  fixedLengthInvalidLengths,
  fixedLengthCoercionDivergence,
  fixedLengthLengthsAboveMaxSafeInteger,
} from 'construction';

export {
  acceptsUint8Array,
  acceptsArrayBuffer,
  acceptsDataViewSubrange,
  acceptsStringAsUtf8,
  respectsViewOffsets,
  rejectsNumberChunk,
  rejectsObjectChunk,
} from 'chunk-types';

export {
  zeroLengthUint8ArrayIsNoop,
  zeroLengthArrayBufferIsNoop,
  zeroLengthStringIsNoop,
} from 'zero-length-writes';

export { writeCopiesData } from 'copy-semantics';

export {
  resizableShrinkAfterWrite,
  resizableGrowAfterWrite,
  resizableBufferShrinkToZeroAfterWrite,
  viewDetachedAfterWrite,
  bufferDetachedAfterWrite,
  alreadyDetachedBufferAtWrite,
  alreadyDetachedViewAtWrite,
  outOfBoundsViewAtWrite,
  lyingAboutLength,
} from 'buffer-lifecycle';

export {
  writesBeforeReads,
  readsBeforeWrites,
  aggregateContentPreserved,
} from 'ordering';

export {
  supportsByobReader,
  partialFillAcrossReads,
  byobViewLyingAboutLength,
  byobViewLyingAfterEnqueue,
  eofReturnsZeroLengthView,
} from 'byob';

export {
  queuedWritesAndCloseBufferUntilRead,
  defaultHighWaterMarkIsOne,
  explicitHighWaterMarkIsInitialDesiredSize,
  desiredSizeTracksBytes,
  stringWriteDesiredSizeAccounting,
  readyReflectsBackpressure,
} from 'backpressure';

export {
  closeResolvesPendingRead,
  closeThenReadIsDone,
  bufferedDataDrainsBeforeDone,
} from 'close-propagation';

export {
  abortRejectsPendingRead,
  abortRejectsSubsequentReads,
  abortRejectsSubsequentWrites,
} from 'abort-propagation';

export {
  cancelRejectsPendingWriteAndClose,
  cancelRejectsSubsequentWrites,
  cancelledReaderReadsResolveDone,
} from 'cancel-propagation';

export {
  exactLengthSingleWrite,
  exactLengthTwoWrites,
  zeroLengthStreamClosesCleanly,
  highWaterMarkCappedAtExpectedLength,
  highWaterMarkKeptWhenSmaller,
  highWaterMarkCappedWithBigintLength,
  cappedHighWaterMarkWithDataFlow,
} from 'fixed-length';

export {
  singleOverwriteErrorsReadable,
  incrementalOverwriteErrorsReadable,
  underwriteErrorsStream,
  closeWithoutAnyWriteErrorsStream,
  abortSkipsUnderwriteCheck,
} from 'fixed-length-errors';

export {
  teeIdentityBothBranches,
  teeFixedLengthBothBranches,
  teeSingleBranchReadDoesNotHang,
} from 'tee';

export {
  teeCreatesNoDemand,
  singleBranchReadDrivesWriter,
  writerDesiredSizeAcrossTee,
  cancelOneBranchKeepsWriterFlowing,
  writeAfterBothBranchesCancel,
} from 'tee-backpressure';

export {
  teeOfTeeBranchDeliversToAllLeaves,
  nestedTeeSingleLeafReadDoesNotHang,
} from 'tee-nested';

export {
  drainingReaderExposure,
  drainingReaderExpectedLengthPassThrough,
  drainingReaderDrainsIdentityStream,
  drainingReaderBatchesBufferedChunks,
  drainingReaderDrainsFixedLengthStream,
  drainingReaderLocking,
} from 'draining-reader';

export {
  responseTextReadsIdentityBody,
  responseBodyIsTheSameStream,
  requestWithIdentityBody,
  fixedLengthResponseText,
  largeResponseBody,
  largeRequestBody,
  largeFixedLengthResponseBody,
  fixedLengthUnderwriteRejectsBodyRead,
} from 'body-integration';

export {
  pipeToBetweenIdentityStreams,
  pipeToDeliversLargeBody,
  pipeToPropagatesSourceError,
  pipeToPropagatesDestinationError,
  pipeThroughIdentityStream,
  circularPipeThrough,
} from 'pipe-integration';
