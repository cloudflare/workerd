// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the htmlrewriter streams suite. Explicit named
// re-exports only.

export {
  passthroughJsValueStream,
  passthroughJsByteStream,
  handlerAcrossChunkBoundaries,
  rewrittenBodyIsReadableStream,
  contentFromReadableStream,
  identityStreamBody,
  cancelDoesNotReachSource,
  erroringSourceRejectsConsumption,
  largeDocumentThroughHandler,
} from 'rewrite-streams';
