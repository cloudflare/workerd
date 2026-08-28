// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the readable-byte suite. Explicit named re-exports
// only. The default export serves the SELF binding used by the
// integration tests (echo).

export default {
  async fetch(request) {
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
} from 'pull-timing';

export {
  byobRequestOnDefaultRead,
  enqueueDiscardsByobRequest,
  closeWithPartiallyFilledView,
  readAfterCloseReturnsEmptyView,
  readDetachesCallerBuffer,
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
