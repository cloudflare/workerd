// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the compression streams suite. Re-exports are explicit
// (not `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test. The default export
// serves the SELF loopback fetches driven by body-integration.js.

import { handleFetch } from 'body-integration';

export default { fetch: handleFetch };

export {
  toStringTagBranding,
  codecFactoryNotExposed,
  sidesAreStableStreamInstances,
  accessorBrandChecks,
} from 'api-surface';

export {
  validFormatsConstruct,
  invalidFormatThrows,
  nonStringFormatThrows,
} from 'construction';

export { allFormatsRoundTrip, pendingReadServedOnWrite } from 'round-trip';

export {
  byteAtATimeCompression,
  splitCompressedInput,
  allFormatsChunkedWrites,
} from 'chunk-boundaries';

export { largePayloadRoundtrip } from 'large-payload';

export { emptyPayloadRoundTrip } from 'empty-stream';

export {
  invalidDataRejectsWrite,
  invalidDataErrorsIteration,
  invalidMagicBytesReject,
} from 'corrupt-input';

export {
  trailingDataRejectsWrite,
  closeWithoutAnyDataRejects,
  truncatedMemberRejectsClose,
} from 'strict-checks';

export { byobReadSupported } from 'byob';

export { writesSettleWithoutReads } from 'backpressure';

export {
  abortRejectsPendingRead,
  cancelSettlesPendingRead,
  abortErrorsBothSides,
  cancelReadableWritableAftermath,
} from 'propagation';

export {
  compressDecompressPipeChain,
  transformSourceThroughCompression,
  compressionThroughIdentity,
  badDataPropagatesThroughPipes,
} from 'pipe-integration';

export {
  responseBodyThroughDecompression,
  decompressThroughTransformsToIdentity,
  compressionPipeline,
  decompressionPipeline,
} from 'body-integration';

export { decompressionStreamUnhandledRejection } from 'unhandled-rejection';

export {
  mutationAfterWriteIsInvisible,
  detachAfterWriteStillDelivers,
  resizableShrinkAfterWriteStillDelivers,
  alreadyDetachedChunkIsNoop,
  lyingMetadataNeverConsulted,
} from 'buffer-lifecycle';
