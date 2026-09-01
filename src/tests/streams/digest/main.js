// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the digest streams suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  isRealWritableStreamSubclass,
  toStringTagBranding,
  accessorsAreBrandChecked,
  constructorIsSubclassable,
} from 'api-surface';

export {
  algorithmNamesAreCaseInsensitive,
  crcNamesStillRequireAnExactSpelling,
  unknownAlgorithmThrowsSynchronously,
  algorithmAsObject,
  allAlgorithms,
  optionBagAcceptsObjectsAndRejectsPrimitives,
  argumentTypesAreCheckedBeforeAlgorithmLookup,
} from 'construction';

export {
  md5Vectors,
  typedArrayChunks,
  awsChecksumVectors,
  multipleWritesAccumulate,
} from 'digest-vectors';

export {
  stringChunksAreNotTextEncodedByDefault,
  toWellFormedMatchesTextEncoder,
  toWellFormedIsInertForValidInput,
  toWellFormedDoesNotChangeBytesWritten,
  toWellFormedDefaultsToFalse,
  toWellFormedIsCoerced,
  toWellFormedJoinsSurrogatePairsAcrossChunks,
  toWellFormedFlushesDanglingLeadSurrogate,
  defaultEncodingIsPerChunk,
} from 'string-encoding';

export {
  rejectsNonByteChunks,
  sharedArrayBufferHandling,
  dataViewRespectsOffset,
} from 'chunk-types';

export {
  digestPromiseIdentityIsStable,
  bytesWrittenIsBigInt,
} from 'digest-promise';

export {
  closeResolvesDigest,
  abortRejectsDigest,
  abortAfterWritesRejectsDigest,
  writeAfterCloseRejects,
  doubleCloseIsSafe,
  abandonedStreamDoesNotCrash,
  abandonedWriterDoesNotThrow,
  unusedStreamDoesNotCrash,
} from 'lifecycle';

export {
  disposeErrorsDigestAndWrites,
  disposeThenZeroLengthWrite,
  disposeIsIdempotent,
  disposeAfterCloseIsNoOp,
} from 'dispose';

export { abandonedDigestReporting } from 'unhandled-rejection';

export {
  mutationAfterWriteIsInvisible,
  detachAfterWriteIsInvisible,
  lyingMetadataNeverConsulted,
} from 'buffer-lifecycle';

export {
  pipeToWorks,
  pipeThroughIntoDigest,
  responseBodyIntoDigest,
} from 'pipe-integration';

export { largeChunksDigest } from 'large-payload';

export {
  abandonedDigestNotSettledByGc,
  writerRemainsOperableAcrossGc,
} from 'gc-interplay';

export {
  thenInterceptionDuringDigestResolution,
  writeFromWriteContinuation,
  optionGetterReentersConstructor,
} from 'reentrancy';

export { desiredSizeCountsAndRecovers } from 'backpressure';
