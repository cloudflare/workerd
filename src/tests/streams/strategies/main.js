// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the queuing strategies suite. Re-exports are explicit
// (not `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  toStringTagBranding,
  accessorPlacementAndBrandChecks,
  highWaterMarkReflectsInit,
  sizeFunctionIdentity,
  sizeFunctionShape,
} from 'api-surface';

export {
  initDictionaryIsRequired,
  missingHighWaterMarkDiverges,
  highWaterMarkIsUnrestrictedDouble,
} from 'construction';

export {
  countSizeIsAlwaysOne,
  byteLengthSizeOnBufferSources,
  byteLengthSizeReadsPlainObjects,
  byteLengthSizeNullishDiverges,
  byteLengthSizeShadowingGetterDiverges,
} from 'size-semantics';

export {
  strategiesDriveReadableDesiredSize,
  strategiesDriveWritableDesiredSize,
  classSizeUsableInPlainStrategyBag,
  fractionalAndZeroHighWaterMarks,
} from 'integration';

export {
  chunkGetterThrowPropagates,
  chunkGetterReentersSize,
  sizeIsReceiverAgnostic,
  initGetterReentersConstructor,
} from 'reentrancy';
